#include <MediaCinemaRAW/ContainerWriter.h>
#include <MediaCinemaRAW/Encoder.h>
// SPDX-License-Identifier: GPL-3.0-only
//
// Upstream oracle: encoder writer -> motioncam-decoder motion APIs.
// Built by tools/verify_gyro.py against an external motioncam-decoder checkout;
// never vendors upstream sources.
#include <motioncam/Decoder.hpp>

#include <cassert>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

int main(int argc, char** argv) {
    const std::string path =
        argc > 1 ? argv[1] : std::string("gyro_interop.mcraw");

    constexpr int width = 64, height = 8, stride = width * 2;
    std::vector<uint8_t> raw(static_cast<size_t>(height - 1) * stride + width * 2, 0);
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x) {
            const uint16_t v = static_cast<uint16_t>((x * 17 + y * 31) & 4095);
            raw[static_cast<size_t>(y) * stride + x * 2] = static_cast<uint8_t>(v);
            raw[static_cast<size_t>(y) * stride + x * 2 + 1] = static_cast<uint8_t>(v >> 8);
        }
    std::vector<uint8_t> blob;
    mediacinemaraw::encode(raw.data(), raw.size(), width, height, stride,
                           false, 0, height, false, blob);

    {
        mediacinemaraw::ContainerWriter writer(
            path,
            "{\"manufacturer\":\"Example\",\"model\":\"Camera 1\","
            "\"UniqueCameraModel\":\"Example Camera 1\","
            "\"extraData\":{\"audioSampleRate\":48000,\"audioChannels\":1}}");
        writer.writeFrame(blob, 1000000000LL,
                          "{\"width\":64,\"height\":8,\"compressionType\":7}");
        writer.writeFrame(blob, 1033333333LL,
                          "{\"width\":64,\"height\":8,\"compressionType\":7}");
        const int16_t pcm[2] = {100, -200};
        writer.writeAudio(pcm, 2, 1001000000LL);
        mediacinemaraw::GyroSample chunk1[3] = {
            {1002000000LL, 0.1f, -0.2f, 0.3f},
            {1003000000LL, 1.0f, 2.0f, 3.0f},
            {1004000000LL, -1.5f, 0.0f, 9.81f},
        };
        writer.writeGyro(chunk1, 3);
        writer.writeGyro(chunk1, 0);  // no-op
        mediacinemaraw::GyroSample chunk2[1] = {{1005000000LL, 4.0f, 5.0f, 6.0f}};
        writer.writeGyro(chunk2, 1);
        mediacinemaraw::AccelerometerSample accel1[2] = {
            {1002500000LL, 0.0f, 9.81f, 0.0f},
            {1003500000LL, 1.0f, 9.0f, -1.0f},
        };
        writer.writeAccelerometer(accel1, 2);
        writer.writeAccelerometer(accel1, 0);  // no-op
        mediacinemaraw::AccelerometerSample accel2[1] = {{1005500000LL, 0.5f, 9.5f, 0.25f}};
        writer.writeAccelerometer(accel2, 1);
        writer.close();
        assert(writer.frameCount() == 2);
    }

    motioncam::Decoder decoder(path);
    assert(decoder.getFrames().size() == 2);
    assert(decoder.hasGyroData());
    assert(decoder.hasAccelerometerData());
    std::vector<motioncam::MotionSample> gyro;
    decoder.loadGyroData(gyro);
    assert(gyro.size() == 4);
    assert(gyro[0].timestampNs == 1002000000LL);
    assert(std::fabs(gyro[0].x - 0.1f) < 1e-6f);
    assert(std::fabs(gyro[0].y + 0.2f) < 1e-6f);
    assert(std::fabs(gyro[0].z - 0.3f) < 1e-6f);
    assert(gyro[0].reserved == 0);
    assert(gyro[1].timestampNs == 1003000000LL);
    assert(std::fabs(gyro[2].z - 9.81f) < 1e-4f);
    assert(gyro[3].timestampNs == 1005000000LL);
    assert(std::fabs(gyro[3].x - 4.0f) < 1e-6f);
    std::vector<motioncam::MotionSample> accel;
    decoder.loadAccelerometerData(accel);
    assert(accel.size() == 3);
    assert(accel[0].timestampNs == 1002500000LL);
    assert(std::fabs(accel[0].y - 9.81f) < 1e-4f);
    assert(accel[0].reserved == 0);
    assert(accel[1].timestampNs == 1003500000LL);
    assert(accel[2].timestampNs == 1005500000LL);
    assert(std::fabs(accel[2].x - 0.5f) < 1e-6f);

    // Frames remain decodable alongside gyro.
    for (auto ts : decoder.getFrames()) {
        std::vector<uint8_t> data;
        nlohmann::json meta;
        decoder.loadFrame(ts, data, meta);
        assert(meta["width"] == 64 && meta["height"] == 8);
        assert(meta["compressionType"] == 7);
        assert(data.size() == static_cast<size_t>(width * height * 2));
    }

    std::printf("upstream gyro+accel interop passed (4 gyro, 3 accel)\n");
    return 0;
}
