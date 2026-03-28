#include "webserver.h"
#include "web_ui.h"
#include "led_manager.h"
#include "ota_manager.h"
#include "matter_bridge.h"
#include "config_store.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_wifi.h"
#include "esp_system.h"
#include "cJSON.h"
#include <cstring>
#include <cstdlib>
#include <ctime>

static const char* TAG = "webserver";

WebServer* WebServer::s_instance_ = nullptr;

static void restart_cb(void*) { esp_restart(); }

// ── Constructor / Destructor ──────────────────────────────────────────────────

WebServer::WebServer(Networking& net, LedManager& leds)
    : net_(net), leds_(leds)
{
    s_instance_ = this;
    // Depth-1 queue; each item is a fixed 32-byte command name (copied by value)
    cmd_queue_ = xQueueCreate(1, 32);
}

WebServer::~WebServer()
{
    stop();
    if (cmd_queue_) { vQueueDelete(cmd_queue_); cmd_queue_ = nullptr; }
}

// ── start() / stop() ─────────────────────────────────────────────────────────

void WebServer::start()
{
    httpd_config_t cfg   = HTTPD_DEFAULT_CONFIG();
    cfg.stack_size       = 8192;
    cfg.max_open_sockets = 7;
    cfg.lru_purge_enable = true;
    cfg.uri_match_fn     = httpd_uri_match_wildcard;
    cfg.send_wait_timeout = 5;  // keep default; WS dead-socket cleanup uses sess_trigger_close

    if (httpd_start(&server_, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server");
        return;
    }

    // Register URI handlers — store 'this' as user_ctx
    httpd_uri_t root    = { "/",                   HTTP_GET,  on_root,             this };
    httpd_uri_t stat    = { "/api/status",         HTTP_GET,  on_api_status,       this };
    httpd_uri_t cmd     = { "/api/cmd",            HTTP_POST, on_api_cmd,          this };
    httpd_uri_t cfg_uri = { "/api/cfg",            HTTP_POST, on_api_cfg,          this };
    httpd_uri_t ota_uri = { "/api/ota",            HTTP_POST, on_api_ota,          this };
    httpd_uri_t ws   = {
        .uri      = "/ws",
        .method   = HTTP_GET,
        .handler  = on_ws,
        .user_ctx = this,
        .is_websocket = true
    };

    httpd_register_uri_handler(server_, &root);
    httpd_register_uri_handler(server_, &stat);
    httpd_register_uri_handler(server_, &cmd);
    httpd_register_uri_handler(server_, &cfg_uri);
    httpd_register_uri_handler(server_, &ota_uri);
    httpd_register_uri_handler(server_, &ws);

    xTaskCreate(ws_push_task,  "ws_push",  4096, this, 2, &ws_task_handle_);
    xTaskCreate(cmd_exec_task, "web_cmd",  4096, this, 3, &cmd_task_handle_);

    ESP_LOGI(TAG, "HTTP server started on port 80");
}

void WebServer::stop()
{
    if (ws_task_handle_)  { vTaskDelete(ws_task_handle_);  ws_task_handle_  = nullptr; }
    if (cmd_task_handle_) { vTaskDelete(cmd_task_handle_); cmd_task_handle_ = nullptr; }
    if (server_)          { httpd_stop(server_);           server_          = nullptr; }
}

// ── HTTP handlers ─────────────────────────────────────────────────────────────

esp_err_t WebServer::on_root(httpd_req_t* req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    return httpd_resp_send(req, WEB_UI_HTML, WEB_UI_HTML_LEN);
}

esp_err_t WebServer::on_api_status(httpd_req_t* req)
{
    auto* self = static_cast<WebServer*>(req->user_ctx);
    char* json = self->build_status_json();
    if (!json) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    esp_err_t ret = httpd_resp_send(req, json, strlen(json));
    free(json);
    return ret;
}

esp_err_t WebServer::on_api_cmd(httpd_req_t* req)
{
    auto* self = static_cast<WebServer*>(req->user_ctx);

    char buf[128] = {};
    int  len = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (len <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No body");
        return ESP_FAIL;
    }

    cJSON* body = cJSON_Parse(buf);
    const char* cmd_name = nullptr;
    if (body) {
        cJSON* c = cJSON_GetObjectItem(body, "cmd");
        if (cJSON_IsString(c)) cmd_name = c->valuestring;
    }

    if (!cmd_name) {
        if (body) cJSON_Delete(body);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing 'cmd'");
        return ESP_FAIL;
    }

    // ── Inline (fast, not queued) handlers ──────────────────────────────────

    // Per-strip or both-strip colour (led-color, led1-color, led2-color)
    auto handle_color = [&](LedManager::Target tgt) -> bool {
        cJSON* rj = cJSON_GetObjectItem(body, "r");
        cJSON* gj = cJSON_GetObjectItem(body, "g");
        cJSON* bj = cJSON_GetObjectItem(body, "b");
        if (cJSON_IsNumber(rj) && cJSON_IsNumber(gj) && cJSON_IsNumber(bj)) {
            self->leds_.set_color(tgt,
                (uint8_t)rj->valueint,
                (uint8_t)gj->valueint,
                (uint8_t)bj->valueint);
            return true;
        }
        return false;
    };

    // Per-strip brightness
    auto handle_bright = [&](LedManager::Target tgt) -> bool {
        cJSON* bj = cJSON_GetObjectItem(body, "brightness");
        if (cJSON_IsNumber(bj)) {
            self->leds_.set_brightness(tgt, (uint8_t)bj->valueint);
            return true;
        }
        return false;
    };

    bool handled_inline = false;
    if      (strcmp(cmd_name, "led-color")  == 0) handled_inline = handle_color(LedManager::Target::BOTH);
    else if (strcmp(cmd_name, "led1-color") == 0) handled_inline = handle_color(LedManager::Target::STRIP_1);
    else if (strcmp(cmd_name, "led2-color") == 0) handled_inline = handle_color(LedManager::Target::STRIP_2);
    else if (strcmp(cmd_name, "led-bright")  == 0) handled_inline = handle_bright(LedManager::Target::BOTH);
    else if (strcmp(cmd_name, "led1-bright") == 0) handled_inline = handle_bright(LedManager::Target::STRIP_1);
    else if (strcmp(cmd_name, "led2-bright") == 0) handled_inline = handle_bright(LedManager::Target::STRIP_2);

    if (handled_inline) {
        self->save_led_config();
        cJSON_Delete(body);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
        return httpd_resp_send(req, "{\"ok\":true,\"msg\":\"OK\"}",
                               HTTPD_RESP_USE_STRLEN);
    }

    // Copy name to a fixed-size buffer (queue items are 32 bytes, value-copied)
    char name[32] = {};
    strncpy(name, cmd_name, sizeof(name) - 1);
    cJSON_Delete(body);

    // Non-blocking send — if queue is full (command already pending) report busy
    bool queued = (xQueueSend(self->cmd_queue_, name, 0) == pdTRUE);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    const char* resp = queued
        ? "{\"ok\":true,\"msg\":\"Command accepted\"}"
        : "{\"ok\":false,\"msg\":\"Busy \xe2\x80\x94 try again\"}";
    return httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
}

esp_err_t WebServer::on_api_cfg(httpd_req_t* req)
{
    auto* self = static_cast<WebServer*>(req->user_ctx);

    char buf[384] = {};
    int len = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (len <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No body");
        return ESP_FAIL;
    }

    cJSON* body = cJSON_Parse(buf);
    if (!body) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad JSON");
        return ESP_FAIL;
    }

    cJSON* j;

    // ── LED strip lengths (apply + persist) ──────────────────────────────────
    bool led_changed = false;
    if ((j = cJSON_GetObjectItem(body, "strip1_len")) && cJSON_IsNumber(j)) {
        self->leds_.set_active_len(LedManager::Target::STRIP_1, (uint16_t)j->valueint);
        led_changed = true;
    }
    if ((j = cJSON_GetObjectItem(body, "strip2_len")) && cJSON_IsNumber(j)) {
        self->leds_.set_active_len(LedManager::Target::STRIP_2, (uint16_t)j->valueint);
        led_changed = true;
    }
    if (led_changed) self->save_led_config();

    // ── Timezone override (save; applied by networking on next connect) ────────
    if ((j = cJSON_GetObjectItem(body, "tz_override")) && cJSON_IsString(j)) {
        NetCfg nc;
        ConfigStore::load(nc);
        snprintf(nc.tz_override, sizeof(nc.tz_override), "%s", j->valuestring);
        ConfigStore::save(nc);
    }

    // ── mDNS hostname (apply live + persist) ──────────────────────────────────
    if ((j = cJSON_GetObjectItem(body, "mdns_hostname")) && cJSON_IsString(j)) {
        self->net_.set_mdns_hostname(j->valuestring);
        NetCfg nc;
        ConfigStore::load(nc);
        snprintf(nc.mdns_hostname, sizeof(nc.mdns_hostname), "%s", j->valuestring);
        ConfigStore::save(nc);
    }

    // ── WiFi credentials update (saves to NVS + WiFi driver; restarts) ────────
    if ((j = cJSON_GetObjectItem(body, "wifi_ssid")) && cJSON_IsString(j)
        && strlen(j->valuestring) > 0) {
        const char* new_ssid = j->valuestring;
        const char* new_pass = "";
        cJSON* jp = cJSON_GetObjectItem(body, "wifi_password");
        if (jp && cJSON_IsString(jp)) new_pass = jp->valuestring;

        // Save to our app NVS
        NetCfg nc = {};
        ConfigStore::load(nc);
        snprintf(nc.ssid,     sizeof(nc.ssid),     "%s", new_ssid);
        snprintf(nc.password, sizeof(nc.password), "%s", new_pass);
        ConfigStore::save(nc);

        // Update WiFi driver NVS — CHIP's ESPWiFiDriver::Init() reads this via
        // esp_wifi_get_config() on next boot, covering both WiFi-only and Matter.
        wifi_config_t wcfg = {};
        strncpy((char*)wcfg.sta.ssid,     new_ssid, sizeof(wcfg.sta.ssid)     - 1);
        strncpy((char*)wcfg.sta.password, new_pass, sizeof(wcfg.sta.password) - 1);
        wcfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
        esp_wifi_set_config(WIFI_IF_STA, &wcfg);

        // Send response before restarting
        cJSON_Delete(body);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
        httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);

        // Restart 1.5 s later (after response is sent and client receives it)
        esp_timer_handle_t t;
        esp_timer_create_args_t ta = {};
        ta.callback = restart_cb;
        ta.name     = "wifi_restart";
        esp_timer_create(&ta, &t);
        esp_timer_start_once(t, 1500000);   // 1.5 s
        return ESP_OK;
    }

    cJSON_Delete(body);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    return httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
}

