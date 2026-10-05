// SPDX-License-Identifier: GPL-3.0-only
#include <MediaCinemaRAW/ContainerWriter.h>

#include <algorithm>
#include <stdexcept>
#include <vector>

#include "detail/ContainerFormat.h"

namespace mediacinemaraw {
namespace {

// Streaming granularity for large audio/motion payloads: `sampleCount` is
// bounded only by the uint32 item size, so payloads are staged in ~96 KiB
// slices instead of one contiguous buffer.
constexpr size_t kAudioSliceSamples = 8192;
constexpr size_t kMotionSliceSamples = 4096;
constexpr size_t kScratchReserve = 16384;

void AppendMotionSamples(std::vector<uint8_t>& scratch, int64_t timestampNs, float x, float y,
                         float z) {
    detail::AppendLE(scratch, timestampNs);
    detail::AppendFloatLE(scratch, x);
    detail::AppendFloatLE(scratch, y);
    detail::AppendFloatLE(scratch, z);
    detail::AppendLE(scratch, uint32_t(0)); // Reserved, always zero.
}

} // namespace

ContainerWriter::ContainerWriter(const std::string& path, const std::string& metadata)
    : output_(path, std::ios::binary | std::ios::trunc) {
    if (!output_)
        throw std::runtime_error("cannot open output");
    writeBytes(detail::kFileMagic, sizeof(detail::kFileMagic));
    writeItemHeader(static_cast<uint32_t>(detail::ItemType::Metadata),
                    detail::CheckedItemSize(metadata.size()));
    writeBytes(metadata.data(), metadata.size());
}

ContainerWriter::~ContainerWriter() {
    try {
        close();
    } catch (...) {
    }
}

int64_t ContainerWriter::tellPosition() {
    const auto position = output_.tellp();
    if (position == std::ofstream::pos_type(-1))
        throw std::runtime_error("tell failed");
    return int64_t(position);
}

void ContainerWriter::writeBytes(const void* data, size_t size) {
    if (size == 0)
        return;
    output_.write(static_cast<const char*>(data), size);
    if (!output_)
        throw std::runtime_error("write failed");
}

void ContainerWriter::writeItemHeader(uint32_t type, uint32_t payloadSize) {
    uint8_t header[8];
    for (int i = 0; i < 4; ++i) {
        header[i] = static_cast<uint8_t>(type >> (8 * i));
        header[4 + i] = static_cast<uint8_t>(payloadSize >> (8 * i));
    }
    writeBytes(header, sizeof(header));
}

void ContainerWriter::writeFrame(const std::vector<uint8_t>& frameData, int64_t timestampNs,
                                 const std::string& frameMetadata) {
    if (closed_)
        throw std::logic_error("closed");
    if (!frameIndex_.empty() && timestampNs <= frameIndex_.back().timestampNs)
        throw std::invalid_argument("timestamp");
    // Motion coalescing (see the header): at most one pending chunk per
    // sensor lands ahead of this frame, so motion chunks stay within
    // frames + 1. Flushes run after the timestamp check so a rejected
    // frame emits nothing.
    flushPendingGyro();
    flushPendingAccelerometer();
    const int64_t offset = tellPosition();
    writeItemHeader(static_cast<uint32_t>(detail::ItemType::FrameData),
                    detail::CheckedItemSize(frameData.size()));
    writeBytes(frameData.data(), frameData.size());
    writeItemHeader(static_cast<uint32_t>(detail::ItemType::Metadata),
                    detail::CheckedItemSize(frameMetadata.size()));
    writeBytes(frameMetadata.data(), frameMetadata.size());
    frameIndex_.push_back({offset, timestampNs});
}

void ContainerWriter::writeAudio(const int16_t* samples, size_t sampleCount, int64_t timestampNs) {
    if (closed_)
        throw std::logic_error("closed");
    if (sampleCount > 0 && samples == nullptr)
        throw std::invalid_argument("null audio samples");
    const uint32_t payload = detail::AudioDataPayloadSize(sampleCount);
    const int64_t offset = tellPosition();
    writeItemHeader(static_cast<uint32_t>(detail::ItemType::AudioData), payload);
    std::vector<uint8_t> scratch;
    scratch.reserve(kScratchReserve);
    for (size_t i = 0; i < sampleCount;) {
        scratch.clear();
        const size_t slice = std::min(sampleCount - i, kAudioSliceSamples);
        for (size_t j = 0; j < slice; ++j) {
            const uint16_t value = uint16_t(samples[i + j]);
            scratch.push_back(static_cast<uint8_t>(value));
            scratch.push_back(static_cast<uint8_t>(value >> 8));
        }
        writeBytes(scratch.data(), scratch.size());
        i += slice;
    }
    writeItemHeader(static_cast<uint32_t>(detail::ItemType::AudioTimestamp), 8);
    scratch.clear();
    detail::AppendLE(scratch, timestampNs);
    writeBytes(scratch.data(), scratch.size());
    audioIndex_.push_back({offset, timestampNs});
}

void ContainerWriter::writeGyro(const GyroSample* samples, size_t sampleCount) {
    if (closed_)
        throw std::logic_error("closed");
    if (sampleCount == 0)
        return;
    if (samples == nullptr)
        throw std::invalid_argument("null gyro samples");
    // Single-call limit, before buffering a single sample.
    detail::GyroDataPayloadSize(sampleCount);
    pendingGyro_.insert(pendingGyro_.end(), samples, samples + sampleCount);
    if (pendingGyro_.size() > detail::kMaxPendingMotionSamples)
        flushPendingGyro();
}

void ContainerWriter::writeAccelerometer(const AccelerometerSample* samples, size_t sampleCount) {
    if (closed_)
        throw std::logic_error("closed");
    if (sampleCount == 0)
        return;
    if (samples == nullptr)
        throw std::invalid_argument("null accelerometer samples");
    // Single-call limit, before buffering a single sample.
    detail::AccelerometerDataPayloadSize(sampleCount);
    pendingAccelerometer_.insert(pendingAccelerometer_.end(), samples, samples + sampleCount);
    if (pendingAccelerometer_.size() > detail::kMaxPendingMotionSamples)
        flushPendingAccelerometer();
}

void ContainerWriter::flushPendingGyro() {
    if (pendingGyro_.empty())
        return;
    const uint32_t payload = detail::GyroDataPayloadSize(pendingGyro_.size());
    const int64_t offset = tellPosition();
    writeItemHeader(static_cast<uint32_t>(detail::ItemType::GyroData), payload);
    std::vector<uint8_t> scratch;
    scratch.reserve(detail::kMotionHeaderSize);
    detail::AppendLE(scratch, detail::kMotionVersion);
    detail::AppendLE(scratch, detail::CheckedItemSize(pendingGyro_.size()));
    writeBytes(scratch.data(), scratch.size());
    scratch.reserve(kMotionSliceSamples * detail::kMotionSampleWireSize);
    for (size_t i = 0; i < pendingGyro_.size();) {
        scratch.clear();
        const size_t slice = std::min(pendingGyro_.size() - i, kMotionSliceSamples);
        for (size_t j = 0; j < slice; ++j) {
            const auto& sample = pendingGyro_[i + j];
            AppendMotionSamples(scratch, sample.timestampNs, sample.x, sample.y, sample.z);
        }
        writeBytes(scratch.data(), scratch.size());
        i += slice;
    }
    gyroIndex_.push_back({offset, pendingGyro_.front().timestampNs});
    pendingGyro_.clear();
}

void ContainerWriter::flushPendingAccelerometer() {
    if (pendingAccelerometer_.empty())
        return;
    const uint32_t payload = detail::AccelerometerDataPayloadSize(pendingAccelerometer_.size());
    const int64_t offset = tellPosition();
    writeItemHeader(static_cast<uint32_t>(detail::ItemType::AccelerometerData), payload);
    std::vector<uint8_t> scratch;
    scratch.reserve(detail::kMotionHeaderSize);
    detail::AppendLE(scratch, detail::kMotionVersion);
    detail::AppendLE(scratch, detail::CheckedItemSize(pendingAccelerometer_.size()));
    writeBytes(scratch.data(), scratch.size());
    scratch.reserve(kMotionSliceSamples * detail::kMotionSampleWireSize);
    for (size_t i = 0; i < pendingAccelerometer_.size();) {
        scratch.clear();
        const size_t slice = std::min(pendingAccelerometer_.size() - i, kMotionSliceSamples);
        for (size_t j = 0; j < slice; ++j) {
            const auto& sample = pendingAccelerometer_[i + j];
            AppendMotionSamples(scratch, sample.timestampNs, sample.x, sample.y, sample.z);
        }
        writeBytes(scratch.data(), scratch.size());
        i += slice;
    }
    accelerometerIndex_.push_back({offset, pendingAccelerometer_.front().timestampNs});
    pendingAccelerometer_.clear();
}

void ContainerWriter::close() {
    if (closed_)
        return;
    closed_ = true;
    // Tail order is a compatibility contract: pre-gyro readers (MotionCam
    // Tools v1.0, decoder era Jan 2026) stop their tail scan at the first
    // motion item, so the audio index MUST precede any trailing motion data
    // or they report no audio. Gyro-era parsers break on accelerometer
    // items — same cure. Order-free readers don't care.
    // Index sections are buffered whole: their entry counts already live in
    // memory, so this adds no order of magnitude.
    std::vector<uint8_t> scratch;
    if (!audioIndex_.empty()) {
        writeItemHeader(static_cast<uint32_t>(detail::ItemType::AudioIndex),
                        detail::AudioIndexPayloadSize(audioIndex_.size()));
        scratch.clear();
        scratch.reserve(16 + audioIndex_.size() * 16);
        detail::AppendLE(scratch, int64_t(audioIndex_.size()));
        detail::AppendLE(scratch, audioIndex_.front().timestampNs / 1000000);
        for (const auto& entry : audioIndex_) {
            detail::AppendLE(scratch, entry.fileOffset);
            detail::AppendLE(scratch, entry.timestampNs);
        }
        writeBytes(scratch.data(), scratch.size());
    }
    // Trailing samples AFTER the audio index (see above).
    flushPendingGyro();
    flushPendingAccelerometer();
    if (!gyroIndex_.empty()) {
        writeItemHeader(static_cast<uint32_t>(detail::ItemType::GyroIndex),
                        detail::MotionIndexPayloadSize(gyroIndex_.size()));
        scratch.clear();
        scratch.reserve(8 + gyroIndex_.size() * 16);
        detail::AppendLE(scratch, detail::kMotionVersion);
        detail::AppendLE(scratch, detail::CheckedItemSize(gyroIndex_.size()));
        for (const auto& entry : gyroIndex_) {
            detail::AppendLE(scratch, entry.fileOffset);
            detail::AppendLE(scratch, entry.timestampNs);
        }
        writeBytes(scratch.data(), scratch.size());
    }
    if (!accelerometerIndex_.empty()) {
        writeItemHeader(static_cast<uint32_t>(detail::ItemType::AccelerometerIndex),
                        detail::MotionIndexPayloadSize(accelerometerIndex_.size()));
        scratch.clear();
        scratch.reserve(8 + accelerometerIndex_.size() * 16);
        detail::AppendLE(scratch, detail::kMotionVersion);
        detail::AppendLE(scratch, detail::CheckedItemSize(accelerometerIndex_.size()));
        for (const auto& entry : accelerometerIndex_) {
            detail::AppendLE(scratch, entry.fileOffset);
            detail::AppendLE(scratch, entry.timestampNs);
        }
        writeBytes(scratch.data(), scratch.size());
    }
    writeItemHeader(static_cast<uint32_t>(detail::ItemType::FrameIndex),
                    detail::FrameIndexPayloadSize(frameIndex_.size()));
    const int64_t indexOffset = tellPosition();
    scratch.clear();
    scratch.reserve(frameIndex_.size() * 16);
    for (const auto& entry : frameIndex_) {
        detail::AppendLE(scratch, entry.fileOffset);
        detail::AppendLE(scratch, entry.timestampNs);
    }
    writeBytes(scratch.data(), scratch.size());
    writeItemHeader(static_cast<uint32_t>(detail::ItemType::Footer), 16);
    scratch.clear();
    scratch.reserve(16);
    detail::AppendLE(scratch, detail::kFooterMagic);
    detail::AppendLE(scratch, detail::CheckedItemSize(frameIndex_.size()));
    detail::AppendLE(scratch, indexOffset);
    writeBytes(scratch.data(), scratch.size());
    output_.flush();
    if (!output_)
        throw std::runtime_error("finalize failed");
    output_.close();
}

} // namespace mediacinemaraw
