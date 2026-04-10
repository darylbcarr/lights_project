/**
 * @file networking.cpp
 * @brief WiFi STA, SNTP, IP geolocation, network status
 */

#include "networking.h"
#include "tz_lookup.h"
#include "event_log.h"

#include <cstring>
#include <cstdio>
#include <cstdlib>
#include "esp_log.h"
#include "esp_wifi.h"
#include "mdns.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_sntp.h"
#include "esp_http_client.h"
#include "nvs_flash.h"
#include "lwip/dns.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_coexist.h"

static const char* TAG = "networking";

// ── Module-static instance pointer for C-linkage callbacks ───────────────────
Networking* Networking::s_instance_ = nullptr;


// ── HTTP receive buffer ───────────────────────────────────────────────────────
static constexpr int HTTP_BUF_SIZE = 2048;

// ── Constructor / Destructor ──────────────────────────────────────────────────

Networking::Networking()
{
    s_instance_ = this;
}

Networking::~Networking()
{
    esp_sntp_stop();
    s_instance_ = nullptr;
}

// ── Configuration ─────────────────────────────────────────────────────────────

void Networking::set_wifi_credentials(const char* ssid, const char* password)
{
    strncpy(ssid_,     ssid,     sizeof(ssid_)     - 1);
    strncpy(password_, password, sizeof(password_) - 1);
    ESP_LOGI(TAG, "WiFi credentials stored for SSID: %s", ssid_);
}

void Networking::set_timezone_override(const char* tz)
{
    strncpy(tz_override_, tz, sizeof(tz_override_) - 1);
    ESP_LOGI(TAG, "Timezone override: %s", tz_override_);
}

void Networking::set_mdns_hostname_hint(const char* name)
{
    if (name) strncpy(mdns_hostname_hint_, name, sizeof(mdns_hostname_hint_) - 1);
}

// ── begin() ───────────────────────────────────────────────────────────────────

