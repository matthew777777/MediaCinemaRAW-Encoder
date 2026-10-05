# Version 3 container format

All integers are little-endian. This documents the `.mcraw` container
written by `mediacinemaraw::ContainerWriter`. Frame payloads are described
in [FORMAT.md](FORMAT.md).

## File structure

```
bytes magic          # "MOTION " + version byte 0x03 (8 bytes)
item  container_metadata
item* frames, audio, motion
item  audio_index?   # only when audio was written
item* trailing motion data + motion indexes
item  frame_index
item  footer
```

Every item is `u32 type | u32 payload_size | payload bytes`.

## Item types

| Type | Name                | Payload                                          |
| ---- | ------------------- | ------------------------------------------------ |
| 0    | Footer              | `u32 magic (0x8A905612)`, `u32 frame_count`, `i64 frame_index_offset` |
| 1    | Frame index         | per frame: `i64 data_offset`, `i64 timestamp_ns` |
| 2    | Frame data          | one type 7 payload, followed by its item 3 metadata JSON |
| 3    | Metadata            | JSON bytes (container-level once, then per frame) |
| 4    | Audio index         | `i64 chunk_count`, `i64 first_timestamp_ms`, then per chunk: `i64 data_offset`, `i64 timestamp_ns` |
| 5    | Audio data          | PCM16 LE samples                                 |
| 6    | Audio timestamp     | `i64 timestamp_ns` (one, right after each chunk) |
| 8    | Gyro index          | `u32 version (1)`, `u32 chunk_count`, then per chunk: `i64 data_offset`, `i64 first_sample_timestamp_ns` |
| 9    | Gyro data           | `u32 version (1)`, `u32 sample_count`, then 24-byte samples |
| 12   | Accelerometer index | same layout as item 8                            |
| 13   | Accelerometer data  | same layout as item 9                            |

Type 7 is unused. Types 10/11 (OIS) are never written; readers skip them.
A motion sample is `i64 timestamp_ns`, three `f32` axes, one `u32`
reserved word (always zero). Gyro axes are rad/s, accelerometer axes m/s^2
including gravity, in the source platform axis convention.

`frame_index_offset` in the footer points at the frame index entries
(the item 1 payload), not at its header.

## Ordering and coalescing contracts

Two rules keep files readable by deployed parsers:

1. The audio index precedes any trailing motion data. Pre-gyro readers
   stop their tail scan at the first motion item and would otherwise
   report no audio.
2. Motion chunks never exceed `frames + 1` per sensor. The writer
   buffers samples and flushes at most one chunk per sensor ahead of
   each frame, plus a trailing chunk at `close()`; `motioncam-decoder`
   rejects files that violate this with "Invalid gyro index" and aborts
   the whole open.

Each motion index entry timestamp is the first buffered sample's
timestamp, so callers should emit samples in timestamp order on the same
nanosecond timeline as frames.
