# MediaCinemaRAW FAQ

Shared FAQ for the whole MediaCinemaRAW project: **MediaCinemaRAW-Encoder**
and **MediaCinemaRAW-Decoder**. This file is kept identical in both
repositories.

## What is MediaCinemaRAW?

MediaCinemaRAW (`.mcraw`) is a lossless RAW-video format: a version-3
container holding compression type 7 frame payloads plus JSON metadata,
PCM16 audio, and motion samples.

## Which repository do I need?

| Task | Repository | Entry point |
| --- | --- | --- |
| Encode RAW16 / packed RAW10 to type 7 | Encoder | `mediacinemaraw::encode()` |
| Write a `.mcraw` file | Encoder | `mediacinemaraw::ContainerWriter` |
| Decode a type-7 payload | Decoder | `mediacinemaraw::decode()`, `queryPayloadDimensions()` |
| Read a `.mcraw` file | Decoder | `mediacinemaraw::ContainerReader` |

The repos do not depend on each other. They are linked only by the format
and by the external interop scripts described below.

## Is this a clean-room implementation?

Yes. Both libraries were written without copying third-party decoder source.
The separate [`motioncam-decoder`](https://github.com/mirsadm/motioncam-decoder)
project and the sibling Encoder/Decoder checkout are used only as external
interoperability test oracles. They are not vendored, copied, or linked in.
Both repos are `GPL-3.0-only`.

## What is compatible?

- Frame compression type **7 only**. The decoder rejects legacy type 6 with
  an explicit error.
- Container version **3 only** (`MOTION␣3`, footer magic `0x8A905612`).
- Payloads are readable by existing type-7 `.mcraw` readers.

## What can the encoder take as input?

- Android `RAW16` (`uint16` per pixel) or packed `RAW10` (4 pixels in 5 bytes).
- `width` must be even, `height % 4 == 0`; `cropTop` must be even and
  `cropTop + cropHeight <= height`.
- Row stride may exceed the visible row size. The final row may omit padding,
  as Android image planes sometimes do.
- Even-row cropping preserves Bayer CFA phase.
- Optional 4x same-colour Bayer downscaling averages four same-colour samples.
  It is therefore **not** a lossless representation of the full-resolution
  frame.

## What does decoding produce?

- Row-major `uint16` samples in Bayer order, with no demosaic, black-level
  subtraction, scaling, or orientation applied.
- `width`/`height` are the visible dimensions from the frame JSON
  (`width`, `height`, `compressionType`). The payload header stores padded
  width `(width + 63) / 64 * 64`; padding columns are discarded.
- `queryPayloadDimensions()` inspects padded width/height without decoding.

## What is in the container?

- Container-level JSON and one JSON object per frame.
- Frame JSON carries at least `width`, `height`, `compressionType`, often plus
  `pixelFormat`, `timestamp`, `exposureTime`, `iso`, `asShotNeutral`.
- PCM16 audio chunks with nanosecond timestamps (`-1` when an old chunk has
  no timestamp item).
- Gyro and accelerometer samples as 24-byte `MotionSample`
  (`int64` timestamp plus three `float` axes). `hasGyroData() == false` or
  `hasAccelerometerData() == false` means the file has no such data.
- OIS and unknown item types are skipped so they do not break index discovery.
- Frame timestamps use one nanosecond timeline (for example 33,333,333 ns at
  30 fps) and are strictly increasing.

## My file plays badly / has no audio / looks dark. Is it broken?

Usually not:

- Other players need explicit type-7 support.
- A dark preview normally means black level (`64`) / white level (`1023` on
  typical 10-bit captures) was not applied by the viewer.
- Portrait clips only record orientation (for example `screenOrientation: 90`)
  as metadata; the decoder does not rotate pixels.
- `exposureTime`, `iso`, and `asShotNeutral` are informational.
- Missing audio/gyro/accel means that stream was not recorded.
- Quick sanity check: list frames with `mcraw_dump`, decode all frames,
  confirm dimensions are stable and min/max look like sensor data instead of
  all zeros.

## How are errors reported?

- `std::invalid_argument` for bad caller geometry (null payload, odd width,
  bad height/crop/stride).
- `std::runtime_error` for corrupt content (truncated payload, padded-width
  or height mismatch, bad offsets, unsupported compression type, odd audio
  size, duplicate timestamps, missing frame).
- Container audio helpers return `0` when `extraData.audioSampleRate` or
  `audioChannels` are absent. JSON metadata is returned as raw strings; the
  caller parses it, so there is no JSON library dependency.

## How do I export DNG?

Briefly: combine the decoded Bayer plane with `blackLevel`, `whiteLevel`,
color/forward matrices, `asShotNeutral`, and `sensorArrangment` from the
container and frame JSON. Copy `UniqueCameraModel` to DNG tag `50708` rather
than writing a fixed application name. The bundled `mcraw_dump` PGM/WAV
output is a preview aid, not a DNG converter:

```sh
./build/mcraw_dump input.mcraw --pgm frame.pgm --wav audio.wav
```

## Performance and porting notes?

C++17 with only the standard library. ARM NEON fast paths plus a portable
scalar fallback. The wire format is little-endian. `ContainerReader` streams
from disk, so large multi-hundred-megabyte clips do not need to be fully
loaded.

## How is this tested?

- Self-contained unit tests in each repo.
- 240 deterministic interop combinations (RAW16/RAW10, crop, downscale,
  stride, constant blocks, all supported bit widths):
  `tools/verify_compatibility.py /path/to/motioncam-decoder` on the encoder
  side and `tools/verify_interop.py /path/to/MediaCinemaRAW-Encoder` on the
  decoder side, both under AddressSanitizer/UBSan.
- Real-clip check: decode every frame, verify stable dimensions and
  increasing timestamps, verify audio duration roughly matches video duration.
