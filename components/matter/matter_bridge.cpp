/**
 * @file matter_bridge.cpp
 * @brief Matter bridge implementation — single Extended Color Light that
 *        controls both WS2812B strips together as one unified light.
 */

#include "matter_bridge.h"
#include "event_log.h"
#include <esp_log.h>
#include <esp_matter_core.h>
#include <app/server/Server.h>
#include <app/server/CommissioningWindowManager.h>
#include <setup_payload/ManualSetupPayloadGenerator.h>
#include <setup_payload/QRCodeSetupPayloadGenerator.h>
#include <setup_payload/SetupPayload.h>
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_efuse.h"
#include "esp_efuse_table.h"
#include "esp_wifi.h"
#include "host/ble_gap.h"

// ── Cluster and attribute ID constants ────────────────────────────────────────
// ZCL / Matter cluster IDs
static constexpr uint32_t CLUSTER_ON_OFF = 0x0006;
static constexpr uint32_t CLUSTER_LEVEL  = 0x0008;
static constexpr uint32_t CLUSTER_COLOR  = 0x0300;

// OnOff attribute
static constexpr uint32_t ATTR_ON_OFF    = 0x0000;  ///< OnOff::OnOff

// LevelControl attribute
static constexpr uint32_t ATTR_LEVEL_CUR = 0x0000;  ///< LevelControl::CurrentLevel

// ColorControl attributes
static constexpr uint32_t ATTR_HUE       = 0x0000;  ///< ColorControl::CurrentHue
static constexpr uint32_t ATTR_SAT       = 0x0001;  ///< ColorControl::CurrentSaturation
static constexpr uint32_t ATTR_CUR_X     = 0x0003;  ///< ColorControl::CurrentX  (XY mode)
static constexpr uint32_t ATTR_CUR_Y     = 0x0004;  ///< ColorControl::CurrentY  (XY mode)
static constexpr uint32_t ATTR_COLOR_TEMP= 0x0007;  ///< ColorControl::ColorTemperatureMireds
static constexpr uint32_t ATTR_COLOR_MODE= 0x0008;  ///< ColorControl::ColorMode

static const char* TAG = "MatterBridge";

MatterBridge* MatterBridge::s_instance_ = nullptr;

// ── Constructor ───────────────────────────────────────────────────────────────

MatterBridge::MatterBridge(LedManager& leds)
    : leds_(leds)
{
    s_instance_ = this;
}

// ── init ──────────────────────────────────────────────────────────────────────

esp_err_t MatterBridge::init()
{
    ESP_LOGI(TAG, "Initialising Matter node (single Extended Color Light)");

    // ── Node (Root endpoint 0) ─────────────────────────────────────────────
    esp_matter::node::config_t node_cfg = {};
    esp_matter::node_t* node = esp_matter::node::create(
        &node_cfg, attr_cb, identify_cb);

    if (!node) {
        ESP_LOGE(TAG, "Failed to create Matter node");
        return ESP_FAIL;
    }

    // ── Endpoint 1: unified light (controls both strips) ──────────────────
    {
        esp_matter::endpoint::extended_color_light::config_t cfg = {};
        cfg.on_off.on_off                                  = ep_.on;
        cfg.level_control.current_level                    = ep_.level;
        cfg.level_control_lighting.start_up_current_level  = ep_.level;
        cfg.color_control.color_mode                       = 0x00;  // HS mode default
        cfg.color_control.enhanced_color_mode              = 0x00;

        // Pass `this` as priv_data — no per-strip index needed for a single endpoint.
        esp_matter::endpoint_t* ep = esp_matter::endpoint::extended_color_light::create(
            node, &cfg, esp_matter::ENDPOINT_FLAG_NONE, static_cast<void*>(this));

        if (!ep) {
            ESP_LOGE(TAG, "Failed to create light endpoint");
            return ESP_FAIL;
        }
        ep_.id = esp_matter::endpoint::get_id(ep);
        ESP_LOGI(TAG, "Light endpoint id=%u", ep_.id);

        // Add HueSaturation feature so Alexa sends MoveToHueAndSaturation for
        // colour commands ("set to green") instead of falling back to CT.
        {
            esp_matter::cluster_t* cc = esp_matter::cluster::get(ep, CLUSTER_COLOR);
            esp_matter::cluster::color_control::feature::hue_saturation::config_t hs = {};
            esp_matter::cluster::color_control::feature::hue_saturation::add(cc, &hs);
        }
    }

    // Both strips render as one — enable spanning mode now.
    leds_.set_linked(true);
    ESP_LOGI(TAG, "LedManager linked mode enabled (effects span both strips)");

    return ESP_OK;
}

