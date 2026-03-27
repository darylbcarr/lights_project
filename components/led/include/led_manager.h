#pragma once

#include <cstdint>
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "led_strip.h"

class LedManager {
public:
    static constexpr uint16_t DEFAULT_MAX_LEN = 30;
    static constexpr int      STRIP_COUNT     = 2;

    enum class Effect : uint8_t {
        OFF = 0,
        STATIC,
        BREATHE,
        RAINBOW,
        CHASE,
        SPARKLE,
        WIPE,
        COMET,
        COUNT
    };

    enum class Target : uint8_t {
        STRIP_1 = 0,
        STRIP_2 = 1,
        BOTH    = 2
    };

    static const char* effect_name(Effect e);
    static Effect      effect_next(Effect e);
    static uint8_t     effect_count();

    LedManager(gpio_num_t gpio1, gpio_num_t gpio2, uint16_t max_len = DEFAULT_MAX_LEN);
    ~LedManager();

    esp_err_t init();
    void      start();

    void set_color(Target t, uint8_t r, uint8_t g, uint8_t b);
    void set_brightness(Target t, uint8_t brightness);
    void set_effect(Target t, Effect e);
    void set_active_len(Target t, uint16_t len);
    void next_effect(Target t);

    /**
     * @brief When true, both strips are rendered as one virtual strip
     *        so that positional effects (rainbow, chase, comet, wipe, sparkle)
     *        span continuously across all LEDs.  Strip 0 is the master for
     *        color, brightness, effect, and phase state.
     */
    void set_linked(bool linked);
    bool get_linked() const;

    Effect   get_effect(int idx)     const;
    uint8_t  get_brightness(int idx) const;
    uint16_t get_active_len(int idx) const;
    void     get_color(int idx, uint8_t& r, uint8_t& g, uint8_t& b) const;

private:
    struct StripState {
        led_strip_handle_t handle     = nullptr;
        gpio_num_t         gpio;
        uint16_t           max_len;
        uint16_t           active_len;
        Effect             effect     = Effect::STATIC;
        uint8_t            r          = 255;
        uint8_t            g          = 255;
        uint8_t            b          = 255;
        uint8_t            brightness = 128;
        uint32_t           phase      = 0;
        uint16_t           chase_pos  = 0;
        bool               wipe_fill  = true;
        uint16_t           wipe_pos   = 0;
    };

    StripState        strips_[STRIP_COUNT];
    SemaphoreHandle_t mutex_        = nullptr;
    TaskHandle_t      task_handle_  = nullptr;
    bool              linked_       = false;

    void tick_strip(StripState& s);
    void tick_combined();
    void apply_pixel(StripState& s, uint16_t idx, uint8_t r, uint8_t g, uint8_t b);
    void apply_virtual(int vidx, uint8_t r, uint8_t g, uint8_t b, uint8_t bri);

    void fx_off(StripState& s);
    void fx_static(StripState& s);
    void fx_breathe(StripState& s);
    void fx_rainbow(StripState& s);
    void fx_chase(StripState& s);
    void fx_sparkle(StripState& s);
    void fx_wipe(StripState& s);
    void fx_comet(StripState& s);

    static void hue_to_rgb(uint8_t hue, uint8_t& r, uint8_t& g, uint8_t& b);
    static void effect_task(void* arg);
};