void Networking::begin()
{
    if (begun_) {
        ESP_LOGI(TAG, "Networking::begin() already called — skipping");
        return;
    }
    begun_ = true;

    ESP_LOGI(TAG, "Networking::begin()");

    // NVS is initialised by main.cpp before begin() is called.

    // 1. TCP/IP stack
    // esp_netif_init() is idempotent in ESP-IDF 5.x.
    ESP_ERROR_CHECK(esp_netif_init());

    // 2. Default event loop
    // ESP_ERR_INVALID_STATE = already created by esp_matter::start() — use it.
    {
        esp_err_t e = esp_event_loop_create_default();
        if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) ESP_ERROR_CHECK(e);
    }

    // 3. Default STA netif
    // esp_matter::start() creates "WIFI_STA_DEF" before we reach here; reuse it.
    netif_ = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (!netif_) {
        netif_ = esp_netif_create_default_wifi_sta();
    }

    // 4. WiFi driver
    // Skip silently if Matter already called esp_wifi_init().
    {
        wifi_init_config_t wifi_cfg = WIFI_INIT_CONFIG_DEFAULT();
        esp_err_t e = esp_wifi_init(&wifi_cfg);
        if (e != ESP_OK) {
            ESP_LOGW(TAG, "esp_wifi_init: %s (already initialised by Matter)",
                     esp_err_to_name(e));
        }
    }

    // 4b. Pre-populate mDNS hostname in status so /api/status and the WebSocket
    //     always return the real name, even before WiFi connects and mdns_task runs.
    //     Works on both WiFi and Matter paths — MAC is readable after esp_wifi_init.
    {
        char hostname[32] = {};
        if (mdns_hostname_hint_[0] != '\0') {
            strncpy(hostname, mdns_hostname_hint_, sizeof(hostname) - 1);
        } else {
            uint8_t mac[6] = {};
            if (esp_wifi_get_mac(WIFI_IF_STA, mac) == ESP_OK) {
                snprintf(hostname, sizeof(hostname), "clock_%02x%02x", mac[4], mac[5]);
            }
        }
        if (hostname[0] != '\0') {
            strncpy(status_.mdns_hostname, hostname, sizeof(status_.mdns_hostname) - 1);
            ESP_LOGI(TAG, "mDNS hostname (pre-start): %s", hostname);
        }
    }

    // 5. Register event handlers
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID,
        s_wifi_event_handler, this, nullptr));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP,
        s_ip_event_handler, this, nullptr));

    // 6. Configure SNTP before any path that might call start_sntp().
    //    Guard against double-init: Matter or a prior begin() may have already
    //    started SNTP (esp_sntp_setoperatingmode asserts if called while running).
    if (!esp_sntp_enabled()) {
        esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
        esp_sntp_setservername(0, NTP_SERVER_1);
        esp_sntp_setservername(1, NTP_SERVER_2);
        sntp_set_time_sync_notification_cb(s_sntp_sync_cb);
    }

    // 7. Connect WiFi or bootstrap from Matter's existing connection.
    //    If SSID is empty, Matter manages WiFi; we skip connect but still
    //    watch for the IP event so SNTP can start when Matter gets an address.
    if (ssid_[0] != '\0') {
        wifi_config_t cfg = {};
        strncpy((char*)cfg.sta.ssid,     ssid_,     sizeof(cfg.sta.ssid)     - 1);
        strncpy((char*)cfg.sta.password, password_, sizeof(cfg.sta.password) - 1);
        cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
        cfg.sta.pmf_cfg.capable    = true;
        cfg.sta.pmf_cfg.required   = false;

        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &cfg));

        // If Matter already started WiFi, WIFI_EVENT_STA_START won't fire
        // again — call connect() directly in that case.
        esp_err_t e = esp_wifi_start();
        if (e != ESP_OK) {
            ESP_LOGW(TAG, "esp_wifi_start: %s (already started by Matter) — connecting directly",
                     esp_err_to_name(e));
            esp_wifi_connect();
        }
        // On ESP_OK, WIFI_EVENT_STA_START fires → s_wifi_event_handler → connect()
    } else {
        ESP_LOGI(TAG, "No SSID configured — Matter manages WiFi");
        // Give WiFi higher coex priority so it can complete the 4-way
        // association handshake without timing out due to BLE occupying the
        // radio during BLE commissioning.  BLE still works with lower priority
        // (its indications are retried automatically), just at higher latency.
        // Restored to ESP_COEX_PREFER_BALANCE once WiFi has an IP.
        esp_coex_preference_set(ESP_COEX_PREFER_WIFI);
        ESP_LOGI(TAG, "Coex preference → WIFI (Matter BLE commissioning path)");

        // Ensure WiFi STA is in the right mode and started so CHIP's
        // NetworkCommissioning driver can use it during BLE commissioning.
        // esp_matter::start() should have done this, but being explicit here
        // avoids a race where our begin() is called before CHIP's async WiFi
        // init task has completed.
        {
            esp_err_t e = esp_wifi_set_mode(WIFI_MODE_STA);
            if (e != ESP_OK)
                ESP_LOGW(TAG, "WiFi set_mode STA (Matter): %s", esp_err_to_name(e));
            e = esp_wifi_start();
            if (e != ESP_OK)
                ESP_LOGW(TAG, "WiFi start (Matter): %s (may already be started)",
                         esp_err_to_name(e));
        }
        // If Matter already obtained an IP before we registered our event handler,
        // bootstrap SNTP now instead of waiting for an event that already fired.
        esp_netif_ip_info_t ip_info = {};
        if (netif_ && esp_netif_get_ip_info(netif_, &ip_info) == ESP_OK &&
            ip_info.ip.addr != 0) {
            ESP_LOGI(TAG, "Matter already has IP — bootstrapping SNTP");
            on_got_ip(&ip_info);
        }
    }

    // 8. Apply timezone override immediately if set
    if (tz_override_[0] != '\0') {
        setenv("TZ", tz_override_, 1);
        tzset();
        strncpy(status_.posix_tz, tz_override_, sizeof(status_.posix_tz) - 1);
    }

    ESP_LOGI(TAG, "WiFi started — connecting to '%s'...", ssid_);
}

// ── WiFi event handler ────────────────────────────────────────────────────────

