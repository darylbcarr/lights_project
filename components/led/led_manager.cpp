/**
 * @file led_manager.cpp
 * @brief WS2812B dual-strip LED manager with extensible effects
 *
 * Effects
 * ───────
 *  OFF      — strips dark
 *  STATIC   — solid colour at brightness
 *  BREATHE  — sinusoidal pulse (~3 s period)
 *  RAINBOW  — hue rotates across all pixels
 *  CHASE    — lit pixel with short tail sweeping the strip
 *  SPARKLE  — random pixels twinkle
 *  WIPE     — fills then clears the strip from one end
 *  COMET    — bright head + 8-pixel fading tail
 *
 * Each strip is independent by default.  Target::BOTH applies changes to both.
 * In linked mode (set_linked(true)) both strips are rendered as one virtual
 * strip so positional effects span continuously across all LEDs.
 * The effect task runs at ~30 fps (33 ms tick) on a dedicated FreeRTOS task.
 */

#include "led_manager.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <cstring>
#include <cstdlib>
#include <cmath>

static const char* TAG = "LedMgr";

// ── Static helpers ────────────────────────────────────────────────────────────

const char* LedManager::effect_name(Effect e)
{
    switch (e) {
        case Effect::OFF:     return "Off";
        case Effect::STATIC:  return "Static";
        case Effect::BREATHE: return "Breathe";
        case Effect::RAINBOW: return "Rainbow";
        case Effect::CHASE:   return "Chase";
        case Effect::SPARKLE: return "Sparkle";
        case Effect::WIPE:    return "Wipe";
        case Effect::COMET:   return "Comet";
        default:              return "Unknown";
    }
}

LedManager::Effect LedManager::effect_next(Effect e)
{
    uint8_t n = (static_cast<uint8_t>(e) + 1) % static_cast<uint8_t>(Effect::COUNT);
    return static_cast<Effect>(n);
}

uint8_t LedManager::effect_count()
{
    return static_cast<uint8_t>(Effect::COUNT);
}

// ── Constructor / Destructor ──────────────────────────────────────────────────

LedManager::LedManager(gpio_num_t gpio1, gpio_num_t gpio2, uint16_t max_len)
{
    strips_[0].gpio       = gpio1;
    strips_[0].max_len    = max_len;
    strips_[0].active_len = max_len;

    strips_[1].gpio       = gpio2;
    strips_[1].max_len    = max_len;
    strips_[1].active_len = max_len;

    mutex_ = xSemaphoreCreateMutex();
}

LedManager::~LedManager()
{
    if (task_handle_) { vTaskDelete(task_handle_); task_handle_ = nullptr; }
    for (auto& s : strips_) {
        if (s.handle) { led_strip_del(s.handle); s.handle = nullptr; }
    }
    if (mutex_) { vSemaphoreDelete(mutex_); mutex_ = nullptr; }
}

// ── init() ────────────────────────────────────────────────────────────────────

esp_err_t LedManager::init()
{
    for (auto& s : strips_) {
        led_strip_config_t cfg = {};
        cfg.strip_gpio_num          = static_cast<int>(s.gpio);
        cfg.max_leds                = s.max_len;
        cfg.led_model               = LED_MODEL_WS2812;
        cfg.color_component_format  = LED_STRIP_COLOR_COMPONENT_FMT_GRB;

        led_strip_rmt_config_t rmt_cfg = {};
        rmt_cfg.resolution_hz = 10 * 1000 * 1000;  // 10 MHz

        esp_err_t ret = led_strip_new_rmt_device(&cfg, &rmt_cfg, &s.handle);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "led_strip_new_rmt_device GPIO%d: %s",
                     (int)s.gpio, esp_err_to_name(ret));
            return ret;
        }
        led_strip_clear(s.handle);
        ESP_LOGI(TAG, "Strip GPIO%d ready, %u px", (int)s.gpio, s.max_len);
    }
    return ESP_OK;
}

// ── start() ───────────────────────────────────────────────────────────────────

void LedManager::start()
{
    xTaskCreate(effect_task, "led_fx", 3072, this, 2, &task_handle_);
}

// ── Public setters ────────────────────────────────────────────────────────────