// ── start ─────────────────────────────────────────────────────────────────────

// ── ensure_unique_discriminator ───────────────────────────────────────────────
// On first boot, generates a device-unique 12-bit discriminator from the
// ESP32's chip ID (eFuse block1) and stores it in the "fctry" NVS partition
// under the "chip-factory" namespace.  CHIP's device layer reads it from
// there, so every device gets a different manual pairing code.
// Subsequent boots find the key already present and leave it unchanged.

static void ensure_unique_discriminator()
{
    // The "fctry" NVS partition must be initialised before use.
    // esp_matter::start() does this internally, but we run before it.
    esp_err_t init_err = nvs_flash_init_partition("fctry");
    if (init_err == ESP_ERR_NVS_NO_FREE_PAGES ||
        init_err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "fctry NVS corrupted — erasing");
        nvs_flash_erase_partition("fctry");
        init_err = nvs_flash_init_partition("fctry");
    }
    if (init_err != ESP_OK) {
        ESP_LOGW(TAG, "fctry NVS init failed (%s) — using default discriminator",
                 esp_err_to_name(init_err));
        return;
    }

    nvs_handle_t h;
    esp_err_t err = nvs_open_from_partition("fctry", "chip-factory",
                                            NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "fctry NVS open failed (%s) — using default discriminator",
                 esp_err_to_name(err));
        return;
    }

    uint32_t stored_disc = 0;
    if (nvs_get_u32(h, "discriminator", &stored_disc) == ESP_OK) {
        ESP_LOGI(TAG, "Discriminator already set: %lu", (unsigned long)stored_disc);
        nvs_close(h);
        return;
    }

    // Not set yet — derive from chip ID (64-bit eFuse MAC / unique ID)
    uint8_t mac[8] = {};
    esp_efuse_read_field_blob(ESP_EFUSE_MAC_FACTORY, mac, 48);  // 48-bit MAC

    // Mix bytes into a 12-bit value (0–4094; 4095 is reserved by Matter spec)
    uint32_t disc = ((uint32_t)(mac[3] ^ mac[4]) << 4) |
                    ((mac[5] ^ mac[2]) & 0x0F);
    disc = disc & 0xFFF;
    if (disc == 0)    disc = 1;
    if (disc == 4095) disc = 4094;

    err = nvs_set_u32(h, "discriminator", disc);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Unique discriminator set: %lu (0x%03lX)",
                 (unsigned long)disc, (unsigned long)disc);
    } else {
        ESP_LOGW(TAG, "Failed to save discriminator: %s", esp_err_to_name(err));
    }
}

// ── start ─────────────────────────────────────────────────────────────────────

