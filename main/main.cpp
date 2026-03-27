/**
 * @file main.cpp
 * @brief Application entry point — LED controller with display + button menu
 *
 * Boot sequence
 * ─────────────
 *  1. Construct all objects (Networking, Display, Menu).
 *  2. Display: init I2C bus, show splash.
 *  3. Menu: wire dismiss function + build full tree.
 *  4. Networking: WiFi + SNTP chain (async).
 *  5. UART console: start shell task.
 *  6. Button poll task: 50 Hz A/B buttons → menu navigation + blank wake.
 *  7. Blank timer task: tick once per second, enforce 5-minute display timeout.
 *
 * I2C bus
 * ───────
 *  Display owns the I2C bus handle (GPIO8/9) and exposes a FreeRTOS mutex via
 *  getBusMutex().  All bus access must take this mutex for the transaction.
 *
 * Dismiss / info-screen blocking
 * ───────────────────────────────
 *  Menu callbacks that show info screens call wait_for_dismiss() which spins
 *  on dismiss_fn_.  Because those callbacks run ON the button_poll task stack
 *  (via select() → execute() → lambda), button_poll is blocked while an
 *  info screen is showing.  dismiss_fn_ therefore polls the GPIO buttons
 *  directly rather than depending on button_poll to set a flag.
 *
 * Hardware
 * ────────
 *  LED:      GPIO13 (330Ω)
 *  I2C bus:  SDA=GPIO8  SCL=GPIO9
 *  Display:  SSD1306 @ 0x3C  (new i2c_master driver)
 *
 * Button gestures
 * ───────────────
 *  Short press A   → menu previous
 *  Short press B   → menu next
 *  Long press A    → select / enter submenu
 *  A + B           → back to parent menu / dismiss info screen
 *  Any press       → wake display if blanked
 */

#include <cstdio>
#include <string>
#include "esp_log.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/gpio.h"
#include "esp_wifi.h"
#include "nvs.h"

#include "networking.h"
#include "console_commands.h"
#include "display.h"
#include "menu.h"
#include "webserver.h"
#include "led_manager.h"
#include "config_store.h"
#include "matter_bridge.h"
#include "ota_manager.h"
#include "esp_ota_ops.h"

static const char* TAG = "main";

// ── Configuration defaults (used when NVS has no saved value) ─────────────────

static constexpr const char* WIFI_SSID_DEFAULT     = "";
static constexpr const char* WIFI_PASSWORD_DEFAULT = "";
// static constexpr const char* TZ_OVERRIDE_DEFAULT = "CST6CDT,M3.2.0,M11.1.0";
static constexpr const char* TZ_OVERRIDE_DEFAULT   = "";

static constexpr gpio_num_t  I2C_SDA             = GPIO_NUM_8;
static constexpr gpio_num_t  I2C_SCL             = GPIO_NUM_9;
static constexpr uint32_t    LONG_PRESS_MS       = 800;
static constexpr gpio_num_t  BUTTON_A            = GPIO_NUM_10;  // menu previous
static constexpr gpio_num_t  BUTTON_B            = GPIO_NUM_11;  // menu next

// ── Shared system objects (static lifetime) ───────────────────────────────────

static Networking    s_net;
static Display       s_display;
static Menu          s_menu(s_display);
static LedManager    s_leds(GPIO_NUM_1, GPIO_NUM_2, 350);
static MatterBridge  s_matter(s_leds);
static WebServer     s_webserver(s_net, s_leds);
static OtaManager    s_ota;

static void clear_wifi_credentials();  // defined before app_main


// ── dismiss_fn implementation ─────────────────────────────────────────────────
// Called by Menu::wait_for_dismiss() at 50ms intervals.
// Runs on the button_poll task stack — polls GPIO buttons directly.

