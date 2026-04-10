#include "event_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "cJSON.h"
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <ctime>

static const char* TAG          = "event_log";
static const char* NVS_NS       = "clk_cfg";
static const char* NVS_KEY      = "log_cats";  ///< uint8 bitmask: bit0=startup, bit1=led_web, bit2=led_matter
static const char* NVS_KEY_BUF  = "log_buf";   ///< packed ring-buffer blob

// Compact on-disk entry — avoids time_t size ambiguity and alignment padding.
// Must stay in sync with EventLog::MSG_LEN (72) and MAX_ENTRIES (100).
struct __attribute__((packed)) SavedEntry {
    uint32_t unix_ts;   // seconds since epoch; 0 = SNTP not synced at log time
    uint8_t  category;
    char     msg[72];
};
struct __attribute__((packed)) SavedBlob {
    uint8_t      count;
    uint8_t      head;
    SavedEntry   entries[100];
};

// ── Singleton ─────────────────────────────────────────────────────────────────

static EventLog g_instance;
EventLog& EventLog::instance() { return g_instance; }

// ── init ──────────────────────────────────────────────────────────────────────

esp_err_t EventLog::init()
{
    mutex_ = xSemaphoreCreateMutex();
    if (!mutex_) {
        ESP_LOGE(TAG, "Failed to create mutex");
        return ESP_ERR_NO_MEM;
    }

    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        uint8_t mask = 0;
        if (nvs_get_u8(h, NVS_KEY, &mask) == ESP_OK) {
            for (int i = 0; i < CAT_COUNT; i++) {
                cat_enabled_[i] = (mask >> i) & 1u;
            }
        }
        nvs_close(h);
    }

    ESP_LOGI(TAG, "init: startup=%d led_web=%d led_matter=%d",
             cat_enabled_[0], cat_enabled_[1], cat_enabled_[2]);
    load_entries();
    return ESP_OK;
}

// ── log ───────────────────────────────────────────────────────────────────────

void EventLog::log(Category cat, const char* fmt, ...)
{
    if (cat >= CAT_COUNT || !cat_enabled_[cat] || !mutex_) return;

    Entry e = {};
    e.uptime_s = (uint32_t)(esp_timer_get_time() / 1'000'000ULL);
    time_t now = time(nullptr);
    e.unix_ts  = (now >= 1000000000L) ? now : 0;   // 0 = SNTP not synced
    e.category = (uint8_t)cat;

    va_list ap;
    va_start(ap, fmt);
    vsnprintf(e.msg, sizeof(e.msg), fmt, ap);
    va_end(ap);

    xSemaphoreTake(mutex_, portMAX_DELAY);
    entries_[head_] = e;
    head_ = (head_ + 1) % MAX_ENTRIES;
    if (count_ < MAX_ENTRIES) count_++;
    xSemaphoreGive(mutex_);

    save_entries();
}

// ── set_category_enabled ──────────────────────────────────────────────────────

void EventLog::set_category_enabled(Category cat, bool enabled)
{
    if (cat >= CAT_COUNT) return;
    cat_enabled_[cat] = enabled;
    save_enables();
    ESP_LOGI(TAG, "cat %d → %s", (int)cat, enabled ? "on" : "off");
}

// ── clear ─────────────────────────────────────────────────────────────────────

void EventLog::clear()
{
    if (!mutex_) return;
    xSemaphoreTake(mutex_, portMAX_DELAY);
    head_  = 0;
    count_ = 0;
    xSemaphoreGive(mutex_);
    save_entries();
    ESP_LOGI(TAG, "log cleared");
}

// ── save_entries / load_entries ───────────────────────────────────────────────

void EventLog::save_entries() const
{
    auto* blob = static_cast<SavedBlob*>(malloc(sizeof(SavedBlob)));
    if (!blob) { ESP_LOGW(TAG, "save_entries: out of memory"); return; }

    xSemaphoreTake(mutex_, portMAX_DELAY);
    blob->count = (uint8_t)count_;
    blob->head  = (uint8_t)head_;
    for (int i = 0; i < MAX_ENTRIES; i++) {
        blob->entries[i].unix_ts  = (uint32_t)entries_[i].unix_ts;
        blob->entries[i].category = entries_[i].category;
        memcpy(blob->entries[i].msg, entries_[i].msg, MSG_LEN);
    }
    xSemaphoreGive(mutex_);

    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_blob(h, NVS_KEY_BUF, blob, sizeof(SavedBlob));
        nvs_commit(h);
        nvs_close(h);
    }
    free(blob);
}