// ── OTA endpoint ─────────────────────────────────────────────────────────────
// POST /api/ota
//   {"action": "check"}           → trigger version-only check (non-blocking)
//   {"action": "update"}          → trigger full OTA install (non-blocking)
//   {"auto_update": true/false}   → persist auto-update setting

esp_err_t WebServer::on_api_ota(httpd_req_t* req)
{
    auto* self = static_cast<WebServer*>(req->user_ctx);

    char buf[128] = {};
    int len = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (len <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No body");
        return ESP_FAIL;
    }

    const char* msg = "OK";

    if (self->ota_) {
        cJSON* body = cJSON_Parse(buf);
        if (body) {
            cJSON* action = cJSON_GetObjectItem(body, "action");
            cJSON* autoj  = cJSON_GetObjectItem(body, "auto_update");

            if (cJSON_IsString(action)) {
                if (strcmp(action->valuestring, "check") == 0) {
                    self->ota_->trigger_check();
                    msg = "Check triggered";
                } else if (strcmp(action->valuestring, "update") == 0) {
                    self->ota_->trigger_update();
                    msg = "Update triggered";
                }
            }
            if (cJSON_IsBool(autoj)) {
                self->ota_->set_auto_update(cJSON_IsTrue(autoj));
                msg = "Auto-update saved";
            }
            cJSON_Delete(body);
        }
    } else {
        msg = "OTA not available";
    }

    char resp[64];
    snprintf(resp, sizeof(resp), "{\"ok\":true,\"msg\":\"%s\"}", msg);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    return httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
}