esp_err_t MatterBridge::start(bool fresh_commissioning)
{
    ensure_unique_discriminator();
    ESP_LOGI(TAG, "Starting Matter stack");
    esp_err_t err = esp_matter::start(event_cb);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_matter::start failed: %s", esp_err_to_name(err));
        return err;
    }

    // ── Coex fix: clear stale WiFi credentials on fresh commissioning ────────
    // esp_wifi_set_config() persists credentials in the WiFi driver's NVS.
    // On a fresh Matter commissioning attempt, those stale credentials cause
    // CHIP's ConnectivityManager to call esp_wifi_connect() while BLE is
    // advertising, and the coex module fails the WiFi 4-way handshake
    // ("Coexist: Wi-Fi connect fail") every ~5 s.  Clearing the config stops
    // CHIP from retrying, giving BLE uncontested radio time.
    //
    // This fix is ONLY applied when fresh_commissioning=true (i.e. the caller
    // just wiped CHIP_KVS and is starting a new commissioning session).  On
    // normal reboots (fresh_commissioning=false) the fix must NOT run:
    //   • WiFi-path devices (never Matter-commissioned) always have
    //     FabricCount==0, so the old unconditional check fired every boot,
    //     clearing the stored WiFi credentials and causing intermittent
    //     connection failures due to BLE+WiFi coexistence.
    //   • Commissioned devices (FabricCount>0) reconnect using the credentials
    //     delivered by CHIP's own provisioning — leave those untouched.
    if (fresh_commissioning) {
        wifi_config_t empty = {};
        if (esp_wifi_set_config(WIFI_IF_STA, &empty) == ESP_OK) {
            ESP_LOGI(TAG, "Fresh commissioning — cleared WiFi driver NVS (BLE coex fix)");
        } else {
            ESP_LOGW(TAG, "Failed to clear WiFi credentials");
        }
    }

    // Force ColorMode to HS (0) so Alexa classifies both strips as Color Lights
    // and sends MoveToHueAndSaturation for colour commands.
    // NVS may persist an old CT (2) or XY (1) mode from a prior session.
    esp_matter_attr_val_t hs_mode = esp_matter_uint8(0x00);
    esp_matter::attribute::update(ep_.id, CLUSTER_COLOR, ATTR_COLOR_MODE, &hs_mode);
    ESP_LOGI(TAG, "ColorMode forced to HS on endpoint %u", ep_.id);

    // Print commissioning codes at INFO level (CHIP only prints them at DEBUG).
    auto info = get_commissioning_info();
    chip::SetupPayload payload;
    payload.version           = 0;
    payload.vendorID          = 0;
    payload.productID         = 0;
    payload.commissioningFlow = chip::CommissioningFlow::kStandard;
    payload.setUpPINCode      = info.pin_code;
    payload.discriminator.SetLongValue(info.discriminator);
    // kBLE: device is commissioned via BLE on first pairing.
    // Without this, QRCodeSetupPayloadGenerator returns an empty string.
    payload.rendezvousInformation.SetValue(
        chip::RendezvousInformationFlags(chip::RendezvousInformationFlag::kBLE));

    std::string manualCode;
    std::string qrCode;
    chip::ManualSetupPayloadGenerator(payload)
        .payloadDecimalStringRepresentation(manualCode);
    chip::QRCodeSetupPayloadGenerator(payload)
        .payloadBase38Representation(qrCode);

    manual_code_ = manualCode;

    ESP_LOGI(TAG, "╔══════════════════════════════╗");
    ESP_LOGI(TAG, "║   Matter Commissioning Info  ║");
    ESP_LOGI(TAG, "╠══════════════════════════════╣");
    ESP_LOGI(TAG, "║ PIN:   %-22lu ║", (unsigned long)info.pin_code);
    ESP_LOGI(TAG, "║ Disc:  %-22u ║", info.discriminator);
    if (!manualCode.empty())
        ESP_LOGI(TAG, "║ Code:  %-22s ║", manualCode.c_str());
    if (!qrCode.empty())
        ESP_LOGI(TAG, "║ QR:    %-22s ║", qrCode.c_str());
    ESP_LOGI(TAG, "╚══════════════════════════════╝");

    return err;
}

// ── apply ─────────────────────────────────────────────────────────────────────

void MatterBridge::apply()
{
    if (!ep_.on) {
        if (event_log_) event_log_->log(EventLog::CAT_LED_MATTER, "Off");
        leds_.set_brightness(LedManager::Target::STRIP_1, 0);
        leds_.set_brightness(LedManager::Target::STRIP_2, 0);
        leds_.set_effect(LedManager::Target::STRIP_1, LedManager::Effect::STATIC);
        leds_.set_effect(LedManager::Target::STRIP_2, LedManager::Effect::STATIC);
        return;
    }

    uint8_t r, g, b;
    switch (ep_.color_mode) {
        case 0x00:  // HS
            hsv_to_rgb(ep_.hue, ep_.saturation, 255, r, g, b);
            break;
        case 0x01:  // XY (Alexa color picker)
            xy_to_rgb(ep_.color_x, ep_.color_y, r, g, b);
            break;
        case 0x02:  // Color Temperature
        default:
            ct_to_rgb(ep_.color_temp, r, g, b);
            break;
    }

    static const char* mode_str[] = { "HS", "XY", "CT" };
    ESP_LOGI(TAG, "[%s] → RGB(%u,%u,%u) bri=%u",
             mode_str[ep_.color_mode & 0x03], r, g, b, ep_.level);
    if (event_log_) {
        event_log_->log(EventLog::CAT_LED_MATTER, "[%s] RGB(%u,%u,%u) bri=%u",
                        mode_str[ep_.color_mode & 0x03], r, g, b, ep_.level);
    }
    leds_.set_effect(LedManager::Target::STRIP_1, LedManager::Effect::STATIC);
    leds_.set_effect(LedManager::Target::STRIP_2, LedManager::Effect::STATIC);
    leds_.set_color(LedManager::Target::STRIP_1, r, g, b);
    leds_.set_color(LedManager::Target::STRIP_2, r, g, b);
    leds_.set_brightness(LedManager::Target::STRIP_1, ep_.level);
    leds_.set_brightness(LedManager::Target::STRIP_2, ep_.level);
}

