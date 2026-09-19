#include <MediaCinemaRAW/Encoder.h>
#include <MediaCinemaRAW/ContainerWriter.h>
// SPDX-License-Identifier: GPL-3.0-only

#include <cassert>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

uint32_t loadU32(const std::string& b, size_t p) {
    assert(p + 4 <= b.size());
    return static_cast<uint32_t>(static_cast<uint8_t>(b[p])) |
           (static_cast<uint32_t>(static_cast<uint8_t>(b[p + 1])) << 8) |
           (static_cast<uint32_t>(static_cast<uint8_t>(b[p + 2])) << 16) |
           (static_cast<uint32_t>(static_cast<uint8_t>(b[p + 3])) << 24);
}

int64_t loadI64(const std::string& b, size_t p) {
    assert(p + 8 <= b.size());
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i)
        v |= static_cast<uint64_t>(static_cast<uint8_t>(b[p + i])) << (8 * i);
    int64_t out = 0;
    std::memcpy(&out, &v, 8);
    return out;
}

float loadF32(const std::string& b, size_t p) {
    const uint32_t u = loadU32(b, p);
    float f = 0;
    std::memcpy(&f, &u, 4);
    return f;
}

// Collect every item header {type -> {offsets of payloads}} by linear scan.
// Footer-adjacent frame index (type 1) and footer (type 0) included.
struct ItemRef {
    uint32_t type;
    uint32_t size;
    size_t payload;
};