void LedManager::set_color(Target t, uint8_t r, uint8_t g, uint8_t b)
{
    xSemaphoreTake(mutex_, portMAX_DELAY);
    for (int i = 0; i < STRIP_COUNT; i++) {
        if (t == Target::BOTH || static_cast<int>(t) == i) {
            strips_[i].r = r;
            strips_[i].g = g;
            strips_[i].b = b;
        }
    }
    xSemaphoreGive(mutex_);
}

void LedManager::set_brightness(Target t, uint8_t brightness)
{
    xSemaphoreTake(mutex_, portMAX_DELAY);
    for (int i = 0; i < STRIP_COUNT; i++) {
        if (t == Target::BOTH || static_cast<int>(t) == i)
            strips_[i].brightness = brightness;
    }
    xSemaphoreGive(mutex_);
}

void LedManager::set_effect(Target t, Effect e)
{
    xSemaphoreTake(mutex_, portMAX_DELAY);
    for (int i = 0; i < STRIP_COUNT; i++) {
        if (t == Target::BOTH || static_cast<int>(t) == i) {
            strips_[i].effect    = e;
            strips_[i].phase     = 0;
            strips_[i].chase_pos = 0;
            strips_[i].wipe_pos  = 0;
            strips_[i].wipe_fill = true;
        }
    }
    xSemaphoreGive(mutex_);
}

void LedManager::set_active_len(Target t, uint16_t len)
{
    xSemaphoreTake(mutex_, portMAX_DELAY);
    for (int i = 0; i < STRIP_COUNT; i++) {
        if (t == Target::BOTH || static_cast<int>(t) == i) {
            if (len > strips_[i].max_len) len = strips_[i].max_len;
            strips_[i].active_len = len;
        }
    }
    xSemaphoreGive(mutex_);
}

void LedManager::next_effect(Target t)
{
    xSemaphoreTake(mutex_, portMAX_DELAY);
    for (int i = 0; i < STRIP_COUNT; i++) {
        if (t == Target::BOTH || static_cast<int>(t) == i) {
            Effect e = effect_next(strips_[i].effect);
            strips_[i].effect    = e;
            strips_[i].phase     = 0;
            strips_[i].chase_pos = 0;
            strips_[i].wipe_pos  = 0;
            strips_[i].wipe_fill = true;
            ESP_LOGI(TAG, "Strip %d → %s", i, effect_name(e));
        }
    }
    xSemaphoreGive(mutex_);
}

// ── Public getters ────────────────────────────────────────────────────────────

LedManager::Effect LedManager::get_effect(int idx) const
{
    if (idx < 0 || idx >= STRIP_COUNT) return Effect::OFF;
    return strips_[idx].effect;
}

uint8_t LedManager::get_brightness(int idx) const
{
    if (idx < 0 || idx >= STRIP_COUNT) return 0;
    return strips_[idx].brightness;
}

uint16_t LedManager::get_active_len(int idx) const
{
    if (idx < 0 || idx >= STRIP_COUNT) return 0;
    return strips_[idx].active_len;
}

void LedManager::get_color(int idx, uint8_t& r, uint8_t& g, uint8_t& b) const
{
    if (idx < 0 || idx >= STRIP_COUNT) { r = g = b = 0; return; }
    r = strips_[idx].r;
    g = strips_[idx].g;
    b = strips_[idx].b;
}

// ── Effect task ───────────────────────────────────────────────────────────────

void LedManager::effect_task(void* arg)
{
    auto* self = static_cast<LedManager*>(arg);
    while (true) {
        xSemaphoreTake(self->mutex_, portMAX_DELAY);
        if (self->linked_) {
            self->tick_combined();
        } else {
            for (auto& s : self->strips_) {
                if (s.handle) self->tick_strip(s);
            }
        }
        xSemaphoreGive(self->mutex_);
        vTaskDelay(pdMS_TO_TICKS(33));   // ~30 fps
    }
}

// ── tick_strip ────────────────────────────────────────────────────────────────

