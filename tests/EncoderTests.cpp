#include <MediaCinemaRAW/Encoder.h>
// SPDX-License-Identifier: GPL-3.0-only

#include <cassert>
#include <cstdint>
#include <stdexcept>
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
}