// ── input_poll_fn implementation ──────────────────────────────────────────────
// Called by Menu::show_text_input() at 50 ms intervals.
// Reads all button states.
static Menu::InputEvent input_poll_fn()
{
    Menu::InputEvent ev = {};
    ev.btnA = (gpio_get_level(BUTTON_A) == 0);
    ev.btnB = (gpio_get_level(BUTTON_B) == 0);

    // ── Panic WiFi reset during blocking setup screens ────────────────────
    // Uses wall-clock time (tick count) so the 5 s threshold is accurate
    // even when input_poll_fn is not called at a fixed rate — e.g. the
    // 1.8 s hint delay in show_text_input, or the 500 ms debounce in the
    // Matter pairing screen, both create gaps where no calls happen.
    //
    // A+B held 5 s → clear WiFi + restart.
    static TickType_t s_panic_start = 0;   // tick when hold began; 0 = not held
    static bool       s_panic_shown = false;

    bool both = ev.btnA && ev.btnB;
    bool held = both;

    if (held) {
        if (s_panic_start == 0)
            s_panic_start = xTaskGetTickCount();
        uint32_t elapsed_ms = (xTaskGetTickCount() - s_panic_start) * portTICK_PERIOD_MS;
        if (elapsed_ms >= 5000) {
            s_display.clear();
            s_display.print(0, "WiFi Reset!");
            s_display.print(2, "Restarting...");
            vTaskDelay(pdMS_TO_TICKS(1500));
            clear_wifi_credentials();
            esp_restart();
        } else if (elapsed_ms >= 3000 && !s_panic_shown) {
            s_panic_shown = true;
            s_display.clear();
            s_display.print(0, "WiFi Reset?");
            s_display.print(2, "Keep holding...");
            s_display.print(4, "Release:cancel");
        }
    } else {
        s_panic_start = 0;
        s_panic_shown = false;
    }

    return ev;
}

// Returns true while both hardware buttons are held (dismiss gesture).
// wait_for_dismiss() in Menu accumulates hold duration and fires after 800ms.
static bool dismiss_fn()
{
    return (gpio_get_level(BUTTON_A) == 0) && (gpio_get_level(BUTTON_B) == 0);
}


// ── Button poll task ──────────────────────────────────────────────────────────
// Runs at 50 Hz. Handles buttons A and B for menu navigation.
// Calls s_menu.wake() on any activity so the blank timer resets.

static void encoder_task(void* /*arg*/)
{
    bool     btnA_last        = false;
    bool     btnB_last        = false;
    uint32_t btnA_press_tick  = 0;
    uint32_t btnB_press_tick  = 0;
    bool     btnA_long_fired  = false;
    bool     btnB_long_fired  = false;
    bool     both_last        = false;
    int      h_scroll_counter = 0;
    uint32_t panic_hold_ms    = 0;
    bool     panic_shown      = false;

    while (true) {
        // ── Read hardware buttons (active-low, pull-up) ───────────────────────
        bool btnA = (gpio_get_level(BUTTON_A) == 0);
        bool btnB = (gpio_get_level(BUTTON_B) == 0);

        bool btnA_fall = (btnA  && !btnA_last);   // press edge
        bool btnA_rise = (!btnA &&  btnA_last);   // release edge
        bool btnB_fall = (btnB  && !btnB_last);
        bool btnB_rise = (!btnB &&  btnB_last);
        bool both      = btnA && btnB;
        bool both_fall = both && !both_last;

        // ── Panic WiFi reset (A+B held ≥ 5 s, works even when display blanked) ──
        if (both) {
            panic_hold_ms += 20;
            if (panic_hold_ms >= 5000) {
                s_display.clear();
                s_display.print(0, "WiFi Reset!");
                s_display.print(2, "Restarting...");
                vTaskDelay(pdMS_TO_TICKS(1500));
                clear_wifi_credentials();
                esp_restart();
            } else if (panic_hold_ms >= 3000 && !panic_shown) {
                panic_shown = true;
                s_display.clear();
                s_display.print(0, "WiFi Reset?");
                s_display.print(2, "Keep holding...");
                s_display.print(4, "Release:cancel");
            }
        } else {
            if (panic_shown) {
                panic_shown = false;
                s_menu.render();
            }
            panic_hold_ms = 0;
        }

        // ── Wake-up suppression ───────────────────────────────────────────────
        // Any input wakes the display; the triggering gesture is consumed.
        if ((btnA || btnB) && s_menu.is_blanked()) {
            s_menu.wake();
            btnA_last        = btnA;
            btnB_last        = btnB;
            btnA_long_fired  = true;   // suppress GPIO press cycle until release
            btnB_long_fired  = true;
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        // ── Hardware buttons ──────────────────────────────────────────────────
        // Short press A → previous,  Short press B → next
        // Long press A or B (≥800ms) → select
        // Both pressed simultaneously → reserved for dismiss_fn (info screens)
        if (both) {
            // Suppress individual actions while both are held so that releasing
            // to a single button doesn't trigger that button's short/long press.
            btnA_long_fired = true;
            btnB_long_fired = true;
            // Falling edge → navigate back (also serves as dismiss in info screens)
            if (both_fall && !s_menu.is_blanked()) {
                s_menu.back();
                s_menu.render_scrolled(false);
                h_scroll_counter = 0;
            }
        } else {
            // ── Falling edges (press start) ───────────────────────────────────
            if (btnA_fall) {
                btnA_press_tick = xTaskGetTickCount();
                btnA_long_fired = false;
                s_menu.wake();
            }
            if (btnB_fall) {
                btnB_press_tick = xTaskGetTickCount();
                btnB_long_fired = false;
                s_menu.wake();
            }
            // ── Long-press detection (while held) ─────────────────────────────
            if (btnA && !btnA_long_fired) {
                uint32_t held = (xTaskGetTickCount() - btnA_press_tick) * portTICK_PERIOD_MS;
                if (held >= LONG_PRESS_MS) {
                    btnA_long_fired = true;
                    if (!s_menu.is_blanked()) {
                        s_menu.select();
                        s_menu.render();
                        h_scroll_counter = 0;
                    }
                }
            }
            // B long press has no action (B is next-only; only A long = select)
            // ── Rising edges (release) → short press if no long press fired ──
            if (btnA_rise && !btnA_long_fired) {
                s_menu.wake();
                s_menu.previous();
                s_menu.render_scrolled(false);
                h_scroll_counter = 0;
            }
            if (btnB_rise && !btnB_long_fired) {
                s_menu.wake();
                s_menu.next();
                s_menu.render_scrolled(true);
                h_scroll_counter = 0;
            }
        }

        btnA_last = btnA;
        btnB_last = btnB;
        both_last = both;

        // ── Horizontal scroll tick (every ~300 ms) ────────────────────────────
        if (++h_scroll_counter >= 15) {
            h_scroll_counter = 0;
            s_menu.tick_h_scroll();
        }

        vTaskDelay(pdMS_TO_TICKS(20));   // 50 Hz
    }
}

// ── Blank timer task ──────────────────────────────────────────────────────────

static void blank_timer_task(void* /*arg*/)
{
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        s_menu.tick_blank_timer();
    }
}

