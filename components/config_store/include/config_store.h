#pragma once
#include <cstdint>
#include "esp_err.h"

/// Persisted clock-motor state
struct ClockCfg {
    int32_t  sensor_offset_steps = 0; ///< Steps from slot trigger to 12:00
    bool     motor_reverse = false;   ///< Flip motor direction
    uint32_t step_delay_us = 2000;    ///< Per-half-step delay (µs)
    int32_t  disp_minute   = -1;      ///< Last known displayed minute (0-59, -1=unknown)
    int32_t  disp_hour     = -1;      ///< Last known displayed hour   (0-11, -1=unknown)
};

/// Persisted per-strip LED state
struct LedStripCfg {
    uint16_t len        = 0;   ///< 0 = never saved; main.cpp applies per-strip hardware defaults
    uint8_t  r          = 255;
    uint8_t  g          = 255;
    uint8_t  b          = 255;
    uint8_t  brightness = 10;
    uint8_t  effect     = 1;   ///< LedManager::Effect cast to uint8_t (1 = STATIC)
};

/// Persisted LED state for all strips
struct LedCfg {
    LedStripCfg strip[2];
};

/// Persisted network credentials and timezone override
struct NetCfg {
    char ssid[64]           = {};
    char password[64]       = {};
    char tz_override[80]    = {};
    char mdns_hostname[32]  = {};   ///< e.g. "clock_a1b2"; empty = derive from MAC
    bool wifi_only          = false; ///< true = WiFi chosen at first-time setup; Matter never starts
    bool matter_commissioned = false; ///< true = Matter commissioning completed; skip first-time setup on reboot
};

/**
 * Thin wrapper around ESP-IDF NVS.
 * All methods open, read/write, then close the NVS handle.
 * Call init() once after nvs_flash_init().
 */
class ConfigStore {
public:
    /// Verify the NVS namespace is accessible.  Call once after nvs_flash_init().
    static esp_err_t init();

    // ── Clock ─────────────────────────────────────────────────────────────────
    static bool load(ClockCfg& out);
    static bool save(const ClockCfg& cfg);
    /// Convenience: update only the displayed 12h position (called every minute).
    static bool save_disp_position(int32_t hour, int32_t min);

    // ── LEDs ──────────────────────────────────────────────────────────────────
    static bool load(LedCfg& out);
    static bool save(const LedCfg& cfg);

    // ── Network ───────────────────────────────────────────────────────────────
    static bool load(NetCfg& out);
    static bool save(const NetCfg& cfg);
};
