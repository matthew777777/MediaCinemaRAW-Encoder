// SPDX-License-Identifier: GPL-3.0-only
//
// Self-contained validation for the encoder and container writer. Assert
// based with no test framework dependency; the CMake build compiles with
// -UNDEBUG so the checks stay live in Release builds. Wire-format goldens
// (sizes and FNV-1a hashes) pin byte compatibility with deployed readers —
// any change in output bytes fails here first.
#include <MediaCinemaRAW/ContainerWriter.h>
#include <MediaCinemaRAW/Encoder.h>

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

// ---------------------------------------------------------------------------
// Container inspection helpers.
// ---------------------------------------------------------------------------

uint32_t LoadU32LE(const std::string& bytes, size_t position) {
    assert(position + 4 <= bytes.size());
    return static_cast<uint32_t>(static_cast<uint8_t>(bytes[position])) |
           (static_cast<uint32_t>(static_cast<uint8_t>(bytes[position + 1])) << 8) |
           (static_cast<uint32_t>(static_cast<uint8_t>(bytes[position + 2])) << 16) |
           (static_cast<uint32_t>(static_cast<uint8_t>(bytes[position + 3])) << 24);
}

uint32_t LoadU32LE(const std::vector<uint8_t>& bytes, size_t position) {
    assert(position + 4 <= bytes.size());
    return uint32_t(bytes[position]) | (uint32_t(bytes[position + 1]) << 8) |
           (uint32_t(bytes[position + 2]) << 16) | (uint32_t(bytes[position + 3]) << 24);
}

int64_t LoadI64LE(const std::string& bytes, size_t position) {
    assert(position + 8 <= bytes.size());
    uint64_t value = 0;
    for (int i = 0; i < 8; ++i)
        value |= static_cast<uint64_t>(static_cast<uint8_t>(bytes[position + i])) << (8 * i);
    int64_t out = 0;
    std::memcpy(&out, &value, 8);
    return out;
}

float LoadF32LE(const std::string& bytes, size_t position) {
    const uint32_t bits = LoadU32LE(bytes, position);
    float value = 0;
    std::memcpy(&value, &bits, 4);
    return value;
}

uint64_t Fnv1aHash(const void* data, size_t size) {
    uint64_t hash = 1469598103934665603ULL;
    const uint8_t* bytes = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < size; ++i) {
        hash ^= bytes[i];
        hash *= 1099511628211ULL;
    }
    return hash;
}

uint64_t Fnv1aHash(const std::string& bytes) {
    return Fnv1aHash(bytes.data(), bytes.size());
}

uint64_t Fnv1aHash(const std::vector<uint8_t>& bytes) {
    return Fnv1aHash(bytes.data(), bytes.size());
}

struct ItemRef {
    uint32_t type;
    uint32_t size;
    size_t payloadOffset;
};

// Collects every item header by linear scan. Stops at the trailing
// frame-index list (type 1) and footer (type 0).
std::vector<ItemRef> ScanContainerItems(const std::string& bytes) {
    assert(bytes.size() > 8);
    assert(bytes.compare(0, 7, "MOTION ") == 0);
    assert(static_cast<uint8_t>(bytes[7]) == 3);
    size_t position = 8;
    assert(LoadU32LE(bytes, position) == 3); // Container metadata.
    position += 8 + LoadU32LE(bytes, position + 4);

    std::vector<ItemRef> items;
    while (position + 8 <= bytes.size()) {
        const uint32_t type = LoadU32LE(bytes, position);
        const uint32_t size = LoadU32LE(bytes, position + 4);
        if (type == 1 || type == 0)
            break;
        assert(position + 8 + size <= bytes.size());
        items.push_back({type, size, position + 8});
        position += 8 + size;
    }
    return items;
}