// ── clear_matter_commissioning_data ───────────────────────────────────────────
// Erases CHIP's fabric + config data from NVS so the next start() treats the
// device as uncommissioned and enables BLE advertising for fresh pairing.
// Preserves the unique discriminator stored in the separate "fctry" partition.
static void clear_matter_commissioning_data()
{
    // "CHIP_KVS" is the KeyValueStoreManagerImpl namespace (uppercase, exact).
    // "chip-config" holds factory/device-config keys in the default NVS partition.
    const char* chip_ns[] = { "CHIP_KVS", "chip-config" };
    for (const char* ns : chip_ns) {
        nvs_handle_t h;
        if (nvs_open(ns, NVS_READWRITE, &h) == ESP_OK) {
            nvs_erase_all(h);
            nvs_commit(h);
            nvs_close(h);
        }
    }
    ESP_LOGI(TAG, "Cleared Matter commissioning data (CHIP_KVS, chip-config)");
}

// ── clear_wifi_credentials ────────────────────────────────────────────────────
// Full factory reset: wipes WiFi credentials AND Matter commissioning data.
//   1. App NVS (ConfigStore / clk_cfg namespace) — read by this firmware.
//   2. WiFi driver NVS (esp_wifi_set_config)     — read by CHIP/Matter.
//   3. CHIP commissioning data (chip-kvs, chip-config) — fabrics, ACLs, etc.
// All three must be cleared; wiping only some leaves stale state that causes
// CHIP to disable BLE advertising on the next boot.
static void clear_wifi_credentials()
{
    NetCfg empty = {};
    ConfigStore::load(empty);
    empty.ssid[0]          = '\0';
    empty.password[0]      = '\0';
    empty.wifi_only         = false;
    empty.matter_commissioned = false;
    ConfigStore::save(empty);

    wifi_config_t wcfg = {};
    esp_wifi_set_config(WIFI_IF_STA, &wcfg);   // clears driver NVS copy

    clear_matter_commissioning_data();
}

