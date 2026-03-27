/**
 * @file ota_manager.cpp
 * @brief OTA firmware update — polls GitHub version.json, downloads via HTTPS.
 */

#include "ota_manager.h"
#include "display.h"

#include <cstring>
#include <cstdio>
#include "esp_log.h"
#include "esp_app_desc.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_crt_bundle.h"
#include "esp_ota_ops.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "cJSON.h"

static const char* TAG = "ota";

static constexpr const char* NVS_NS  = "ota_cfg";
static constexpr const char* NVS_KEY = "auto_upd";

// ── Public: running version ───────────────────────────────────────────────────

const char* OtaManager::running_version()
{
    return esp_app_get_description()->version;
}

// ── Public: is_update_available ───────────────────────────────────────────────

bool OtaManager::is_update_available() const
{
    return latest_ver_buf_[0] != '\0'
        && strcmp(latest_ver_buf_, running_version()) != 0;
}

// ── NVS settings ─────────────────────────────────────────────────────────────

void OtaManager::load_settings()
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        uint8_t val = 1;
        nvs_get_u8(h, NVS_KEY, &val);
        auto_update_ = (val != 0);
        nvs_close(h);
    }
    ESP_LOGI(TAG, "Auto update: %s", auto_update_ ? "enabled" : "disabled");
}

void OtaManager::save_settings()
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, NVS_KEY, auto_update_ ? 1 : 0);
        nvs_commit(h);
        nvs_close(h);
    }
}

void OtaManager::set_auto_update(bool en)
{
    auto_update_ = en;
    save_settings();
    ESP_LOGI(TAG, "Auto update set to: %s", en ? "enabled" : "disabled");
}

// ── Public: start background task ─────────────────────────────────────────────

void OtaManager::start(Display& display, std::function<bool()> is_connected_fn)
{
    display_         = &display;
    is_connected_fn_ = is_connected_fn;
    action_queue_    = xQueueCreate(1, sizeof(TaskAction));
    xTaskCreate(ota_task, "ota_check", 12288, this, 2, nullptr);
    ESP_LOGI(TAG, "OTA task started  (running v%s)", running_version());
}

// ── Public: triggers ──────────────────────────────────────────────────────────

void OtaManager::trigger_check()
{
    if (!action_queue_) return;
    TaskAction a = TaskAction::CHECK_ONLY;
    xQueueOverwrite(action_queue_, &a);
}

void OtaManager::trigger_update()
{
    if (!action_queue_) return;
    TaskAction a = TaskAction::CHECK_AND_INSTALL;
    xQueueOverwrite(action_queue_, &a);
}

// ── Public: synchronous check (console) ───────────────────────────────────────

esp_err_t OtaManager::check_now()
{
    if (!is_connected_fn_ || !is_connected_fn_()) {
        ESP_LOGW(TAG, "check_now: WiFi not connected");
        return ESP_ERR_INVALID_STATE;
    }
    return do_check_and_update();
}

// ── Background task ───────────────────────────────────────────────────────────

void OtaManager::ota_task(void* arg)
{
    auto* self = static_cast<OtaManager*>(arg);
    self->load_settings();

    // Wait for WiFi before first check — poll every 5 s up to BOOT_DELAY_S.
    int waited = 0;
    while (waited < BOOT_DELAY_S && self->is_connected_fn_ &&
           !self->is_connected_fn_()) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        waited += 5;
    }

    while (true) {
        // Block until a manual trigger arrives or the periodic interval elapses.
        TaskAction action = TaskAction::PERIODIC;
        TickType_t timeout_ticks = pdMS_TO_TICKS((uint32_t)CHECK_INTERVAL_S * 1000UL);
        xQueueReceive(self->action_queue_, &action, timeout_ticks);

        self->checking_ = true;

        if (action == TaskAction::CHECK_ONLY) {
            // Fetch version info only — update latest_ver_buf_, do not install.
            char ver[VER_BUF_LEN] = {};
            char url[URL_BUF_LEN] = {};
            if (self->fetch_version_info(ver, sizeof(ver), url, sizeof(url)) && ver[0]) {
                strncpy(self->latest_ver_buf_, ver, VER_BUF_LEN - 1);
                ESP_LOGI(TAG, "Version check: running v%s  latest v%s",
                         running_version(), self->latest_ver_buf_);
            }
        } else if (action == TaskAction::CHECK_AND_INSTALL) {
            self->do_check_and_update();
        } else {
            // PERIODIC: only check+install if auto update is enabled.
            if (self->auto_update_) {
                self->do_check_and_update();
            }
        }

        self->checking_ = false;
    }
}

// ── Version fetch ─────────────────────────────────────────────────────────────