std::vector<ItemRef> scanItems(const std::string& bytes) {
    // Skip 8-byte header + container metadata item.
    assert(bytes.size() > 8);
    size_t pos = 0;
    assert(bytes.compare(0, 7, "MOTION ") == 0);
    assert(static_cast<uint8_t>(bytes[7]) == 3);
    pos = 8;
    assert(loadU32(bytes, pos) == 3);
    const uint32_t metaSize = loadU32(bytes, pos + 4);
    pos += 8 + metaSize;
    std::vector<ItemRef> items;
    // Stop before the trailing frame-index-list + footer (16 + 8 + 16 + 8).
    while (pos + 8 <= bytes.size()) {
        const uint32_t type = loadU32(bytes, pos);
        const uint32_t size = loadU32(bytes, pos + 4);
        // Frame index list (type 1) terminates the generic region.
        if (type == 1 || type == 0) break;
        assert(pos + 8 + size <= bytes.size());
        items.push_back({type, size, pos + 8});
        pos += 8 + size;
    }
    return items;
}

}  // namespace

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

    // Argument guards: null with n > 0 throws before any I/O.
    {
        mediacinemaraw::ContainerWriter writer("test.mcraw", "{}");
        bool threw = false;
        try {
            writer.writeGyro(nullptr, 1);
        } catch (const std::invalid_argument&) {
            threw = true;
        }
        assert(threw);
        threw = false;
        try {
            writer.writeAccelerometer(nullptr, 1);
        } catch (const std::invalid_argument&) {
            threw = true;
        }
        assert(threw);
        threw = false;
        try {
            writer.writeAudio(nullptr, 2, 0);
        } catch (const std::invalid_argument&) {
            threw = true;
        }
        assert(threw);
        // Empty motion calls are a no-op (valid ptr or null alike).
        mediacinemaraw::GyroSample dummy{0, 0, 0, 0};
        writer.writeGyro(&dummy, 0);
        writer.writeGyro(nullptr, 0);
        mediacinemaraw::AccelerometerSample dummyA{0, 0, 0, 0};
        writer.writeAccelerometer(&dummyA, 0);
        writer.writeAccelerometer(nullptr, 0);
        writer.writeFrame(first, 1000000000LL,
                          "{\"width\":64,\"height\":8,\"compressionType\":7}");
        writer.close();
    }
    std::remove("test.mcraw");

    // Overflow guards: payload would exceed u32 item size.
    {
        mediacinemaraw::ContainerWriter writer("test.mcraw", "{}");
        mediacinemaraw::GyroSample dummy{0, 0, 0, 0};
        bool threw = false;
        try {
            writer.writeGyro(&dummy, (size_t)(UINT32_MAX - 8) / 24 + 1);
        } catch (const std::length_error&) {
            threw = true;
        }
        assert(threw);
        mediacinemaraw::AccelerometerSample dummyA{0, 0, 0, 0};
        threw = false;
        try {
            writer.writeAccelerometer(&dummyA, (size_t)(UINT32_MAX - 8) / 24 + 1);
        } catch (const std::length_error&) {
            threw = true;
        }
        assert(threw);
        int16_t s = 0;
        threw = false;
        try {
            writer.writeAudio(&s, (size_t)UINT32_MAX / 2 + 1, 0);
        } catch (const std::length_error&) {
            threw = true;
        }
        assert(threw);
        writer.writeFrame(first, 1000000000LL,
                          "{\"width\":64,\"height\":8,\"compressionType\":7}");
        writer.close();
    }
    std::remove("test.mcraw");

    // Full motion container: gyro 2 chunks (3 + 1), accel 2 chunks (2 + 1).
    {
        mediacinemaraw::ContainerWriter writer("test.mcraw",
            "{\"manufacturer\":\"Example\",\"model\":\"Camera 1\","
            "\"UniqueCameraModel\":\"Example Camera 1\","
            "\"uniqueCameraModel\":\"Example Camera 1\","
            "\"extraData\":{\"audioSampleRate\":48000,\"audioChannels\":2}}");
        writer.writeFrame(first,1000000000LL,"{\"width\":64,\"height\":8,\"compressionType\":7}");
        int16_t pcm[]={1,-2,3,-4}; writer.writeAudio(pcm,4,1001000000LL);
        mediacinemaraw::GyroSample chunk1[]={
            {1002000000LL,0.1f,-0.2f,0.3f},
            {1003000000LL,1.0f,2.0f,3.0f},
            {1004000000LL,-1.5f,0.0f,9.81f},
        };
        writer.writeGyro(chunk1,3);
        writer.writeGyro(chunk1,0);  // no-op: must not create an index entry
        mediacinemaraw::GyroSample chunk2[]={ {1005000000LL,4.0f,5.0f,6.0f} };
        writer.writeGyro(chunk2,1);
        mediacinemaraw::AccelerometerSample accel1[]={
            {1002500000LL,0.0f,9.81f,0.0f},
            {1003500000LL,1.0f,9.0f,-1.0f},
        };
        writer.writeAccelerometer(accel1,2);
        writer.writeAccelerometer(accel1,0);  // no-op
        mediacinemaraw::AccelerometerSample accel2[]={ {1005500000LL,0.5f,9.5f,0.25f} };
        writer.writeAccelerometer(accel2,1);
        writer.close(); assert(writer.frameCount()==1);
        // Post-close writes must fail; double close() is idempotent.
        bool threw = false;
        try { writer.writeGyro(chunk2,1); } catch (const std::logic_error&) { threw = true; }
        assert(threw);
        threw = false;
        try { writer.writeAccelerometer(accel2,1); } catch (const std::logic_error&) { threw = true; }
        assert(threw);
        threw = false;
        try { writer.writeAudio(pcm,4,1001000000LL); } catch (const std::logic_error&) { threw = true; }
        assert(threw);
        threw = false;
        try { writer.writeFrame(first,1033333333LL,"{\"width\":64,\"height\":8,\"compressionType\":7}"); }
        catch (const std::logic_error&) { threw = true; }
        assert(threw);
        writer.close();
    }
    {
        std::ifstream input("test.mcraw", std::ios::binary);
        const std::string bytes((std::istreambuf_iterator<char>(input)),
                                std::istreambuf_iterator<char>());
        assert(bytes.find("\"UniqueCameraModel\":\"Example Camera 1\"") != std::string::npos);
        assert(bytes.find("\"uniqueCameraModel\":\"Example Camera 1\"") != std::string::npos);

        const auto items = scanItems(bytes);
        int gyroData = 0, gyroIndex = 0, accelData = 0, accelIndex = 0;
        size_t dataPayloads[2] = {0, 0};
        uint32_t dataCounts[2] = {0, 0};
        size_t accelPayloads[2] = {0, 0};
        uint32_t accelCounts[2] = {0, 0};
        for (const auto& it : items) {
            if (it.type == 9) {
                assert(gyroData < 2);
                dataPayloads[gyroData] = it.payload;
                assert(it.size >= 8);
                assert(loadU32(bytes, it.payload) == 1);  // version
                dataCounts[gyroData] = loadU32(bytes, it.payload + 4);
                assert(8u + dataCounts[gyroData] * 24u == it.size);
                ++gyroData;
            } else if (it.type == 8) {
                ++gyroIndex;
                assert(it.size == 8 + 2 * 16);  // 2 chunks, empty call emitted nothing
                assert(loadU32(bytes, it.payload) == 1);
                assert(loadU32(bytes, it.payload + 4) == 2);
                // Index timestamps are the first sample of each chunk.
                assert(loadI64(bytes, it.payload + 8 + 8) == 1002000000LL);
                assert(loadI64(bytes, it.payload + 24 + 8) == 1005000000LL);
            } else if (it.type == 13) {
                assert(accelData < 2);
                accelPayloads[accelData] = it.payload;
                assert(it.size >= 8);
                assert(loadU32(bytes, it.payload) == 1);
                accelCounts[accelData] = loadU32(bytes, it.payload + 4);
                assert(8u + accelCounts[accelData] * 24u == it.size);
                ++accelData;
            } else if (it.type == 12) {
                ++accelIndex;
                assert(it.size == 8 + 2 * 16);
                assert(loadU32(bytes, it.payload) == 1);
                assert(loadU32(bytes, it.payload + 4) == 2);
                assert(loadI64(bytes, it.payload + 8 + 8) == 1002500000LL);
                assert(loadI64(bytes, it.payload + 24 + 8) == 1005500000LL);
            }
        }
        assert(gyroData == 2 && gyroIndex == 1);
        assert(accelData == 2 && accelIndex == 1);
        assert(dataCounts[0] == 3 && dataCounts[1] == 1);
        assert(accelCounts[0] == 2 && accelCounts[1] == 1);
        // Sample bytes: timestamp + axes + reserved == 0.
        assert(loadI64(bytes, dataPayloads[0] + 8) == 1002000000LL);
        assert(loadF32(bytes, dataPayloads[0] + 16) == 0.1f);
        assert(loadU32(bytes, dataPayloads[0] + 28) == 0);
        assert(loadI64(bytes, dataPayloads[0] + 8 + 24) == 1003000000LL);
        assert(loadI64(bytes, dataPayloads[0] + 8 + 48) == 1004000000LL);
        assert(loadF32(bytes, dataPayloads[0] + 8 + 48 + 16) == 9.81f);
        assert(loadI64(bytes, dataPayloads[1] + 8) == 1005000000LL);
        assert(loadF32(bytes, dataPayloads[1] + 16) == 4.0f);
        assert(loadU32(bytes, dataPayloads[1] + 28) == 0);
        // Accelerometer bytes share the 24-byte MotionSample layout.
        assert(loadI64(bytes, accelPayloads[0] + 8) == 1002500000LL);
        assert(loadF32(bytes, accelPayloads[0] + 16) == 0.0f);
        assert(loadF32(bytes, accelPayloads[0] + 20) == 9.81f);
        assert(loadU32(bytes, accelPayloads[0] + 28) == 0);
        assert(loadI64(bytes, accelPayloads[0] + 8 + 24) == 1003500000LL);
        assert(loadI64(bytes, accelPayloads[1] + 8) == 1005500000LL);
        assert(loadF32(bytes, accelPayloads[1] + 16) == 0.5f);
        assert(loadU32(bytes, accelPayloads[1] + 28) == 0);
    }
    std::remove("test.mcraw");
}