void Networking::s_wifi_event_handler(void* arg, esp_event_base_t /*base*/,
                                      int32_t id, void* /*data*/)
{
    Networking* self = static_cast<Networking*>(arg);

    if (id == WIFI_EVENT_STA_START) {
        if (self->ssid_[0] != '\0') {
            esp_wifi_connect();
        }
    } else if (id == WIFI_EVENT_STA_CONNECTED) {
        self->retry_count_ = 0;
        wifi_ap_record_t ap = {};
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
            strncpy(self->status_.ssid, (char*)ap.ssid,
                    sizeof(self->status_.ssid) - 1);
            self->status_.rssi = ap.rssi;
        }
        ESP_LOGI(TAG, "WiFi associated with '%s'  RSSI=%d dBm",
                 self->status_.ssid, self->status_.rssi);
    } else if (id == WIFI_EVENT_STA_DISCONNECTED) {
        self->on_wifi_disconnected();
    }
}

// ── IP event handler ──────────────────────────────────────────────────────────

void Networking::s_ip_event_handler(void* arg, esp_event_base_t /*base*/,
                                    int32_t id, void* data)
{
    Networking* self = static_cast<Networking*>(arg);
    if (id == IP_EVENT_STA_GOT_IP) {
        auto* event = static_cast<ip_event_got_ip_t*>(data);
        self->on_got_ip(&event->ip_info);
    }
}

// ── SNTP sync callback (C linkage) ────────────────────────────────────────────

void Networking::s_sntp_sync_cb(struct timeval* /*tv*/)
{
    if (s_instance_) {
        const bool first_sync = !s_instance_->status_.sntp_synced;
        s_instance_->status_.sntp_synced = true;
        ESP_LOGI(TAG, "SNTP synchronised");
        if (first_sync && s_instance_->event_log_) {
            s_instance_->event_log_->log(EventLog::CAT_STARTUP, "SNTP: time synced");
        }
    }
}

// ── Connection events ─────────────────────────────────────────────────────────

void Networking::on_got_ip(esp_netif_ip_info_t* ip_info)
{
    status_.wifi_connected = true;

    snprintf(status_.local_ip,  sizeof(status_.local_ip),
             IPSTR, IP2STR(&ip_info->ip));
    snprintf(status_.gateway,   sizeof(status_.gateway),
             IPSTR, IP2STR(&ip_info->gw));
    snprintf(status_.netmask,   sizeof(status_.netmask),
             IPSTR, IP2STR(&ip_info->netmask));

    populate_dns();
    refresh_wifi_info();   // reliable point to read SSID + RSSI (IP fully established)

    ESP_LOGI(TAG, "Got IP  local=%s  gw=%s  ssid=%s  rssi=%d dBm",
             status_.local_ip, status_.gateway, status_.ssid, status_.rssi);

    if (event_log_) {
        event_log_->log(EventLog::CAT_STARTUP, "WiFi: %s  IP=%s",
                        status_.ssid, status_.local_ip);
    }

    // WiFi connected — restore balanced coex so BLE/Matter-over-IP coexist fairly
    if (ssid_[0] == '\0') {
        esp_coex_preference_set(ESP_COEX_PREFER_BALANCE);
        ESP_LOGI(TAG, "Coex preference → BALANCE (WiFi connected)");
    }

    // Start SNTP now that we have IP
    start_sntp();

    // Launch geolocation and mDNS in separate tasks — both must not run
    // inside the event handler callback (mdns_init registers its own event
    // handlers and blocks briefly; doing so here can deadlock the event loop).
    xTaskCreate(geo_task,  "geo",      4096, this, 4, nullptr);
    xTaskCreate(mdns_task, "net_mdns", 4096, this, 3, nullptr);
}

void Networking::on_wifi_disconnected()
{
    status_.wifi_connected = false;
    status_.rssi           = 0;

    // When SSID is empty, Matter's NetworkCommissioning driver manages WiFi.
    // Let CHIP handle its own reconnect cycle (it retries after
    // CONFIG_WIFI_STATION_RECONNECT_INTERVAL = 1000 ms, which is long enough
    // for the coex module's internal reconnect lock to clear naturally).
    // Do NOT call esp_wifi_connect() here and do NOT restart the driver:
    // stop/start clears the WiFi AP scan cache, forcing a fresh channel scan
    // on every connect attempt — that scan takes 1–3 s and consumes the
    // post-AddNOC BLE quiet windows before authentication can start.
    if (ssid_[0] == '\0') {
        ESP_LOGD(TAG, "WiFi disconnected — Matter manages reconnect");
        return;
    }

    ESP_LOGW(TAG, "WiFi disconnected (retry %d/%d)",
             retry_count_, MAX_RETRY);
    if (event_log_) {
        event_log_->log(EventLog::CAT_STARTUP, "WiFi: disconnected");
    }

    if (retry_count_ < MAX_RETRY) {
        ++retry_count_;
        // Simple exponential back-off: 1s, 2s, 4s … capped at 30s
        uint32_t delay_ms = std::min(1000u << (retry_count_ - 1), 30000u);
        ESP_LOGI(TAG, "Reconnecting in %lu ms...", (unsigned long)delay_ms);
        vTaskDelay(pdMS_TO_TICKS(delay_ms));
        esp_wifi_connect();
    } else {
        ESP_LOGE(TAG, "Max retries reached. WiFi giving up.");
    }
}