void LedManager::tick_strip(StripState& s)
{
    switch (s.effect) {
        case Effect::OFF:     fx_off(s);     break;
        case Effect::STATIC:  fx_static(s);  break;
        case Effect::BREATHE: fx_breathe(s); break;
        case Effect::RAINBOW: fx_rainbow(s); break;
        case Effect::CHASE:   fx_chase(s);   break;
        case Effect::SPARKLE: fx_sparkle(s); break;
        case Effect::WIPE:    fx_wipe(s);    break;
        case Effect::COMET:   fx_comet(s);   break;
        default: break;
    }
    s.phase++;
    led_strip_refresh(s.handle);
}

// ── apply_pixel ───────────────────────────────────────────────────────────────

void LedManager::apply_pixel(StripState& s, uint16_t idx,
                              uint8_t r, uint8_t g, uint8_t b)
{
    if (!s.handle || idx >= s.active_len) return;
    uint8_t rs = static_cast<uint8_t>(static_cast<uint16_t>(r) * s.brightness / 255);
    uint8_t gs = static_cast<uint8_t>(static_cast<uint16_t>(g) * s.brightness / 255);
    uint8_t bs = static_cast<uint8_t>(static_cast<uint16_t>(b) * s.brightness / 255);
    led_strip_set_pixel(s.handle, idx, rs, gs, bs);
}

// ── Linked mode ───────────────────────────────────────────────────────────────

void LedManager::set_linked(bool linked)
{
    xSemaphoreTake(mutex_, portMAX_DELAY);
    linked_ = linked;
    xSemaphoreGive(mutex_);
}

bool LedManager::get_linked() const
{
    return linked_;
}

// Write a pixel to the correct physical strip by virtual index.
// Strip 0 occupies virtual indices 0..len0-1, strip 1 the rest.
// Strip 0 is physically reversed: virtual 0 maps to the far end (physical len0-1)
// and virtual len0-1 maps to the center (physical 0), so effects travel from
// the outer tip of strip 0 inward to the center, then outward along strip 1.
// Brightness is applied here (caller passes the raw r/g/b + desired brightness).
void LedManager::apply_virtual(int vidx, uint8_t r, uint8_t g, uint8_t b, uint8_t bri)
{
    uint8_t rs = static_cast<uint8_t>(static_cast<uint16_t>(r) * bri / 255);
    uint8_t gs = static_cast<uint8_t>(static_cast<uint16_t>(g) * bri / 255);
    uint8_t bs = static_cast<uint8_t>(static_cast<uint16_t>(b) * bri / 255);
    int len0 = strips_[0].active_len;
    if (vidx < len0)
        led_strip_set_pixel(strips_[0].handle, static_cast<uint16_t>(len0 - 1 - vidx), rs, gs, bs);
    else
        led_strip_set_pixel(strips_[1].handle, static_cast<uint16_t>(vidx - len0), rs, gs, bs);
}

