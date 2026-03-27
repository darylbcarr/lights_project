#include "config_store.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include <cstring>

static const char* TAG = "config_store";
static const char* NS  = "clk_cfg";

// ── init ─────────────────────────────────────────────────────────────────────

esp_err_t ConfigStore::init()
{
    nvs_handle_t h;
    esp_err_t ret = nvs_open(NS, NVS_READWRITE, &h);
    if (ret == ESP_OK) {
        nvs_close(h);
        ESP_LOGI(TAG, "NVS namespace '%s' OK", NS);
    } else {
        ESP_LOGW(TAG, "NVS namespace '%s' open failed: %s", NS, esp_err_to_name(ret));
    }
    return ret;
}

// ── Clock ─────────────────────────────────────────────────────────────────────

bool ConfigStore::save(const ClockCfg& c)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return false;
    nvs_set_i32(h, "clk_sen_stp",  c.sensor_offset_steps);
    nvs_set_u8 (h, "clk_mrev",     c.motor_reverse ? 1u : 0u);
    nvs_set_u32(h, "clk_step_us",  c.step_delay_us);
    nvs_set_i32(h, "clk_dmin",     c.disp_minute);
    nvs_set_i32(h, "clk_dhour",    c.disp_hour);
    bool ok = (nvs_commit(h) == ESP_OK);
    nvs_close(h);
    return ok;
}

bool ConfigStore::load(ClockCfg& c)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return false;
    int32_t i32; uint8_t u8; uint32_t u32;
    if (nvs_get_i32(h, "clk_sen_stp", &i32) == ESP_OK) c.sensor_offset_steps = i32;
    if (nvs_get_u8 (h, "clk_mrev",    &u8)  == ESP_OK) c.motor_reverse = (u8 != 0);
    if (nvs_get_u32(h, "clk_step_us", &u32) == ESP_OK) c.step_delay_us = u32;
    if (nvs_get_i32(h, "clk_dmin",    &i32) == ESP_OK) c.disp_minute   = i32;
    if (nvs_get_i32(h, "clk_dhour",   &i32) == ESP_OK) c.disp_hour     = i32;
    nvs_close(h);
    return true;
}

bool ConfigStore::save_disp_position(int32_t hour, int32_t min)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return false;
    nvs_set_i32(h, "clk_dmin",  min);
    nvs_set_i32(h, "clk_dhour", hour);
    bool ok = (nvs_commit(h) == ESP_OK);
    nvs_close(h);
    return ok;
}

// ── LEDs ──────────────────────────────────────────────────────────────────────

bool ConfigStore::save(const LedCfg& c)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return false;
    char key[12];
    for (int i = 0; i < 2; i++) {
        snprintf(key, sizeof(key), "led%d_len", i); nvs_set_u16(h, key, c.strip[i].len);
        snprintf(key, sizeof(key), "led%d_r",   i); nvs_set_u8 (h, key, c.strip[i].r);
        snprintf(key, sizeof(key), "led%d_g",   i); nvs_set_u8 (h, key, c.strip[i].g);
        snprintf(key, sizeof(key), "led%d_b",   i); nvs_set_u8 (h, key, c.strip[i].b);
        snprintf(key, sizeof(key), "led%d_br",  i); nvs_set_u8 (h, key, c.strip[i].brightness);
        snprintf(key, sizeof(key), "led%d_fx",  i); nvs_set_u8 (h, key, c.strip[i].effect);
    }
    bool ok = (nvs_commit(h) == ESP_OK);
    nvs_close(h);
    return ok;
}

bool ConfigStore::load(LedCfg& c)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return false;
    char key[12];
    uint8_t u8; uint16_t u16;
    for (int i = 0; i < 2; i++) {
        snprintf(key, sizeof(key), "led%d_len", i);
        if (nvs_get_u16(h, key, &u16) == ESP_OK) c.strip[i].len = u16;
        snprintf(key, sizeof(key), "led%d_r", i);
        if (nvs_get_u8(h, key, &u8) == ESP_OK) c.strip[i].r = u8;
        snprintf(key, sizeof(key), "led%d_g", i);
        if (nvs_get_u8(h, key, &u8) == ESP_OK) c.strip[i].g = u8;
        snprintf(key, sizeof(key), "led%d_b", i);
        if (nvs_get_u8(h, key, &u8) == ESP_OK) c.strip[i].b = u8;
        snprintf(key, sizeof(key), "led%d_br", i);
        if (nvs_get_u8(h, key, &u8) == ESP_OK) c.strip[i].brightness = u8;
        snprintf(key, sizeof(key), "led%d_fx", i);
        if (nvs_get_u8(h, key, &u8) == ESP_OK) c.strip[i].effect = u8;
    }
    nvs_close(h);
    return true;
}

// ── Network ───────────────────────────────────────────────────────────────────

bool ConfigStore::save(const NetCfg& c)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return false;
    nvs_set_str(h, "net_ssid",      c.ssid);
    nvs_set_str(h, "net_pass",      c.password);
    nvs_set_str(h, "net_tz",        c.tz_override);
    nvs_set_str(h, "net_mdns",      c.mdns_hostname);
    nvs_set_u8 (h, "net_wifi_only",  c.wifi_only          ? 1 : 0);
    nvs_set_u8 (h, "net_matter_ok", c.matter_commissioned ? 1 : 0);
    bool ok = (nvs_commit(h) == ESP_OK);
    nvs_close(h);
    return ok;
}

bool ConfigStore::load(NetCfg& c)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return false;
    size_t len;
    len = sizeof(c.ssid);           nvs_get_str(h, "net_ssid",  c.ssid,           &len);
    len = sizeof(c.password);       nvs_get_str(h, "net_pass",  c.password,       &len);
    len = sizeof(c.tz_override);    nvs_get_str(h, "net_tz",    c.tz_override,    &len);
    len = sizeof(c.mdns_hostname);  nvs_get_str(h, "net_mdns",  c.mdns_hostname,  &len);
    uint8_t wo = 0; nvs_get_u8(h, "net_wifi_only",  &wo); c.wifi_only          = (wo != 0);
    uint8_t mc = 0; nvs_get_u8(h, "net_matter_ok", &mc); c.matter_commissioned = (mc != 0);
    nvs_close(h);
    return true;
}
