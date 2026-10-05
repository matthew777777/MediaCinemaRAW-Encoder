// SPDX-License-Identifier: GPL-3.0-only
//
// Version 3 `.mcraw` container writer: encoded frames plus JSON metadata,
// timestamped PCM16 audio, and gyro/accelerometer motion samples.
//
// Gyro uses item types 8 (index) / 9 (data), accelerometer 12 / 13, both
// version 1 with 24-byte samples: int64 timestampNs, three float32 axes,
// one uint32 reserved field (always zero), all little-endian. Gyro axes are
// rad/s, accelerometer axes m/s^2 including gravity, in the source platform
// axis convention. No OIS (10/11) output. See docs/CONTAINER.md.
#pragma once

#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

namespace mediacinemaraw {

// Caller-side motion samples. The writer appends the zero `reserved` word on
// the wire so the on-disk layout matches the readers' 24-byte MotionSample.
struct GyroSample {
    int64_t timestampNs;
    float x, y, z;
};

struct AccelerometerSample {
    int64_t timestampNs;
    float x, y, z;
};

class ContainerWriter {
  public:
    // Opens `path` for writing (truncated) and writes the file header plus
    // the container-level JSON metadata. Throws std::runtime_error when the
    // file cannot be opened or the header write fails.
    ContainerWriter(const std::string& path, const std::string& metadata);
    ~ContainerWriter();

    ContainerWriter(const ContainerWriter&) = delete;
    ContainerWriter& operator=(const ContainerWriter&) = delete;

    // Appends one encoded frame with its container metadata JSON (at least
    // `width`, `height`, `compressionType`). Timestamps must be strictly
    // increasing. Throws std::logic_error if closed, std::invalid_argument
    // on a non-increasing timestamp, std::length_error if an item would
    // exceed its uint32 size field.
    void writeFrame(const std::vector<uint8_t>& frameData, int64_t timestampNs,
                    const std::string& frameMetadata);

    // Appends one PCM16 audio chunk with a nanosecond timestamp. Each call
    // emits one chunk. Throws std::logic_error if closed,
    // std::invalid_argument on null samples with `sampleCount > 0`,
    // std::length_error if the item would exceed its uint32 size field.
    void writeAudio(const int16_t* samples, size_t sampleCount, int64_t timestampNs);

    // Buffers gyro samples; they are NOT written per call. Each accepted
    // writeFrame() flushes at most one pending chunk ahead of the frame and
    // close() flushes the remainder, so gyro chunks never exceed
    // frames + 1 for any call cadence. motioncam-decoder rejects files with
    // more motion chunks than frames + 1 ("Invalid gyro index" aborts the
    // whole open, hiding audio too), while sensor recorders drain ~100x/s;
    // coalescing keeps every take openable.
    //
    // Empty calls (`sampleCount == 0`) are a no-op and emit no index entry.
    // The chunk index timestamp is the first buffered sample's timestamp, so
    // callers should emit samples in timestamp order on the same nanosecond
    // timeline as frames. Throws like writeAudio() above.
    void writeGyro(const GyroSample* samples, size_t sampleCount);

    // Accelerometer. Same buffering contract as writeGyro(), but item types
    // 12 (index) / 13 (data). Values are m/s^2 including gravity with the
    // platform axis convention preserved.
    void writeAccelerometer(const AccelerometerSample* samples, size_t sampleCount);

    // Writes the trailing indexes and footer. Idempotent; the destructor
    // calls it implicitly (swallowing errors). Throws std::runtime_error if
    // finalization fails.
    void close();

    size_t frameCount() const noexcept { return frameIndex_.size(); }

  private:
    struct IndexEntry {
        int64_t fileOffset;
        int64_t timestampNs;
    };

    std::ofstream output_;
    std::vector<IndexEntry> frameIndex_;
    std::vector<IndexEntry> audioIndex_;
    std::vector<IndexEntry> gyroIndex_;
    std::vector<IndexEntry> accelerometerIndex_;
    std::vector<GyroSample> pendingGyro_;
    std::vector<AccelerometerSample> pendingAccelerometer_;
    bool closed_ = false;

    void writeItemHeader(uint32_t type, uint32_t payloadSize);
    void writeBytes(const void* data, size_t size);
    int64_t tellPosition();
    // Emit at most one pending chunk per sensor; no-op when the buffer is
    // empty. Declared here so writeFrame()/close() share the flush points.
    void flushPendingGyro();
    void flushPendingAccelerometer();
};

} // namespace mediacinemaraw