// Renders both strips as a single virtual strip of (len0 + len1) pixels.
// Strip 0 is the master: its effect, color, brightness, and phase are used.
// Must be called with mutex_ already held.
void LedManager::tick_combined()
{
    StripState& m = strips_[0];
    if (!m.handle || !strips_[1].handle) return;

    int len0  = static_cast<int>(m.active_len);
    int len1  = static_cast<int>(strips_[1].active_len);
    int total = len0 + len1;
    if (total == 0) return;

    auto clear_all = [&]() {
        for (int i = 0; i < static_cast<int>(m.max_len); i++)
            led_strip_set_pixel(m.handle, i, 0, 0, 0);
        for (int i = 0; i < static_cast<int>(strips_[1].max_len); i++)
            led_strip_set_pixel(strips_[1].handle, i, 0, 0, 0);
    };

    auto clear_v = [&](int vidx) {
        if (vidx < len0) led_strip_set_pixel(m.handle, static_cast<uint16_t>(len0 - 1 - vidx), 0, 0, 0);
        else             led_strip_set_pixel(strips_[1].handle, static_cast<uint16_t>(vidx - len0), 0, 0, 0);
    };

    switch (m.effect) {
        case Effect::OFF:
            clear_all();
            break;

        case Effect::STATIC:
            for (int i = 0; i < total; i++)
                apply_virtual(i, m.r, m.g, m.b, m.brightness);
            for (int i = total; i < static_cast<int>(m.max_len); i++)
                led_strip_set_pixel(m.handle, i, 0, 0, 0);
            break;

        case Effect::BREATHE: {
            float t     = static_cast<float>(m.phase % 90) / 90.0f;
            float lvl   = (1.0f - cosf(2.0f * static_cast<float>(M_PI) * t)) * 0.5f;
            uint8_t bsc = static_cast<uint8_t>(static_cast<float>(m.brightness) * lvl);
            for (int i = 0; i < total; i++)
                apply_virtual(i, m.r, m.g, m.b, bsc);
            for (int i = total; i < static_cast<int>(m.max_len); i++)
                led_strip_set_pixel(m.handle, i, 0, 0, 0);
            break;
        }

        case Effect::RAINBOW:
            for (int i = 0; i < total; i++) {
                uint8_t hue = static_cast<uint8_t>((i * 256 / total + m.phase * 3) & 0xFF);
                uint8_t r, g, b;
                hue_to_rgb(hue, r, g, b);
                apply_virtual(i, r, g, b, m.brightness);
            }
            for (int i = total; i < static_cast<int>(m.max_len); i++)
                led_strip_set_pixel(m.handle, i, 0, 0, 0);
            break;

        case Effect::CHASE: {
            if (m.phase % 2 == 0) {
                if (m.wipe_fill) {
                    if (m.chase_pos < static_cast<uint16_t>(total - 1)) m.chase_pos++;
                    else                                                  m.wipe_fill = false;
                } else {
                    if (m.chase_pos > 0) m.chase_pos--;
                    else                 m.wipe_fill = true;
                }
            }
            clear_all();
            int dir = m.wipe_fill ? 1 : -1;
            auto set_v = [&](int offset, uint8_t scale) {
                int pos = static_cast<int>(m.chase_pos) - dir * offset;
                if (pos < 0 || pos >= total) return;
                uint8_t rs = static_cast<uint8_t>(static_cast<uint16_t>(m.r) * m.brightness / 255 * scale / 255);
                uint8_t gs = static_cast<uint8_t>(static_cast<uint16_t>(m.g) * m.brightness / 255 * scale / 255);
                uint8_t bs = static_cast<uint8_t>(static_cast<uint16_t>(m.b) * m.brightness / 255 * scale / 255);
                if (pos < len0) led_strip_set_pixel(m.handle, static_cast<uint16_t>(len0 - 1 - pos), rs, gs, bs);
                else            led_strip_set_pixel(strips_[1].handle, static_cast<uint16_t>(pos - len0), rs, gs, bs);
            };
            set_v(0, 255);
            set_v(1, 127);
            set_v(2,  50);
            set_v(3,  20);
            break;
        }

        case Effect::SPARKLE: {
            if (m.phase == 0) clear_all();
            int on_idx  = rand() % total;
            int off_idx = rand() % total;
            apply_virtual(on_idx, m.r, m.g, m.b, m.brightness);
            clear_v(off_idx);
            break;
        }

        case Effect::WIPE: {
            if (m.phase % 2 == 0) {
                m.wipe_pos++;
                if (m.wipe_pos > static_cast<uint16_t>(total)) {
                    m.wipe_pos  = 0;
                    m.wipe_fill = !m.wipe_fill;
                }
            }
            for (int i = 0; i < total; i++) {
                bool lit = m.wipe_fill ? (i < static_cast<int>(m.wipe_pos))
                                       : (i >= (total - static_cast<int>(m.wipe_pos)));
                if (lit) apply_virtual(i, m.r, m.g, m.b, m.brightness);
                else     clear_v(i);
            }
            break;
        }

        case Effect::COMET: {
            // Advance head every tick, bouncing at each end
            if (m.wipe_fill) {
                if (m.chase_pos < static_cast<uint16_t>(total - 1)) m.chase_pos++;
                else                                                  m.wipe_fill = false;
            } else {
                if (m.chase_pos > 0) m.chase_pos--;
                else                 m.wipe_fill = true;
            }
            clear_all();
            int head = static_cast<int>(m.chase_pos);
            int dir  = m.wipe_fill ? 1 : -1;
            static constexpr int TAIL_LEN = 8;
            for (int tail = 0; tail < TAIL_LEN; tail++) {
                int pos = head - dir * tail;
                if (pos < 0 || pos >= total) break;
                float fade = 1.0f - static_cast<float>(tail) / TAIL_LEN;
                uint8_t rs = static_cast<uint8_t>(m.r * fade * m.brightness / 255.0f);
                uint8_t gs = static_cast<uint8_t>(m.g * fade * m.brightness / 255.0f);
                uint8_t bs = static_cast<uint8_t>(m.b * fade * m.brightness / 255.0f);
                if (pos < len0) led_strip_set_pixel(m.handle, static_cast<uint16_t>(len0 - 1 - pos), rs, gs, bs);
                else            led_strip_set_pixel(strips_[1].handle, static_cast<uint16_t>(pos - len0), rs, gs, bs);
            }
            break;
        }

        default: break;
    }

    m.phase++;
    strips_[1].phase = m.phase;   // keep strip 1 in sync for smooth mode transitions

    led_strip_refresh(m.handle);
    led_strip_refresh(strips_[1].handle);
}