std::string ReadWholeFile(const char* path) {
    std::ifstream input(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
}

// ---------------------------------------------------------------------------
// Input fixtures.
// ---------------------------------------------------------------------------

constexpr char kScratchContainer[] = "test.mcraw";
constexpr char kFrameJson[] = "{\"width\":64,\"height\":8,\"compressionType\":7}";

// 64x8 RAW16 gradient with stride slack; last row omits padding.
std::vector<uint8_t> MakeGradientRaw16() {
    constexpr int width = 64, height = 8, stride = width * 2 + 16;
    std::vector<uint8_t> raw((height - 1) * stride + width * 2, 0);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const uint16_t value = static_cast<uint16_t>((x * 17 + y * 31) & 4095);
            raw[y * stride + x * 2] = static_cast<uint8_t>(value);
            raw[y * stride + x * 2 + 1] = static_cast<uint8_t>(value >> 8);
        }
    }
    return raw;
}

std::vector<uint8_t> EncodeGradient() {
    const std::vector<uint8_t> raw = MakeGradientRaw16();
    std::vector<uint8_t> encoded;
    mediacinemaraw::encode(raw.data(), raw.size(), 64, 8, 64 * 2 + 16, false, 0, 8, false, encoded);
    return encoded;
}

// ---------------------------------------------------------------------------
// Encoder suites.
// ---------------------------------------------------------------------------

void TestEncodeIsDeterministic() {
    const std::vector<uint8_t> raw = MakeGradientRaw16();
    std::vector<uint8_t> first, second;
    mediacinemaraw::encode(raw.data(), raw.size(), 64, 8, 64 * 2 + 16, false, 0, 8, false, first);
    mediacinemaraw::encode(raw.data(), raw.size(), 64, 8, 64 * 2 + 16, false, 0, 8, false, second);
    assert(!first.empty());
    assert(first == second);
}