// ── LED config snapshot ───────────────────────────────────────────────────────

void WebServer::save_led_config()
{
    LedCfg cfg;
    for (int i = 0; i < LedManager::STRIP_COUNT; i++) {
        cfg.strip[i].len        = leds_.get_active_len(i);
        cfg.strip[i].brightness = leds_.get_brightness(i);
        cfg.strip[i].effect     = static_cast<uint8_t>(leds_.get_effect(i));
        leds_.get_color(i, cfg.strip[i].r, cfg.strip[i].g, cfg.strip[i].b);
    }
    ConfigStore::save(cfg);
}

esp_err_t WebServer::on_ws(httpd_req_t* req)
{
    if (req->method == HTTP_GET) {
        ESP_LOGI(TAG, "WebSocket client connected, fd=%d",
                 httpd_req_to_sockfd(req));
        return ESP_OK;
    }
    // Drain any incoming client frames (push-only server)
    httpd_ws_frame_t pkt = {};
    uint8_t buf[64]      = {};
    pkt.payload          = buf;
    httpd_ws_recv_frame(req, &pkt, sizeof(buf));
    return ESP_OK;
}

// ── WebSocket broadcast ───────────────────────────────────────────────────────

struct WsBcastArg {
    httpd_handle_t hd;
    char*          json;
    size_t         len;
};

