#pragma once
#include <cstddef>
#include <cstdint>
using esp_err_t = int;
constexpr int ESP_OK = 0, LEDC_TIMER_0 = 0, LEDC_CHANNEL_0 = 0;
constexpr int PIXFORMAT_RGB565 = 1, FRAMESIZE_QVGA = 1;
constexpr int CAMERA_FB_IN_PSRAM = 1, CAMERA_FB_IN_DRAM = 0, CAMERA_GRAB_WHEN_EMPTY = 0;
struct camera_config_t {
    int pin_pwdn, pin_reset, pin_xclk, pin_sccb_sda, pin_sccb_scl;
    int pin_d7, pin_d6, pin_d5, pin_d4, pin_d3, pin_d2, pin_d1, pin_d0;
    int pin_vsync, pin_href, pin_pclk, xclk_freq_hz, ledc_timer, ledc_channel;
    int pixel_format, frame_size, fb_count, fb_location, grab_mode;
};
struct camera_fb_t { uint8_t* buf; size_t len, width, height; int format; };
static uint8_t cameraData[320 * 240 * 2] = {};
static camera_fb_t testFrame = {cameraData, sizeof(cameraData), 320, 240, PIXFORMAT_RGB565};
static int frameGets = 0, returns = 0, deinitializes = 0;
static bool failCapture = false;
inline esp_err_t esp_camera_init(const camera_config_t*) { return ESP_OK; }
inline esp_err_t esp_camera_deinit() { ++deinitializes; return ESP_OK; }
inline camera_fb_t* esp_camera_fb_get() { if (failCapture) return nullptr; ++frameGets; return &testFrame; }
inline void esp_camera_fb_return(camera_fb_t*) { ++returns; }
