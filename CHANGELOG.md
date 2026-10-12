# Changelog

## Unreleased

- Added header-only `LensShading.h`: `buildLensShadingJson()` embeds one
  container-level `lensShadingMap` gain grid for lens shading correction,
  with validation and bilinear sampling shared with the sibling decoder.
  The upstream-oracle interop suite reads 4x3 and 64x48 maps back through
  motioncam-decoder value-for-value.
- Restructured the codebase without changing the wire format: encoder
  internals moved to documented `src/detail/` modules (SIMD, bit packing,
  tile loop, container constants), public headers documented, validation
  suite split into named cases. Output is byte-identical; all golden
  hashes unchanged.
- Added `docs/FORMAT.md` and `docs/CONTAINER.md` wire-format notes.
- CMake: warning flags, installable package config, position-independent
  code, and an optional `MEDIACINEMARAW_MOTIONCAM_DECODER_DIR` interop
  test target. The forced `-O3` is gone; standard build types apply.
- Interop scripts accept `--cxx` and discover encoder sources automatically.
- CI now covers Debug builds and the upstream interop suites.

## 0.1.0

- Lossless RAW16 / packed RAW10 encoder for compression type 7, with
  even-row cropping, optional 4x same-colour downscaling, stride support,
  NEON/SSE2 fast paths, and a byte-identical multithreaded entry point.
- Version-3 container writer: frames, JSON metadata, PCM16 audio, and
  coalesced gyro/accelerometer motion compatible with motioncam-decoder.