static void do_ws_broadcast(void* arg)
{
    auto* a = static_cast<WsBcastArg*>(arg);
    size_t clients = 8;
    int    fds[8]  = {};

    if (httpd_get_client_list(a->hd, &clients, fds) == ESP_OK) {
        httpd_ws_frame_t pkt = {};
        pkt.type             = HTTPD_WS_TYPE_TEXT;
        pkt.payload          = reinterpret_cast<uint8_t*>(a->json);
        pkt.len              = a->len;
        for (size_t i = 0; i < clients; i++) {
            if (httpd_ws_get_fd_info(a->hd, fds[i]) == HTTPD_WS_CLIENT_WEBSOCKET) {
                if (httpd_ws_send_frame_async(a->hd, fds[i], &pkt) != ESP_OK) {
                    // Socket is dead — close it so future broadcasts skip it
                    httpd_sess_trigger_close(a->hd, fds[i]);
                }
            }
        }
    }

    free(a->json);
    free(a);
}

void WebServer::ws_push_task(void* arg)
{
    auto* self = static_cast<WebServer*>(arg);
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        if (!self->server_) continue;

        char* json = self->build_status_json();
        if (!json) continue;

        auto* a = static_cast<WsBcastArg*>(malloc(sizeof(WsBcastArg)));
        if (!a) { free(json); continue; }

        a->hd   = self->server_;
        a->json = json;
        a->len  = strlen(json);

        if (httpd_queue_work(self->server_, do_ws_broadcast, a) != ESP_OK) {
            free(json);
            free(a);
        }
    }
}

// ── Command executor ──────────────────────────────────────────────────────────

void WebServer::cmd_exec_task(void* arg)
{
    auto* self = static_cast<WebServer*>(arg);
    char  name[32];
    while (true) {
        if (xQueueReceive(self->cmd_queue_, name, portMAX_DELAY) == pdTRUE) {
            self->dispatch_cmd(name);
        }
    }
}