void TestEncodeRejectsTruncatedInput() {
    const std::vector<uint8_t> raw = MakeGradientRaw16();
    std::vector<uint8_t> output;
    bool rejected = false;
    try {
        mediacinemaraw::encode(raw.data(), 4, 64, 8, 64 * 2 + 16, false, 0, 8, false, output);
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    assert(rejected);
}

void TestPaddingClampAndPayloadGoldens() {
    // NOTE: in-range metadata tail padding was tried and reverted: deployed
    // readers validate zero padding, so the metadata streams keep zero tails
    // and these goldens pin that wire convention.
    const std::vector<uint8_t> gradient = EncodeGradient();
    // 64x8 pattern has no padding (padded width == width), so the edge-tile
    // clamp cannot affect it; this pins the fast-path bytes instead.
    assert(gradient.size() == 1156);
    assert(Fnv1aHash(gradient) == 0xa0ca7970d54a27fdULL);

    // Padded: 66x4 constant 3000 (padded width 128). Zero padding used to
    // drag tile channels to lo=0/hi=3000 (16-bit, 708 bytes); the clamp
    // keeps padded samples at the visible lo so every channel is bits=0
    // and the tile payload is empty.
    constexpr int paddedWidth = 66, paddedHeight = 4;
    constexpr int paddedStride = paddedWidth * 2;
    std::vector<uint8_t> paddedRaw(paddedHeight * paddedStride, 0);
    for (int y = 0; y < paddedHeight; ++y)
        for (int x = 0; x < paddedWidth; ++x) {
            paddedRaw[y * paddedStride + x * 2] = uint8_t(3000);
            paddedRaw[y * paddedStride + x * 2 + 1] = uint8_t(3000 >> 8);
        }
    std::vector<uint8_t> first, second;
    mediacinemaraw::encode(paddedRaw.data(), paddedRaw.size(), paddedWidth, paddedHeight,
                           paddedStride, false, 0, paddedHeight, false, first);
    mediacinemaraw::encode(paddedRaw.data(), paddedRaw.size(), paddedWidth, paddedHeight,
                           paddedStride, false, 0, paddedHeight, false, second);
    assert(first == second);
    // Buffer reuse: encoding into the same vector must match fresh output.
    mediacinemaraw::encode(paddedRaw.data(), paddedRaw.size(), paddedWidth, paddedHeight,
                           paddedStride, false, 0, paddedHeight, false, first);
    assert(first == second);
    assert(LoadU32LE(first, 0) == 128); // Padded width preserved.
    assert(LoadU32LE(first, 4) == 4);   // Height preserved.
    assert(LoadU32LE(first, 8) == 16);  // Bits offset: zero tile payload.
    // Was 708 before the clamp; the refs tail keeps zero padding.
    assert(first.size() == 156);
    assert(Fnv1aHash(first) == 0x907450a98cf63d41ULL);

    // Unpadded 64x4 constant 3000 golden.
    constexpr int plainWidth = 64, plainHeight = 4;
    constexpr int plainStride = plainWidth * 2;
    std::vector<uint8_t> plainRaw(plainHeight * plainStride, 0);
    for (int y = 0; y < plainHeight; ++y)
        for (int x = 0; x < plainWidth; ++x) {
            plainRaw[y * plainStride + x * 2] = uint8_t(3000);
            plainRaw[y * plainStride + x * 2 + 1] = uint8_t(3000 >> 8);
        }
    std::vector<uint8_t> plain;
    mediacinemaraw::encode(plainRaw.data(), plainRaw.size(), plainWidth, plainHeight, plainStride,
                           false, 0, plainHeight, false, plain);
    assert(plain.size() == 156);
    assert(Fnv1aHash(plain) == 0x6f46729dbbcc0211ULL);
}

void TestParallelMatchesSerial() {
    auto xorshift = [](uint32_t& state) -> uint16_t {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        return uint16_t(state >> 7);
    };
    auto check = [&](int width, int height, int slack, bool raw10, int cropTop, int cropHeight,
                     bool bin) {
        const size_t rowBytes = raw10 ? size_t(width) / 4 * 5 : size_t(width) * 2;
        const int stride = int(rowBytes) + slack;
        std::vector<uint8_t> raw(size_t(height - 1) * stride + rowBytes, 0);
        uint32_t state = 0x243F6A89u;
        for (int y = 0; y < height; ++y)
            for (int x = 0; x < width; ++x) {
                uint16_t value = xorshift(state);
                if (raw10)
                    value %= 1024;
                if (raw10) {
                    raw[y * stride + x / 4 * 5 + x % 4] = uint8_t(value >> 2);
                    if (x % 4 == 0)
                        raw[y * stride + x / 4 * 5 + 4] = 0;
                    raw[y * stride + x / 4 * 5 + 4] =
                        uint8_t(raw[y * stride + x / 4 * 5 + 4] | ((value & 3) << (2 * (x % 4))));
                } else {
                    raw[y * stride + x * 2] = uint8_t(value);
                    raw[y * stride + x * 2 + 1] = uint8_t(value >> 8);
                }
            }
        std::vector<uint8_t> serial, parallel;
        mediacinemaraw::encode(raw.data(), raw.size(), width, height, stride, raw10, cropTop,
                               cropHeight, bin, serial);
        for (unsigned threads : {0u, 1u, 2u, 3u, 7u}) {
            mediacinemaraw::encode_parallel(raw.data(), raw.size(), width, height, stride, raw10,
                                            cropTop, cropHeight, bin, parallel, threads);
            assert(parallel == serial);
        }
        // Repeated parallel calls are deterministic.
        std::vector<uint8_t> parallelAgain;
        mediacinemaraw::encode_parallel(raw.data(), raw.size(), width, height, stride, raw10,
                                        cropTop, cropHeight, bin, parallelAgain, 0);
        assert(parallelAgain == serial);
    };
    check(64, 8, 16, false, 0, 8, false);   // Full tiles, slack stride.
    check(68, 12, 0, false, 0, 12, false);  // Partial tiles, 3 bands vs 7 workers.
    check(68, 16, 8, true, 2, 12, false);   // RAW10, crop, partial tiles.
    check(128, 8, 4, false, 0, 8, true);    // Downscale, 1 band (serial fallback).
    check(260, 24, 24, true, 0, 24, false); // RAW10 wide, many bands.

    // Error parity: same geometry rejection before any thread spawns.
    std::vector<uint8_t> tiny(64, 0), output;
    for (int bad = 0; bad < 2; ++bad) {
        const int stride = bad ? 10 : 128;
        const int cropTop = bad ? 0 : 1;
        bool serialThrew = false, parallelThrew = false;
        try {
            mediacinemaraw::encode(tiny.data(), tiny.size(), 64, 8, stride, false, cropTop, 8,
                                   false, output);
        } catch (const std::invalid_argument&) {
            serialThrew = true;
        }
        try {
            mediacinemaraw::encode_parallel(tiny.data(), tiny.size(), 64, 8, stride, false, cropTop,
                                            8, false, output, 4);
        } catch (const std::invalid_argument&) {
            parallelThrew = true;
        }
        assert(serialThrew && parallelThrew);
    }
}

void TestConvenienceOverload() {
    // Full-frame lossless: identical to the 10-argument form with
    // cropTop = 0, cropHeight = height, downscale = false.
    constexpr int width = 68, height = 12, stride = width * 2 + 8;
    std::vector<uint8_t> raw((height - 1) * stride + width * 2, 0);
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x) {
            const uint16_t value = static_cast<uint16_t>((x * 257 + y * 13) & 8191);
            raw[y * stride + x * 2] = static_cast<uint8_t>(value);
            raw[y * stride + x * 2 + 1] = static_cast<uint8_t>(value >> 8);
        }
    std::vector<uint8_t> full, shorthand;
    mediacinemaraw::encode(raw.data(), raw.size(), width, height, stride, false, 0, height, false,
                           full);
    mediacinemaraw::encode(raw.data(), raw.size(), width, height, stride, shorthand);
    assert(shorthand == full);

    constexpr int raw10Width = 64, raw10Height = 8;
    constexpr int raw10Stride = raw10Width / 4 * 5;
    std::vector<uint8_t> raw10(raw10Height * raw10Stride, 0);
    for (int y = 0; y < raw10Height; ++y)
        for (int x = 0; x < raw10Width; ++x) {
            const uint16_t value = static_cast<uint16_t>((x * 31 + y * 7) & 1023);
            raw10[y * raw10Stride + x / 4 * 5 + x % 4] = static_cast<uint8_t>(value >> 2);
            if (x % 4 == 0)
                raw10[y * raw10Stride + x / 4 * 5 + 4] = 0;
            raw10[y * raw10Stride + x / 4 * 5 + 4] = static_cast<uint8_t>(
                raw10[y * raw10Stride + x / 4 * 5 + 4] | ((value & 3) << (2 * (x % 4))));
        }
    mediacinemaraw::encode(raw10.data(), raw10.size(), raw10Width, raw10Height, raw10Stride, true,
                           0, raw10Height, false, full);
    mediacinemaraw::encode(raw10.data(), raw10.size(), raw10Width, raw10Height, raw10Stride,
                           shorthand, true);
    assert(shorthand == full);
}