// ── mDNS ──────────────────────────────────────────────────────────────────────

void Networking::start_mdns()
{
    // Derive hostname: saved hint → fall back to "clock_XXXX" from last 2 MAC bytes.
    char hostname[32] = {};
    if (mdns_hostname_hint_[0] != '\0') {
        strncpy(hostname, mdns_hostname_hint_, sizeof(hostname) - 1);
    } else {
        uint8_t mac[6] = {};
        esp_wifi_get_mac(WIFI_IF_STA, mac);
        snprintf(hostname, sizeof(hostname), "clock_%02x%02x", mac[4], mac[5]);
    }

    // With CONFIG_USE_MINIMAL_MDNS=n, Matter calls mdns_init() via EspDnssdInit()
    // during chip::Server::Init() (at boot, before WiFi connects).  Our call here
    // gets ESP_ERR_INVALID_STATE on the Matter path — that is expected and correct.
    // On the direct-WiFi path (no Matter), our call succeeds and we own mDNS.
    esp_err_t err = mdns_init();
    const bool we_own = (err == ESP_OK);
    mdns_delegate_mode_ = !we_own;  // watchdog re-uses this to know delegate mode

    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "mDNS init failed: %s", esp_err_to_name(err));
        return;
    }

    if (we_own) {
        // Our mdns_init() was called after the IP event already fired so the
        // predefined event handler missed the IP_EVENT_GOT_IP.  We must enable
        // the PCB manually via mdns_netif_action(ENABLE_IP4).
        //
        // ORDERING MATTERS: set hostname and services BEFORE enabling the PCB.
        // _mdns_enable_pcb() calls _mdns_probe_all_pcbs() internally; if the
        // hostname is already stored at that moment the probe starts immediately
        // with the correct name.  Enabling first (old order) queues the PCB
        // creation in the mDNS daemon task; mdns_hostname_set then runs before
        // the daemon processes the enable — the hostname is stored but there is
        // no PCB yet to probe, so mDNS never announces itself until something
        // external (e.g. a website hostname-change call) retriggers it.

        // 1. Set hostname and instance name.
        mdns_hostname_set(hostname);
        mdns_instance_name_set(hostname);

        // 2. Register HTTP service.
        esp_err_t svc = mdns_service_add(nullptr, "_http", "_tcp", 80, nullptr, 0);
        if (svc != ESP_OK && svc != ESP_ERR_INVALID_ARG) {
            ESP_LOGW(TAG, "mdns_service_add: %s", esp_err_to_name(svc));
        }

        // 3. Enable the PCB last — daemon creates it and immediately probes.
        if (netif_) {
            mdns_netif_action(netif_, MDNS_EVENT_ENABLE_IP4);
        }
        ESP_LOGI(TAG, "mDNS started: %s.local", hostname);

        // ── Matter path race: chip[DIS] will overwrite our global hostname ────
        // When ssid_ is empty, Matter manages WiFi. Our mdns_init() occasionally
        // wins the race against chip[DIS]'s init, so we land here even though
        // chip[DIS] will later call mdns_hostname_set("chip-XXXXXXXXXXXX...").
        // That overwrites our global hostname, making "testa.local" unreachable.
        // Poll for that change and immediately re-register our name as a delegate.
        if (ssid_[0] == '\0') {
            ESP_LOGI(TAG, "mDNS: Matter path — polling for chip[DIS] hostname override");
            for (int i = 0; i < 300; ++i) {   // 300 × 100 ms = 30 s max
                vTaskDelay(pdMS_TO_TICKS(100));
                char cur[MDNS_NAME_BUF_LEN] = {};
                if (mdns_hostname_get(cur) == ESP_OK &&
                        cur[0] != '\0' &&
                        strcasecmp(cur, hostname) != 0) {
                    ESP_LOGI(TAG, "mDNS: chip[DIS] set hostname '%s'; registering "
                             "'%s' as delegate", cur, hostname);
                    esp_netif_ip_info_t ip_info = {};
                    if (netif_ && esp_netif_get_ip_info(netif_, &ip_info) == ESP_OK
                            && ip_info.ip.addr != 0) {
                        mdns_ip_addr_t ip_entry = {};
                        ip_entry.addr.type       = ESP_IPADDR_TYPE_V4;
                        ip_entry.addr.u_addr.ip4 = ip_info.ip;
                        ip_entry.next            = nullptr;
                        esp_err_t dh = mdns_delegate_hostname_add(hostname, &ip_entry);
                        vTaskDelay(pdMS_TO_TICKS(200));
                        bool exists = mdns_hostname_exists(hostname);
                        ESP_LOGI(TAG, "mDNS delegate '%s.local' → %s (exists=%d)",
                                 hostname, status_.local_ip, (int)exists);
                        if (!exists) {
                            ESP_LOGW(TAG, "mDNS delegate not confirmed — watchdog will retry");
                        }
                        (void)dh;
                    }
                    break;
                }
            }
        }
    } else {
        // ── Matter path: Matter owns mDNS hostname (operational node ad) ─────
        // Register our custom name as a *delegated* hostname so that
        // clock.local (or whatever the user chose) resolves to the device IP
        // without touching the main hostname that Matter's chip[DIS] uses.
        //
        // WHY THE DELAY:
        // Delegate hostnames only respond when the mDNS PCB is past PROBE_3
        // (mdns_priv_pcb_is_after_probing() returns true from ANNOUNCE_1 onward).
        // The PCB probe cycle (~870ms to ANNOUNCE_1, ~3.1s to RUNNING) starts when
        // WiFi connects.  We wait 5 seconds to make sure it reaches RUNNING before
        // registering the delegate.
        //
        // Note: the EspDnssdPublishService() dedup patches (hostname + service)
        // in ESP32DnssdImpl.cpp prevent chip[DIS] from restarting the PCB probe
        // cycle on subsequent Advertise() calls once the service already exists.
        //
        // NOTE: register the _http service NOW (before the delay) so it is
        // included in the initial probe cycle and does not restart the PCB
        // after we add the delegate.
        {
            esp_err_t svc = mdns_service_add(nullptr, "_http", "_tcp", 80, nullptr, 0);
            if (svc != ESP_OK && svc != ESP_ERR_INVALID_ARG) {
                ESP_LOGW(TAG, "mdns_service_add: %s", esp_err_to_name(svc));
            }
        }

        // Additionally: mdns_receive.c is_ours() requires the global hostname
        // (set by chip[DIS] via mdns_hostname_set) to be non-empty before it will
        // respond to ANY host query including delegates.  Poll for it first.

        // Step 1: confirm chip[DIS] has set the global hostname (up to 5 s).
        bool hostname_ready = false;
        {
            char global_host[MDNS_NAME_BUF_LEN] = {};
            for (int i = 0; i < 50; ++i) {
                if (mdns_hostname_get(global_host) == ESP_OK && global_host[0] != '\0') {
                    hostname_ready = true;
                    ESP_LOGI(TAG, "mDNS: global hostname '%s' confirmed (chip[DIS])", global_host);
                    break;
                }
                global_host[0] = '\0';
                vTaskDelay(pdMS_TO_TICKS(100));
            }
            if (!hostname_ready) {
                ESP_LOGW(TAG, "mDNS: global hostname not set after 5 s — skipping delegate");
            }
        }

        if (hostname_ready) {
            // Step 2: wait for PCB probe cycle to complete.
            // The probe cycle starts at WiFi connect and reaches RUNNING in ~3.1 s.
            // 5 s gives comfortable margin.
            ESP_LOGI(TAG, "mDNS: waiting 5 s for PCB probe cycle to complete...");
            vTaskDelay(pdMS_TO_TICKS(5000));

            // Step 3: read IP now (after the delay, in case DHCP renewed).
            esp_netif_ip_info_t ip_info = {};
            if (netif_ && esp_netif_get_ip_info(netif_, &ip_info) == ESP_OK
                    && ip_info.ip.addr != 0) {
                mdns_ip_addr_t ip_entry = {};
                ip_entry.addr.type       = ESP_IPADDR_TYPE_V4;
                ip_entry.addr.u_addr.ip4 = ip_info.ip;
                ip_entry.next            = nullptr;
                mdns_delegate_hostname_remove(hostname); // clean up stale entry on reconnect
                esp_err_t dh = mdns_delegate_hostname_add(hostname, &ip_entry);
                if (dh != ESP_OK) {
                    ESP_LOGW(TAG, "mDNS delegate add failed: %s", esp_err_to_name(dh));
                } else {
                    // Brief yield so the ADD action is processed before we read back.
                    vTaskDelay(pdMS_TO_TICKS(200));
                    bool exists = mdns_hostname_exists(hostname);
                    ESP_LOGI(TAG, "mDNS delegate '%s.local' → %s (exists=%d)",
                             hostname, status_.local_ip, (int)exists);
                    if (!exists) {
                        ESP_LOGW(TAG, "mDNS delegate NOT confirmed in host_list — "
                                 "watchdog will retry");
                    }
                }
            } else {
                ESP_LOGW(TAG, "mDNS: no IP after delay — delegate not registered");
            }
        }
    }

    strncpy(status_.mdns_hostname, hostname, sizeof(status_.mdns_hostname) - 1);
}