// ── attr_cb ───────────────────────────────────────────────────────────────────

esp_err_t MatterBridge::attr_cb(
    esp_matter::attribute::callback_type_t type,
    uint16_t /*endpoint_id*/, uint32_t cluster_id,
    uint32_t attribute_id, esp_matter_attr_val_t* val,
    void* priv_data)
{
    if (type != esp_matter::attribute::POST_UPDATE) return ESP_OK;
    if (!priv_data || !val) return ESP_OK;

    auto* self = static_cast<MatterBridge*>(priv_data);

    if (cluster_id == CLUSTER_ON_OFF && attribute_id == ATTR_ON_OFF) {
        self->ep_.on = val->val.b;
        ESP_LOGD(TAG, "OnOff=%d", (int)val->val.b);
        self->apply();

    } else if (cluster_id == CLUSTER_LEVEL && attribute_id == ATTR_LEVEL_CUR) {
        self->ep_.level = val->val.u8;
        ESP_LOGD(TAG, "Level=%u", val->val.u8);
        if (self->ep_.on) self->apply();

    } else if (cluster_id == CLUSTER_COLOR) {
        ESP_LOGD(TAG, "COLOR attr=0x%04lX type=%u u8=%u u16=%u",
                 (unsigned long)attribute_id, val->type,
                 val->val.u8, val->val.u16);
        if (attribute_id == ATTR_COLOR_MODE) {
            self->ep_.color_mode = val->val.u8;
            ESP_LOGI(TAG, "ColorMode=%u", val->val.u8);
            // Mode change alone doesn't update color — wait for the coordinate attrs
        } else if (attribute_id == ATTR_HUE) {
            self->ep_.hue = val->val.u8;
            ESP_LOGD(TAG, "Hue=%u", val->val.u8);
            if (self->ep_.on) self->apply();
        } else if (attribute_id == ATTR_SAT) {
            self->ep_.saturation = val->val.u8;
            ESP_LOGD(TAG, "Sat=%u", val->val.u8);
            if (self->ep_.on) self->apply();
        } else if (attribute_id == ATTR_CUR_X) {
            self->ep_.color_x = val->val.u16;
            ESP_LOGD(TAG, "X=%u", val->val.u16);
            if (self->ep_.on) self->apply();
        } else if (attribute_id == ATTR_CUR_Y) {
            self->ep_.color_y = val->val.u16;
            ESP_LOGD(TAG, "Y=%u", val->val.u16);
            if (self->ep_.on) self->apply();
        } else if (attribute_id == ATTR_COLOR_TEMP) {
            self->ep_.color_temp = val->val.u16;
            ESP_LOGD(TAG, "CT=%u mireds", val->val.u16);
            if (self->ep_.on) self->apply();
        }
    }

    return ESP_OK;
}

// ── event_cb ──────────────────────────────────────────────────────────────────

