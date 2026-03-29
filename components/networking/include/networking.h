#pragma once

class EventLog;  // forward declaration — avoids header dependency

/**
 * @file networking.h
 * @brief WiFi station, SNTP time sync, IP geolocation, and network status
 *
 * Lifecycle
 * ─────────
 *  1. begin()         — init NVS, netif, event loop, start WiFi connect
 *  2. WiFi connected  → request IP geolocation → resolve POSIX TZ string
 *                     → apply via setenv("TZ", ...) / tzset()
 *                     → start SNTP
 *  3. SNTP synced     → system time is set
 *  4. Disconnect      → auto-reconnect with exponential back-off
 *
 * Geolocation
 * ───────────
 *  Uses http://ip-api.com/json (HTTP, no API key required).
 *  Returns city, region, country, lat/lon, and an IANA timezone name
 *  (e.g. "America/Chicago").  tz_lookup converts IANA → POSIX TZ string.
 *  If tz_override_ is set it bypasses geolocation entirely.
 *
 * Network status
 * ──────────────
 *  get_status() returns a NetStatus struct with local IP, gateway, DNS,
 *  external IP (from geolocation response), RSSI, SSID, and geo fields.
 */

#include "esp_event.h"
#include "esp_netif.h"
#include <cstdint>
#include <string>

// ── Geolocation / status struct ───────────────────────────────────────────────
struct NetStatus {
    // WiFi
    bool        wifi_connected  = false;
    int8_t      rssi            = 0;
    char        ssid[33]        = {};

    // IP layer
    char        local_ip[16]    = {};
    char        gateway[16]     = {};
    char        netmask[16]     = {};
    char        dns_primary[16] = {};

    // Geolocation (populated after first successful geo query)
    char        external_ip[46] = {};   // supports IPv6 too
    char        city[64]        = {};
    char        region[64]      = {};
    char        country[64]     = {};
    char        country_code[4] = {};
    float       latitude        = 0.0f;
    float       longitude       = 0.0f;
    char        isp[80]         = {};

    // Time
    char        iana_tz[64]     = {};   // e.g. "America/Chicago"
    char        posix_tz[80]    = {};   // e.g. "CST6CDT,M3.2.0,M11.1.0"
    bool        sntp_synced     = false;

    // mDNS
    char        mdns_hostname[32] = {}; // e.g. "clock_a1b2"
};

/// NTP server list (primary, fallback)
static constexpr const char* NTP_SERVER_1 = "pool.ntp.org";
static constexpr const char* NTP_SERVER_2 = "time.google.com";

// ── Networking class ──────────────────────────────────────────────────────────
class Networking {
public:
    Networking();
    ~Networking();

    // ── Configuration (call before begin()) ──────────────────────────────────
    void set_wifi_credentials(const char* ssid, const char* password);

    /**
     * @brief Skip geolocation and use this POSIX TZ string directly.
     *        e.g. "CST6CDT,M3.2.0,M11.1.0"
     */
    void set_timezone_override(const char* tz);

    /**
     * @brief Override the default MAC-derived mDNS hostname before begin().
     *        e.g. "clock_a1b2" → accessible at clock_a1b2.local
     */
    void set_mdns_hostname_hint(const char* name);

    // ── Lifecycle ─────────────────────────────────────────────────────────────
    void begin();

    // ── Status ────────────────────────────────────────────────────────────────
    const NetStatus& get_status() const { return status_; }
    bool is_connected()   const { return status_.wifi_connected; }
    bool is_time_synced() const { return status_.sntp_synced; }

    /** Re-read SSID and RSSI from the driver.  No-op if not connected. */
    void refresh_wifi_info();

    /**
     * @brief Tell Networking whether the device is already Matter-commissioned.
     *        Call before begin().  When true, mDNS is allowed to start even on
     *        the Matter WiFi path (BLE is inactive on reboot after commissioning).
     *        When false (first-time commissioning, BLE still active), mDNS is
     *        suppressed to prevent packet-buffer conflict with CHIP's mDNS stack.
     */
    void set_matter_commissioned(bool commissioned) { matter_commissioned_ = commissioned; }

    /** Wire in an EventLog to receive WiFi/SNTP startup events. Call before begin(). */
    void set_event_log(EventLog* log) { event_log_ = log; }

    /**
     * @brief Live-update the mDNS hostname after begin() — no restart needed.
     *        Sends goodbye for old name and announces the new one immediately.
     *        Persistence is the caller's responsibility (save to NetCfg/ConfigStore).
     */
    void set_mdns_hostname(const char* name);

private:
    // ── Event handlers (static trampoline → instance method) ─────────────────
    static void s_wifi_event_handler(void* arg, esp_event_base_t base,
                                     int32_t id, void* data);
    static void s_ip_event_handler(void* arg, esp_event_base_t base,
                                   int32_t id, void* data);
    static void s_sntp_sync_cb(struct timeval* tv);

    void on_wifi_connected();
    void on_wifi_disconnected();
    void on_got_ip(esp_netif_ip_info_t* ip_info);

    // ── Internal helpers ──────────────────────────────────────────────────────
    void start_sntp();
    void start_mdns();
    void do_geolocation();          // runs in a short-lived task
    bool fetch_geolocation();       // HTTP GET ip-api.com, parse JSON
    void apply_timezone(const char* iana_tz);
    void populate_dns();

    static void geo_task(void* arg);  // FreeRTOS task wrapper for do_geolocation
    static void mdns_task(void* arg); // FreeRTOS task wrapper for start_mdns

    // ── State ─────────────────────────────────────────────────────────────────
    esp_netif_t*    netif_              = nullptr;
    NetStatus       status_             = {};
    char            ssid_[33]           = {};
    char            password_[65]       = {};
    char            tz_override_[80]    = {};
    char            mdns_hostname_hint_[32] = {};
    int             retry_count_        = 0;

    static constexpr int MAX_RETRY = 10;
    static constexpr int GEO_HTTP_TIMEOUT_MS = 10000;

    bool begun_               = false;  // guards against double-call from Matter path
    bool matter_commissioned_ = false;  // set by main before begin(); enables mDNS on Matter path
    bool mdns_delegate_mode_  = false;  // true when Matter owns mDNS and we use a delegate hostname
    EventLog* event_log_      = nullptr;

    // Module-static pointer so the SNTP callback (C linkage) can reach us
    static Networking* s_instance_;
};