void Networking::set_mdns_hostname(const char* name)
{
    if (!name || name[0] == '\0') return;

    if (ssid_[0] == '\0') {
        // Matter path: operate on the delegated hostname, not the main hostname.
        if (mdns_hostname_hint_[0] != '\0') {
            mdns_delegate_hostname_remove(mdns_hostname_hint_);
        }
        esp_netif_ip_info_t ip_info = {};
        if (netif_ && esp_netif_get_ip_info(netif_, &ip_info) == ESP_OK
                && ip_info.ip.addr != 0) {
            mdns_ip_addr_t ip_entry = {};
            ip_entry.addr.type       = ESP_IPADDR_TYPE_V4;
            ip_entry.addr.u_addr.ip4 = ip_info.ip;
            ip_entry.next            = nullptr;
            mdns_delegate_hostname_add(name, &ip_entry);
        }
    } else {
        // Direct WiFi: we own the hostname.
        mdns_hostname_set(name);
    }

    strncpy(mdns_hostname_hint_, name, sizeof(mdns_hostname_hint_) - 1);
    strncpy(status_.mdns_hostname, name, sizeof(status_.mdns_hostname) - 1);
    ESP_LOGI(TAG, "mDNS hostname updated: %s.local", name);
}

// ── SNTP ──────────────────────────────────────────────────────────────────────

