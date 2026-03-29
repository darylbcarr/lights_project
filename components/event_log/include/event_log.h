#pragma once
#include <cstdint>
#include <ctime>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

/**
 * Thread-safe RAM ring buffer for categorized event logging.
 *
 * Three categories: Startup, LED-Web, LED-Matter (all off by default).
 * Category enables are persisted to NVS under the "clk_cfg" namespace.
 * When the buffer is full the oldest entry is overwritten (round-robin).
 * Timestamps use wall clock when SNTP is synced, else uptime.
 *
 * Access the singleton via EventLog::instance().
 */
class EventLog {
public:
    enum Category : uint8_t {
        CAT_STARTUP    = 0,  ///< Boot, WiFi, SNTP, Matter lifecycle events
        CAT_LED_WEB    = 1,  ///< LED changes from the web UI
        CAT_LED_MATTER = 2,  ///< LED changes from Matter / Alexa
        CAT_COUNT      = 3
    };

    static EventLog& instance();

    /// Load category enables from NVS. Call once after nvs_flash_init().
    esp_err_t init();

    /// Log a printf-style message if the category is currently enabled.
    /// Thread-safe; safe to call from any FreeRTOS task.
    void log(Category cat, const char* fmt, ...) __attribute__((format(printf, 3, 4)));

    /// Returns a heap-allocated JSON string. Caller must free(). Thread-safe.
    /// Format: {"cats":{...},"entries":[{"ts":"HH:MM:SS","cat":"...","msg":"..."},...]}
    /// Entries are newest-first.
    char* get_json() const;

    /// Enable or disable a category; change is persisted to NVS immediately.
    void set_category_enabled(Category cat, bool enabled);

    bool is_category_enabled(Category cat) const {
        return cat < CAT_COUNT && cat_enabled_[cat];
    }

    /// Discard all buffered entries (category enables are unchanged).
    void clear();

private:
    static constexpr int MAX_ENTRIES = 100;
    static constexpr int MSG_LEN     = 72;

    struct Entry {
        uint32_t uptime_s;     ///< Seconds since boot (always valid)
        time_t   unix_ts;      ///< Wall clock, 0 = SNTP not yet synced
        uint8_t  category;
        char     msg[MSG_LEN]; ///< Null-terminated message string
    };

    Entry                 entries_[MAX_ENTRIES] = {};
    int                   head_  = 0;   ///< Next write position (= oldest when full)
    int                   count_ = 0;   ///< Number of valid entries [0..MAX_ENTRIES]
    bool                  cat_enabled_[CAT_COUNT] = {};  // all false = all off
    mutable SemaphoreHandle_t mutex_ = nullptr;

    void save_enables() const;
};