void EventLog::load_entries()
{
    auto* blob = static_cast<SavedBlob*>(malloc(sizeof(SavedBlob)));
    if (!blob) { ESP_LOGW(TAG, "load_entries: out of memory"); return; }

    size_t sz = sizeof(SavedBlob);
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) { free(blob); return; }
    esp_err_t err = nvs_get_blob(h, NVS_KEY_BUF, blob, &sz);
    nvs_close(h);

    if (err != ESP_OK || sz != sizeof(SavedBlob)) { free(blob); return; }

    count_ = blob->count;
    head_  = blob->head;
    for (int i = 0; i < MAX_ENTRIES; i++) {
        entries_[i].uptime_s = 0;  // previous boot's uptime is meaningless
        entries_[i].unix_ts  = (time_t)blob->entries[i].unix_ts;
        entries_[i].category = blob->entries[i].category;
        memcpy(entries_[i].msg, blob->entries[i].msg, MSG_LEN);
    }
    free(blob);
    ESP_LOGI(TAG, "Restored %d log entries from NVS", count_);
}

// ── save_enables ──────────────────────────────────────────────────────────────

void EventLog::save_enables() const
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    uint8_t mask = 0;
    for (int i = 0; i < CAT_COUNT; i++) {
        if (cat_enabled_[i]) mask |= (uint8_t)(1u << i);
    }
    nvs_set_u8(h, NVS_KEY, mask);
    nvs_commit(h);
    nvs_close(h);
}

// ── get_json ──────────────────────────────────────────────────────────────────

static void fmt_timestamp(uint32_t uptime_s, time_t unix_ts, char* buf, size_t bufsz)
{
    if (unix_ts > 0) {
        struct tm tm_info = {};
        localtime_r(&unix_ts, &tm_info);
        strftime(buf, bufsz, "%H:%M:%S", &tm_info);
    } else {
        // Show uptime when wall clock is not yet synced
        uint32_t s = uptime_s;
        uint32_t d = s / 86400; s %= 86400;
        uint32_t h = s / 3600;  s %= 3600;
        uint32_t m = s / 60;    s %= 60;
        if (d > 0) {
            snprintf(buf, bufsz, "+%ud%02u:%02u", (unsigned)d, (unsigned)h, (unsigned)m);
        } else {
            snprintf(buf, bufsz, "+%02u:%02u:%02u", (unsigned)h, (unsigned)m, (unsigned)s);
        }
    }
}

static const char* cat_name(uint8_t cat)
{
    switch (cat) {
        case EventLog::CAT_STARTUP:    return "Startup";
        case EventLog::CAT_LED_WEB:    return "Web";
        case EventLog::CAT_LED_MATTER: return "Matter";
        default:                        return "?";
    }
}

char* EventLog::get_json() const
{
    if (!mutex_) return nullptr;

    xSemaphoreTake(mutex_, portMAX_DELAY);

    cJSON* root = cJSON_CreateObject();
    if (!root) { xSemaphoreGive(mutex_); return nullptr; }

    // Category enables
    cJSON* cats = cJSON_AddObjectToObject(root, "cats");
    if (cats) {
        cJSON_AddBoolToObject(cats, "startup",    (cJSON_bool)cat_enabled_[CAT_STARTUP]);
        cJSON_AddBoolToObject(cats, "led_web",    (cJSON_bool)cat_enabled_[CAT_LED_WEB]);
        cJSON_AddBoolToObject(cats, "led_matter", (cJSON_bool)cat_enabled_[CAT_LED_MATTER]);
    }

    // Entries — newest first
    // With head_ pointing to the next write slot:
    //   newest entry is at (head_-1+MAX) % MAX
    //   oldest is at head_ when full, 0 when not
    cJSON* arr = cJSON_AddArrayToObject(root, "entries");
    if (arr) {
        for (int i = 0; i < count_; i++) {
            int idx = ((head_ - 1 - i) % MAX_ENTRIES + MAX_ENTRIES) % MAX_ENTRIES;
            const Entry& e = entries_[idx];

            cJSON* item = cJSON_CreateObject();
            if (!item) break;

            char ts[20];
            fmt_timestamp(e.uptime_s, e.unix_ts, ts, sizeof(ts));
            cJSON_AddStringToObject(item, "ts",  ts);
            cJSON_AddStringToObject(item, "cat", cat_name(e.category));
            cJSON_AddStringToObject(item, "msg", e.msg);
            cJSON_AddItemToArray(arr, item);
        }
    }

    char* result = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    xSemaphoreGive(mutex_);
    return result;
}