void Networking::start_sntp()
{
    if (esp_sntp_enabled()) {
        ESP_LOGI(TAG, "SNTP already running — skipping init");
        return;
    }
    esp_sntp_init();
    // ESP_LOGI(TAG, "SNTP started (servers: %s, %s)", NTP_SERVER_1, NTP_SERVER_2);
}

// ── Geolocation ───────────────────────────────────────────────────────────────

void Networking::geo_task(void* arg)
{
    static_cast<Networking*>(arg)->do_geolocation();
    vTaskDelete(nullptr);
}

void Networking::mdns_task(void* arg)
{
    auto* self = static_cast<Networking*>(arg);
    self->start_mdns();

    // On the Matter path, become a watchdog: re-register the delegate hostname
    // every 30 s if something (mDNS re-init, PCB restart, Matter re-advertise)
    // ever removes it.  This is the recovery path that covers all unknown removal
    // scenarios without needing to pinpoint the exact culprit.
    // Run watchdog whenever Matter owns mDNS and we registered a delegate hostname.
    // Originally guarded by ssid_[0]=='\0' (pure Matter-WiFi path), but Matter
    // now also starts on WiFi-only devices — use the delegate-mode flag instead.
    if (self->mdns_delegate_mode_) {
        for (;;) {
            vTaskDelay(pdMS_TO_TICKS(10000));

            const char* hostname = self->status_.mdns_hostname;
            if (hostname[0] == '\0') continue;

            if (!mdns_hostname_exists(hostname)) {
                ESP_LOGW(TAG, "mDNS delegate '%s' gone — re-registering", hostname);
                esp_netif_ip_info_t ip_info = {};
                if (self->netif_ &&
                        esp_netif_get_ip_info(self->netif_, &ip_info) == ESP_OK &&
                        ip_info.ip.addr != 0) {
                    mdns_ip_addr_t ip_entry = {};
                    ip_entry.addr.type       = ESP_IPADDR_TYPE_V4;
                    ip_entry.addr.u_addr.ip4 = ip_info.ip;
                    ip_entry.next            = nullptr;
                    esp_err_t dh = mdns_delegate_hostname_add(hostname, &ip_entry);
                    ESP_LOGI(TAG, "mDNS delegate re-register '%s': %s",
                             hostname, esp_err_to_name(dh));
                } else {
                    ESP_LOGW(TAG, "mDNS delegate watchdog: no IP yet, skipping");
                }
            } else {
                ESP_LOGD(TAG, "mDNS delegate '%s' OK", hostname);
            }
        }
        // unreachable — task never deletes itself on Matter path
    }

    vTaskDelete(nullptr);
}

