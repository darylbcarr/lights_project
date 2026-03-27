/**
 * @file console_commands.cpp
 * @brief UART line-oriented command shell
 *
 * Uses only driver/uart.h — no esp_console / argtable3 managed components.
 */

#include "console_commands.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <ctime>
#include "driver/uart.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "config_store.h"
#include "esp_wifi.h"
#include "ota_manager.h"

static const char* TAG = "console";

// ── UART configuration ────────────────────────────────────────────────────────
static constexpr uart_port_t  CONSOLE_UART    = UART_NUM_0;
static constexpr int          CONSOLE_BAUD    = 115200;
static constexpr int          CONSOLE_RX_BUF  = 512;
static constexpr int          CONSOLE_LINE_MAX = 128;

// ── Module-level pointers ─────────────────────────────────────────────────────
static Networking*             s_net        = nullptr;
static MatterBridge*           s_matter     = nullptr;
static OtaManager*             s_ota        = nullptr;
static SemaphoreHandle_t       s_bus_mutex  = nullptr;
static i2c_master_bus_handle_t s_bus_handle = nullptr;

// ── UART helpers ──────────────────────────────────────────────────────────────

static void uart_puts(const char* s)
{
    uart_write_bytes(CONSOLE_UART, s, strlen(s));
}

static void uart_printf(const char* fmt, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    uart_puts(buf);
}

static void prompt()
{
    uart_puts("\r\nlights> ");
}

static int read_line(char* buf, int len)
{
    int pos = 0;
    buf[0]  = '\0';
    while (pos < len - 1) {
        uint8_t c;
        int n = uart_read_bytes(CONSOLE_UART, &c, 1, portMAX_DELAY);
        if (n <= 0) continue;
        if (c == '\r' || c == '\n') { uart_write_bytes(CONSOLE_UART, "\r\n", 2); break; }
        if (c == 0x7F || c == 0x08) {
            if (pos > 0) { --pos; uart_puts("\b \b"); }
            continue;
        }
        if (c >= 0x20 && c < 0x7F) {
            uart_write_bytes(CONSOLE_UART, (char*)&c, 1);
            buf[pos++] = (char)c;
        }
    }
    buf[pos] = '\0';
    return pos;
}

// ── Tokeniser ─────────────────────────────────────────────────────────────────

static int tokenise(char* buf, char* argv[], int max_argc)
{
    int argc = 0;
    char* p  = buf;
    while (*p && argc < max_argc) {
        while (*p == ' ') ++p;
        if (!*p) break;
        argv[argc++] = p;
        while (*p && *p != ' ') ++p;
        if (*p) *p++ = '\0';
    }
    return argc;
}

// ── Help ──────────────────────────────────────────────────────────────────────

static void do_help()
{
    uart_puts(
        "\r\nAvailable commands:\r\n"
        "  net-status              Show network status (IP, geo, RSSI, etc.)\r\n"
        "  i2c-scan                Scan I2C bus and print responding addresses\r\n"
        "  time [<fmt>]            Print current time (optional strftime format)\r\n"
        "  matter-pair             Reopen BLE commissioning window (fast advertising)\r\n"
        "  check-ota               Check GitHub for a firmware update now\r\n"
        "  clear-wifi              Erase stored WiFi credentials and restart\r\n"
        "  help                    Show this list\r\n"
    );
}

// ── net-status printer ────────────────────────────────────────────────────────