// ── hue_to_rgb ────────────────────────────────────────────────────────────────

void LedManager::hue_to_rgb(uint8_t hue, uint8_t& r, uint8_t& g, uint8_t& b)
{
    uint8_t region = hue / 43;
    uint8_t rem    = static_cast<uint8_t>((hue - region * 43) * 6);
    uint8_t q      = 255 - rem;
    uint8_t t      = rem;
    switch (region) {
        case 0:  r = 255; g = t;   b = 0;   break;
        case 1:  r = q;   g = 255; b = 0;   break;
        case 2:  r = 0;   g = 255; b = t;   break;
        case 3:  r = 0;   g = q;   b = 255; break;
        case 4:  r = t;   g = 0;   b = 255; break;
        default: r = 255; g = 0;   b = q;   break;
    }
}

// ── Effects ───────────────────────────────────────────────────────────────────

void LedManager::fx_off(StripState& s)
{
    for (uint16_t i = 0; i < s.max_len; i++)
        led_strip_set_pixel(s.handle, i, 0, 0, 0);
}

void LedManager::fx_static(StripState& s)
{
    for (uint16_t i = 0; i < s.active_len; i++)
        apply_pixel(s, i, s.r, s.g, s.b);
    // blank any pixels beyond active_len
    for (uint16_t i = s.active_len; i < s.max_len; i++)
        led_strip_set_pixel(s.handle, i, 0, 0, 0);
}

void LedManager::fx_breathe(StripState& s)
{
    // Full period ≈ 90 ticks × 33 ms ≈ 3 s
    float t     = static_cast<float>(s.phase % 90) / 90.0f;
    float level = (1.0f - cosf(2.0f * static_cast<float>(M_PI) * t)) * 0.5f;
    auto  bsc   = static_cast<uint8_t>(static_cast<float>(s.brightness) * level);
    for (uint16_t i = 0; i < s.active_len; i++) {
        uint8_t rs = static_cast<uint8_t>(static_cast<uint16_t>(s.r) * bsc / 255);
        uint8_t gs = static_cast<uint8_t>(static_cast<uint16_t>(s.g) * bsc / 255);
        uint8_t bs = static_cast<uint8_t>(static_cast<uint16_t>(s.b) * bsc / 255);
        led_strip_set_pixel(s.handle, i, rs, gs, bs);
    }
    for (uint16_t i = s.active_len; i < s.max_len; i++)
        led_strip_set_pixel(s.handle, i, 0, 0, 0);
}

void LedManager::fx_rainbow(StripState& s)
{
    for (uint16_t i = 0; i < s.active_len; i++) {
        uint8_t hue = static_cast<uint8_t>(
            (i * 256 / s.active_len + s.phase * 3) & 0xFF);
        uint8_t r, g, b;
        hue_to_rgb(hue, r, g, b);
        apply_pixel(s, i, r, g, b);
    }
    for (uint16_t i = s.active_len; i < s.max_len; i++)
        led_strip_set_pixel(s.handle, i, 0, 0, 0);
}

