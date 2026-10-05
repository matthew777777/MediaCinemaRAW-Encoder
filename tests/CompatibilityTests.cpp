// SPDX-License-Identifier: GPL-3.0-only
//
// Round-trip oracle: encode with this repo, decode with motioncam-decoder,
// compare pixel-for-pixel. Built by tools/verify_compatibility.py against an
// external motioncam-decoder checkout; never vendors upstream sources.
//
// Modes:
//   no args            240 deterministic RAW16/RAW10/crop/bin/stride cases
//   --fixture <path>   write one encoded 64x8 payload for external harnesses
//   <container.mcraw>  verify a 2-frame all-0xFF container via upstream Decoder
#include <MediaCinemaRAW/Encoder.h>

#include <motioncam/Decoder.hpp>
#include <motioncam/RawData.hpp>

#include <cassert>
#include <cstring>
#include <fstream>
#include <iostream>
#include <random>
#include <string>
#include <vector>

namespace {

constexpr int kSeed = 7139;
constexpr int kHeight = 24;
constexpr int kCropTop = 2;
constexpr int kCropHeight = 16;
constexpr int kStrideSlack = 24;

void RunRoundTrips() {
    std::mt19937 random(kSeed);
    int cases = 0;
    for (bool raw10 : {false, true})
        for (bool bin : {false, true})
            for (int width : {4, 64, 68, 128, 260})
                for (int mode = 0; mode < 12; ++mode) {
                    const int stride = (raw10 ? width / 4 * 5 : width * 2) + kStrideSlack;
                    std::vector<uint16_t> pixels(width * kHeight);
                    for (auto& pixel : pixels) {
                        const unsigned ranges[] = {1,  2,   4,    8,    16,    32,
                                                   64, 256, 1024, 4096, 16384, 65536};
                        pixel = raw10 ? random() % std::min(1024u, ranges[mode])
                                      : random() % ranges[mode];
                        // Constant, nonzero references.
                        if (mode == 0)
                            pixel = raw10 ? 800 : 50000;
                    }
                    // Last row deliberately omits padding, as Android image
                    // planes may do.
                    const size_t rowBytes = raw10 ? width / 4 * 5 : width * 2;
                    std::vector<uint8_t> raw((kHeight - 1) * stride + rowBytes, 0xAD);
                    for (int y = 0; y < kHeight; ++y)
                        for (int x = 0; x < width; ++x) {
                            const uint16_t value = pixels[y * width + x];
                            if (raw10) {
                                raw[y * stride + x / 4 * 5 + x % 4] = value >> 2;
                                if (x % 4 == 0)
                                    raw[y * stride + x / 4 * 5 + 4] = 0;
                                raw[y * stride + x / 4 * 5 + 4] =
                                    static_cast<uint8_t>(raw[y * stride + x / 4 * 5 + 4] |
                                                         ((value & 3) << (2 * (x % 4))));
                            } else {
                                raw[y * stride + x * 2] = static_cast<uint8_t>(value);
                                raw[y * stride + x * 2 + 1] = static_cast<uint8_t>(value >> 8);
                            }
                        }
                    std::vector<uint8_t> encoded;
                    mediacinemaraw::encode(raw.data(), raw.size(), width, kHeight, stride, raw10,
                                           kCropTop, kCropHeight, bin, encoded);
                    const int decodedWidth = bin ? width / 2 : width;
                    const int decodedHeight = bin ? kCropHeight / 2 : kCropHeight;
                    std::vector<uint16_t> decoded(decodedWidth * decodedHeight);
                    assert(motioncam::raw::Decode(decoded.data(), decodedWidth, decodedHeight,
                                                  encoded.data(),
                                                  encoded.size()) == decoded.size());
                    for (int y = 0; y < decodedHeight; ++y)
                        for (int x = 0; x < decodedWidth; ++x) {
                            unsigned expected;
                            if (bin) {
                                const int sx = x / 2 * 4 + x % 2;
                                const int sy = kCropTop + y / 2 * 4 + y % 2;
                                expected =
                                    (unsigned(pixels[sy * width + sx]) +
                                     pixels[sy * width + sx + 2] + pixels[(sy + 2) * width + sx] +
                                     pixels[(sy + 2) * width + sx + 2] + 2) /
                                    4;
                            } else {
                                expected = pixels[(kCropTop + y) * width + x];
                            }
                            assert(decoded[y * decodedWidth + x] == expected);
                        }
                    ++cases;
                }

    std::vector<uint8_t> raw(64 * 8 * 2, 255), encoded;
    for (int bad = 0; bad < 3; ++bad) {
        bool threw = false;
        try {
            mediacinemaraw::encode(raw.data(), bad == 0 ? 10 : raw.size(), 64, 8, 128, false,
                                   bad == 1 ? 1 : 0, bad == 2 ? 6 : 8, false, encoded);
        } catch (const std::invalid_argument&) {
            threw = true;
        }
        assert(threw);
    }
    std::cout << cases << " lossless RAW16/RAW10/crop/bin/stride roundtrips passed\n";
}

void WriteFixture(const char* path) {
    std::vector<uint8_t> raw(64 * 8 * 2, 255), encoded;
    mediacinemaraw::encode(raw.data(), raw.size(), 64, 8, 128, false, 0, 8, false, encoded);
    std::ofstream output(path, std::ios::binary);
    output.write(reinterpret_cast<const char*>(encoded.data()), encoded.size());
}

void VerifyContainer(const char* path) {
    motioncam::Decoder decoder(path);
    assert(decoder.getFrames().size() == 2);
    std::vector<uint8_t> data;
    nlohmann::json meta;
    for (auto timestamp : decoder.getFrames()) {
        decoder.loadFrame(timestamp, data, meta);
        assert(data.size() == 64 * 8 * 2);
        for (auto value : data)
            assert(value == 255);
    }
    std::cout << "Java container read and all frame pixels verified by "
                 "upstream Decoder\n";
}

} // namespace

int main(int argc, char** argv) {
    if (argc > 1 && std::string(argv[1]) == "--fixture") {
        WriteFixture(argv[2]);
    } else if (argc > 1) {
        VerifyContainer(argv[1]);
    } else {
        RunRoundTrips();
    }
    return 0;
}
