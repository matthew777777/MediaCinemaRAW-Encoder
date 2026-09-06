# MediaCinemaRAW Encoder

A portable C++17 encoder for the **MediaCinemaRAW** lossless RAW-frame format.
It accepts Android RAW16 or packed RAW10 input and emits compression type 7
frame payloads compatible with existing `.mcraw` readers.

This is an independent **clean-room implementation**. It was written without
copying decoder source code. The separate
[`motioncam-decoder`](https://github.com/mirsadm/motioncam-decoder) project is
used only as an interoperability test oracle and is not included in this
repository or linked into the encoder library.

## Features

- Lossless RAW16 and RAW10 encoding
- Even-row cropping that preserves Bayer CFA phase
- Optional 4x same-colour Bayer downscaling
- Row-stride and final-row-without-padding support
- ARM NEON acceleration with a portable scalar fallback
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

## API

```cpp
#include <MediaCinemaRAW/Encoder.h>

std::vector<uint8_t> encoded;
mediacinemaraw::encode(
    rawBytes, rawByteCount, width, height, rowStride, isPackedRaw10,
    cropTop, cropHeight, downscale4x, encoded);
```

The output is one encoded frame payload. Applications remain responsible for
writing container records, JSON camera metadata, timestamps, audio, gyro data,
and the final `.mcraw` index.

## Format compatibility

The encoder targets MediaCinemaRAW container metadata compression type `7`.
PhotonCamera validation covers 240 deterministic combinations of RAW16,
RAW10, crop, downscale, stride, constant blocks, and supported bit widths. Each
payload is decoded and compared pixel-for-pixel with the reference decoder.

## License

GPL-3.0. See [LICENSE](LICENSE).