static void do_net_status()
{
    if (!s_net) { uart_puts("Networking not available.\r\n"); return; }

    const NetStatus& s = s_net->get_status();

    uart_puts("\r\n──── Network Status ──────────────────────────────────\r\n");

    // WiFi
    uart_printf("  WiFi connected : %s\r\n", s.wifi_connected ? "YES" : "NO");
    if (s.wifi_connected) {
        uart_printf("  SSID           : %s\r\n", s.ssid);
        uart_printf("  RSSI           : %d dBm\r\n", (int)s.rssi);
        uart_printf("  Local IP       : %s\r\n", s.local_ip);
        uart_printf("  Gateway        : %s\r\n", s.gateway);
        uart_printf("  Netmask        : %s\r\n", s.netmask);
        uart_printf("  DNS            : %s\r\n", s.dns_primary);
    }

    // Geolocation
    uart_puts(  "  ─────────────────────────────────────────────────────\r\n");
    if (s.external_ip[0]) {
        uart_printf("  External IP    : %s\r\n", s.external_ip);
        uart_printf("  Location       : %s, %s, %s (%s)\r\n",
                    s.city, s.region, s.country, s.country_code);
        uart_printf("  Coordinates    : %.4f, %.4f\r\n",
                    s.latitude, s.longitude);
        uart_printf("  ISP            : %s\r\n", s.isp);
        uart_printf("  IANA timezone  : %s\r\n", s.iana_tz);
        uart_printf("  POSIX TZ       : %s\r\n", s.posix_tz);
    } else {
        uart_puts("  Geolocation    : not yet available\r\n");
        if (s.posix_tz[0]) {
            uart_printf("  POSIX TZ       : %s  (manual override)\r\n", s.posix_tz);
        }
    }

    // SNTP
    uart_puts(  "  ─────────────────────────────────────────────────────\r\n");
    uart_printf("  SNTP synced    : %s\r\n", s.sntp_synced ? "YES" : "NO");
    if (s.sntp_synced) {
        time_t now = time(nullptr);
        struct tm tm_info = {};
        localtime_r(&now, &tm_info);
        char tbuf[32];
        strftime(tbuf, sizeof(tbuf), "%Y-%m-%dT%H:%M:%S %Z", &tm_info);
        uart_printf("  System time    : %s\r\n", tbuf);
    }
    uart_puts(  "─────────────────────────────────────────────────────\r\n");
}

// ── I2C bus scan ──────────────────────────────────────────────────────────────

static void do_i2c_scan()
{
    if (!s_bus_handle) { uart_puts("I2C bus handle not available.\r\n"); return; }

    uart_puts("\r\nScanning I2C bus (0x01–0x7F)...\r\n");
    uart_puts("     0  1  2  3  4  5  6  7  8  9  A  B  C  D  E  F\r\n");

    int found = 0;
    xSemaphoreTake(s_bus_mutex, portMAX_DELAY);
    for (int row = 0; row < 8; row++) {
        uart_printf("%02X: ", row << 4);
        for (int col = 0; col < 16; col++) {
            uint8_t addr = (uint8_t)((row << 4) | col);
            if (addr < 0x04 || addr > 0x77) {
                // Reserved addresses — skip without probing
                uart_puts("   ");
                continue;
            }
            esp_err_t ret = i2c_master_probe(s_bus_handle, addr, 10);
            if (ret == ESP_OK) {
                uart_printf("%02X ", addr);
                found++;
            } else {
                uart_puts("-- ");
            }
        }
        uart_puts("\r\n");
    }
    xSemaphoreGive(s_bus_mutex);

    if (found == 0) {
        uart_puts("No devices found.\r\n");
    } else {
        uart_printf("%d device(s) found.\r\n", found);
    }
}

// ── Command dispatch ──────────────────────────────────────────────────────────

