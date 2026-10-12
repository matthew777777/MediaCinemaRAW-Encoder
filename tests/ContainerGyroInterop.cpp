// SPDX-License-Identifier: GPL-3.0-only
//
// Upstream oracle: encoder writer -> motioncam-decoder motion APIs.
// Built by tools/verify_gyro.py against an external motioncam-decoder
// checkout; never vendors upstream sources.
#include <MediaCinemaRAW/ContainerWriter.h>
#include <MediaCinemaRAW/Encoder.h>
#include <MediaCinemaRAW/LensShading.h>

#include <motioncam/Decoder.hpp>

#include <cassert>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace {

constexpr int kWidth = 64;
constexpr int kHeight = 8;
constexpr int kStride = kWidth * 2;
constexpr char kFrameJson[] = "{\"width\":64,\"height\":8,\"compressionType\":7}";

std::vector<uint8_t> EncodeTestFrame() {
    std::vector<uint8_t> raw(static_cast<size_t>(kHeight - 1) * kStride + kWidth * 2, 0);
    for (int y = 0; y < kHeight; ++y)
        for (int x = 0; x < kWidth; ++x) {
            const uint16_t value = static_cast<uint16_t>((x * 17 + y * 31) & 4095);
            raw[static_cast<size_t>(y) * kStride + x * 2] = static_cast<uint8_t>(value);
            raw[static_cast<size_t>(y) * kStride + x * 2 + 1] = static_cast<uint8_t>(value >> 8);
        }
    std::vector<uint8_t> blob;
    mediacinemaraw::encode(raw.data(), raw.size(), kWidth, kHeight, kStride, false, 0, kHeight,
                           false, blob);
    return blob;
}

void WriteBasicFile(const std::string& path, const std::vector<uint8_t>& frame) {
    mediacinemaraw::ContainerWriter writer(
        path, "{\"manufacturer\":\"Example\",\"model\":\"Camera 1\","
              "\"UniqueCameraModel\":\"Example Camera 1\","
              "\"extraData\":{\"audioSampleRate\":48000,\"audioChannels\":1}}");
    writer.writeFrame(frame, 1000000000LL, kFrameJson);
    writer.writeFrame(frame, 1033333333LL, kFrameJson);
    const int16_t pcm[2] = {100, -200};
    writer.writeAudio(pcm, 2, 1001000000LL);
    mediacinemaraw::GyroSample gyroFirst[3] = {
        {1002000000LL, 0.1f, -0.2f, 0.3f},
        {1003000000LL, 1.0f, 2.0f, 3.0f},
        {1004000000LL, -1.5f, 0.0f, 9.81f},
    };
    writer.writeGyro(gyroFirst, 3);
    writer.writeGyro(gyroFirst, 0); // No-op.
    mediacinemaraw::GyroSample gyroSecond[1] = {{1005000000LL, 4.0f, 5.0f, 6.0f}};
    writer.writeGyro(gyroSecond, 1);
    mediacinemaraw::AccelerometerSample accelFirst[2] = {
        {1002500000LL, 0.0f, 9.81f, 0.0f},
        {1003500000LL, 1.0f, 9.0f, -1.0f},
    };
    writer.writeAccelerometer(accelFirst, 2);
    writer.writeAccelerometer(accelFirst, 0); // No-op.
    mediacinemaraw::AccelerometerSample accelSecond[1] = {{1005500000LL, 0.5f, 9.5f, 0.25f}};
    writer.writeAccelerometer(accelSecond, 1);
    writer.close();
    assert(writer.frameCount() == 2);
}

void CheckBasicFile(const std::string& path) {
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

    // Frames remain decodable alongside motion.
    for (auto timestamp : decoder.getFrames()) {
        std::vector<uint8_t> data;
        nlohmann::json meta;
        decoder.loadFrame(timestamp, data, meta);
        assert(meta["width"] == 64 && meta["height"] == 8);
        assert(meta["compressionType"] == 7);
        assert(data.size() == static_cast<size_t>(kWidth * kHeight * 2));
    }
    std::printf("upstream gyro+accel interop passed (4 gyro, 3 accel)\n");
}

void CheckCrowdedMotion(const std::vector<uint8_t>& frame) {
    // Many tiny drains (sensor recorders drain ~100x/s) must coalesce to
    // <= frames + 1 chunks: the upstream reader rejects more with
    // "Invalid gyro index", aborting the whole open.
    const std::string path = "gyro_interop_many.mcraw";
    int64_t sensorNs = 2000000000LL;
    {
        mediacinemaraw::ContainerWriter writer(path, "{}");
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
            writer.writeFrame(frame, 2000000000LL + int64_t(f) * 33333333LL, kFrameJson);
        }
        mediacinemaraw::GyroSample gyroTail{sensorNs, 0, 0, 0};
        writer.writeGyro(&gyroTail, 1);
        mediacinemaraw::AccelerometerSample accelTail{sensorNs, 0, 0, 0};
        writer.writeAccelerometer(&accelTail, 1);
        writer.close();
    }
    motioncam::Decoder crowded(path);
    assert(crowded.getFrames().size() == 5);
    std::vector<motioncam::MotionSample> gyro, accel;
    crowded.loadGyroData(gyro);
    crowded.loadAccelerometerData(accel);
    assert(gyro.size() == 5 * 50 * 7 + 1);
    assert(accel.size() == 5 * 50 * 7 + 1);
    assert(gyro.front().timestampNs == 2000000000LL);
    assert(gyro.back().timestampNs == sensorNs);
    assert(accel.front().timestampNs == 2000000000LL);
    assert(accel.back().timestampNs == sensorNs);
    for (size_t i = 1; i < gyro.size(); ++i)
        assert(gyro[i].timestampNs >= gyro[i - 1].timestampNs);
    std::remove(path.c_str());
    std::printf("upstream crowded-motion interop passed (1751 + 1751)\n");
}

