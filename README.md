# MediaCinemaRAW Encoder

A portable C++17 encoder for the **MediaCinemaRAW** lossless RAW-frame format.
It accepts Android RAW16 or packed RAW10 input and emits compression type 7
frame payloads compatible with existing `.mcraw` readers.

This is an independent **decoder-compatible encoder implementation**. During
development, the separate
[`motioncam-decoder`](https://github.com/mirsadm/motioncam-decoder) project was
consulted as a reference for understanding the `.mcraw` container format,
metadata structures, and expected decoder behavior, and was also used for
interoperability testing.

The encoder code in this repository was written independently and does not
include or link against `motioncam-decoder`. No decoder source code was
intentionally copied into the encoder. Because the reference implementation
was inspected during development, however, this project does **not** claim to
be a formal clean-room implementation.

## Features

- Lossless RAW16 and RAW10 encoding
- Even-row cropping that preserves Bayer CFA phase
- Optional 4x same-colour Bayer downscaling
- Row-stride and final-row-without-padding support
- ARM NEON acceleration with a portable scalar fallback
- Version-3 container writer with embedded PCM16 audio plus gyro and
  accelerometer motion (gyro 8/9, accel 12/13, version 1, 24-byte samples;
  no OIS output)
- No runtime dependencies beyond the C++ standard library

The optional downscale mode averages four same-colour samples and is therefore
not a lossless representation of the original full-resolution frame.

## Build and test

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

To run the full interoperability suite against a separate decoder checkout:

```sh
python3 tools/verify_compatibility.py /path/to/motioncam-decoder
```

To verify the gyro/accelerometer container path against the upstream oracle (and,
optionally, the sibling decoder checkout):

```sh
python3 tools/verify_gyro.py /path/to/motioncam-decoder [/path/to/MediaCinemaRAW-Decoder]
```

## API

```cpp
#include <MediaCinemaRAW/Encoder.h>

std::vector<uint8_t> encoded;
mediacinemaraw::encode(
    rawBytes, rawByteCount, width, height, rowStride, isPackedRaw10,
    cropTop, cropHeight, downscale4x, encoded);
```

Use `mediacinemaraw::ContainerWriter` to combine encoded frames, JSON metadata,
timestamped PCM16 chunks, and binary gyro/accelerometer samples into one
finalized `.mcraw` file. Both motion streams are opt-in via `writeGyro()` /
`writeAccelerometer()` (empty calls emit nothing); samples are buffered and
coalesced to at most one chunk per sensor per frame plus a trailing chunk at
`close()`, because motioncam-decoder rejects files with more motion chunks
than frames + 1. Gyro axes are rad/s, accel axes are m/s^2 including gravity
in the source platform convention.

Container metadata should include `UniqueCameraModel` with the actual camera
identity (for example, `Google Pixel 8 Pro`). Exporters should copy that value
to DNG tag 50708 instead of assigning a fixed application name. For compatibility
with readers that follow lower-camel JSON naming, PhotonCamera also emits the
same value as `uniqueCameraModel`.

## Format compatibility

The encoder targets MediaCinemaRAW container metadata compression type `7`.
PhotonCamera validation covers 240 deterministic combinations of RAW16,
RAW10, crop, downscale, stride, constant blocks, and supported bit widths. Each
payload is decoded and compared pixel-for-pixel with the reference decoder.

## FAQ

See [FAQ](FAQ.md) for the shared whole-project FAQ covering the encoder,
decoder, format, troubleshooting, and DNG export notes.

## License

GPL-3.0. See [LICENSE](LICENSE).