static void dispatch(char* line)
{
    if (!line || line[0] == '\0') return;

    char* argv[8];
    int   argc = tokenise(line, argv, 8);
    if (argc == 0) return;

    const char* cmd = argv[0];

    if (strcmp(cmd, "net-status") == 0) {
        do_net_status();
        return;
    }
    if (strcmp(cmd, "time") == 0) {
        const char* fmt = (argc >= 2) ? argv[1] : "%Y-%m-%dT%H:%M:%S";
        time_t now = time(nullptr);
        struct tm tm_info = {};
        localtime_r(&now, &tm_info);
        char tbuf[64];
        strftime(tbuf, sizeof(tbuf), fmt, &tm_info);
        uart_printf("%s\r\n", tbuf);
        return;
    }
    // if (strcmp(cmd, "enc-test") == 0) {
    //     if (!s_encoder || !s_bus_mutex) {
    //         uart_puts("Encoder not available.\r\n");
    //         return;
    //     }
    //     int n = (argc >= 2) ? atoi(argv[1]) : 100;
    //     uart_printf("Polling encoder %d times (50ms interval)...\r\n", n);
    //     uart_puts("  [pos_raw] [delta] [btn]\r\n");
    //         int32_t last_pos = 0;
    //     for (int i = 0; i < n; i++) {
    //         xSemaphoreTake(s_bus_mutex, portMAX_DELAY);
    //         int8_t delta = s_encoder->read_delta();
    //         bool   btn   = s_encoder->button_pressed();
    //         xSemaphoreGive(s_bus_mutex);
    //         // Also read raw position for diagnostics
    //         // (read_delta already updated last_pos_ internally)
    //         uart_printf("  delta=%-3d  btn=%d\r\n", (int)delta, (int)btn);
    //         vTaskDelay(pdMS_TO_TICKS(50));
    //     }
    //     uart_puts("enc-test done.\r\n");
    //     return;
    // }
    if (strcmp(cmd, "i2c-scan") == 0) {
        do_i2c_scan();
        return;
    }
    if (strcmp(cmd, "matter-pair") == 0) {
        if (!s_matter) { uart_puts("Matter not available.\r\n"); return; }
        esp_err_t e = s_matter->open_commissioning_window();
        if (e == ESP_OK)
            uart_puts("BLE commissioning window reopened — fast advertising active.\r\n");
        else if (e == ESP_ERR_INVALID_STATE)
            uart_puts("Device already commissioned; remove it from Alexa first.\r\n");
        else
            uart_puts("Failed to open commissioning window (check log).\r\n");
        return;
    }
    if (strcmp(cmd, "check-ota") == 0) {
        if (!s_ota) { uart_puts("OTA not available.\r\n"); return; }
        uart_printf("Running v%s  — checking for updates...\r\n",
                    OtaManager::running_version());
        esp_err_t e = s_ota->check_now();
        if (e == ESP_ERR_INVALID_STATE)
            uart_puts("WiFi not connected.\r\n");
        else if (e != ESP_OK)
            uart_printf("OTA check failed: %s\r\n", esp_err_to_name(e));
        // On success the device either restarted (update applied) or logged
        // "Firmware is up to date" — no extra message needed here.
        return;
    }
    if (strcmp(cmd, "clear-wifi") == 0) {
        // Clear both app NVS (ConfigStore) and WiFi driver NVS.
        // Wiping only one leaves the other copy to reconnect on reboot.
        NetCfg cfg = {};
        ConfigStore::load(cfg);
        cfg.ssid[0]     = '\0';
        cfg.password[0] = '\0';
        ConfigStore::save(cfg);
        wifi_config_t wcfg = {};
        esp_wifi_set_config(WIFI_IF_STA, &wcfg);
        uart_puts("WiFi credentials cleared. Restarting...\r\n");
        vTaskDelay(pdMS_TO_TICKS(200));
        esp_restart();
        return;
    }
    if (strcmp(cmd, "help") == 0) {
        do_help();
        return;
    }

    uart_puts("Unknown command. Type 'help' for a list.\r\n");
}

// ── Console task ──────────────────────────────────────────────────────────────

static void console_task(void* /*arg*/)
{
    char line[CONSOLE_LINE_MAX];
    uart_puts("\r\n=== Clock console ready. Type 'help' for commands. ===\r\n");
    prompt();
    while (true) {
        read_line(line, sizeof(line));
        dispatch(line);
        prompt();
    }
}

// ── Public entry point ────────────────────────────────────────────────────────

void console_start(Networking*             net,
                   MatterBridge*           matter,
                   OtaManager*             ota,
                   SemaphoreHandle_t       bus_mutex,
                   i2c_master_bus_handle_t bus_handle)
{
    s_net        = net;
    s_matter     = matter;
    s_ota        = ota;
    s_bus_mutex  = bus_mutex;
    s_bus_handle = bus_handle;

    uart_config_t cfg = {};
    cfg.baud_rate  = CONSOLE_BAUD;
    cfg.data_bits  = UART_DATA_8_BITS;
    cfg.parity     = UART_PARITY_DISABLE;
    cfg.stop_bits  = UART_STOP_BITS_1;
    cfg.flow_ctrl  = UART_HW_FLOWCTRL_DISABLE;
    cfg.source_clk = UART_SCLK_DEFAULT;

    ESP_ERROR_CHECK(uart_param_config(CONSOLE_UART, &cfg));
    ESP_ERROR_CHECK(uart_driver_install(CONSOLE_UART, CONSOLE_RX_BUF, 0, 0, nullptr, 0));

    xTaskCreate(console_task, "console", 4096, nullptr, 3, nullptr);
    ESP_LOGI(TAG, "UART console started at %d baud", CONSOLE_BAUD);
}