void CheckLensShading(const std::vector<uint8_t>& frame) {
    // A per-frame map from buildLensShadingJson() must survive the upstream
    // oracle: motioncam-decoder exposes frame JSON untouched, so the nested
    // planes read back value-for-value. Gains are exactly representable,
    // so the upstream doubles compare exactly.
    const std::string path = "gyro_interop_lens.mcraw";
    mediacinemaraw::LensShadingMap map;
    map.columns = 4;
    map.rows = 3;
    map.gains.resize(48);
    for (size_t i = 0; i < map.gains.size(); ++i)
        map.gains[i] = 1.0f + float(i % 8) * 0.25f;
    const std::string frameJson = "{\"width\":64,\"height\":8,\"compressionType\":7," +
                                  mediacinemaraw::buildLensShadingJson(map) + "}";
    {
        mediacinemaraw::ContainerWriter writer(
            path, "{\"manufacturer\":\"Example\",\"model\":\"Camera 1\","
                  "\"UniqueCameraModel\":\"Example Camera 1\","
                  "\"extraData\":{\"audioSampleRate\":48000,\"audioChannels\":1}}");
        writer.writeFrame(frame, 1000000000LL, frameJson);
        const int16_t pcm[2] = {100, -200};
        writer.writeAudio(pcm, 2, 1001000000LL);
        writer.close();
    }
    motioncam::Decoder decoder(path);
    assert(decoder.getFrames().size() == 1);
    std::vector<uint8_t> data;
    nlohmann::json meta;
    decoder.loadFrame(decoder.getFrames()[0], data, meta);
    assert(meta["lensShadingMapWidth"] == 4);
    assert(meta["lensShadingMapHeight"] == 3);
    const nlohmann::json& planes = meta["lensShadingMap"];
    assert(planes.size() == 4);
    for (size_t ch = 0; ch < 4; ++ch) {
        assert(planes[ch].size() == 12);
        for (size_t i = 0; i < 12; ++i)
            assert(planes[ch][i].get<double>() == 1.0 + double((i * 4 + ch) % 8) * 0.25);
    }
    assert(meta["compressionType"] == 7);
    assert(data.size() == static_cast<size_t>(kWidth * kHeight * 2));
    std::vector<motioncam::AudioChunk> audio;
    decoder.loadAudio(audio);
    assert(audio.size() == 1 && audio[0].second.size() == 2);
    assert(decoder.audioSampleRateHz() == 48000);
    std::remove(path.c_str());
    std::printf("upstream lens shading interop passed (4x3 map, 4x12 gains)\n");
}

void CheckLargeLensShading(const std::vector<uint8_t>& frame) {
    // A realistic-size sensor map (17x17 nested planes, as MotionCam Pro
    // writes) must pass through the upstream reader the same way: no
    // metadata size cap.
    const std::string path = "gyro_interop_lens_big.mcraw";
    mediacinemaraw::LensShadingMap map;
    map.columns = 17;
    map.rows = 17;
    map.gains.resize(static_cast<size_t>(17) * 17 * 4, 1.5f);
    for (int ch = 0; ch < 4; ++ch)
        map.gains[size_t(ch)] = 1.0f + float(ch) * 0.5f;
    map.gains.back() = 2.5f;
    const std::string frameJson = "{\"width\":64,\"height\":8,\"compressionType\":7," +
                                  mediacinemaraw::buildLensShadingJson(map) + "}";
    {
        mediacinemaraw::ContainerWriter writer(path, "{\"model\":\"Camera 1\"}");
        writer.writeFrame(frame, 1000000000LL, frameJson);
        writer.close();
    }
    motioncam::Decoder decoder(path);
    assert(decoder.getFrames().size() == 1);
    std::vector<uint8_t> data;
    nlohmann::json meta;
    decoder.loadFrame(decoder.getFrames()[0], data, meta);
    assert(meta["lensShadingMapWidth"] == 17);
    assert(meta["lensShadingMapHeight"] == 17);
    const nlohmann::json& planes = meta["lensShadingMap"];
    assert(planes.size() == 4);
    for (size_t ch = 0; ch < 4; ++ch) {
        assert(planes[ch].size() == 289);
        assert(planes[ch][0].get<double>() == 1.0 + double(ch) * 0.5);
        assert(planes[ch][100].get<double>() == 1.5);
    }
    assert(planes[3][288].get<double>() == 2.5);
    std::remove(path.c_str());
    std::printf("upstream large lens shading interop passed (17x17 map)\n");
}

} // namespace

int main(int argc, char** argv) {
    const std::string path = argc > 1 ? argv[1] : std::string("gyro_interop.mcraw");
    const std::vector<uint8_t> frame = EncodeTestFrame();
    WriteBasicFile(path, frame);
    CheckBasicFile(path);
    CheckLensShading(frame);
    CheckLargeLensShading(frame);
    CheckCrowdedMotion(frame);
    return 0;
}