bool OtaManager::fetch_version_info(char* out_ver, size_t ver_len,
                                    char* out_url, size_t url_len)
{
    static constexpr int BUF = 512;
    char* buf = static_cast<char*>(malloc(BUF));
    if (!buf) return false;
    memset(buf, 0, BUF);

    esp_http_client_config_t cfg = {};
    cfg.url               = VERSION_CHECK_URL;
    cfg.timeout_ms        = HTTP_TIMEOUT_MS;
    cfg.method            = HTTP_METHOD_GET;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;

    // HTTPS requires ~50 KB for mbedTLS context; skip if heap is too low.
    if (esp_get_free_heap_size() < 60 * 1024) {
        ESP_LOGW(TAG, "Low heap (%lu B) — skipping version check",
                 (unsigned long)esp_get_free_heap_size());
        free(buf);
        return false;
    }

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) { free(buf); return false; }

    bool ok = false;
    if (esp_http_client_open(client, 0) == ESP_OK) {
        esp_http_client_fetch_headers(client);
        int n = esp_http_client_read(client, buf, BUF - 1);
        buf[n > 0 ? n : 0] = '\0';

        if (esp_http_client_get_status_code(client) == 200 && n > 0) {
            cJSON* root = cJSON_Parse(buf);
            if (root) {
                cJSON* ver = cJSON_GetObjectItemCaseSensitive(root, "version");
                cJSON* url = cJSON_GetObjectItemCaseSensitive(root, "url");
                if (cJSON_IsString(ver) && cJSON_IsString(url)) {
                    strncpy(out_ver, ver->valuestring, ver_len - 1);
                    strncpy(out_url, url->valuestring, url_len - 1);
                    ok = true;
                }
                cJSON_Delete(root);
            } else {
                ESP_LOGE(TAG, "version.json parse failed");
            }
        } else {
            ESP_LOGE(TAG, "version.json HTTP %d",
                     esp_http_client_get_status_code(client));
        }
    } else {
        ESP_LOGE(TAG, "version.json fetch failed (no network?)");
    }

    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    free(buf);
    return ok;
}

// ── Check & update ────────────────────────────────────────────────────────────

esp_err_t OtaManager::do_check_and_update()
{
    char new_ver[VER_BUF_LEN] = {};
    char bin_url[URL_BUF_LEN] = {};

    ESP_LOGI(TAG, "Checking for updates (running v%s)...", running_version());

    if (!fetch_version_info(new_ver, sizeof(new_ver), bin_url, sizeof(bin_url))) {
        ESP_LOGW(TAG, "Version check failed — will retry next cycle");
        return ESP_FAIL;
    }

    // Always update cached latest version.
    strncpy(latest_ver_buf_, new_ver, VER_BUF_LEN - 1);
    ESP_LOGI(TAG, "Latest: v%s  Running: v%s", new_ver, running_version());

    if (strcmp(new_ver, running_version()) == 0) {
        ESP_LOGI(TAG, "Firmware is up to date");
        return ESP_OK;
    }

    // ── New version available — show on display and download ──────────────────
    ESP_LOGI(TAG, "Update available: v%s → v%s", running_version(), new_ver);

    if (display_) {
        char line[80];
        display_->clear();
        display_->print(0, "  OTA Update   ");
        snprintf(line, sizeof(line), "v%s->v%s", running_version(), new_ver);
        display_->print(2, line);
        display_->print(4, "Downloading...");
        display_->print(6, "Do not power off");
    }

    esp_http_client_config_t http_cfg = {};
    http_cfg.url               = bin_url;
    http_cfg.timeout_ms        = 60000;
    http_cfg.crt_bundle_attach = esp_crt_bundle_attach;
    http_cfg.keep_alive_enable = true;
    // GitHub releases redirect to a CDN; the 302 response headers are large.
    // Default buffer (512 B) overflows before the redirect is processed.
    http_cfg.buffer_size       = 4096;
    http_cfg.buffer_size_tx    = 1024;

    esp_https_ota_config_t ota_cfg = {};
    ota_cfg.http_config            = &http_cfg;

    esp_err_t ret = esp_https_ota(&ota_cfg);

    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "OTA download complete — restarting");
        if (display_) {
            display_->clear();
            display_->print(0, "  OTA Complete  ");
            display_->print(3, "  Restarting... ");
        }
        vTaskDelay(pdMS_TO_TICKS(1500));
        esp_restart();
    } else {
        ESP_LOGE(TAG, "OTA failed: %s", esp_err_to_name(ret));
        if (display_) {
            char ver_line[40];
            display_->clear();
            display_->print(0, "  OTA Failed    ");
            display_->print(2, esp_err_to_name(ret));
            display_->print(5, "Still on:");
            snprintf(ver_line, sizeof(ver_line), "  v%s", running_version());
            display_->print(6, ver_line);
            vTaskDelay(pdMS_TO_TICKS(4000));
            display_->clear();
        }
    }

    return ret;
}