// ---------------------------------------------------------------------------
// Container suites.
// ---------------------------------------------------------------------------

void TestContainerArgumentGuards() {
    // Null samples with n > 0 throw before any I/O.
    mediacinemaraw::ContainerWriter writer(kScratchContainer, "{}");
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
    // Empty motion calls are a no-op (valid pointer or null alike).
    mediacinemaraw::GyroSample gyroDummy{0, 0, 0, 0};
    writer.writeGyro(&gyroDummy, 0);
    writer.writeGyro(nullptr, 0);
    mediacinemaraw::AccelerometerSample accelDummy{0, 0, 0, 0};
    writer.writeAccelerometer(&accelDummy, 0);
    writer.writeAccelerometer(nullptr, 0);
    writer.writeFrame(EncodeGradient(), 1000000000LL, kFrameJson);
    writer.close();
    std::remove(kScratchContainer);
}

void TestContainerOverflowGuards() {
    // Payloads that would exceed the uint32 item size throw length_error
    // without writing anything.
    mediacinemaraw::ContainerWriter writer(kScratchContainer, "{}");
    mediacinemaraw::GyroSample gyroDummy{0, 0, 0, 0};
    bool threw = false;
    try {
        writer.writeGyro(&gyroDummy, (size_t)(UINT32_MAX - 8) / 24 + 1);
    } catch (const std::length_error&) {
        threw = true;
    }
    assert(threw);
    mediacinemaraw::AccelerometerSample accelDummy{0, 0, 0, 0};
    threw = false;
    try {
        writer.writeAccelerometer(&accelDummy, (size_t)(UINT32_MAX - 8) / 24 + 1);
    } catch (const std::length_error&) {
        threw = true;
    }
    assert(threw);
    int16_t sample = 0;
    threw = false;
    try {
        writer.writeAudio(&sample, (size_t)UINT32_MAX / 2 + 1, 0);
    } catch (const std::length_error&) {
        threw = true;
    }
    assert(threw);
    writer.writeFrame(EncodeGradient(), 1000000000LL, kFrameJson);
    writer.close();
    std::remove(kScratchContainer);
}