// ── update_wifi_credentials_from_menu ─────────────────────────────────────────
// Called from the WiFi restart loop when the user presses a button.
// Presents the credential-entry screen (same as first-time WiFi setup).
// On confirm: saves to our NVS and updates the WiFi driver NVS so CHIP also
//   picks up the new credentials via esp_wifi_get_config() on next boot.
// Returns false if the user cancelled (restart loop should continue).
// On save: calls esp_restart() directly — never returns true.
static bool update_wifi_credentials_from_menu()
{
    std::string new_ssid, new_pw;
    if (!s_menu.show_wifi_credentials(new_ssid, new_pw) || new_ssid.empty()) {
        return false;   // cancelled
    }

    // Save to our app NVS (used by Networking on WiFi-only path)
    NetCfg nc = {};
    ConfigStore::load(nc);
    snprintf(nc.ssid,     sizeof(nc.ssid),     "%s", new_ssid.c_str());
    snprintf(nc.password, sizeof(nc.password), "%s", new_pw.c_str());
    ConfigStore::save(nc);

    // Update WiFi driver NVS — CHIP's ESPWiFiDriver::Init() reads this on boot
    // via esp_wifi_get_config(), so both WiFi-only and Matter paths use the
    // new credentials without needing to touch CHIP_KVS fabric data.
    wifi_config_t wcfg = {};
    strncpy((char*)wcfg.sta.ssid,     new_ssid.c_str(), sizeof(wcfg.sta.ssid)     - 1);
    strncpy((char*)wcfg.sta.password, new_pw.c_str(),   sizeof(wcfg.sta.password) - 1);
    wcfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    esp_wifi_set_config(WIFI_IF_STA, &wcfg);

    s_display.clear();
    s_display.print(0, "WiFi updated.");
    s_display.print(2, "Restarting...");
    vTaskDelay(pdMS_TO_TICKS(1500));
    esp_restart();
    return true;  // unreachable
}

// ── app_main ──────────────────────────────────────────────────────────────────