void WebServer::dispatch_cmd(const char* cmd)
{
    ESP_LOGI(TAG, "Executing command: %s", cmd);

    if      (strcmp(cmd, "led-off")        == 0) leds_.set_effect(LedManager::Target::BOTH, LedManager::Effect::OFF);
    else if (strcmp(cmd, "led-static")     == 0) leds_.set_effect(LedManager::Target::BOTH, LedManager::Effect::STATIC);
    else if (strcmp(cmd, "led-breathe")    == 0) leds_.set_effect(LedManager::Target::BOTH, LedManager::Effect::BREATHE);
    else if (strcmp(cmd, "led-rainbow")    == 0) leds_.set_effect(LedManager::Target::BOTH, LedManager::Effect::RAINBOW);
    else if (strcmp(cmd, "led-chase")      == 0) leds_.set_effect(LedManager::Target::BOTH, LedManager::Effect::CHASE);
    else if (strcmp(cmd, "led-sparkle")    == 0) leds_.set_effect(LedManager::Target::BOTH, LedManager::Effect::SPARKLE);
    else if (strcmp(cmd, "led-wipe")       == 0) leds_.set_effect(LedManager::Target::BOTH, LedManager::Effect::WIPE);
    else if (strcmp(cmd, "led-comet")      == 0) leds_.set_effect(LedManager::Target::BOTH, LedManager::Effect::COMET);
    else if (strcmp(cmd, "led-next")       == 0) leds_.next_effect(LedManager::Target::BOTH);
    // Per-strip effects
    else if (strcmp(cmd, "led1-static")   == 0) leds_.set_effect(LedManager::Target::STRIP_1, LedManager::Effect::STATIC);
    else if (strcmp(cmd, "led1-breathe")  == 0) leds_.set_effect(LedManager::Target::STRIP_1, LedManager::Effect::BREATHE);
    else if (strcmp(cmd, "led1-rainbow")  == 0) leds_.set_effect(LedManager::Target::STRIP_1, LedManager::Effect::RAINBOW);
    else if (strcmp(cmd, "led1-chase")    == 0) leds_.set_effect(LedManager::Target::STRIP_1, LedManager::Effect::CHASE);
    else if (strcmp(cmd, "led1-sparkle")  == 0) leds_.set_effect(LedManager::Target::STRIP_1, LedManager::Effect::SPARKLE);
    else if (strcmp(cmd, "led1-wipe")     == 0) leds_.set_effect(LedManager::Target::STRIP_1, LedManager::Effect::WIPE);
    else if (strcmp(cmd, "led1-comet")    == 0) leds_.set_effect(LedManager::Target::STRIP_1, LedManager::Effect::COMET);
    else if (strcmp(cmd, "led2-static")   == 0) leds_.set_effect(LedManager::Target::STRIP_2, LedManager::Effect::STATIC);
    else if (strcmp(cmd, "led2-breathe")  == 0) leds_.set_effect(LedManager::Target::STRIP_2, LedManager::Effect::BREATHE);
    else if (strcmp(cmd, "led2-rainbow")  == 0) leds_.set_effect(LedManager::Target::STRIP_2, LedManager::Effect::RAINBOW);
    else if (strcmp(cmd, "led2-chase")    == 0) leds_.set_effect(LedManager::Target::STRIP_2, LedManager::Effect::CHASE);
    else if (strcmp(cmd, "led2-sparkle")  == 0) leds_.set_effect(LedManager::Target::STRIP_2, LedManager::Effect::SPARKLE);
    else if (strcmp(cmd, "led2-wipe")     == 0) leds_.set_effect(LedManager::Target::STRIP_2, LedManager::Effect::WIPE);
    else if (strcmp(cmd, "led2-comet")    == 0) leds_.set_effect(LedManager::Target::STRIP_2, LedManager::Effect::COMET);
    else if (strcmp(cmd, "led-bright-up")  == 0) {
        uint8_t b = leds_.get_brightness(0);
        leds_.set_brightness(LedManager::Target::BOTH, b > 230 ? 255 : b + 25);
    }
    else if (strcmp(cmd, "led-bright-down") == 0) {
        uint8_t b = leds_.get_brightness(0);
        leds_.set_brightness(LedManager::Target::BOTH, b < 25 ? 0 : b - 25);
    }
    else ESP_LOGW(TAG, "Unknown command: %s", cmd);

    // Persist LED state after any LED command (effects, brightness, next)
    if (strncmp(cmd, "led", 3) == 0) {
        save_led_config();
    }
}

// ── JSON status builder ───────────────────────────────────────────────────────