void Networking::do_geolocation()
{
    // If user supplied a manual override, skip geolocation but still log
    if (tz_override_[0] != '\0') {
        ESP_LOGI(TAG, "Timezone override active (%s) — skipping geolocation",
                 tz_override_);
        return;
    }

    // Try immediately; on failure, retry with increasing delays.
    // Retries handle transient DNS failures after WiFi reconnect (e.g.
    // after BLE commissioning where WiFi just reconnected from coex instability).
    static constexpr int RETRY_DELAYS_S[] = { 5, 15, 30 };
    static constexpr int MAX_ATTEMPTS = 1 + 3;

    // Wait for heap to recover before making any HTTP request.
    // During BLE commissioning heap can drop to tens of bytes; plain HTTP needs
    // ~8 KB.  60 KB is well above the danger zone and below the ~81 KB stable
    // Matter-mode heap, so this check passes immediately post-commissioning.
    static constexpr size_t HEAP_MIN_BYTES = 60 * 1024;
    for (int waited = 0; esp_get_free_heap_size() < HEAP_MIN_BYTES && waited < 30; ++waited) {
        ESP_LOGW(TAG, "Geolocation: low heap (%lu B) — waiting 1s",
                 (unsigned long)esp_get_free_heap_size());
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    for (int i = 0; i < MAX_ATTEMPTS; ++i) {
        if (i > 0) {
            ESP_LOGI(TAG, "Geolocation retry %d/%d in %ds...",
                     i, MAX_ATTEMPTS - 1, RETRY_DELAYS_S[i - 1]);
            vTaskDelay(pdMS_TO_TICKS(RETRY_DELAYS_S[i - 1] * 1000));
        }
        if (fetch_geolocation()) {
            return;
        }
        ESP_LOGW(TAG, "Geolocation attempt %d/%d failed", i + 1, MAX_ATTEMPTS);
    }
    ESP_LOGE(TAG, "Geolocation failed after all retries — timezone will remain UTC");
}

bool Networking::fetch_geolocation()
{
    // ip-api.com returns JSON with all fields we need, free, no key.
    // Fields: status,city,regionName,country,countryCode,
    //         lat,lon,isp,query,timezone
    static const char* GEO_URL =
        "http://ip-api.com/json"
        "?fields=status,city,regionName,country,countryCode,"
        "lat,lon,isp,query,timezone";

    ESP_LOGI(TAG, "Geolocation: querying %s", GEO_URL);

    // Heap-allocate receive buffer
    char* buf = static_cast<char*>(malloc(HTTP_BUF_SIZE));
    if (!buf) {
        ESP_LOGE(TAG, "Geolocation: out of memory");
        return false;
    }
    int buf_pos = 0;
    memset(buf, 0, HTTP_BUF_SIZE);

    esp_http_client_config_t cfg = {};
    cfg.url             = GEO_URL;
    cfg.timeout_ms      = GEO_HTTP_TIMEOUT_MS;
    cfg.method          = HTTP_METHOD_GET;

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        ESP_LOGE(TAG, "Geolocation: http client init failed");
        free(buf);
        return false;
    }

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Geolocation: open failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        free(buf);
        return false;
    }

    int content_len = esp_http_client_fetch_headers(client);
    if (content_len < 0) {
        ESP_LOGW(TAG, "Geolocation: unknown content length, reading anyway");
    }

    int read_len = esp_http_client_read(client,
                                        buf + buf_pos,
                                        HTTP_BUF_SIZE - buf_pos - 1);
    if (read_len > 0) buf_pos += read_len;
    buf[buf_pos] = '\0';

    int http_status = esp_http_client_get_status_code(client);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (http_status != 200) {
        ESP_LOGE(TAG, "Geolocation: HTTP %d", http_status);
        free(buf);
        return false;
    }

    ESP_LOGD(TAG, "Geolocation response: %s", buf);

    // ── Parse JSON ────────────────────────────────────────────────────────────
    cJSON* root = cJSON_Parse(buf);
    free(buf);

    if (!root) {
        ESP_LOGE(TAG, "Geolocation: JSON parse failed");
        return false;
    }

    auto str = [&](const char* key, char* dest, size_t dsize) {
        cJSON* item = cJSON_GetObjectItemCaseSensitive(root, key);
        if (cJSON_IsString(item) && item->valuestring) {
            strncpy(dest, item->valuestring, dsize - 1);
        }
    };

    cJSON* status_item = cJSON_GetObjectItemCaseSensitive(root, "status");
    if (!cJSON_IsString(status_item) ||
        strcmp(status_item->valuestring, "success") != 0) {
        ESP_LOGE(TAG, "Geolocation: API returned non-success status");
        cJSON_Delete(root);
        return false;
    }

    str("query",       status_.external_ip,  sizeof(status_.external_ip));
    str("city",        status_.city,          sizeof(status_.city));
    str("regionName",  status_.region,        sizeof(status_.region));
    str("country",     status_.country,       sizeof(status_.country));
    str("countryCode", status_.country_code,  sizeof(status_.country_code));
    str("isp",         status_.isp,           sizeof(status_.isp));
    str("timezone",    status_.iana_tz,       sizeof(status_.iana_tz));

    cJSON* lat = cJSON_GetObjectItemCaseSensitive(root, "lat");
    cJSON* lon = cJSON_GetObjectItemCaseSensitive(root, "lon");
    if (cJSON_IsNumber(lat)) status_.latitude  = (float)lat->valuedouble;
    if (cJSON_IsNumber(lon)) status_.longitude = (float)lon->valuedouble;

    cJSON_Delete(root);

    ESP_LOGI(TAG, "Geolocation: %s, %s, %s (%s)  lat=%.2f lon=%.2f",
             status_.city, status_.region, status_.country,
             status_.country_code, status_.latitude, status_.longitude);
    ESP_LOGI(TAG, "Geolocation: external IP=%s  ISP=%s",
             status_.external_ip, status_.isp);
    ESP_LOGI(TAG, "Geolocation: IANA timezone=%s", status_.iana_tz);

    // ── Resolve IANA → POSIX TZ ───────────────────────────────────────────────
    apply_timezone(status_.iana_tz);
    return true;
}

