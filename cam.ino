// NC2 firmware for the AI Thinker ESP32-CAM. Flash together with the NC2 Lua client.
#include <Arduino.h>
#include "esp_camera.h"
#include "camera_codec.h"
#include <cstdio>
#include <cstring>

namespace {
constexpr uint32_t AckTimeoutMs = 1500;
constexpr unsigned MaxRetries = 3;
constexpr unsigned KeyframeInterval = 30;
unsigned side = 72;
bool streaming = false, snapshot = false, cameraReady = false, flashOn = false;
bool pending = false, awaitingDone = false;
uint32_t sequence = 0, previousSequence = 0, sentAt = 0, lastCapture = 0;
unsigned retries = 0, sinceKeyframe = 0;
size_t offset = 0, sentLength = 0;
std::vector<uint8_t> pixels, previous;
nc2::Encoded encoded;
char commandBuffer[64];
size_t commandLength = 0;
bool commandOverflow = false;

bool initCamera() {
    if (cameraReady) return true;
    camera_config_t c = {};
    c.pin_pwdn = 32; c.pin_reset = -1; c.pin_xclk = 0;
    c.pin_sccb_sda = 26; c.pin_sccb_scl = 27;
    c.pin_d7 = 35; c.pin_d6 = 34; c.pin_d5 = 39; c.pin_d4 = 36;
    c.pin_d3 = 21; c.pin_d2 = 19; c.pin_d1 = 18; c.pin_d0 = 5;
    c.pin_vsync = 25; c.pin_href = 23; c.pin_pclk = 22;
    c.xclk_freq_hz = 20000000;
    c.ledc_timer = LEDC_TIMER_0; c.ledc_channel = LEDC_CHANNEL_0;
    c.pixel_format = PIXFORMAT_RGB565;
    c.frame_size = FRAMESIZE_QVGA; // quarter of the old VGA capture area
    c.fb_count = 1;
    c.fb_location = psramFound() ? CAMERA_FB_IN_PSRAM : CAMERA_FB_IN_DRAM;
    c.grab_mode = CAMERA_GRAB_WHEN_EMPTY;
    const esp_err_t result = esp_camera_init(&c);
    if (result != ESP_OK) {
        Serial.printf("NC2 ERROR camera-init %d\n", result);
        return false;
    }
    // Keep the sensor's neutral colour/exposure defaults: no extra saturation
    // boost or clipped RGB gains. Auto exposure/white balance remain enabled.
    cameraReady = true;
    return true;
}

void cancelTransfer() {
    pending = awaitingDone = false;
    previous.clear(); // an unacknowledged frame must never become a delta base
    previousSequence = 0;
    sinceKeyframe = 0;
    encoded.payload.clear();
}

void stopCamera() {
    streaming = snapshot = false;
    cancelTransfer();
    if (cameraReady) {
        esp_camera_deinit();
        cameraReady = false;
    }
}

void sendPacket() {
    if (!offset) {
        Serial.printf("NC2 BEGIN %lu %lu %u %u %c %u %lu\n",
            static_cast<unsigned long>(sequence),
            static_cast<unsigned long>(encoded.mode == 'D' ? previousSequence : 0),
            side, side, encoded.mode, static_cast<unsigned>(encoded.payload.size()),
            static_cast<unsigned long>(encoded.checksum));
    }
    if (offset < encoded.payload.size()) {
        sentLength = std::min(nc2::ChunkChars, encoded.payload.size() - offset);
        Serial.printf("NC2 DATA %lu %u ", static_cast<unsigned long>(sequence), static_cast<unsigned>(offset));
        Serial.write(reinterpret_cast<const uint8_t*>(encoded.payload.data() + offset), sentLength);
        Serial.print('\n');
    } else {
        awaitingDone = true;
        Serial.printf("NC2 END %lu\n", static_cast<unsigned long>(sequence));
    }
    sentAt = millis();
}

bool capture() {
    if (!initCamera()) return false;
    // Return the queued single buffer first, then acquire a fresh frame. This
    // avoids displaying an image captured before a slow transfer/long pause.
    camera_fb_t* fb = esp_camera_fb_get();
    if (!fb) return false;
    esp_camera_fb_return(fb);
    fb = esp_camera_fb_get();
    if (!fb) return false;
    const bool ok = fb->format == PIXFORMAT_RGB565 &&
        nc2::downsample(fb->buf, fb->len, fb->width, fb->height, side, pixels);
    esp_camera_fb_return(fb); // never hold the camera buffer during serial I/O
    if (!ok) return false;
    encoded = nc2::encode(pixels, previous, sinceKeyframe < KeyframeInterval);
    ++sequence;
    if (!sequence) ++sequence; // zero is reserved for 'no reference'
    offset = sentLength = retries = 0;
    awaitingDone = false;
    pending = true;
    lastCapture = millis();
    sendPacket();
    return true;
}

void handleCommand(const char* command) {
    unsigned long seq = 0;
    unsigned next = 0;
    int used = 0;
    if (sscanf(command, "ACK %lu %u%n", &seq, &next, &used) == 2 && command[used] == '\0') {
        if (pending && !awaitingDone && seq == sequence && next == offset + sentLength) {
            offset = next;
            retries = 0;
            sendPacket();
        }
    } else if (sscanf(command, "DONE %lu%n", &seq, &used) == 1 && command[used] == '\0') {
        if (pending && awaitingDone && seq == sequence) {
            previous.swap(pixels);
            previousSequence = sequence;
            sinceKeyframe = encoded.mode == 'D' ? sinceKeyframe + 1 : 0;
            pending = awaitingDone = false;
            snapshot = false;
            encoded.payload.clear();
        }
    } else if (sscanf(command, "NAK %lu%n", &seq, &used) == 1 && command[used] == '\0') {
        if (pending && seq == sequence) cancelTransfer();
    } else if (!strcmp(command, "STREAM")) {
        cancelTransfer();
        streaming = true;
        snapshot = false;
    } else if (!strcmp(command, "STOP")) {
        stopCamera();
        Serial.println("NC2 STATUS stopped");
    } else if (!strcmp(command, "PIC")) {
        cancelTransfer();
        streaming = false;
        snapshot = true;
    } else if (sscanf(command, "SIZE %u%n", &next, &used) == 1 && command[used] == '\0') {
        if (next == 51 || next == 72 || next == 102) {
            cancelTransfer();
            side = next;
            Serial.printf("NC2 STATUS size %u\n", side);
        }
    } else if (!strcmp(command, "KEY")) {
        cancelTransfer();
    } else if (!strcmp(command, "FLASH")) {
        flashOn = !flashOn;
        digitalWrite(4, flashOn ? HIGH : LOW);
    } else if (!strcmp(command, "HELLO")) {
        Serial.println("NC2 READY 2 RGB332 51 72 102");
    }
}

void readCommands() {
    while (Serial.available()) {
        const char ch = static_cast<char>(Serial.read());
        if (ch == '\n') {
            if (!commandOverflow) {
                commandBuffer[commandLength] = '\0';
                handleCommand(commandBuffer);
            }
            commandLength = 0;
            commandOverflow = false;
        } else if (ch != '\r') {
            if (commandLength + 1 < sizeof(commandBuffer)) commandBuffer[commandLength++] = ch;
            else commandOverflow = true;
        }
    }
}
} // namespace

void setup() {
    Serial.begin(115200);
    pinMode(4, OUTPUT);
    digitalWrite(4, LOW);
    pixels.reserve(nc2::MaxPixels);
    previous.reserve(nc2::MaxPixels);
    // Camera mode works offline; it no longer waits indefinitely for Wi-Fi.
    Serial.println("NC2 READY 2 RGB332 51 72 102");
}

void loop() {
    readCommands(); // STOP/FLASH work even while awaiting an ACK
    if (pending) {
        if (static_cast<uint32_t>(millis() - sentAt) >= AckTimeoutMs) {
            if (++retries > MaxRetries) {
                stopCamera();
                Serial.println("NC2 ERROR ack-timeout");
            } else sendPacket();
        }
    } else if ((streaming || snapshot) && static_cast<uint32_t>(millis() - lastCapture) >= 30) {
        if (!capture()) {
            Serial.println("NC2 ERROR capture");
            stopCamera();
        }
    }
    delay(1); // yield to ESP32 tasks, no artificial per-chunk/per-frame sleep
}
