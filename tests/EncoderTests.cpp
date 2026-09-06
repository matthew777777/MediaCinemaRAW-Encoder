#include <MediaCinemaRAW/Encoder.h>
#include <MediaCinemaRAW/ContainerWriter.h>
// SPDX-License-Identifier: GPL-3.0-only

#include <cassert>
#include <cstdio>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

int main() {
    constexpr int width = 64;
    constexpr int height = 8;
    constexpr int stride = width * 2 + 16;
    std::vector<uint8_t> raw((height - 1) * stride + width * 2, 0);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const uint16_t value = static_cast<uint16_t>((x * 17 + y * 31) & 4095);
            raw[y * stride + x * 2] = static_cast<uint8_t>(value);
            raw[y * stride + x * 2 + 1] = static_cast<uint8_t>(value >> 8);
        }
    }

    std::vector<uint8_t> first;
    std::vector<uint8_t> second;
    mediacinemaraw::encode(raw.data(), raw.size(), width, height, stride,
                           false, 0, height, false, first);
    mediacinemaraw::encode(raw.data(), raw.size(), width, height, stride,
                           false, 0, height, false, second);
    assert(!first.empty());
    assert(first == second);

    bool rejected = false;
    try {
        mediacinemaraw::encode(raw.data(), 4, width, height, stride,
                               false, 0, height, false, first);
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    assert(rejected);
    {
        mediacinemaraw::ContainerWriter writer("test.mcraw",
            "{\"manufacturer\":\"Example\",\"model\":\"Camera 1\","
            "\"UniqueCameraModel\":\"Example Camera 1\","
            "\"uniqueCameraModel\":\"Example Camera 1\","
            "\"extraData\":{\"audioSampleRate\":48000,\"audioChannels\":2}}");
        writer.writeFrame(first,1000000000LL,"{\"width\":64,\"height\":8,\"compressionType\":7}");
        int16_t pcm[]={1,-2,3,-4}; writer.writeAudio(pcm,4,1001000000LL);
        mediacinemaraw::GyroSample gyro[]={ {1002000000LL,0.1f,-0.2f,0.3f} };
        writer.writeGyro(gyro,1); writer.close(); assert(writer.frameCount()==1);
    }
    {
        std::ifstream input("test.mcraw", std::ios::binary);
        const std::string bytes((std::istreambuf_iterator<char>(input)),
                                std::istreambuf_iterator<char>());
        assert(bytes.find("\"UniqueCameraModel\":\"Example Camera 1\"") != std::string::npos);
        assert(bytes.find("\"uniqueCameraModel\":\"Example Camera 1\"") != std::string::npos);
    }
    std::remove("test.mcraw");
}