void Networking::apply_timezone(const char* iana_tz)
{
    const char* posix = tz_lookup(iana_tz);
    if (posix && posix[0] != '\0') {
        strncpy(status_.posix_tz, posix, sizeof(status_.posix_tz) - 1);
        ESP_LOGI(TAG, "Timezone resolved: %s → %s", iana_tz, posix);
        setenv("TZ", posix, 1);
        tzset();
    } else {
        ESP_LOGW(TAG, "Timezone lookup failed for '%s' — using UTC", iana_tz);
        setenv("TZ", "UTC0", 1);
        tzset();
        strncpy(status_.posix_tz, "UTC0", sizeof(status_.posix_tz) - 1);
    }
}

// ── Helpers ───────────────────────────────────────────────────────────────────

void Networking::populate_dns()
{
    const ip_addr_t* dns = dns_getserver(0);
    if (dns && !ip_addr_isany(dns)) {
        snprintf(status_.dns_primary, sizeof(status_.dns_primary),
                 IPSTR, IP2STR(&dns->u_addr.ip4));
    }
}

void Networking::refresh_wifi_info()
{
    if (!status_.wifi_connected) return;
    wifi_ap_record_t ap = {};
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        status_.rssi = ap.rssi;
        if (ap.ssid[0] != '\0')
            strncpy(status_.ssid, (char*)ap.ssid, sizeof(status_.ssid) - 1);
    }
}