extern "C" void app_main()
{
    ESP_LOGI(TAG, "═══════════════════════════════════════════");
    ESP_LOGI(TAG, "  Analog Clock Driver  — ESP32-S3  v%s", OtaManager::running_version());
    ESP_LOGI(TAG, "═══════════════════════════════════════════");

    // Silence httpd warnings from Alexa/Hue-bridge probes hitting unknown URIs.
    esp_log_level_set("httpd_uri",  ESP_LOG_ERROR);
    esp_log_level_set("httpd_txrx", ESP_LOG_ERROR);

    // ── OTA rollback guard ────────────────────────────────────────────────────
    // If rollback is enabled (CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y), the
    // bootloader marks a freshly-flashed OTA image as "pending verify".
    // Calling this confirms the new firmware is good; without it a subsequent
    // reboot would roll back to the previous slot.
    esp_ota_mark_app_valid_cancel_rollback();

    // ── 0. NVS flash init (must be first; Networking::begin() reuses it) ─────
    {
        esp_err_t ret = nvs_flash_init();
        if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
            ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
            ESP_LOGW(TAG, "NVS partition issue — erasing and reinitialising");
            ESP_ERROR_CHECK(nvs_flash_erase());
            ret = nvs_flash_init();
        }
        ESP_ERROR_CHECK(ret);
        ConfigStore::init();
    }

    // ── 0a. Load persisted configuration ────────────────────────────────────
    LedCfg    ledCfg;
    NetCfg    netCfg;
    ConfigStore::load(ledCfg);
    ConfigStore::load(netCfg);

    // Per-strip hardware defaults for new installations (len==0 means never saved).
    if (ledCfg.strip[0].len == 0) ledCfg.strip[0].len = 30;  // Left
    if (ledCfg.strip[1].len == 0) ledCfg.strip[1].len = 30;  // Right

    // Resolve WiFi credentials (NVS → fallback to compile-time defaults)
    const char* wifi_ssid = (netCfg.ssid[0] != '\0') ? netCfg.ssid : WIFI_SSID_DEFAULT;
    const char* wifi_pass = (netCfg.password[0] != '\0') ? netCfg.password : WIFI_PASSWORD_DEFAULT;
    const char* tz_override = (netCfg.tz_override[0] != '\0') ? netCfg.tz_override : TZ_OVERRIDE_DEFAULT;
    ESP_LOGI(TAG, "WiFi SSID: %s (source: %s)", wifi_ssid,
             netCfg.ssid[0] ? "NVS" : "default");

    // ── 1a. Hardware buttons — configured FIRST so A+B failsafe is always live ─
    // Must happen before any code that could fail or stall, so that the panic
    // WiFi-reset (A+B held ≥ 5 s in button_poll / input_poll_fn) works even
    // when the display is absent.
    {
        const gpio_config_t btn_cfg = {
            .pin_bit_mask = (1ULL << BUTTON_A) | (1ULL << BUTTON_B),
            .mode         = GPIO_MODE_INPUT,
            .pull_up_en   = GPIO_PULLUP_ENABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type    = GPIO_INTR_DISABLE,
        };
        gpio_config(&btn_cfg);
        ESP_LOGI(TAG, "Buttons: A=GPIO%d  B=GPIO%d  (configured early)", (int)BUTTON_A, (int)BUTTON_B);
    }

    // ── 1b. Display — inits I2C bus, exposes bus handle + mutex ──────────────
    // Display::init() retries SSD1306 detection up to 3× internally.
    // If the display is absent all s_display methods are no-ops, so boot
    // continues normally — networking and failsafe still work.
    ESP_LOGI(TAG, "Initialising display...");
    if (!s_display.init(I2C_NUM_0, 0x3C, (int)I2C_SDA, (int)I2C_SCL)) {
        ESP_LOGW(TAG, "Display not found — continuing headless (display ops are no-ops)");
    } else {
        s_display.clear();
        s_display.print(1, " LED Controller");
        s_display.print(2, " Initializing");
        s_display.print(4, " Please wait...");
    }

    // ── 2. LED strips ────────────────────────────────────────────────────────
    ESP_LOGI(TAG, "Initialising LED strips...");
    if (s_leds.init() != ESP_OK) {
        ESP_LOGW(TAG, "LED strip init failed — continuing without LEDs");
    }
    s_leds.start();

    // ── 2c. Matter bridge — init after LED strips are running ─────────────────
    if (s_matter.init() != ESP_OK) {
        ESP_LOGW(TAG, "Matter init failed — continuing without Matter");
    }

    // Apply persisted LED config (after start so the effect task is running)
    for (int i = 0; i < LedManager::STRIP_COUNT; i++) {
        LedManager::Target tgt = (i == 0) ? LedManager::Target::STRIP_1
                                           : LedManager::Target::STRIP_2;
        s_leds.set_active_len(tgt, ledCfg.strip[i].len);
        s_leds.set_brightness(tgt, ledCfg.strip[i].brightness);
        s_leds.set_color(tgt, ledCfg.strip[i].r, ledCfg.strip[i].g, ledCfg.strip[i].b);
        s_leds.set_effect(tgt, static_cast<LedManager::Effect>(ledCfg.strip[i].effect));
    }

    // ── 3. Menu — wire dismiss, then build full tree ──────────────────────────
    s_menu.set_dismiss_fn(dismiss_fn);
    s_menu.set_input_fn(input_poll_fn);
    s_menu.set_encoder_ok(false);
    s_menu.set_ota(&s_ota);
    ESP_LOGI(TAG, "Building menu...");
    s_menu.build(s_net, s_leds);

    // ── 4. Matter stack — init already done; populate pairing info before setup ─
    // ensure_unique_discriminator() runs inside start(), so the real discriminator
    // is only in NVS after start() completes.  We pre-populate with the NVS
    // value here (may still be 3840 on first boot) and refresh after start().
    auto refresh_matter_pairing_info = [&]() {
        auto info = s_matter.get_commissioning_info();
        s_menu.set_matter_pairing_info(info.pin_code, info.discriminator,
                                       s_matter.manual_code());
    };
    refresh_matter_pairing_info();

    // ── 4b. UART console — start early so it's available during setup screens ──
    // Networking and OTA are not yet started; pointers are valid but commands
    // that need connectivity will return "not available" / "WiFi not connected"
    // until the setup flow completes and networking begins.
    console_start(&s_net,
                  &s_matter,
                  &s_ota,
                  s_display.getBusMutex(),
                  s_display.getBusHandle());

    // ── 4c–4e. First-run setup loop ───────────────────────────────────────────
    // Skipped when SSID is already stored or device is Matter-commissioned.
    // Loops so the user can go back from the Matter pairing screen to the
    // choice screen (encoder short press / single A or B on pairing screen).
    // Matter is started at most once; the pairing screen stays until confirmed.
    // Check NVS before starting the Matter stack.  Passed to Networking so it
    // can decide whether mDNS is safe to start on the Matter WiFi path:
    //   false → first-time commissioning, BLE is active → suppress mDNS
    //   true  → returning device, BLE is inactive on this boot → allow mDNS
    // wifi_only: set during first-time setup when the user chose WiFi over Matter.
    // When true, the Matter stack (BLE, CHIP) is never started on any boot,
    // avoiding all radio coexistence issues and freeing ~150 KB of heap.
    // Cleared automatically when WiFi credentials are erased (A+B factory reset).
    const bool wifi_only = netCfg.wifi_only;

    const bool matter_commissioned = wifi_only ? false : netCfg.matter_commissioned;
    s_net.set_matter_commissioned(matter_commissioned);

    bool did_first_time_setup = false;
    bool matter_started       = false;
    if (netCfg.ssid[0] == '\0' && !matter_commissioned) {
        bool setup_done = false;

        while (!setup_done) {
            auto result = s_menu.first_time_setup();

            if (result == Menu::SetupResult::WiFiSaved) {
                ConfigStore::load(netCfg);
                // Persist the WiFi-only choice so Matter never starts on subsequent boots.
                netCfg.wifi_only = true;
                ConfigStore::save(netCfg);
                wifi_ssid = (netCfg.ssid[0] != '\0') ? netCfg.ssid : WIFI_SSID_DEFAULT;
                wifi_pass = (netCfg.password[0] != '\0') ? netCfg.password : WIFI_PASSWORD_DEFAULT;
                did_first_time_setup = true;
                setup_done = true;

            } else {
                // MatterChosen — start Matter once, then show pairing screen.
                // User can press back (enc short press / single A or B) to
                // return to the choice screen.
                if (!matter_started) {
                    // Erase stale fabric data before start() so CHIP always
                    // begins BLE advertising for fresh pairing.  Without this,
                    // a prior commissioning (or old NVS from a previous flash)
                    // causes CHIP to find FabricCount > 0, disable BLE
                    // immediately, and print "Fabric already commissioned."
                    clear_matter_commissioning_data();
                    s_matter.start(true);  // fresh_commissioning=true: apply BLE coex fix
                    matter_started = true;
                    refresh_matter_pairing_info();  // discriminator now correct

                    // Register networking event handlers and ensure WiFi STA is
                    // started BEFORE the pairing screen blocks.  CHIP's BLE
                    // commissioning flow sends WiFi credentials and immediately
                    // tries to connect — if our event handlers are not registered
                    // yet we miss the IP event, and if WiFi STA isn't explicitly
                    // started we can hit "Haven't to connect to a suitable AP now!".
                    s_net.set_wifi_credentials(wifi_ssid, wifi_pass);
                    if (tz_override[0] != '\0')
                        s_net.set_timezone_override(tz_override);
                    if (netCfg.mdns_hostname[0] != '\0')
                        s_net.set_mdns_hostname_hint(netCfg.mdns_hostname);
                    s_net.begin();   // subsequent call after the loop is a no-op
                }
                bool confirmed = s_menu.show_matter_pairing_standalone(
                    [&]() { return s_matter.is_commissioned(); });
                if (confirmed) {
                    // Persist our own commissioning flag so the next boot
                    // skips first-time setup without relying on CHIP_KVS state.
                    netCfg.matter_commissioned = true;
                    ConfigStore::save(netCfg);
                    setup_done = true;
                }
                // else: loop back to first_time_setup choice screen
            }
        }
    }

    // ── 4c. Start Matter stack (returning devices only) ──────────────────────
    // Skipped entirely when wifi_only=true (user chose WiFi at first-time setup).
    // For first-time WiFi setup, Matter start is deferred until after WiFi
    // connects (section 5a).  Here we start it early so the LED endpoints are
    // available if the device is already commissioned.
    // If the device has no fabric, Matter will start BLE advertising; section 5b
    // disables it once WiFi connects so BLE does not interfere with the radio.
    if (!wifi_only && !matter_started && !did_first_time_setup) {
        s_matter.start();
        refresh_matter_pairing_info();  // discriminator now correct
        matter_started = true;
    }

    // ── 5. Networking — async WiFi + SNTP + geolocation ──────────────────────
    s_net.set_wifi_credentials(wifi_ssid, wifi_pass);
    if (tz_override[0] != '\0') {
        s_net.set_timezone_override(tz_override);
    }
    if (netCfg.mdns_hostname[0] != '\0') {
        s_net.set_mdns_hostname_hint(netCfg.mdns_hostname);
    }
    s_net.begin();

    // ── 5a. After first-time WiFi setup, verify connection succeeds ───────────
    // If the entered credentials are wrong the user sees an error and the device
    // restarts back into first-time setup automatically (NVS credentials are
    // cleared before restarting so the setup screen shows again).
    if (did_first_time_setup && wifi_ssid[0] != '\0') {
        s_display.clear();
        s_display.print(0, "Connecting...");
        s_display.print(2, wifi_ssid);

        bool connected = false;
        for (int i = 0; i < 30 && !connected; i++) {   // up to 15 s
            vTaskDelay(pdMS_TO_TICKS(500));
            connected = s_net.is_connected();
        }

        if (!connected) {
            s_display.clear();
            s_display.print(0, "Connect failed!");
            s_display.print(2, "Check SSID &");
            s_display.print(3, "password.");
            s_display.print(5, "Restarting...");
            vTaskDelay(pdMS_TO_TICKS(3000));

            clear_wifi_credentials();
            esp_restart();
        }

        // WiFi connected — now safe to start Matter (BLE coex no longer a risk
        // since WiFi is already associated and the handshake is complete).
        // Skipped entirely on the wifi_only path (Matter was never wanted).
        if (!wifi_only && !matter_started) {
            s_matter.start();
            refresh_matter_pairing_info();
            matter_started = true;
            // Stop BLE if no fabric — prevents radio contention.
            if (!s_matter.is_commissioned()) {
                s_matter.disable_ble_advertising();
            }
        }
    }

    // ── 5b. For returning devices — wait for WiFi or restart loop ────────────
    // Runs when credentials exist (WiFi-only) or Matter is commissioned.
    // Initial 15 s fast poll, then a 10-minute restart loop on failure:
    //   A+B held 2 s → full factory reset (clears credentials + Matter fabric)
    //   Single button / encoder → re-enter WiFi credentials (preserves fabric)
    //   10-minute timeout → esp_restart() to retry on next boot
    {
        bool has_network_config = (wifi_ssid[0] != '\0') || matter_commissioned;
        if (!did_first_time_setup && has_network_config) {
            // Determine SSID to display (WiFi-only has it; Matter reads driver NVS)
            char display_ssid[33] = {};
            if (wifi_ssid[0] != '\0') {
                strncpy(display_ssid, wifi_ssid, sizeof(display_ssid) - 1);
            } else {
                wifi_config_t wc = {};
                if (esp_wifi_get_config(WIFI_IF_STA, &wc) == ESP_OK && wc.sta.ssid[0] != 0)
                    strncpy(display_ssid, (char*)wc.sta.ssid, sizeof(display_ssid) - 1);
                else
                    strncpy(display_ssid, "Matter network", sizeof(display_ssid) - 1);
            }

            s_display.clear();
            s_display.print(0, "Connecting...");
            s_display.print(2, display_ssid);

            bool connected = false;
            for (int i = 0; i < 30 && !connected; i++) {   // up to 15 s
                vTaskDelay(pdMS_TO_TICKS(500));
                connected = s_net.is_connected();
            }

            if (!wifi_only && connected && !s_matter.is_commissioned()) {
                // WiFi is up but no fabric — stop BLE advertising to avoid
                // radio contention with WebSocket traffic.
                s_matter.disable_ble_advertising();
            }

            if (!connected) {
                // ── Restart loop: poll up to 10 minutes, then restart ─────────────
                static constexpr int RESTART_TIMEOUT_S  = 600;
                static constexpr int POLL_INTERVAL_MS   = 50;
                static constexpr int DISPLAY_REFRESH_MS = 5000;

                int  elapsed_ms      = 0;
                int  next_refresh_ms = 0;   // 0 = render immediately
                uint32_t ab_hold_ms  = 0;
                bool btnA_prev = false, btnB_prev = false;

                while (elapsed_ms < RESTART_TIMEOUT_S * 1000) {

                    if (next_refresh_ms <= 0) {
                        int rem_s = (RESTART_TIMEOUT_S * 1000 - elapsed_ms) / 1000;
                        char line[20];
                        s_display.clear();
                        s_display.print(0, "WiFi not found");
                        snprintf(line, sizeof(line), "%.15s", display_ssid);
                        s_display.print(1, line);
                        s_display.print(3, "A+B: Full reset");
                        s_display.print(4, "Btn: New WiFi");
                        snprintf(line, sizeof(line), "Restart: %dm%02ds",
                                 rem_s / 60, rem_s % 60);
                        s_display.print(6, line);
                        next_refresh_ms = DISPLAY_REFRESH_MS;
                    }

                    vTaskDelay(pdMS_TO_TICKS(POLL_INTERVAL_MS));
                    elapsed_ms      += POLL_INTERVAL_MS;
                    next_refresh_ms -= POLL_INTERVAL_MS;

                    if (s_net.is_connected()) { connected = true; break; }

                    // ── Button polling ────────────────────────────────────────────
                    bool btnA = (gpio_get_level(BUTTON_A) == 0);
                    bool btnB = (gpio_get_level(BUTTON_B) == 0);
                    bool both = btnA && btnB;

                    if (both) {
                        ab_hold_ms += POLL_INTERVAL_MS;
                        if (ab_hold_ms >= 2000) {
                            clear_wifi_credentials();
                            esp_restart();
                        }
                    } else {
                        ab_hold_ms = 0;
                        // Falling edge on A or B → re-enter WiFi credentials
                        if ((btnA && !btnA_prev) || (btnB && !btnB_prev)) {
                            if (!update_wifi_credentials_from_menu())
                                next_refresh_ms = 0;  // cancelled: force re-render
                        }
                        btnA_prev = btnA;
                        btnB_prev = btnB;
                    }

                }

                if (!connected) {
                    // 10-minute timeout — restart and try again
                    s_display.clear();
                    s_display.print(0, "WiFi timeout.");
                    s_display.print(2, "Restarting...");
                    vTaskDelay(pdMS_TO_TICKS(1000));
                    esp_restart();
                }
            }

            if (!wifi_only && connected && !s_matter.is_commissioned()) {
                s_matter.disable_ble_advertising();
            }
        }
    }

    // ── 4a. Web server — starts HTTP + WebSocket on port 80 ──────────────────
    s_webserver.set_ota(&s_ota);
    s_webserver.start();

    // ── 4b. OTA update checker — background task, waits for WiFi then polls ──
    // Skip during Matter first-time commissioning: BLE + WiFi + Matter already
    // consume nearly all available heap (~8 KB free).  The 12 KB OTA stack would
    // push PacketBuffer allocations over the edge, causing Matter CASE failures.
    // OTA starts normally on every subsequent reboot (matter_commissioned=true).
    if (!did_first_time_setup || wifi_only) {
        s_ota.start(s_display, [&]() { return s_net.is_connected(); });
    } else {
        ESP_LOGI(TAG, "OTA skipped during first-time Matter commissioning (heap conservation)");
    }

    // ── 6. Button poll task (priority 4) ─────────────────────────────────────
    xTaskCreate(encoder_task,     "button_poll",  4096, nullptr, 4, nullptr);

    // ── 7. Blank timer task (priority 2) ─────────────────────────────────────
    xTaskCreate(blank_timer_task, "blank_timer",  3072, nullptr, 2, nullptr);

    // ── 8. Splash → menu ─────────────────────────────────────────────────────
    s_display.clear();
    s_display.print(0, " LED Controller");
    s_display.print(2, " A/B: navigate");
    s_display.print(3, " Long A: select");
    s_display.print(4, " A+B: back");
    vTaskDelay(pdMS_TO_TICKS(2500));

    s_menu.render();

    ESP_LOGI(TAG, "System running.");
    ESP_LOGI(TAG, "UART console at 115200 baud — type 'help'");
    ESP_LOGI(TAG, "Display blanks after 5 min inactivity.");
}
