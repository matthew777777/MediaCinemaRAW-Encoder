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

uint64_t fnv1aBytes(const std::string& b) {
    uint64_t h = 1469598103934665603ull;
    for (uint8_t c : b) { h ^= c; h *= 1099511628211ull; }
    return h;
}

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

    // Full motion container: writes coalesce — 1 gyro chunk (3 + 1),
    // 1 accel chunk (2 + 1). motioncam-decoder rejects files with more
    // motion chunks than frames + 1, so per-call chunks are not emitted.
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
                assert(it.size == 8 + 1 * 16);  // coalesced, empty call emitted nothing
                assert(loadU32(bytes, it.payload) == 1);
                assert(loadU32(bytes, it.payload + 4) == 1);
                // Index timestamp is the first sample of the chunk.
                assert(loadI64(bytes, it.payload + 8 + 8) == 1002000000LL);
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
                assert(it.size == 8 + 1 * 16);
                assert(loadU32(bytes, it.payload) == 1);
                assert(loadU32(bytes, it.payload + 4) == 1);
                assert(loadI64(bytes, it.payload + 8 + 8) == 1002500000LL);
            }
        }
        assert(gyroData == 1 && gyroIndex == 1);
        assert(accelData == 1 && accelIndex == 1);
        assert(dataCounts[0] == 4);
        assert(accelCounts[0] == 3);
        // Sample bytes: timestamp + axes + reserved == 0, in call order.
        assert(loadI64(bytes, dataPayloads[0] + 8) == 1002000000LL);
        assert(loadF32(bytes, dataPayloads[0] + 16) == 0.1f);
        assert(loadU32(bytes, dataPayloads[0] + 28) == 0);
        assert(loadI64(bytes, dataPayloads[0] + 8 + 24) == 1003000000LL);
        assert(loadI64(bytes, dataPayloads[0] + 8 + 48) == 1004000000LL);
        assert(loadF32(bytes, dataPayloads[0] + 8 + 48 + 16) == 9.81f);
        assert(loadI64(bytes, dataPayloads[0] + 8 + 72) == 1005000000LL);
        assert(loadF32(bytes, dataPayloads[0] + 8 + 72 + 8) == 4.0f);
        assert(loadU32(bytes, dataPayloads[0] + 8 + 72 + 20) == 0);
        // Accelerometer bytes share the 24-byte MotionSample layout.
        assert(loadI64(bytes, accelPayloads[0] + 8) == 1002500000LL);
        assert(loadF32(bytes, accelPayloads[0] + 16) == 0.0f);
        assert(loadF32(bytes, accelPayloads[0] + 20) == 9.81f);
        assert(loadU32(bytes, accelPayloads[0] + 28) == 0);
        assert(loadI64(bytes, accelPayloads[0] + 8 + 24) == 1003500000LL);
        assert(loadI64(bytes, accelPayloads[0] + 8 + 48) == 1005500000LL);
        assert(loadF32(bytes, accelPayloads[0] + 8 + 48 + 8) == 0.5f);
        assert(loadU32(bytes, accelPayloads[0] + 8 + 48 + 20) == 0);
        // Whole-file golden: ContainerWriter I/O must stay byte-identical.
        assert(bytes.size() == 1794);
        assert(fnv1aBytes(bytes) == 0xa5a60214c0d84237ULL);
    }
    std::remove("test.mcraw");

    // Padded-width clamp (#1) + bit-identical fast paths (#2).
    // NOTE: in-range metadata tail padding was tried and reverted: deployed
    // readers validate zero padding, so metadata() keeps zero tails and
    // these goldens pin that wire convention.
    {
        auto u32le = [](const std::vector<uint8_t>& v, size_t p) -> uint32_t {
            assert(p + 4 <= v.size());
            return uint32_t(v[p]) | (uint32_t(v[p + 1]) << 8) |
                   (uint32_t(v[p + 2]) << 16) | (uint32_t(v[p + 3]) << 24);
        };
        auto fnv1a = [](const std::vector<uint8_t>& v) -> uint64_t {
            uint64_t h = 1469598103934665603ull;
            for (uint8_t b : v) { h ^= b; h *= 1099511628211ull; }
            return h;
        };
        // Golden: 64x8 pattern has no padding (ew == w), so #1 cannot
        // affect it; #2 must be bit-identical to the pre-change bytes.
        assert(first.size() == 1156);
        assert(fnv1a(first) == 0xa0ca7970d54a27fdULL);

        // Padded: 66x4 constant 3000 (ew = 128). Zero padding used to
        // drag tile channels to lo=0/hi=3000 (16-bit, 708 bytes); the
        // clamp keeps padded samples at the visible lo so every channel
        // is bits=0 and the tile payload is empty.
        constexpr int wB = 66, hB = 4, sB = wB * 2;
        std::vector<uint8_t> rawB(hB * sB, 0);
        for (int y = 0; y < hB; ++y)
            for (int x = 0; x < wB; ++x) {
                rawB[y * sB + x * 2] = uint8_t(3000);
                rawB[y * sB + x * 2 + 1] = uint8_t(3000 >> 8);
            }
        std::vector<uint8_t> outB1, outB2;
        mediacinemaraw::encode(rawB.data(), rawB.size(), wB, hB, sB,
                               false, 0, hB, false, outB1);
        mediacinemaraw::encode(rawB.data(), rawB.size(), wB, hB, sB,
                               false, 0, hB, false, outB2);
        assert(outB1 == outB2);
        // Buffer reuse: encoding into the same vector must match fresh.
        mediacinemaraw::encode(rawB.data(), rawB.size(), wB, hB, sB,
                               false, 0, hB, false, outB1);
        assert(outB1 == outB2);
        assert(u32le(outB1, 0) == 128);  // ew preserved
        assert(u32le(outB1, 4) == 4);    // h preserved
        assert(u32le(outB1, 8) == 16);   // off1: zero tile payload
        // Was 708 before the clamp; the refs tail keeps zero padding.
        assert(outB1.size() == 156);
        assert(fnv1a(outB1) == 0x907450a98cf63d41ULL);

        // Golden: 64x4 constant 3000, no padding, bit-identical through #2.
        constexpr int wC = 64, hC = 4, sC = wC * 2;
        std::vector<uint8_t> rawC(hC * sC, 0);
        for (int y = 0; y < hC; ++y)
            for (int x = 0; x < wC; ++x) {
                rawC[y * sC + x * 2] = uint8_t(3000);
                rawC[y * sC + x * 2 + 1] = uint8_t(3000 >> 8);
            }
        std::vector<uint8_t> outC;
        mediacinemaraw::encode(rawC.data(), rawC.size(), wC, hC, sC,
                               false, 0, hC, false, outC);
        assert(outC.size() == 156);
        assert(fnv1a(outC) == 0x6f46729dbbcc0211ULL);
    }

    // encode_parallel() must be byte-identical to encode() for any thread
    // count, across RAW16/RAW10, bin, stride slack, and partial edge tiles
    // (band splits must not disturb tile order or the padding clamp).
    {
        auto xorshift = [](uint32_t& s) -> uint16_t {
            s ^= s << 13; s ^= s >> 17; s ^= s << 5;
            return uint16_t(s >> 7);
        };
        auto check = [&](int width, int height, int slack, bool raw10,
                         int cropTop, int cropHeight, bool bin) {
            const size_t rb = raw10 ? size_t(width)/4*5 : size_t(width)*2;
            const int stride = int(rb) + slack;
            std::vector<uint8_t> raw(size_t(height-1)*stride + rb, 0);
            uint32_t st = 0x243F6A89u;
            for (int y = 0; y < height; ++y) for (int x = 0; x < width; ++x) {
                uint16_t v = xorshift(st);
                if (raw10) v %= 1024;
                if (raw10) {
                    raw[y*stride+x/4*5+x%4] = uint8_t(v>>2);
                    if (x%4 == 0) raw[y*stride+x/4*5+4] = 0;
                    raw[y*stride+x/4*5+4] = uint8_t(raw[y*stride+x/4*5+4] | ((v&3) << (2*(x%4))));
                } else {
                    raw[y*stride+x*2] = uint8_t(v); raw[y*stride+x*2+1] = uint8_t(v>>8);
                }
            }
            std::vector<uint8_t> serial, par;
            mediacinemaraw::encode(raw.data(), raw.size(), width, height, stride,
                                   raw10, cropTop, cropHeight, bin, serial);
            for (unsigned t : {0u, 1u, 2u, 3u, 7u}) {
                mediacinemaraw::encode_parallel(raw.data(), raw.size(), width, height, stride,
                                                raw10, cropTop, cropHeight, bin, par, t);
                assert(par == serial);
            }
            // Repeated parallel calls are deterministic.
            std::vector<uint8_t> par2;
            mediacinemaraw::encode_parallel(raw.data(), raw.size(), width, height, stride,
                                            raw10, cropTop, cropHeight, bin, par2, 0);
            assert(par2 == serial);
        };
        check(64, 8, 16, false, 0, 8, false);    // full tiles, slack stride
        check(68, 12, 0, false, 0, 12, false);   // partial tiles, 3 bands vs 7 workers
        check(68, 16, 8, true, 2, 12, false);    // RAW10, crop, partial
        check(128, 8, 4, false, 0, 8, true);     // bin, 1 band (serial fallback)
        check(260, 24, 24, true, 0, 24, false);  // RAW10 wide, many bands
        // Error parity: same geometry rejection before any thread spawns.
        std::vector<uint8_t> tiny(64, 0), tmp;
        for (int bad = 0; bad < 2; ++bad) {
            bool s = false, p = false;
            try { mediacinemaraw::encode(tiny.data(), tiny.size(), 64, 8, bad ? 10 : 128,
                                         false, bad ? 0 : 1, 8, false, tmp); }
            catch (const std::invalid_argument&) { s = true; }
            try { mediacinemaraw::encode_parallel(tiny.data(), tiny.size(), 64, 8, bad ? 10 : 128,
                                                  false, bad ? 0 : 1, 8, false, tmp, 4); }
            catch (const std::invalid_argument&) { p = true; }
            assert(s && p);
        }
    }

    // Convenience overload: full-frame lossless, identical to the
    // 10-argument form with cropTop = 0, cropHeight = height, bin = false.
    {
        constexpr int wO = 68, hO = 12, sO = wO * 2 + 8;
        std::vector<uint8_t> rawO((hO - 1) * sO + wO * 2, 0);
        for (int y = 0; y < hO; ++y)
            for (int x = 0; x < wO; ++x) {
                const uint16_t v = static_cast<uint16_t>((x * 257 + y * 13) & 8191);
                rawO[y * sO + x * 2] = static_cast<uint8_t>(v);
                rawO[y * sO + x * 2 + 1] = static_cast<uint8_t>(v >> 8);
            }
        std::vector<uint8_t> full, easy;
        mediacinemaraw::encode(rawO.data(), rawO.size(), wO, hO, sO,
                               false, 0, hO, false, full);
        mediacinemaraw::encode(rawO.data(), rawO.size(), wO, hO, sO, easy);
        assert(easy == full);
        constexpr int wR = 64, hR = 8, sR = wR / 4 * 5;
        std::vector<uint8_t> rawR(hR * sR, 0);
        for (int y = 0; y < hR; ++y)
            for (int x = 0; x < wR; ++x) {
                const uint16_t v = static_cast<uint16_t>((x * 31 + y * 7) & 1023);
                rawR[y * sR + x / 4 * 5 + x % 4] = static_cast<uint8_t>(v >> 2);
                if (x % 4 == 0) rawR[y * sR + x / 4 * 5 + 4] = 0;
                rawR[y * sR + x / 4 * 5 + 4] =
                    static_cast<uint8_t>(rawR[y * sR + x / 4 * 5 + 4] | ((v & 3) << (2 * (x % 4))));
            }
        mediacinemaraw::encode(rawR.data(), rawR.size(), wR, hR, sR,
                               true, 0, hR, false, full);
        mediacinemaraw::encode(rawR.data(), rawR.size(), wR, hR, sR, easy, true);
        assert(easy == full);
    }

    // Per-frame coalescing: 251 tiny drains per sensor collapse to
    // frames + 1 chunks, so motioncam-decoder always opens the file.
    {
        mediacinemaraw::ContainerWriter writer("test.mcraw", "{}");
        const int64_t t0 = 2000000000LL;
        int64_t sts = t0;
        for (int f = 0; f < 5; ++f) {
            for (int d = 0; d < 50; ++d) {
                mediacinemaraw::GyroSample g[7];
                mediacinemaraw::AccelerometerSample a[7];
                for (int i = 0; i < 7; ++i) {
                    g[i] = {sts, 0.1f, -0.2f, 0.3f};
                    a[i] = {sts, 1.0f, 2.0f, 9.8f};
                    sts += 2000000LL;  // 500 Hz
                }
                writer.writeGyro(g, 7);
                writer.writeAccelerometer(a, 7);
            }
            writer.writeFrame(first, t0 + (int64_t)f * 33333333LL,
                              "{\"width\":64,\"height\":8,\"compressionType\":7}");
        }
        mediacinemaraw::GyroSample gt{sts, 0, 0, 0};
        mediacinemaraw::AccelerometerSample at{sts, 0, 0, 0};
        writer.writeGyro(&gt, 1);
        writer.writeAccelerometer(&at, 1);
        int16_t tail[] = {7, -7};
        writer.writeAudio(tail, 2, t0 + 4 * 33333333LL);
        writer.close();
        assert(writer.frameCount() == 5);
    }
    {
        std::ifstream input("test.mcraw", std::ios::binary);
        const std::string bytes((std::istreambuf_iterator<char>(input)),
                                std::istreambuf_iterator<char>());
        const auto items = scanItems(bytes);
        int gyroData = 0, accelData = 0;
        size_t gyroSamples = 0, accelSamples = 0;
        size_t audioIndexAt = 0, lastFrameAt = 0;
        bool sawAudioIndex = false;
        for (const auto& it : items) {
            const size_t head = it.payload - 8;
            if (it.type == 2) lastFrameAt = head;
            if (it.type == 9) {
                ++gyroData;
                assert(loadU32(bytes, it.payload) == 1);
                gyroSamples += loadU32(bytes, it.payload + 4);
            } else if (it.type == 13) {
                ++accelData;
                assert(loadU32(bytes, it.payload) == 1);
                accelSamples += loadU32(bytes, it.payload + 4);
            } else if (it.type == 4) {
                sawAudioIndex = true;
                audioIndexAt = head;
            } else if (it.type == 8 || it.type == 12) {
                assert(loadU32(bytes, it.payload) == 1);
                assert(loadU32(bytes, it.payload + 4) == 6);
                assert(it.size == 8 + 6 * 16);
            }
        }
        assert(gyroData == 6 && accelData == 6);
        assert(gyroSamples == 5 * 50 * 7 + 1);
        assert(accelSamples == 5 * 50 * 7 + 1);
        // Tail order: pre-gyro readers (MotionCam Tools v1.0) stop their
        // scan at the first motion item, so no motion data may sit between
        // the last frame and the audio index.
        assert(sawAudioIndex);
        for (const auto& it : items) {
            if (it.type == 9 || it.type == 13) {
                const size_t head = it.payload - 8;
                assert(head < lastFrameAt || head > audioIndexAt);
            }
        }
        // Whole-file golden: coalesced multi-chunk layout must stay identical.
        assert(bytes.size() == 90737);
        assert(fnv1aBytes(bytes) == 0xf9edabacf5cbe5bcULL);
    }
    std::remove("test.mcraw");
}