char* WebServer::build_status_json()
{
    net_.refresh_wifi_info();   // update SSID + RSSI before reading
    const NetStatus& net = net_.get_status();
    uint32_t uptime_s  = static_cast<uint32_t>(esp_timer_get_time() / 1'000'000ULL);
    uint32_t free_heap = esp_get_free_heap_size();

    cJSON* root = cJSON_CreateObject();
    if (!root) return nullptr;

    // Time
    {
        time_t now = time(nullptr);
        struct tm tm_info = {};
        localtime_r(&now, &tm_info);
        char tbuf[32], dbuf[32];
        strftime(tbuf, sizeof(tbuf), "%I:%M:%S %p", &tm_info);
        strftime(dbuf, sizeof(dbuf), "%A, %B %d %Y", &tm_info);
        cJSON_AddStringToObject(root, "time", tbuf);
        cJSON_AddStringToObject(root, "date", dbuf);
    }
    cJSON_AddBoolToObject  (root, "sntp",     net.sntp_synced);
    cJSON_AddStringToObject(root, "iana_tz",  net.iana_tz[0]  ? net.iana_tz  : "");
    cJSON_AddStringToObject(root, "posix_tz", net.posix_tz[0] ? net.posix_tz : "");
    cJSON_AddStringToObject(root, "fw_version", OtaManager::running_version());

    // OTA status
    cJSON_AddStringToObject(root, "ota_running",  OtaManager::running_version());
    cJSON_AddStringToObject(root, "ota_latest",   ota_ ? ota_->latest_version()            : "");
    cJSON_AddBoolToObject  (root, "ota_auto",     ota_ ? ota_->is_auto_update_enabled()    : true);
    cJSON_AddBoolToObject  (root, "ota_avail",    ota_ ? ota_->is_update_available()       : false);
    cJSON_AddBoolToObject  (root, "ota_checking", ota_ ? ota_->is_checking()               : false);

    // Matter
    if (matter_) {
        cJSON_AddBoolToObject  (root, "matter_commissioned", matter_->is_commissioned());
        cJSON_AddNumberToObject(root, "matter_fabric_count", matter_->fabric_count());
    }

    // Network / WiFi
    cJSON_AddStringToObject(root, "mdns_hostname", net.mdns_hostname);
    cJSON_AddBoolToObject  (root, "wifi",          net.wifi_connected);
    cJSON_AddStringToObject(root, "ssid",        net.ssid);
    cJSON_AddNumberToObject(root, "rssi",        static_cast<double>(net.rssi));
    cJSON_AddStringToObject(root, "local_ip",    net.local_ip);
    cJSON_AddStringToObject(root, "gateway",     net.gateway);

    // Geolocation
    cJSON_AddStringToObject(root, "external_ip", net.external_ip);
    cJSON_AddStringToObject(root, "city",        net.city);
    cJSON_AddStringToObject(root, "region",      net.region);
    cJSON_AddStringToObject(root, "isp",         net.isp);

    // System
    cJSON_AddNumberToObject(root, "uptime_s",  static_cast<double>(uptime_s));
    cJSON_AddNumberToObject(root, "free_heap", static_cast<double>(free_heap));

    // LED strips
    cJSON* leds_obj = cJSON_AddObjectToObject(root, "leds");
    if (leds_obj) {
        const char* strip_keys[LedManager::STRIP_COUNT] = { "strip1", "strip2" };
        for (int i = 0; i < LedManager::STRIP_COUNT; i++) {
            cJSON* strip = cJSON_AddObjectToObject(leds_obj, strip_keys[i]);
            if (strip) {
                uint8_t r, g, b;
                leds_.get_color(i, r, g, b);
                cJSON_AddStringToObject(strip, "effect",
                    LedManager::effect_name(leds_.get_effect(i)));
                cJSON_AddNumberToObject(strip, "brightness",
                    leds_.get_brightness(i));
                cJSON_AddNumberToObject(strip, "active_len",
                    leds_.get_active_len(i));
                cJSON_AddNumberToObject(strip, "r", r);
                cJSON_AddNumberToObject(strip, "g", g);
                cJSON_AddNumberToObject(strip, "b", b);
            }
        }
    }

    char* result = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return result;  // caller must free()
}
