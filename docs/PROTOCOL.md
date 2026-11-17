# NC2 serial camera protocol

Both endpoints use 115200 baud, 8N1. Lines are printable ASCII terminated by LF;
receivers also accept CRLF. No binary NULs travel through TI Lua's ASI interface.
NC2 replaces the original START_IMAGE/Huffman format: update **both** endpoints.

## Frames and flow control

```text
NC2 BEGIN <sequence> <base-sequence> <width> <height> <mode> <payload-chars> <adler32>
NC2 DATA <sequence> <offset> <base64-chunk>
ACK <sequence> <next-offset>
... repeat DATA / ACK ...
NC2 END <sequence>
DONE <sequence>
```

Sequence numbers are nonzero unsigned 32-bit values. Width and height are equal,
1..102 in the codec; the camera exposes 51, 72 and 102. Offsets/lengths count
Base64 characters. DATA carries at most 192 characters, in groups of four.
In the original WINDOW 1 mode, only one DATA packet is outstanding, so a slow calculator applies backpressure
without guessed sleeps. A BEGIN plus first DATA fits within a 512-byte ASI read.
The client keeps one read and one write in flight, and re-arms reads immediately.
The ASI baud rate is set before connection, and read timeout is 30 milliseconds.

In snapshot mode, the ESP waits 1500 ms for an ACK/DONE and retransmits at most three times. A
first-packet retry repeats BEGIN. Duplicate BEGIN/DATA/END are idempotent.
If retries are exhausted it stops the camera; Tab starts a fresh session. STOP
is handled while waiting for an acknowledgement. There is no blocking line read.

The calculator checks decoded length and Adler-32 over **reconstructed RGB332
pixels**, not merely compressed bytes. Invalid/truncated frames send
`NAK <sequence>`; the ESP discards the pending frame/reference and captures a
new independent frame. It only promotes a delta reference after DONE. Lost DONE
is recovered by replaying DONE on duplicate END. A periodic independent frame
(after at most 30 deltas) further limits dependency chains.

## Pixel format and compression

Each pixel is one RGB332 byte: red in bits 7..5, green in 4..2 and blue in 1..0.
Channel values are rounded to the nearest quantisation level. The LCD palette
expands these to 0..255. Compression is lossless **after** this quantisation;
the original RGB565 image is not lossless.

The encoder selects the shortest binary payload (ties favour independent
frames), then applies standard padded RFC 4648 Base64:

| Mode | Binary payload |
| --- | --- |
| K | Raw RGB332 pixels, row-major |
| R | PackBits-compressed RGB332 pixels |
| P | Palette count minus one (byte), that many palette bytes, then MSB-first packed palette indices |
| D | PackBits of bytewise XOR with the last acknowledged frame |

For P, use max(1, ceil(log2(palette-count))) bits/index. Unused low bits in the
last byte are zero. A count byte of 255 means 256 palette entries. Palette
indices outside the palette and nonzero padding are rejected.

PackBits tokens 0..127 introduce token+1 literal bytes. Tokens 129..255 introduce
257-token copies of the following byte. Token 128 is invalid. The encoder emits
repeats of at least three and limits runs/literals to 128 pixels. Decoders bound
all output by width×height. K/P/R have base-sequence 0. D names the exact previous
sequence and requires matching dimensions. No temporal approximation is used:
a changed pixel is always represented, even if only one channel bit changed.

Maximum raw payload: 10,404 bytes / 13,872 Base64 characters. Compression is
selected only when smaller than raw. Lines over 320 characters are discarded
until the next LF. An invalid frame never replaces the last displayed image.
Buffers/trees do not accumulate across frames.

## Control commands

| Calculator command | Effect |
| --- | --- |
| HELLO | Reply `NC2 READY 2 RGB332 51 72 102` |
| SIZE 51 / SIZE 72 / SIZE 102 | Set output resolution, cancel pending frame and invalidate reference |
| STREAM | Start continuous capture with a fresh reference |
| PIC | Capture one frame at the selected resolution |
| STOP | Cancel transfer and deinitialise camera |
| FLASH | Toggle AI Thinker GPIO 4 LED |
| KEY | Cancel pending transfer/reference; next streaming capture is independent |
| ACK / DONE / NAK | Transfer acknowledgements as above |

Commands are capped at 63 characters; an overlong command is discarded to LF.
`NC2 READY` resets the receiver's reference (e.g. after an ESP reboot). STATUS
and ERROR lines are UI notifications, never pixel data.

## Low-lag extension

The paired client now starts with `WINDOW 2`, `FORMAT 64`, `NOISE 8`, `SIZE 51`.
The BEGIN line appends a colour-format number (16, 64 or 256). An absent format
is accepted as 256 for old test vectors. Update the firmware and TNS together.
A format change invalidates the delta reference, even when dimensions match.

* FORMAT 64: four levels per RGB channel. Pixels use canonical RGB332 codes
  for those levels, and the LCD expands them back to RGB222. Palette packing
  uses at most six bits/pixel plus palette overhead.
* FORMAT 16: pixels are luminance indices 0..15, drawn as index×17 on all channels.
  Palette packing uses at most four bits/pixel plus palette overhead.
* FORMAT 256: original RGB332 quality mode.

WINDOW accepts 1 or 2. With WINDOW 2, a burst contains two DATA lines (384 payload
characters); the receiver sends a cumulative ACK only at a 384-character boundary
or the end of the payload. Retries resend the entire outstanding burst. The
receiver re-ACKs already complete bursts without appending duplicates. A partial
burst waits for its second packet; a missing packet expires the live frame.
The burst including BEGIN fits below 1024 bytes; the client uses 1024-byte ASI
reads. There is still at most one burst outstanding, with bounded buffers.

Live transfers do not retransmit stalled pictures. A burst/DONE wait of 180 ms,
or a total frame deadline of 150 ms + estimated payload serial time with 20%
framing allowance, emits `NC2 DROP <sequence>`. Both sides discard the partial
frame; the sender invalidates its delta base and captures a fresh independent
image. The receiver retains the last good displayed frame. Eight consecutive
failed live frames stop the camera to avoid running forever after a disconnect.
Snapshots retain 1500 ms waits and three retries. These defaults require physical
latency testing; high USB/decoder latency may cause drops rather than freezes.

NOISE 0 disables temporal filtering; NOISE 1..16 adds that many RGB/luminance
levels of hysteresis beyond the profile's half-quantisation step. Raw averaged
samples inside that band reuse the last acknowledged pixel. Outside it, the
pixel is quantised normally. Filtering never uses an unacknowledged reference.
This is intentionally lossy and can delay small changes. Periodic independent
refreshes after 30 deltas bypass the filter.

METRICS 1 enables `NC2 STATS <sequence> <capture-ms> <process-ms> <transfer-ms>
<dropped-count>` after DONE (one line). Capture includes fresh-frame acquisition;
process includes downsampling/filtering/compression; transfer includes serial,
ACK waits, receiver decode/cache and DONE return. METRICS 0 disables wire reports.
The Lua overlay separately measures decode/checksum, render-cache preparation and
paint handler duration (the previous paint). Press T to show/hide it. Millisecond
counters do not measure the panel's physical refresh or camera-to-eye latency.
