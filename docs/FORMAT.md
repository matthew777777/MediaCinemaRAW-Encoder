# Type 7 frame payload format

All integers are little-endian. This documents the payload bytes produced by
`mediacinemaraw::encode()` and consumed by type 7 `.mcraw` readers. The
container that carries these payloads is described in [CONTAINER.md](CONTAINER.md).

## Layout overview

```
u32 padded_width   # (width + 63) / 64 * 64
u32 height
u32 bits_offset    # file offset of the bits stream
u32 refs_offset    # file offset of the refs stream
bytes tile_data    # [16, bits_offset)
bytes bits_stream  # [bits_offset, refs_offset)
bytes refs_stream  # [refs_offset, end)
```

`width` is even and `height` is a multiple of four. Columns at `x >= width`
are padding: the decoder discards them.

## Tiles

The frame is covered row-major by 64x4 tiles (`padded_width / 64` by
`height / 4`). Each tile holds four channels, one per Bayer phase: channel
`c` contains the 64 samples with `x % 2 == c % 2` and `y % 2 == c / 2`
relative to the tile origin, ordered by `(row_pair * 32 + column_pair)`.

Per channel the encoder records a bit width and a reference (`lo`, the
channel minimum) and packs the 64 `sample - lo` deltas:

| max delta   | width | packed size | packing                        |
| ----------- | ----- | ----------- | ------------------------------ |
| 0           | 0     | 0           | nothing stored                 |
| < 2,4,16    | 1,2,4 | w*8         | generic lane packing (below)   |
| < 8         | 3     | 24          | 3 rows of 8, see code          |
| < 32        | 5     | 40          | 5 rows of 8, see code          |
| < 64        | 6     | 48          | 6 rows of 8, see code          |
| < 256       | 8     | 64          | low bytes                      |
| < 1024      | 10    | 80          | per 32-sample half: 32 low bytes + 8 high bytes (2 bits x 4 groups per lane) |
| otherwise   | 16    | 128         | raw u16 literals               |

Deltas are packed into eight interleaved lanes: sample `i` belongs to lane
`i % 8`. Generic widths (1, 2, 4) store `8 / width` samples per byte,
lane-major. The 3/5/6-bit lane layouts are fixed bit shuffles; the exact
mapping is the `PackChannelDeltas()` implementation in
`src/detail/BitPacking.h`, pinned by the golden hashes in `tests/EncoderTests.cpp`.

## Bits and refs streams

Both streams share one structure. Each starts with a `u32` count equal to
the channel count rounded up to a multiple of 64, followed by groups of 64
values. Every group is a 2-byte header plus packed deltas:

```
u8  (width_nibble << 4) | (ref >> 8)
u8  ref & 0xFF
bytes packed (value - ref) deltas at width_nibble
```

`ref` is `min(group_minimum, 4095)`; the 16-bit literal mode is spelled
`15` in the four-bit width field. Values past the true channel count are
zero padding, and readers validate that they decode to zero — the encoder
must keep zero tails (see `AppendMetadataStream()`).

## Encoder conventions

Two encoder-side choices are load-bearing for compatibility even though
they are invisible on the wire as such:

- Edge-tile padding is clamped to the channel's visible minimum before
  packing, so padding cannot inflate the bit width. Visible pixels still
  decode to `lo + delta`.
- The metadata tail padding is zero (see above), not an in-range value.
