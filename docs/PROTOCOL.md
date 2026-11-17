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
Only one DATA packet is outstanding, so a slow calculator applies backpressure
without guessed sleeps. A BEGIN plus first DATA fits within a 512-byte ASI read.
The client keeps one read and one write in flight, and re-arms reads immediately.
The ASI baud rate is set before connection, and read timeout is 30 milliseconds.

The ESP waits 1500 ms for an ACK/DONE and retransmits at most three times. A
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