void TestMotionContainerRoundTrip() {
    // Writes coalesce: 1 gyro chunk (3 + 1 samples), 1 accel chunk (2 + 1).
    // motioncam-decoder rejects files with more motion chunks than
    // frames + 1, so per-call chunks are never emitted.
    const std::vector<uint8_t> frame = EncodeGradient();
    {
        mediacinemaraw::ContainerWriter writer(
            kScratchContainer, "{\"manufacturer\":\"Example\",\"model\":\"Camera 1\","
                               "\"UniqueCameraModel\":\"Example Camera 1\","
                               "\"uniqueCameraModel\":\"Example Camera 1\","
                               "\"extraData\":{\"audioSampleRate\":48000,\"audioChannels\":2}}");
        writer.writeFrame(frame, 1000000000LL, kFrameJson);
        int16_t pcm[] = {1, -2, 3, -4};
        writer.writeAudio(pcm, 4, 1001000000LL);
        mediacinemaraw::GyroSample gyroFirst[] = {
            {1002000000LL, 0.1f, -0.2f, 0.3f},
            {1003000000LL, 1.0f, 2.0f, 3.0f},
            {1004000000LL, -1.5f, 0.0f, 9.81f},
        };
        writer.writeGyro(gyroFirst, 3);
        writer.writeGyro(gyroFirst, 0); // No-op: no index entry.
        mediacinemaraw::GyroSample gyroSecond[] = {{1005000000LL, 4.0f, 5.0f, 6.0f}};
        writer.writeGyro(gyroSecond, 1);
        mediacinemaraw::AccelerometerSample accelFirst[] = {
            {1002500000LL, 0.0f, 9.81f, 0.0f},
            {1003500000LL, 1.0f, 9.0f, -1.0f},
        };
        writer.writeAccelerometer(accelFirst, 2);
        writer.writeAccelerometer(accelFirst, 0); // No-op.
        mediacinemaraw::AccelerometerSample accelSecond[] = {{1005500000LL, 0.5f, 9.5f, 0.25f}};
        writer.writeAccelerometer(accelSecond, 1);
        writer.close();
        assert(writer.frameCount() == 1);
        // Post-close writes must fail; double close() is idempotent.
        bool threw = false;
        try {
            writer.writeGyro(gyroSecond, 1);
        } catch (const std::logic_error&) {
            threw = true;
        }
        assert(threw);
        threw = false;
        try {
            writer.writeAccelerometer(accelSecond, 1);
        } catch (const std::logic_error&) {
            threw = true;
        }
        assert(threw);
        threw = false;
        try {
            writer.writeAudio(pcm, 4, 1001000000LL);
        } catch (const std::logic_error&) {
            threw = true;
        }
        assert(threw);
        threw = false;
        try {
            writer.writeFrame(frame, 1033333333LL, kFrameJson);
        } catch (const std::logic_error&) {
            threw = true;
        }
        assert(threw);
        writer.close();
    }
    {
        const std::string bytes = ReadWholeFile(kScratchContainer);
        assert(bytes.find("\"UniqueCameraModel\":\"Example Camera 1\"") != std::string::npos);
        assert(bytes.find("\"uniqueCameraModel\":\"Example Camera 1\"") != std::string::npos);

        const std::vector<ItemRef> items = ScanContainerItems(bytes);
        int gyroData = 0, gyroIndex = 0, accelData = 0, accelIndex = 0;
        size_t gyroPayload = 0, accelPayload = 0;
        uint32_t gyroSamples = 0, accelSamples = 0;
        for (const ItemRef& item : items) {
            if (item.type == 9) {
                assert(gyroData == 0); // Exactly one coalesced chunk.
                gyroPayload = item.payloadOffset;
                assert(item.size >= 8);
                assert(LoadU32LE(bytes, item.payloadOffset) == 1); // Version.
                gyroSamples = LoadU32LE(bytes, item.payloadOffset + 4);
                assert(8u + gyroSamples * 24u == item.size);
                ++gyroData;
            } else if (item.type == 8) {
                ++gyroIndex;
                assert(item.size == 8 + 1 * 16);
                assert(LoadU32LE(bytes, item.payloadOffset) == 1);
                assert(LoadU32LE(bytes, item.payloadOffset + 4) == 1);
                // Index timestamp is the first sample of the chunk.
                assert(LoadI64LE(bytes, item.payloadOffset + 8 + 8) == 1002000000LL);
            } else if (item.type == 13) {
                assert(accelData == 0);
                accelPayload = item.payloadOffset;
                assert(item.size >= 8);
                assert(LoadU32LE(bytes, item.payloadOffset) == 1);
                accelSamples = LoadU32LE(bytes, item.payloadOffset + 4);
                assert(8u + accelSamples * 24u == item.size);
                ++accelData;
            } else if (item.type == 12) {
                ++accelIndex;
                assert(item.size == 8 + 1 * 16);
                assert(LoadU32LE(bytes, item.payloadOffset) == 1);
                assert(LoadU32LE(bytes, item.payloadOffset + 4) == 1);
                assert(LoadI64LE(bytes, item.payloadOffset + 8 + 8) == 1002500000LL);
            }
        }
        assert(gyroData == 1 && gyroIndex == 1);
        assert(accelData == 1 && accelIndex == 1);
        assert(gyroSamples == 4);
        assert(accelSamples == 3);
        // Sample bytes: timestamp + axes + reserved == 0, in call order.
        assert(LoadI64LE(bytes, gyroPayload + 8) == 1002000000LL);
        assert(LoadF32LE(bytes, gyroPayload + 16) == 0.1f);
        assert(LoadU32LE(bytes, gyroPayload + 28) == 0);
        assert(LoadI64LE(bytes, gyroPayload + 8 + 24) == 1003000000LL);
        assert(LoadI64LE(bytes, gyroPayload + 8 + 48) == 1004000000LL);
        assert(LoadF32LE(bytes, gyroPayload + 8 + 48 + 16) == 9.81f);
        assert(LoadI64LE(bytes, gyroPayload + 8 + 72) == 1005000000LL);
        assert(LoadF32LE(bytes, gyroPayload + 8 + 72 + 8) == 4.0f);
        assert(LoadU32LE(bytes, gyroPayload + 8 + 72 + 20) == 0);
        // Accelerometer bytes share the 24-byte MotionSample layout.
        assert(LoadI64LE(bytes, accelPayload + 8) == 1002500000LL);
        assert(LoadF32LE(bytes, accelPayload + 16) == 0.0f);
        assert(LoadF32LE(bytes, accelPayload + 20) == 9.81f);
        assert(LoadU32LE(bytes, accelPayload + 28) == 0);
        assert(LoadI64LE(bytes, accelPayload + 8 + 24) == 1003500000LL);
        assert(LoadI64LE(bytes, accelPayload + 8 + 48) == 1005500000LL);
        assert(LoadF32LE(bytes, accelPayload + 8 + 48 + 8) == 0.5f);
        assert(LoadU32LE(bytes, accelPayload + 8 + 48 + 20) == 0);
        // Whole-file golden: container I/O must stay byte-identical.
        assert(bytes.size() == 1794);
        assert(Fnv1aHash(bytes) == 0xa5a60214c0d84237ULL);
    }
    std::remove(kScratchContainer);
}

