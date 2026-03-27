#pragma once

/**
 * @file ota_manager.h
 * @brief Over-the-air firmware update via GitHub Releases
 *
 * version.json format (hosted on GitHub, updated by CI on each release):
 *   { "version": "1.2.0",
 *     "url": "https://github.com/darylbcarr/lights_project/releases/download/v1.0.0/lights_project.bin" }
 */

#include <functional>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

class Display;

class OtaManager {
public:
    // ── Update this to your repo ──────────────────────────────────────────────
    static constexpr const char* VERSION_CHECK_URL =
        "https://raw.githubusercontent.com/darylbcarr/lights_project/main/version.json";

    /**
     * @brief Launch the background OTA check task.
     * @param display          Used to show download progress on the OLED.
     * @param is_connected_fn  Returns true when WiFi is up and routable.
     */
    void start(Display& display, std::function<bool()> is_connected_fn);

    /**
     * @brief Synchronous check+install (used by the UART console command).
     *        Blocks the caller until the check completes.  If an update is
     *        found, installs and restarts — the call never returns on success.
     */
    esp_err_t check_now();

    /**
     * @brief Async: post a version-only check to the background task.
     *        Updates latest_version() once complete; does NOT install.
     */
    void trigger_check();

    /**
     * @brief Async: post a full check+install to the background task.
     *        Device restarts automatically if an update is found.
     */
    void trigger_update();

    /** @brief Firmware version currently running (from PROJECT_VER). */
    static const char* running_version();

    /** @brief Latest version fetched from version.json; empty if never checked. */
    const char* latest_version() const { return latest_ver_buf_; }

    /** @brief True if latest_version differs from running_version (and is non-empty). */
    bool is_update_available() const;

    bool is_auto_update_enabled() const { return auto_update_; }

    /** @brief Toggle auto-update; persisted to NVS immediately. */
    void set_auto_update(bool en);

    /** @brief True while the background task is performing a version fetch or OTA download. */
    bool is_checking() const { return checking_; }

private:
    // ── Constants (declared first so array sizes below can reference them) ───
    static constexpr int  BOOT_DELAY_S     = 60;
    static constexpr int  CHECK_INTERVAL_S = 24 * 3600;
    static constexpr int  HTTP_TIMEOUT_MS  = 15000;
    static constexpr int  VER_BUF_LEN      = 32;
    static constexpr int  URL_BUF_LEN      = 256;

    enum class TaskAction : uint8_t { PERIODIC, CHECK_ONLY, CHECK_AND_INSTALL };

    static void  ota_task(void* arg);
    esp_err_t    do_check_and_update();
    bool         fetch_version_info(char* out_ver,  size_t ver_len,
                                    char* out_url,  size_t url_len);
    void         load_settings();
    void         save_settings();

    Display*              display_         = nullptr;
    std::function<bool()> is_connected_fn_ = nullptr;
    QueueHandle_t         action_queue_    = nullptr;

    char          latest_ver_buf_[VER_BUF_LEN] = {};
    bool          auto_update_  = true;
    volatile bool checking_     = false;
};