void MatterBridge::event_cb(
    const chip::DeviceLayer::ChipDeviceEvent* event, intptr_t /*arg*/)
{
    using namespace chip::DeviceLayer;
    if (!event) return;
    switch (event->Type) {
        case DeviceEventType::kCHIPoBLEConnectionEstablished:
            ESP_LOGD(TAG, "BLE connection established");
            if (s_instance_ && s_instance_->event_log_)
                s_instance_->event_log_->log(EventLog::CAT_STARTUP, "Matter: BLE connected");
            break;
        case DeviceEventType::kCHIPoBLEConnectionClosed:
            ESP_LOGI(TAG, "BLE connection closed — resetting WiFi driver for reconnect");
            if (s_instance_ && s_instance_->event_log_)
                s_instance_->event_log_->log(EventLog::CAT_STARTUP, "Matter: BLE closed");
            // During BLE commissioning the coex module repeatedly fails the
            // WiFi 4-way handshake ("Coexist: Wi-Fi connect fail") and leaves
            // the driver in a pending-reconnect state where every subsequent
            // esp_wifi_connect() call is rejected ("Haven't to connect").
            // Now that BLE is closed the radio is free for WiFi, but the coex
            // lock persists until the driver is fully restarted.
            // esp_wifi_stop() clears all coex state.  esp_wifi_start() posts
            // WIFI_EVENT_STA_START, which causes CHIP's event handler to call
            // esp_wifi_connect() immediately on the clean driver.
            {
                wifi_ap_record_t ap = {};
                if (esp_wifi_sta_get_ap_info(&ap) != ESP_OK) {
                    // WiFi not connected — restart the driver to clear coex lock
                    esp_wifi_stop();
                    esp_wifi_start();
                }
            }
            break;
        case DeviceEventType::kCHIPoBLESubscribe: {
            // Alexa has just subscribed to the CHIPoBLE TX characteristic —
            // commissioning data exchange is about to begin.  Request longer
            // BLE connection intervals so the coex module gives WiFi enough
            // time to complete the WPA2 4-way handshake during ConnectNetwork.
            //
            // Matter spec (section 4.16.6) requires commissioners to accept
            // connection parameter updates up to 100 ms interval, so Alexa
            // should honour this request.  With 100 ms intervals the WiFi radio
            // gets ~99 ms windows — sufficient for the entire 4-way handshake.
            //
            // itvl_min/max are in 1.25 ms units:
            //   64  × 1.25 ms =  80 ms
            //   80  × 1.25 ms = 100 ms
            // supervision_timeout is in 10 ms units: 500 × 10 ms = 5 s.
            uint16_t conId = event->CHIPoBLESubscribe.ConId;
            struct ble_gap_upd_params params = {};
            params.itvl_min          = 64;   //  80 ms
            params.itvl_max          = 80;   // 100 ms
            params.latency           = 0;
            params.supervision_timeout = 500; //   5 s
            params.min_ce_len        = 0;
            params.max_ce_len        = 0;
            int rc = ble_gap_update_params(conId, &params);
            if (rc == 0) {
                ESP_LOGI(TAG, "BLE subscribe — requested 80-100 ms intervals (conn %u)", conId);
            } else {
                ESP_LOGW(TAG, "BLE subscribe — ble_gap_update_params rc=%d (conn %u)", rc, conId);
            }
            break;
        }
        case DeviceEventType::kCHIPoBLEConnectionError:
            ESP_LOGW(TAG, "BLE connection error");
            if (s_instance_ && s_instance_->event_log_)
                s_instance_->event_log_->log(EventLog::CAT_STARTUP, "Matter: BLE error");
            break;
        case DeviceEventType::kCommissioningComplete:
            ESP_LOGI(TAG, "Matter commissioning complete");
            if (s_instance_ && s_instance_->event_log_)
                s_instance_->event_log_->log(EventLog::CAT_STARTUP, "Matter: commissioned");
            break;
        case DeviceEventType::kFailSafeTimerExpired:
            ESP_LOGD(TAG, "Fail-safe timer expired");
            break;
        case DeviceEventType::kFabricRemoved:
            ESP_LOGW(TAG, "Matter fabric removed — ready to recommission");
            if (s_instance_ && s_instance_->event_log_)
                s_instance_->event_log_->log(EventLog::CAT_STARTUP, "Matter: fabric removed");
            break;
        default:
            break;
    }
}

// ── identify_cb ───────────────────────────────────────────────────────────────

esp_err_t MatterBridge::identify_cb(
    esp_matter::identification::callback_type_t type,
    uint16_t endpoint_id, uint8_t /*effect_id*/,
    uint8_t /*effect_variant*/, void* priv_data)
{
    if (!priv_data) return ESP_OK;
    ESP_LOGI(TAG, "Identify endpoint_id=%u type=%d", endpoint_id, (int)type);
    // Could briefly blink the relevant strip here if desired
    return ESP_OK;
}

// ── open_commissioning_window ─────────────────────────────────────────────────

esp_err_t MatterBridge::open_commissioning_window()
{
    if (is_commissioned()) {
        ESP_LOGW(TAG, "Device already has a fabric — not opening commissioning window");
        return ESP_ERR_INVALID_STATE;
    }

    using namespace chip;
    auto& mgr = Server::GetInstance().GetCommissioningWindowManager();
    CHIP_ERROR err = mgr.OpenBasicCommissioningWindow();
    if (err != CHIP_NO_ERROR) {
        ESP_LOGE(TAG, "OpenBasicCommissioningWindow failed: %" CHIP_ERROR_FORMAT, err.Format());
        return ESP_FAIL;
    }

    // Re-print pairing codes so the user can find them without scrolling.
    auto info = get_commissioning_info();
    ESP_LOGI(TAG, "BLE commissioning window reopened");
    ESP_LOGI(TAG, "  PIN:  %lu   Disc: %u   Code: %s",
             (unsigned long)info.pin_code, info.discriminator, manual_code_.c_str());
    return ESP_OK;
}