void LedManager::fx_chase(StripState& s)
{
    if (s.active_len == 0) return;

    // Advance every 2 ticks, bouncing at each end
    if (s.phase % 2 == 0) {
        if (s.wipe_fill) {
            if (s.chase_pos < s.active_len - 1) s.chase_pos++;
            else                                 s.wipe_fill = false;
        } else {
            if (s.chase_pos > 0) s.chase_pos--;
            else                 s.wipe_fill = true;
        }
    }

    for (uint16_t i = 0; i < s.active_len; i++)
        led_strip_set_pixel(s.handle, i, 0, 0, 0);

    // Tail trails behind the direction of travel; skip pixels outside the strip
    int dir = s.wipe_fill ? 1 : -1;
    auto set_chase = [&](int offset, uint8_t scale) {
        int pos = static_cast<int>(s.chase_pos) - dir * offset;
        if (pos < 0 || pos >= static_cast<int>(s.active_len)) return;
        uint8_t rs = static_cast<uint8_t>(static_cast<uint16_t>(s.r) * s.brightness / 255 * scale / 255);
        uint8_t gs = static_cast<uint8_t>(static_cast<uint16_t>(s.g) * s.brightness / 255 * scale / 255);
        uint8_t bs = static_cast<uint8_t>(static_cast<uint16_t>(s.b) * s.brightness / 255 * scale / 255);
        led_strip_set_pixel(s.handle, static_cast<uint16_t>(pos), rs, gs, bs);
    };
    set_chase(0, 255);
    set_chase(1, 127);
    set_chase(2, 50);
    set_chase(3, 20);

    for (uint16_t i = s.active_len; i < s.max_len; i++)
        led_strip_set_pixel(s.handle, i, 0, 0, 0);
}

void LedManager::fx_sparkle(StripState& s)
{
    // On first tick of this effect cycle, clear the strip
    if (s.phase == 0) {
        for (uint16_t i = 0; i < s.max_len; i++)
            led_strip_set_pixel(s.handle, i, 0, 0, 0);
    }
    // Each tick: light one pixel, extinguish one
    uint16_t on_idx  = static_cast<uint16_t>(rand() % s.active_len);
    uint16_t off_idx = static_cast<uint16_t>(rand() % s.active_len);
    apply_pixel(s, on_idx, s.r, s.g, s.b);
    led_strip_set_pixel(s.handle, off_idx, 0, 0, 0);
}

void LedManager::fx_wipe(StripState& s)
{
    // Advance position every 2 ticks
    if (s.phase % 2 == 0) {
        s.wipe_pos++;
        if (s.wipe_pos > s.active_len) {
            s.wipe_pos  = 0;
            s.wipe_fill = !s.wipe_fill;
        }
    }
    // Rebuild full strip state
    for (uint16_t i = 0; i < s.active_len; i++) {
        bool lit = s.wipe_fill ? (i < s.wipe_pos)
                               : (i >= (s.active_len - s.wipe_pos));
        if (lit) apply_pixel(s, i, s.r, s.g, s.b);
        else     led_strip_set_pixel(s.handle, i, 0, 0, 0);
    }
    for (uint16_t i = s.active_len; i < s.max_len; i++)
        led_strip_set_pixel(s.handle, i, 0, 0, 0);
}

void LedManager::fx_comet(StripState& s)
{
    if (s.active_len == 0) return;

    // Advance head every tick, bouncing at each end
    if (s.wipe_fill) {
        if (s.chase_pos < s.active_len - 1) s.chase_pos++;
        else                                 s.wipe_fill = false;
    } else {
        if (s.chase_pos > 0) s.chase_pos--;
        else                 s.wipe_fill = true;
    }

    for (uint16_t i = 0; i < s.max_len; i++)
        led_strip_set_pixel(s.handle, i, 0, 0, 0);

    int head = static_cast<int>(s.chase_pos);
    int dir  = s.wipe_fill ? 1 : -1;
    static constexpr int TAIL_LEN = 8;
    for (int tail = 0; tail < TAIL_LEN; tail++) {
        int pos = head - dir * tail;
        if (pos < 0 || pos >= static_cast<int>(s.active_len)) break;
        float fade = 1.0f - static_cast<float>(tail) / TAIL_LEN;
        uint8_t rs = static_cast<uint8_t>(s.r * fade * s.brightness / 255.0f);
        uint8_t gs = static_cast<uint8_t>(s.g * fade * s.brightness / 255.0f);
        uint8_t bs = static_cast<uint8_t>(s.b * fade * s.brightness / 255.0f);
        led_strip_set_pixel(s.handle, static_cast<uint16_t>(pos), rs, gs, bs);
    }
}
