# Nspire live camera — NC2 performance fork

Live ESP32-CAM images on the **original TI-Nspire CX**, using the calculator's
Lua ASI serial interface and a compatible CP2102 USB-to-UART adapter.
This fork focuses on transmission efficiency, image quality and reliable framing.
Based on [paaracetamol/Nspire-live-camera](https://github.com/paaracetamol/Nspire-live-camera).
The [upstream demo](https://www.youtube.com/watch?v=OpnwrEVE5LE) shows the original
implementation, not measured performance of NC2.

## What changed

- **256-colour RGB332 quality option** instead of the old custom 36-colour palette. Neutral sensor
  settings and proper RGB565 channel expansion avoid the old clipped colour boosts.
- **51×51, 72×72 and 102×102 output**, selected from the calculator. 51×51 is the
  fast default; 102×102 has four times as many pixels as the original 51×51 stream.
- Exact box downsampling covers the full centre crop, including the bottom/right
  pixels previously lost to integer truncation. Capture uses QVGA rather than VGA.
- Adaptive raw, PackBits, compact palette, or XOR-delta compression. The shortest
  payload wins, and compression never exceeds raw RGB332. Deltas preserve every
  changed pixel after colour quantisation.
- **Acknowledged 192-character chunks in bounded bursts**, bounded retries and whole-frame checksums
  replace guessed 50 ms chunk sleeps and a 200 ms frame delay. The calculator
  controls how quickly the next packet is sent.
- Frames are decoded once and horizontal render runs are cached. Pixels have no
  overlapping rectangles; partial/corrupt frames never replace the displayed image.
- STOP and FLASH are handled during a transfer. Snapshots work at the selected
  resolution and stay visible after stopping. Camera use works offline.
- Removed per-frame Huffman allocations/leaks, bit-string scratch buffers and an
  unused 256³-entry distance table. Replaced the fragile marker parser, unbounded
  tree cache and per-pixel paint allocations.

**Update the firmware and `readport3.0.tns` together.** NC2 does not understand the
old START_IMAGE protocol. The `.lua` files are now the authoritative sources; the
tracked `.tns` is rebuilt from them. The client is focused on the camera; upstream
chat/LLM placeholder menus are omitted. `discord_config.h` is an unused optional
legacy stub; this firmware needs no network credentials or ArduinoJson library.

## Hardware and first use

1. Use an AI Thinker ESP32-CAM with a compatible camera (the original pin mapping),
   a **CP2102 USB-UART bridge**, and a mini-USB host/OTG connection to the CX.
   This is not a generic USB-webcam driver.
2. Cross UART TX/RX between the bridge and ESP32 and connect their grounds. UART
   logic must be 3.3 V. Give the ESP32-CAM a stable external 5 V supply; do not
   connect two different power sources' positive rails together. Consult the
   [upstream wiring](https://github.com/paaracetamol/Nspire-live-camera#scheme).
3. Build/flash the ESP32 firmware using [PlatformIO](https://platformio.org/):

   ```sh
   pio run -e esp32cam
   pio run -e esp32cam -t upload --upload-port /dev/ttyUSB0
   ```

   Put GPIO 0 low/reset as required by your ESP32-CAM programmer for flashing,
   then release it/reset to run normally. Close serial monitors before attaching
   the bridge to the calculator.
4. Transfer **`readport3.0.tns` from this fork** to the calculator and open it.
   Lua ASI API level 2.7 and a supported serial adapter are required. This code
   uses the standard Lua/ASI APIs; it does not call Ndless functions.
5. Select a resolution from **Menu → Quality**, then press **Tab** to stream.
   **P** takes a photo, **Esc** stops, **F** toggles the flash. These actions are
   also available from Menu → Camera. The last complete image stays on screen.

If the ESP reports `ack-timeout`, it stops the camera in snapshot mode after three retries; press
Tab to start again. A reconnect or restart clears delta references automatically.
Camera initialization/capture errors appear in the calculator's status line.

## Rebuild the calculator document

Install/build [Luna](https://github.com/ndless-nspire/Luna) (2.1 was used here), then:

```sh
python3 tools/build.py --luna /path/to/luna
```

This embeds `camera_protocol.lua` into `readport3.0.lua` and regenerates
`readport3.0.tns`. For the TI script editor, run `python3 tools/build.py` and paste
**`build/camera_nc2.lua`**, which includes the protocol module. The raw client file
alone requires a module loader and should not be pasted by itself.

## Validation and performance limits

Run the host tests with a C++11 compiler, Python 3 and Lua 5.1+ (or a system
liblua5.3/liblua5.4 shared library):

```sh
make test
python3 tools/benchmark.py
```

Tests exercise the production C++ encoder against the production Lua decoder:
40 frame vectors at five fragmentation sizes, all compression modes, primary
colours, crop edges, malformed packets, bounded decoding, duplicate packets,
missing delta references, corrupt checksums, lost ACK/DONE, transfer timeout,
clock rollover and a mock of the calculator's ASI/UI lifecycle. Firmware tests
use host camera/serial stubs; they do not replace the actual ESP32 build.

Validation for this change: the PlatformIO AI Thinker ESP32-CAM firmware build
passed; protocol/client tests passed under Lua 5.1.5 and Lua 5.4; C++ tests passed
with AddressSanitizer/UndefinedBehaviorSanitizer (LeakSanitizer is unavailable in
the runner); and the calculator document was rebuilt with Luna 2.1.

Example **synthetic** 72×72 payloads (raw RGB332 Base64 is 6,912 bytes):

| Frame | Payload | Reduction versus raw Base64 |
| --- | ---: | ---: |
| Unchanged detailed image | 112 bytes | 98.4% |
| Same image with one pixel changed | 116 bytes | 98.3% |
| Random four-colour image | 1,736 bytes | 74.9% |
| Random 256-colour image | 6,912 bytes | 0% |

These are codec measurements, **not hardware FPS or a comparison against the
old Huffman encoder**. Packet headers/ACKs, USB latency, capture and LCD rendering
also take time. Camera noise/motion reduces delta savings, and higher resolution
costs bandwidth and render time. Actual speed, colour appearance, power stability
and sensor byte order still need testing on a physical CX + ESP32-CAM setup.

See [docs/PROTOCOL.md](docs/PROTOCOL.md) for framing, bounds and recovery details.

## Low-lag defaults (latest update)

The default is now **51×51, 64 colours**, two chunks per acknowledgement and
noise hysteresis enabled. Menu → Colours offers 16-shade grayscale and the
original 256-colour quality mode; Menu → Quality still selects resolution.
Menu → Noise → Exact pixels disables temporal filtering. That filter reduces
small sensor fluctuations, at the cost of delaying subtle changes.

Live mode drops stalled/overdue frames and captures fresh ones instead of
retrying old pictures. Snapshots retain reliable retries. A prolonged link failure
stops the camera after eight consecutive dropped frames. The last complete image
remains visible; Tab restarts streaming. Higher resolutions still cost latency.

Press **T** (or Menu → Diagnostics) for measured stage timings: capture,
processing, transfer/ACK, Lua decode, cache preparation, drawing, and dropped
frames. Transfer includes receiver decode/cache, so the displayed stage times
are not all additive. No smooth-video/FPS claim has been measured on hardware.
See the low-lag protocol extension for timeout and packet bounds.