// ── is_commissioned ───────────────────────────────────────────────────────────

bool MatterBridge::is_commissioned() const
{
    return chip::Server::GetInstance().GetFabricTable().FabricCount() > 0;
}

uint8_t MatterBridge::fabric_count() const
{
    return chip::Server::GetInstance().GetFabricTable().FabricCount();
}

// ── disable_ble_advertising ───────────────────────────────────────────────────

void MatterBridge::disable_ble_advertising()
{
    chip::DeviceLayer::ConnectivityMgr().SetBLEAdvertisingEnabled(false);
    ESP_LOGI(TAG, "BLE advertising disabled (WiFi-only mode)");
}

// ── get_commissioning_info ────────────────────────────────────────────────────

MatterBridge::CommissioningInfo MatterBridge::get_commissioning_info() const
{
    CommissioningInfo info = {};
    info.pin_code = 20202021;   // default CHIP test PIN (CONFIG_CHIP_FACTORY_DATA=n)

    // Read the discriminator that was written by ensure_unique_discriminator()
    nvs_handle_t h;
    if (nvs_open_from_partition("fctry", "chip-factory", NVS_READONLY, &h) == ESP_OK) {
        uint32_t d = 3840;  // CHIP default fallback
        nvs_get_u32(h, "discriminator", &d);
        nvs_close(h);
        info.discriminator = (uint16_t)d;
    } else {
        info.discriminator = 3840;
    }
    return info;
}

// ── hsv_to_rgb ────────────────────────────────────────────────────────────────
// Converts Matter hue (0-254), saturation (0-254), value (0-255) → RGB.
// Uses integer arithmetic only — no floats.

void MatterBridge::hsv_to_rgb(uint8_t h, uint8_t s, uint8_t v,
                               uint8_t& r, uint8_t& g, uint8_t& b)
{
    if (s == 0) {
        r = g = b = v;
        return;
    }

    // Scale hue 0-254 to 0-5 sectors + remainder within sector
    uint8_t  sector    = h / 43;
    uint8_t  remainder = (uint8_t)((h - (uint16_t)(sector * 43)) * 6);

    uint8_t p = (uint8_t)((uint16_t)v * (255 - s) / 255);
    uint8_t q = (uint8_t)((uint16_t)v * (255 - ((uint16_t)s * remainder / 255)) / 255);
    uint8_t t = (uint8_t)((uint16_t)v * (255 - ((uint16_t)s * (255 - remainder) / 255)) / 255);

    switch (sector) {
        case 0:  r = v; g = t; b = p; break;
        case 1:  r = q; g = v; b = p; break;
        case 2:  r = p; g = v; b = t; break;
        case 3:  r = p; g = q; b = v; break;
        case 4:  r = t; g = p; b = v; break;
        default: r = v; g = p; b = q; break;
    }
}

// ── xy_to_rgb ─────────────────────────────────────────────────────────────────
// Converts CIE 1931 xy (Matter uint16 = float × 65536) to sRGB [0-255].
// Uses the Wide RGB D65 matrix (same gamut as WS2812B datasheets).
// Integer-only: intermediate values held in 32-bit fixed-point.