void TestPerFrameMotionCoalescing() {
    // 251 tiny drains per sensor collapse to frames + 1 chunks, so
    // motioncam-decoder always opens the file.
    const std::vector<uint8_t> frame = EncodeGradient();
    {
        mediacinemaraw::ContainerWriter writer(kScratchContainer, "{}");
        const int64_t startNs = 2000000000LL;
        int64_t sensorNs = startNs;
        for (int f = 0; f < 5; ++f) {
            for (int d = 0; d < 50; ++d) {
                mediacinemaraw::GyroSample gyro[7];
                mediacinemaraw::AccelerometerSample accel[7];
                for (int i = 0; i < 7; ++i) {
                    gyro[i] = {sensorNs, 0.1f, -0.2f, 0.3f};
                    accel[i] = {sensorNs, 1.0f, 2.0f, 9.8f};
                    sensorNs += 2000000LL; // 500 Hz.
                }
                writer.writeGyro(gyro, 7);
                writer.writeAccelerometer(accel, 7);
            }
            writer.writeFrame(frame, startNs + int64_t(f) * 33333333LL, kFrameJson);
        }
        mediacinemaraw::GyroSample gyroTail{sensorNs, 0, 0, 0};
        mediacinemaraw::AccelerometerSample accelTail{sensorNs, 0, 0, 0};
        writer.writeGyro(&gyroTail, 1);
        writer.writeAccelerometer(&accelTail, 1);
        int16_t tailAudio[] = {7, -7};
        writer.writeAudio(tailAudio, 2, startNs + 4 * 33333333LL);
        writer.close();
        assert(writer.frameCount() == 5);
    }
    {
        const std::string bytes = ReadWholeFile(kScratchContainer);
        const std::vector<ItemRef> items = ScanContainerItems(bytes);
        int gyroChunks = 0, accelChunks = 0;
        size_t gyroSamples = 0, accelSamples = 0;
        size_t audioIndexAt = 0, lastFrameAt = 0;
        bool sawAudioIndex = false;
        for (const ItemRef& item : items) {
            const size_t headerAt = item.payloadOffset - 8;
            if (item.type == 2)
                lastFrameAt = headerAt;
            if (item.type == 9) {
                ++gyroChunks;
                assert(LoadU32LE(bytes, item.payloadOffset) == 1);
                gyroSamples += LoadU32LE(bytes, item.payloadOffset + 4);
            } else if (item.type == 13) {
                ++accelChunks;
                assert(LoadU32LE(bytes, item.payloadOffset) == 1);
                accelSamples += LoadU32LE(bytes, item.payloadOffset + 4);
            } else if (item.type == 4) {
                sawAudioIndex = true;
                audioIndexAt = headerAt;
            } else if (item.type == 8 || item.type == 12) {
                assert(LoadU32LE(bytes, item.payloadOffset) == 1);
                assert(LoadU32LE(bytes, item.payloadOffset + 4) == 6);
                assert(item.size == 8 + 6 * 16);
            }
        }
        assert(gyroChunks == 6 && accelChunks == 6);
        assert(gyroSamples == 5 * 50 * 7 + 1);
        assert(accelSamples == 5 * 50 * 7 + 1);
        // Tail order: pre-gyro readers stop their scan at the first motion
        // item, so no motion data may sit between the last frame and the
        // audio index.
        assert(sawAudioIndex);
        for (const ItemRef& item : items) {
            if (item.type == 9 || item.type == 13) {
                const size_t headerAt = item.payloadOffset - 8;
                assert(headerAt < lastFrameAt || headerAt > audioIndexAt);
            }
        }
        // Whole-file golden: coalesced multi-chunk layout must stay
        // identical.
        assert(bytes.size() == 90737);
        assert(Fnv1aHash(bytes) == 0xf9edabacf5cbe5bcULL);
    }
    std::remove(kScratchContainer);
}

} // namespace

int main() {
    // Drop leftovers from an aborted run so every suite starts clean.
    std::remove(kScratchContainer);
    TestEncodeIsDeterministic();
    std::puts("encode determinism passed");
    TestEncodeRejectsTruncatedInput();
    std::puts("truncated-input rejection passed");
    TestPaddingClampAndPayloadGoldens();
    std::puts("payload goldens passed");
    TestParallelMatchesSerial();
    std::puts("parallel byte-identity passed");
    TestConvenienceOverload();
    std::puts("convenience overload passed");
    TestContainerArgumentGuards();
    std::puts("container argument guards passed");
    TestContainerOverflowGuards();
    std::puts("container overflow guards passed");
    TestMotionContainerRoundTrip();
    std::puts("motion container round-trip passed");
    TestPerFrameMotionCoalescing();
    std::puts("per-frame motion coalescing passed");
    std::puts("all encoder validation suites passed");
    return 0;
}