void MatterBridge::xy_to_rgb(uint16_t x16, uint16_t y16,
                              uint8_t& r, uint8_t& g, uint8_t& b)
{
    // Guard against y=0 (undefined colour point)
    if (y16 == 0) { r = g = b = 255; return; }

    // Convert to fixed-point Q16: x = x16/65536, y = y16/65536
    // z = 1 - x - y   (all in Q16)
    int32_t x = x16;
    int32_t y = y16;
    int32_t z = 65536 - x - y;   // 1 - x - y in Q16

    // Y is normalised to 1.0 (full brightness handled by level separately).
    // X = x/y,  Z = z/y   (both Q16, divided by y/65536)
    // Use 64-bit to avoid overflow in the division.
    int32_t X = (int32_t)(((int64_t)x << 16) / y);  // Q16
    int32_t Y = 65536;                               // Q16 = 1.0
    int32_t Z = (int32_t)(((int64_t)z << 16) / y);  // Q16

    // Wide-gamut D65 XYZ → linear sRGB matrix (Q16 fixed-point coefficients):
    //  [ R ]   [ 3.2406 -1.5372 -0.4986 ]   [ X ]
    //  [ G ] = [-0.9689  1.8758  0.0415 ] × [ Y ]
    //  [ B ]   [ 0.0557 -0.2040  1.0570 ]   [ Z ]
    // Coefficients scaled by 65536 and rounded.
    auto mul16 = [](int32_t a, int32_t b32) -> int32_t {
        return (int32_t)(((int64_t)a * b32) >> 16);
    };
    int32_t linR = mul16( 212310, X) + mul16(-100744, Y) + mul16( -32670, Z);
    int32_t linG = mul16( -63494, X) + mul16( 122937, Y) + mul16(   2720, Z);
    int32_t linB = mul16(   3651, X) + mul16( -13370, Y) + mul16(  69279, Z);

    // Clamp to [0, 65536] then normalise so brightest channel = 65536
    linR = linR < 0 ? 0 : linR;
    linG = linG < 0 ? 0 : linG;
    linB = linB < 0 ? 0 : linB;
    int32_t maxC = linR > linG ? linR : linG;
    if (linB > maxC) maxC = linB;
    if (maxC == 0) { r = g = b = 255; return; }
    // Scale so max channel = 255 (brightness is handled by set_brightness)
    r = (uint8_t)((linR * 255) / maxC);
    g = (uint8_t)((linG * 255) / maxC);
    b = (uint8_t)((linB * 255) / maxC);
}

// ── ct_to_rgb ─────────────────────────────────────────────────────────────────
// Converts color temperature in mireds to an approximate RGB white point.
// Uses Tanner Helland's piecewise approximation, adapted for integer math.
// mireds = 1,000,000 / kelvin.

void MatterBridge::ct_to_rgb(uint16_t mireds,
                              uint8_t& r, uint8_t& g, uint8_t& b)
{
    // Convert mireds → Kelvin, clamp to a sensible range [1000 K, 40000 K]
    uint32_t kelvin = (mireds > 0) ? (1000000u / mireds) : 6500u;
    if (kelvin < 1000)  kelvin = 1000;
    if (kelvin > 40000) kelvin = 40000;

    uint32_t t = kelvin / 100;  // work in hundreds of kelvin

    // ── Red ──────────────────────────────────────────────────────────────────
    uint32_t rv, gv, bv;
    if (t <= 66) {
        rv = 255;
    } else {
        // rv = 329.698727 * (t - 60)^-0.1332047592
        // Approximated with a lookup-friendly integer formula
        int32_t tmp = (int32_t)t - 60;
        // Clamp
        if (tmp < 1) tmp = 1;
        // Use integer approximation: rv ≈ 329699 * 1024 / pow_approx
        // Simple: rv = 255 * 329 / (t - 55)   (good enough for LED drivers)
        rv = (255u * 329u) / (uint32_t)(t - 55 < 1 ? 1 : t - 55);
        if (rv > 255) rv = 255;
    }

    // ── Green ─────────────────────────────────────────────────────────────────
    if (t <= 66) {
        // gv = 99.4708025861 * ln(t) - 161.1195681661
        // Integer approx: gv = 99 * (t*100 - 6000) / (t*100) for simplicity
        // Better: use a small table or linear piecewise
        // Linear approx between 10→66 hundreds: 0→255
        if (t < 10) t = 10;
        gv = (uint32_t)((-10000 + (int32_t)t * 2196) / 100);
        if (gv > 255) gv = 255;
    } else {
        int32_t tmp = (int32_t)t - 60;
        if (tmp < 1) tmp = 1;
        gv = (255u * 288u) / (uint32_t)(tmp + 20 < 1 ? 1 : tmp + 20);
        if (gv > 255) gv = 255;
    }

    // ── Blue ──────────────────────────────────────────────────────────────────
    if (t >= 66) {
        bv = 255;
    } else if (t <= 19) {
        bv = 0;
    } else {
        int32_t tmp = (int32_t)t - 10;
        if (tmp < 1) tmp = 1;
        bv = (uint32_t)((int32_t)tmp * 233 / 47);
        if (bv > 255) bv = 255;
    }

    r = (uint8_t)rv;
    g = (uint8_t)gv;
    b = (uint8_t)bv;
}
