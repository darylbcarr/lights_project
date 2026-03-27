/**
 * @file menu.cpp
 * @brief Full menu system wired to Networking and LedManager
 *
 * Menu structure:
 *   Main
 *   ├── Clock
 *   │   ├── Set Time          (set-time using current SNTP)
 *   │   ├── Stop Movement     (cancel in-progress motor move)
 *   │   ├── Set Clock         (manual HH:MM entry via encoder)
 *   │   ├── Microstep Fwd     (single microstep forward)
 *   │   ├── Microstep Bwd     (single microstep backward)
 *   │   ├── Advance 1 Min     (test advance)
 *   │   ├── Set Sensor Offset (adjust via encoder)
 *   │   └── Calibrate Sensor  (run dark baseline)
 *   ├── Status
 *   │   ├── Clock Status      (displayed min, sensor, offsets)
 *   │   ├── Network Status    (IP, SSID, RSSI, geo)
 *   │   └── Time & Sync       (local time, SNTP state, TZ)
 *   ├── Network
 *   │   ├── Net Info          (full net-status dump)
 *   │   └── Sync Status       (SNTP sync detail)
 *   ├── System
 *   │   ├── Uptime
 *   │   └── About
 *   └── Lights (placeholder for WS2812B)
 *       ├── Color
 *       └── Brightness
 *
 * Display blanks after 5 minutes of inactivity.
 * Any encoder event (rotation or button) wakes it.
 */

#include "menu.h"
#include "display.h"
#include "networking.h"
#include "led_manager.h"
#include "ota_manager.h"
#include "config_store.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <algorithm>
#include <cstdio>
#include <cstring>

static const char *TAG = "Menu";

// ── Helpers ───────────────────────────────────────────────────────────────────

static std::string center_str(const std::string &s, int width = 16)
{
    int len = static_cast<int>(s.size());
    if (len >= width)
        return s.substr(0, width);
    int left = (width - len) / 2;
    return std::string(left, ' ') + s;
}

// ── MenuItem ─────────────────────────────────────────────────────────────────

MenuItem::MenuItem(const std::string &name)
    : name_(name), parent_(nullptr), callback_(nullptr) {}

MenuItem::MenuItem(const std::string &name, Callback callback)
    : name_(name), parent_(nullptr), callback_(callback) {}

void MenuItem::addChild(std::unique_ptr<MenuItem> child)
{
    child->parent_ = this;
    children_.push_back(std::move(child));
}

void MenuItem::execute() const
{
    if (callback_)
    {
        callback_();
    }
    else
    {
        ESP_LOGI(TAG, "No callback for: %s", name_.c_str());
    }
}

// ── Menu core ─────────────────────────────────────────────────────────────────

Menu::Menu(Display &display)
    : display_(display)
{
    action_sem_ = xSemaphoreCreateBinary();
    xSemaphoreGive(action_sem_); // start available
    action_queue_ = xQueueCreate(1, sizeof(MenuItem::Callback *));
    xTaskCreate(action_task_fn, "menu_act", 4096, this, 3, &action_task_handle_);
}

void Menu::next()
{
    if (!current_menu_)
        return;
    if (current_selection_ < current_menu_->getChildren().size() - 1)
    {
        current_selection_++;
        updateDisplayStart();
        h_scroll_offset_ = 0;
        h_scroll_dir_ = 1;
    }
}

void Menu::previous()
{
    if (!current_menu_)
        return;
    if (current_selection_ > 0)
    {
        current_selection_--;
        updateDisplayStart();
        h_scroll_offset_ = 0;
        h_scroll_dir_ = 1;
    }
}

void Menu::select()
{
    if (!current_menu_)
        return;
    const auto &children = current_menu_->getChildren();
    if (children.empty())
        return;
    auto *selected = children[current_selection_].get();
    if (selected->hasChildren())
    {
        current_menu_ = selected;
        current_selection_ = 0;
        display_start_ = 0;
        h_scroll_offset_ = 0;
        h_scroll_dir_ = 1;
        ESP_LOGI(TAG, "Entering: %s", selected->getName().c_str());
    }
    else
    {
        ESP_LOGI(TAG, "Execute: %s", selected->getName().c_str());
        selected->execute();
    }
}

void Menu::back()
{
    if (!current_menu_ || !current_menu_->getParent())
        return;
    current_menu_ = current_menu_->getParent();
    current_selection_ = 0;
    display_start_ = 0;
    h_scroll_offset_ = 0;
    h_scroll_dir_ = 1;
}

void Menu::updateDisplayStart()
{
    if (current_selection_ < display_start_)
    {
        display_start_ = current_selection_;
    }
    else if (current_selection_ >= display_start_ + MAX_VISIBLE_ITEMS)
    {
        display_start_ = current_selection_ - MAX_VISIBLE_ITEMS + 1;
    }
}

std::vector<std::string> Menu::getVisibleItems() const
{
    std::vector<std::string> items;
    if (!current_menu_)
        return items;
    const auto &children = current_menu_->getChildren();
    size_t start = display_start_;
    size_t end = std::min(start + MAX_VISIBLE_ITEMS, children.size());
    for (size_t i = start; i < end; i++)
        items.push_back(children[i]->getName());
    while (items.size() < MAX_VISIBLE_ITEMS)
        items.emplace_back("");
    return items;
}

void Menu::render()
{
    if (blanked_ || !current_menu_)
        return;

    const auto &children = current_menu_->getChildren();

    // Root menu gets a blank separator between the title and the first item.
    const int item_offset = (current_menu_->getParent() == nullptr) ? 2 : 1;

    std::vector<std::string> lines;
    lines.reserve(item_offset + MAX_VISIBLE_ITEMS);

    // Line 0: centred title
    lines.push_back(center_str(current_menu_->getName()));

    // Line 1 (root only): blank separator
    if (item_offset == 2)
        lines.emplace_back("");

    // Lines item_offset...: items (with h-scroll on selected item)
    size_t end = std::min(display_start_ + MAX_VISIBLE_ITEMS, children.size());
    for (size_t i = display_start_; i < end; i++)
    {
        const std::string &name = children[i]->getName();
        if (i == current_selection_ && (int)name.length() > 16)
        {
            int max_off = (int)name.length() - 16;
            int off = std::min(h_scroll_offset_, max_off);
            lines.push_back(name.substr(off, 16));
        }
        else
        {
            lines.push_back(name);
        }
    }
    while ((int)lines.size() < item_offset + (int)MAX_VISIBLE_ITEMS)
        lines.emplace_back("");

    // highlight_line accounts for title row (+ optional blank on root)
    int highlight_line = (current_selection_ >= display_start_)
                             ? (int)(current_selection_ - display_start_) + item_offset
                             : -1;

    display_.writeLines(lines, highlight_line);
}

void Menu::render_scrolled(bool going_next)
{
    if (blanked_)
        return;
    display_.startHardwareScroll(
        going_next ? ScrollMode::HARDWARE_UP : ScrollMode::HARDWARE_DOWN, 7);
    vTaskDelay(pdMS_TO_TICKS(60));
    render();                      // updates buffer; refresh_display skipped while scrolling
    display_.stopHardwareScroll(); // stops scroll and flushes the new buffer
}

void Menu::tick_h_scroll()
{
    if (blanked_ || !current_menu_)
        return;
    const auto &children = current_menu_->getChildren();
    if (children.empty())
        return;
    const std::string &name = children[current_selection_]->getName();
    int name_len = (int)name.length();
    if (name_len <= 16)
        return;

    int max_off = name_len - 16;
    h_scroll_offset_ += h_scroll_dir_;
    if (h_scroll_offset_ >= max_off)
    {
        h_scroll_offset_ = max_off;
        h_scroll_dir_ = -1;
    }
    else if (h_scroll_offset_ <= 0)
    {
        h_scroll_offset_ = 0;
        h_scroll_dir_ = 1;
    }
    render();
}

// ── Display blanking ──────────────────────────────────────────────────────────

void Menu::wake()
{
    idle_seconds_ = 0;
    if (blanked_)
    {
        blanked_ = false;
        display_.clear();
        render();
        ESP_LOGI(TAG, "Display woken");
    }
}

void Menu::tick_blank_timer()
{
    if (blanked_)
        return;
    if (++idle_seconds_ >= BLANK_TIMEOUT_S)
    {
        blanked_ = true;
        display_.clear();
        ESP_LOGI(TAG, "Display blanked after %lu s idle",
                 (unsigned long)BLANK_TIMEOUT_S);
    }
}

// ── wait_for_dismiss ──────────────────────────────────────────────────────────
// Spins (yielding to the scheduler) until dismiss_fn_ returns true (button
// press edge) or until 30s timeout.  The last display line should say
// "Press to return" so the user knows what to do.

static constexpr uint32_t DISMISS_TIMEOUT_MS = 30000;
static constexpr uint32_t DISMISS_HOLD_MS = 800;

void Menu::wait_for_dismiss()
{
    // Debounce: wait for button released from the select/navigate press
    vTaskDelay(pdMS_TO_TICKS(300));

    uint32_t hold_ms = 0;
    uint32_t elapsed = 0;

    while (elapsed < DISMISS_TIMEOUT_MS)
    {
        bool pressed = dismiss_fn_ ? dismiss_fn_() : false;
        if (pressed)
        {
            hold_ms += 50;
            if (hold_ms >= DISMISS_HOLD_MS)
                return; // long press detected
        }
        else
        {
            hold_ms = 0;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
        elapsed += 50;
    }
    ESP_LOGW("Menu", "wait_for_dismiss: timed out");
}

// ── Info sub-screens ──────────────────────────────────────────────────────────
// These are blocking screens; they spin until the button is pressed,
// then return to the caller which re-renders the menu.


void Menu::show_net_status(Networking &net)
{
    const NetStatus &s = net.get_status();
    char buf[20];
    display_.clear();
    display_.print(0, center_str("Net Status").c_str());
    if (s.wifi_connected)
    {
        int row = 1;
        snprintf(buf, sizeof(buf), "%.14s", s.ssid);
        display_.print(row++, buf);
        if (s.mdns_hostname[0] != '\0') {
            snprintf(buf, sizeof(buf), "DNS:%.11s", s.mdns_hostname);
            display_.print(row++, buf);
        }
        snprintf(buf, sizeof(buf), "%.15s", s.local_ip);
        display_.print(row++, buf);
        snprintf(buf, sizeof(buf), "%.15s", s.external_ip);
        display_.print(row++, buf);
        snprintf(buf, sizeof(buf), "%.16s", s.city);
        display_.print(row++, buf);
        snprintf(buf, sizeof(buf), "%.16s", s.iana_tz);
        display_.print(row++, buf);
    }
    else
    {
        display_.print(1, "No connection");
    }
    wait_for_dismiss();
    render();
}

void Menu::show_info_screen(Networking &net)
{
    char buf[20];
    display_.clear();
    display_.print(0, center_str("About").c_str());
    display_.print(1, "LED Controller");
    display_.print(2, "ESP32-S3");
    uint32_t uptime_s = (uint32_t)(esp_timer_get_time() / 1000000ULL);
    snprintf(buf, sizeof(buf), "Up: %luh %02lum",
             (unsigned long)(uptime_s / 3600),
             (unsigned long)((uptime_s % 3600) / 60));
    display_.print(3, buf);
    display_.print(4, net.is_connected() ? "Net: OK" : "Net: offline");
    wait_for_dismiss();
    render();
}

// ── show_wifi_credentials ─────────────────────────────────────────────────────
//
// Single-screen WiFi credential entry (SSID + Password).
// Returns true = confirmed, false = cancelled.
//
// Layout (7 lines, 9px spacing):
//   0  ">Name: <9 chars>"   '>' marks active field
//   1  ">PW:   <9 chars>"
//   2  blank
//   3  charset row 1 (16 chars)
//   4  charset row 2 (16 chars)
//   5  blank
//   6  "ab AB # Sym Done"
//
// Cursor sections (linear navigation, wraps):
//   NAME → PW → GRID (0..N-1) → TAB_ab → TAB_AB → TAB_NUM → TAB_SYM
//   → TAB_DONE → NAME
//
// Long A:  NAME/PW = set active field + jump to grid start
//          GRID    = append char to active text (cursor stays; '_' inserts space)
//          TAB     = switch charset + jump to grid start
//          DONE    = submit if Name non-empty
// Long B:  GRID section only = backspace from active text
// A+B hold ≥800ms: cancel (return false, fields cleared)
// A/B tap: navigate backward / forward
// Encoder rotate: navigate; short press = Long A; long press = Long B

bool Menu::show_wifi_credentials(std::string &ssid, std::string &pw)
{
    if (!input_poll_fn_) return false;

    // ── charsets (32 slots each; '\0' = unused) ───────────────────────────────
    static const char CS_ab[32] = {
        'a','b','c','d','e','f','g','h','i','j','k','l','m','n','o','p',
        'q','r','s','t','u','v','w','x','y','z','_',0,0,0,0,0
    };
    static const char CS_AB[32] = {
        'A','B','C','D','E','F','G','H','I','J','K','L','M','N','O','P',
        'Q','R','S','T','U','V','W','X','Y','Z','_',0,0,0,0,0
    };
    static const char CS_NUM[32] = {
        '0','1','2','3','4','5','6','7','8','9',0,0,0,0,0,0,
        0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0
    };
    static const char CS_SYM[32] = {
        '!','@','#','$','%','^','&','*','(',')','-','=','+','[',']','{',
        '}','|',';',':','\'','"',',','.','<','>','/','?','~','`','\\','_'
    };

    enum class WCSect  { NAME, PW, GRID, TAB_ab, TAB_AB, TAB_NUM, TAB_SYM, TAB_DONE };
    enum class WCField { NAME, PW };
    enum class WCSet   { ab, AB, NUM, SYM };

    WCSect  section   = WCSect::NAME;
    WCField actfield  = WCField::NAME;
    WCSet   actset    = WCSet::ab;
    int     gpos      = 0;
    bool    submitted = false;

    auto cs_ptr = [&]() -> const char* {
        switch (actset) {
            case WCSet::ab:  return CS_ab;
            case WCSet::AB:  return CS_AB;
            case WCSet::NUM: return CS_NUM;
            case WCSet::SYM: return CS_SYM;
        }
        return CS_ab;
    };

    auto cs_size = [&]() -> int {
        const char *p = cs_ptr();
        int n = 32;
        while (n > 0 && p[n-1] == '\0') n--;
        return n ? n : 1;
    };

    auto act_text = [&]() -> std::string& {
        return (actfield == WCField::NAME) ? ssid : pw;
    };

    // ── render ───────────────────────────────────────────────────────────────
    auto render = [&]() {
        const char *cs = cs_ptr();
        std::vector<std::string> lines;

        auto field_line = [](bool active, const char *label,
                             const std::string &txt) -> std::string {
            std::string s;
            s += (active ? '>' : ' ');
            s += label;
            std::string v = txt;
            if ((int)v.size() > 9) v = v.substr(v.size() - 9);
            while ((int)v.size() < 9) v += ' ';
            s += v;
            return s;
        };

        lines.push_back(field_line(actfield == WCField::NAME, "Name: ", ssid));
        lines.push_back(field_line(actfield == WCField::PW,   "PW:   ", pw));
        lines.push_back("");

        std::string r1, r2;
        for (int i =  0; i < 16; i++) r1 += (cs[i] ? cs[i] : ' ');
        for (int i = 16; i < 32; i++) r2 += (cs[i] ? cs[i] : ' ');
        lines.push_back(r1);
        lines.push_back(r2);
        lines.push_back("");
        lines.push_back("ab AB # Sym Done");

        display_.writeLines(lines, -1);

        // Cursor overlay: invert the highlighted cell(s)
        // Tab row "ab AB # Sym Done": ab@0, AB@3, #@6, Sym@8, Done@12
        switch (section) {
            case WCSect::NAME:     display_.invert_char_cells( 0,  0, 7); break;
            case WCSect::PW:       display_.invert_char_cells( 9,  0, 7); break;
            case WCSect::GRID: {
                int row = gpos / 16, col = gpos % 16;
                display_.invert_char_cells((3 + row) * 9, col, 1);
                break;
            }
            case WCSect::TAB_ab:   display_.invert_char_cells(54,  0, 2); break;
            case WCSect::TAB_AB:   display_.invert_char_cells(54,  3, 2); break;
            case WCSect::TAB_NUM:  display_.invert_char_cells(54,  6, 1); break;
            case WCSect::TAB_SYM:  display_.invert_char_cells(54,  8, 3); break;
            case WCSect::TAB_DONE: display_.invert_char_cells(54, 12, 4); break;
        }
    };

    // ── navigation ───────────────────────────────────────────────────────────
    auto nav_fwd = [&]() {
        switch (section) {
            case WCSect::NAME:     section = WCSect::PW;       break;
            case WCSect::PW:       section = WCSect::GRID; gpos = 0; break;
            case WCSect::GRID:
                if (gpos < cs_size() - 1) gpos++;
                else section = WCSect::TAB_ab;
                break;
            case WCSect::TAB_ab:   section = WCSect::TAB_AB;   break;
            case WCSect::TAB_AB:   section = WCSect::TAB_NUM;  break;
            case WCSect::TAB_NUM:  section = WCSect::TAB_SYM;  break;
            case WCSect::TAB_SYM:  section = WCSect::TAB_DONE; break;
            case WCSect::TAB_DONE: section = WCSect::NAME;     break;
        }
    };

    auto nav_bwd = [&]() {
        switch (section) {
            case WCSect::NAME:     section = WCSect::TAB_DONE; break;
            case WCSect::PW:       section = WCSect::NAME;     break;
            case WCSect::GRID:
                if (gpos > 0) gpos--;
                else section = WCSect::PW;
                break;
            case WCSect::TAB_ab:   section = WCSect::GRID; gpos = cs_size() - 1; break;
            case WCSect::TAB_AB:   section = WCSect::TAB_ab;   break;
            case WCSect::TAB_NUM:  section = WCSect::TAB_AB;   break;
            case WCSect::TAB_SYM:  section = WCSect::TAB_NUM;  break;
            case WCSect::TAB_DONE: section = WCSect::TAB_SYM;  break;
        }
    };

    // Long A action (sets submitted if DONE with non-empty ssid)
    auto long_a = [&]() {
        switch (section) {
            case WCSect::NAME:
                actfield = WCField::NAME; section = WCSect::GRID; gpos = 0; break;
            case WCSect::PW:
                actfield = WCField::PW;   section = WCSect::GRID; gpos = 0; break;
            case WCSect::GRID: {
                char c = cs_ptr()[gpos];
                if (c && act_text().size() < 63)
                    act_text() += (c == '_') ? ' ' : c;
                break; // cursor stays on same char
            }
            case WCSect::TAB_ab:
                actset = WCSet::ab;  section = WCSect::GRID; gpos = 0; break;
            case WCSect::TAB_AB:
                actset = WCSet::AB;  section = WCSect::GRID; gpos = 0; break;
            case WCSect::TAB_NUM:
                actset = WCSet::NUM; section = WCSect::GRID; gpos = 0; break;
            case WCSect::TAB_SYM:
                actset = WCSet::SYM; section = WCSect::GRID; gpos = 0; break;
            case WCSect::TAB_DONE:
                if (!ssid.empty()) submitted = true;
                break;
        }
    };

    // Long B — backspace from active field (GRID section only)
    auto long_b = [&]() {
        if (section == WCSect::GRID && !act_text().empty())
            act_text().pop_back();
    };

    static constexpr uint32_t LONG_PRESS_MS = 800;
    static constexpr uint32_t TAP_WINDOW_MS = 400;
    static constexpr int      MAX_STREAK    = 8;

    bool     enc_btn_last = false, btnA_last = false;
    bool     btnB_last    = false, both_last = false;
    uint32_t enc_hold_ms  = 0;  bool enc_longfire  = false;
    uint32_t btnA_hold_ms = 0;  bool btnA_longfire = false;
    uint32_t btnB_hold_ms = 0;  bool btnB_longfire = false;
    uint32_t both_hold_ms = 0;  bool both_longfire = false;
    int      btnA_streak  = 0;  uint32_t btnA_inter_ms = 999;
    int      btnB_streak  = 0;  uint32_t btnB_inter_ms = 999;

    // ── outer loop: editing → confirm → back-to-editing or return ────────────
    while (true)
    {
        // Snapshot buttons at (re-)entry to avoid stale-hold false triggers
        {
            InputEvent init = input_poll_fn_();
            enc_btn_last = init.enc_btn; btnA_last = init.btnA;
            btnB_last    = init.btnB;    both_last = init.btnA && init.btnB;
        }
        submitted = false;
        enc_hold_ms  = 0; enc_longfire  = false;
        btnA_hold_ms = 0; btnA_longfire = false;
        btnB_hold_ms = 0; btnB_longfire = false;
        both_hold_ms = 0; both_longfire = false;
        btnA_streak  = 0; btnA_inter_ms = 999;
        btnB_streak  = 0; btnB_inter_ms = 999;

        render();

        // ── editing event loop ────────────────────────────────────────────────
        while (!submitted)
        {
            InputEvent ev = input_poll_fn_();
            bool enc_btn  = ev.enc_btn;
            bool btnA     = ev.btnA;
            bool btnB     = ev.btnB;
            bool both     = btnA && btnB;
            bool enc_rise = !enc_btn && enc_btn_last;
            bool redraw   = false;

            // ── A+B hold: cancel ──────────────────────────────────────────────
            if (both) {
                both_hold_ms += 50;
                if (both_hold_ms >= LONG_PRESS_MS && !both_longfire) {
                    both_longfire = true;
                    ssid.clear(); pw.clear();
                    return false;
                }
                btnA_hold_ms = 0; btnA_longfire = false; btnA_streak = 0;
                btnB_hold_ms = 0; btnB_longfire = false; btnB_streak = 0;
            } else {
                both_hold_ms = 0; both_longfire = false;
            }

            // ── Encoder rotation: navigate ────────────────────────────────────
            if (!both && ev.delta != 0) {
                int steps = ev.delta;
                if (steps > 0) { while (steps-- > 0) nav_fwd(); }
                else           { while (steps++ < 0) nav_bwd(); }
                redraw = true;
            }

            // ── Button A: tap = nav_bwd (accelerating); hold = Long A ─────────
            if (!btnA) btnA_inter_ms += 50;
            if (btnA_inter_ms >= TAP_WINDOW_MS) btnA_streak = 0;

            if (btnA && !both) {
                if (!btnA_last) {
                    btnA_hold_ms = 0; btnA_longfire = false;
                } else if (!btnA_longfire) {
                    btnA_hold_ms += 50;
                    if (btnA_hold_ms >= LONG_PRESS_MS) {
                        btnA_longfire = true; btnA_streak = 0;
                        long_a(); redraw = true;
                    }
                }
            } else {
                if (btnA_last && !btnA_longfire && !both_last) {
                    btnA_streak = (btnA_inter_ms < TAP_WINDOW_MS)
                        ? (btnA_streak < MAX_STREAK ? btnA_streak + 1 : MAX_STREAK)
                        : 1;
                    btnA_inter_ms = 0;
                    for (int s = 0; s < btnA_streak; s++) nav_bwd();
                    redraw = true;
                }
                btnA_hold_ms = 0; btnA_longfire = false;
            }

            // ── Button B: tap = nav_fwd (accelerating); hold = Long B ─────────
            if (!btnB) btnB_inter_ms += 50;
            if (btnB_inter_ms >= TAP_WINDOW_MS) btnB_streak = 0;

            if (btnB && !both) {
                if (!btnB_last) {
                    btnB_hold_ms = 0; btnB_longfire = false;
                } else if (!btnB_longfire) {
                    btnB_hold_ms += 50;
                    if (btnB_hold_ms >= LONG_PRESS_MS) {
                        btnB_longfire = true; btnB_streak = 0;
                        long_b(); redraw = true;
                    }
                }
            } else {
                if (btnB_last && !btnB_longfire && !both_last) {
                    btnB_streak = (btnB_inter_ms < TAP_WINDOW_MS)
                        ? (btnB_streak < MAX_STREAK ? btnB_streak + 1 : MAX_STREAK)
                        : 1;
                    btnB_inter_ms = 0;
                    for (int s = 0; s < btnB_streak; s++) nav_fwd();
                    redraw = true;
                }
                btnB_hold_ms = 0; btnB_longfire = false;
            }

            // ── Encoder: long press = Long B; short press = Long A ────────────
            if (enc_btn) {
                if (!enc_longfire) {
                    enc_hold_ms += 50;
                    if (enc_hold_ms >= LONG_PRESS_MS) {
                        enc_longfire = true;
                        long_b(); redraw = true;
                    }
                }
            } else {
                if (enc_rise && !enc_longfire) {
                    long_a(); redraw = true;
                }
                enc_hold_ms = 0; enc_longfire = false;
            }

            if (redraw) render();

            enc_btn_last = enc_btn;
            btnA_last    = btnA;
            btnB_last    = btnB;
            both_last    = both;
            vTaskDelay(pdMS_TO_TICKS(50));
        } // editing loop

        // ── Confirm screen ────────────────────────────────────────────────────
        {
            auto trunc16 = [](const std::string &s) -> std::string {
                if ((int)s.size() <= 16) return s;
                return s.substr(0, 15) + "~";
            };
            std::vector<std::string> cl;
            cl.push_back("  Confirm WiFi?");
            cl.push_back("");
            cl.push_back("Name:");
            cl.push_back(trunc16(ssid));
            cl.push_back("PW:");
            cl.push_back(pw.empty() ? "(none)" : trunc16(pw));
            cl.push_back("A:back  B/Enc:ok");
            display_.writeLines(cl, -1);

            bool go_back = false, confirmed = false;
            bool ce_last = false, ca_last = false, cb_last = false, cboth_last = false;
            uint32_t ce_hold = 0; bool ce_long = false;
            uint32_t ca_hold = 0; bool ca_long = false;
            uint32_t cboth_hold = 0; bool cboth_long = false;
            // Wait for all buttons to be fully released before entering the
            // confirm loop.  Without this, the Long-A release that triggered
            // "Done" arrives as the first event and fires "back" immediately.
            while (true) {
                InputEvent ev = input_poll_fn_();
                if (!ev.enc_btn && !ev.btnA && !ev.btnB) break;
                vTaskDelay(pdMS_TO_TICKS(50));
            }
            vTaskDelay(pdMS_TO_TICKS(50)); // one extra tick of margin
            // All inputs are now clear; start with all _last = false
            while (!confirmed && !go_back) {
                InputEvent ev = input_poll_fn_();
                bool ce = ev.enc_btn, ca = ev.btnA, cb = ev.btnB;
                bool cboth = ca && cb;
                bool ce_rise = !ce && ce_last;

                // A+B hold: full cancel
                if (cboth) {
                    cboth_hold += 50;
                    if (cboth_hold >= LONG_PRESS_MS && !cboth_long) {
                        cboth_long = true;
                        ssid.clear(); pw.clear();
                        return false;
                    }
                } else { cboth_hold = 0; cboth_long = false; }

                // Encoder long-press tracking (suppresses short-press on release)
                if (ce) {
                    if (!ce_long) { ce_hold += 50; if (ce_hold >= LONG_PRESS_MS) ce_long = true; }
                } else { ce_hold = 0; ce_long = false; }

                // Encoder short press or B tap = confirm
                if (ce_rise && !ce_long)           confirmed = true;
                if (!cb && cb_last && !cboth_last)  confirmed = true;

                // A tap or Long A = back to editing
                if (!ca && ca_last && !cboth_last) go_back = true;
                if (ca && !cboth) {
                    if (!ca_long) {
                        ca_hold += 50;
                        if (ca_hold >= LONG_PRESS_MS) { ca_long = true; go_back = true; }
                    }
                } else { ca_hold = 0; ca_long = false; }

                ce_last = ce; ca_last = ca; cb_last = cb; cboth_last = cboth;
                vTaskDelay(pdMS_TO_TICKS(50));
            }
            if (go_back) continue; // outer while(true) — re-enter editing
        }
        return true; // confirmed
    } // outer while(true)
}

// ── set_matter_pairing_info ───────────────────────────────────────────────────

void Menu::set_matter_pairing_info(uint32_t pin, uint16_t disc,
                                   const std::string &code)
{
    matter_pin_ = pin;
    matter_disc_ = disc;
    matter_code_ = code;
}

// ── show_matter_pairing_screen ────────────────────────────────────────────────
// Layout (7 lines on 128×64 SSD1306, 9px line spacing):
//   0  " Matter Pairing " (centered)
//   1  "Go to Smart Home"
//   2  (blank)
//   3  "Disc: XXXX"
//   4  "Code:XXXXXXXXXXX"
//   5  (blank)
//   6  back hint (encoder hint only shown when encoder present)
//
// Returns true  = commissioned (auto-exit; proceed to main menu).
//         false = back pressed  (encoder short press or single A/B tap).
static bool show_matter_pairing_screen(Display &display,
                                       uint32_t /*pin*/, uint16_t disc,
                                       const std::string &code,
                                       Menu::InputPollFn poll,
                                       bool encoder_ok,
                                       std::function<bool()> commissioned_fn)
{
    char buf[20];
    display.clear();
    display.print(0, " Matter Pairing ");
    display.print(1, "Go to Smart Home");
    // line 2 intentionally blank
    snprintf(buf, sizeof(buf), "Disc: %u", (unsigned)disc);
    display.print(3, buf);
    snprintf(buf, sizeof(buf), "Code:%.11s", code.empty() ? "see UART   " : code.c_str());
    display.print(4, buf);
    // line 5 intentionally blank
    display.print(6, encoder_ok ? "ShrtPress: back" : "A or B: back");

    if (!poll)
        return true;

    vTaskDelay(pdMS_TO_TICKS(500)); // debounce after selection

    // Snapshot: only auto-exit if commissioning happens DURING this screen.
    // If the device already has a fabric when the screen appears (stale NVS
    // data from a prior flash), the transition never fires and the user must
    // press back manually — preventing an immediate skip to main menu.
    bool was_commissioned = commissioned_fn && commissioned_fn();

    bool     enc_last = false;
    uint32_t enc_hold = 0;

    while (true)
    {
        // Auto-exit only on false → true transition
        if (commissioned_fn && !was_commissioned && commissioned_fn())
            return true;

        auto ev   = poll();
        bool both = ev.btnA && ev.btnB;

        // Encoder short press = back
        if (ev.enc_btn) {
            enc_hold += 100;
        } else {
            if (enc_last && enc_hold < 800) {
                vTaskDelay(pdMS_TO_TICKS(200));
                return false;
            }
            enc_hold = 0;
        }
        enc_last = ev.enc_btn;

        // Single A or B (not both) = back
        if (!both && (ev.btnA || ev.btnB)) {
            vTaskDelay(pdMS_TO_TICKS(200)); // wait for release
            return false;
        }

        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

// ── first_time_setup ──────────────────────────────────────────────────────────

Menu::SetupResult Menu::first_time_setup()
{
    if (!input_poll_fn_)
        return SetupResult::Pending;

    // ── 2-item choice screen ──────────────────────────────────────────────────
    int choice = 0; // 0 = Matter, 1 = Setup WiFi
    bool enc_btn_last = false;
    bool btnA_last = false;
    bool btnB_last = false;
    bool both_last = false;
    uint32_t enc_hold = 0;
    bool enc_longfire = false;
    uint32_t btnA_hold = 0;
    bool btnA_longfire = false;

    auto render_choice = [&]()
    {
        display_.clear();
        display_.print(0, "  First-Time Setup");
        display_.print(2, choice == 0 ? "> Matter" : "  Matter");
        display_.print(3, choice == 1 ? "> Setup WiFi" : "  Setup WiFi");
        if (encoder_ok_) {
            display_.print(5, "Rot/A/B: select");
            display_.print(6, "LongA/Enc: ok");
        } else {
            display_.print(5, "A/B: select");
            display_.print(6, "LongA: confirm");
        }
    };
    render_choice();

    bool selected = false;
    while (!selected)
    {
        InputEvent ev = input_poll_fn_();
        bool enc_btn = ev.enc_btn;
        bool btnA = ev.btnA;
        bool btnB = ev.btnB;
        bool both = btnA && btnB;

        bool enc_rise = !enc_btn && enc_btn_last;
        bool btnA_rise = !btnA && btnA_last && !both_last;
        bool btnB_rise = !btnB && btnB_last && !both_last;

        // Rotation or short A/B → toggle selection
        if (ev.delta != 0)
        {
            choice = (choice + 1) % 2;
            render_choice();
        }
        if (btnA_rise && !btnA_longfire)
        {
            choice = (choice + 1) % 2;
            render_choice();
        }
        if (btnB_rise)
        {
            choice = (choice + 1) % 2;
            render_choice();
        }

        // Encoder short press → confirm
        if (enc_rise && !enc_longfire)
            selected = true;

        // Encoder long press → confirm
        if (enc_btn)
        {
            if (!enc_longfire && (enc_hold += 50) >= 800)
            {
                enc_longfire = true;
                selected = true;
            }
        }
        else
        {
            enc_hold = 0;
            enc_longfire = false;
        }

        // Long press A → confirm (B long press is not a select action)
        if (btnA && !both)
        {
            if (!btnA_longfire && (btnA_hold += 50) >= 800)
            {
                btnA_longfire = true;
                selected = true;
            }
        }
        else
        {
            btnA_hold = 0;
            btnA_longfire = false;
        }


        enc_btn_last = enc_btn;
        btnA_last = btnA;
        btnB_last = btnB;
        both_last = both;
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    if (choice == 0)
    {
        // Return to app_main so it can start the Matter stack at shallow stack
        // depth before showing the pairing screen.
        return SetupResult::MatterChosen;
    }

    // ── WiFi path: SSID + password text entry ─────────────────────────────────
    // Loops back to the choice screen if the user cancels (hold-both).
    while (true)
    {
        std::string ssid, pass;
        if (!show_wifi_credentials(ssid, pass))
        {
            // Cancelled — restart the whole first-time setup choice screen
            return first_time_setup();
        }
        // Empty pass is valid (open network); proceed regardless.

        NetCfg cfg = {};
        ConfigStore::load(cfg);
        strncpy(cfg.ssid, ssid.c_str(), sizeof(cfg.ssid) - 1);
        strncpy(cfg.password, pass.c_str(), sizeof(cfg.password) - 1);
        cfg.ssid[sizeof(cfg.ssid) - 1] = '\0';
        cfg.password[sizeof(cfg.password) - 1] = '\0';
        ConfigStore::save(cfg);

        display_.clear();
        display_.print(0, "  WiFi Saved!");
        display_.print(2, ssid.c_str());
        display_.print(4, "Connecting...");
        vTaskDelay(pdMS_TO_TICKS(2000));
        return SetupResult::WiFiSaved;
    }
}

// ── show_matter_pairing_standalone ────────────────────────────────────────────
// Called from app_main AFTER s_matter.start() so BLE is advertising when the
// user sees the pairing screen.

bool Menu::show_matter_pairing_standalone(std::function<bool()> commissioned_fn)
{
    return show_matter_pairing_screen(display_, matter_pin_, matter_disc_,
                                      matter_code_, input_poll_fn_,
                                      encoder_ok_, commissioned_fn);
}

// ── Async action dispatch ─────────────────────────────────────────────────────
// Slow motor callbacks (Set Time, Advance) are posted here so they run on
// action_task and encoder_task stays responsive during motor movement.
// The semaphore ensures at most one action is queued or running at a time;
// extra presses are silently discarded.

void Menu::post_action(MenuItem::Callback cb)
{
    if (xSemaphoreTake(action_sem_, 0) != pdTRUE)
    {
        ESP_LOGW(TAG, "Action already running — request discarded");
        return;
    }
    auto *cb_ptr = new MenuItem::Callback(std::move(cb));
    if (xQueueSend(action_queue_, &cb_ptr, 0) != pdTRUE)
    {
        delete cb_ptr;
        xSemaphoreGive(action_sem_);
    }
}

void Menu::action_task_fn(void *arg)
{
    Menu *self = static_cast<Menu *>(arg);
    MenuItem::Callback *cb_ptr = nullptr;
    while (true)
    {
        if (xQueueReceive(self->action_queue_, &cb_ptr, portMAX_DELAY) == pdTRUE)
        {
            if (cb_ptr)
            {
                (*cb_ptr)();
                delete cb_ptr;
                cb_ptr = nullptr;
            }
            xSemaphoreGive(self->action_sem_);
        }
    }
}

// ── show_ota_screen ───────────────────────────────────────────────────────────
// Triggers a version check on the ota_task, waits for result, then offers to
// install if an update is available.  Runs on encoder_task stack via callback.

void Menu::show_ota_screen()
{
    if (!ota_) return;

    display_.clear();
    display_.print(0, " Firmware Update");
    char line[40];
    snprintf(line, sizeof(line), " v%s", OtaManager::running_version());
    display_.print(2, "Current:");
    display_.print(3, line);
    display_.print(5, "Checking...");

    ota_->trigger_check();

    // Wait up to 20 s for the ota_task to complete the version fetch.
    // Give 1 s initial slack so the task has time to set checking_ = true.
    vTaskDelay(pdMS_TO_TICKS(1000));
    for (int i = 0; i < 38; i++) {
        if (!ota_->is_checking()) break;
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    // ── Show result ───────────────────────────────────────────────────────────
    display_.clear();
    display_.print(0, " Firmware Update");
    snprintf(line, sizeof(line), " v%s", OtaManager::running_version());
    display_.print(2, "Current:");
    display_.print(3, line);
    display_.print(5, "Latest:");
    if (ota_->latest_version()[0]) {
        snprintf(line, sizeof(line), " v%s", ota_->latest_version());
        display_.print(6, line);
    } else {
        display_.print(6, " unavailable");
    }

    if (ota_->is_update_available()) {
        display_.print(7, encoder_ok_ ? "Sel:Install B:No" : "A:Install  B:No ");

        // Wait for user to confirm install or dismiss.
        bool confirmed = false;
        if (input_poll_fn_) {
            InputEvent prev = {};
            for (int t = 0; t < 600; t++) {          // up to 30 s (50 ms × 600)
                vTaskDelay(pdMS_TO_TICKS(50));
                InputEvent ev = input_poll_fn_();
                bool enc_press = ev.enc_btn && !prev.enc_btn;
                bool a_press   = ev.btnA   && !prev.btnA;
                bool b_press   = ev.btnB   && !prev.btnB;
                if (enc_press || a_press) { confirmed = true; break; }
                if (b_press) break;
                prev = ev;
            }
        }

        if (confirmed) {
            display_.clear();
            display_.print(0, " Firmware Update");
            display_.print(3, "  Updating...   ");
            display_.print(5, "Do not power off");
            ota_->trigger_update();
            // Block here — device restarts automatically on success.
            // If update fails, ota_task shows an error on the display,
            // then we fall through after 90 s and return to the menu.
            vTaskDelay(pdMS_TO_TICKS(90000));
        }
    } else if (ota_->latest_version()[0]) {
        display_.print(7, "   Up to date   ");
        vTaskDelay(pdMS_TO_TICKS(2500));
    } else {
        // Check failed — show briefly then return.
        vTaskDelay(pdMS_TO_TICKS(2500));
    }

    render();
}

// ── build() — full menu tree with wired callbacks ────────────────────────────

void Menu::build(Networking &net, LedManager &leds)
{
    leds_ = &leds;

    char root_title[24];
    snprintf(root_title, sizeof(root_title), "Main v%s", OtaManager::running_version());
    auto root = std::make_unique<MenuItem>(root_title);

    // ── Status ────────────────────────────────────────────────────────────────
    auto stat = std::make_unique<MenuItem>("Status");

    stat->addChild(std::make_unique<MenuItem>("Network", [this, &net]()
                                              { show_net_status(net); }));

    stat->addChild(std::make_unique<MenuItem>("About", [this, &net]()
                                              { show_info_screen(net); }));

    // ── Network ───────────────────────────────────────────────────────────────
    auto netm = std::make_unique<MenuItem>("Network");

    netm->addChild(std::make_unique<MenuItem>("Net Info", [this, &net]()
                                              { show_net_status(net); }));

    netm->addChild(std::make_unique<MenuItem>("Set WiFi", [this]()
                                              {
        std::string ssid, pass;
        if (!show_wifi_credentials(ssid, pass)) { render(); return; }

        // Load existing config to preserve tz_override, then overwrite credentials
        NetCfg cfg;
        ConfigStore::load(cfg);
        strncpy(cfg.ssid,     ssid.c_str(), sizeof(cfg.ssid)     - 1);
        strncpy(cfg.password, pass.c_str(), sizeof(cfg.password)  - 1);
        cfg.ssid[sizeof(cfg.ssid) - 1]         = '\0';
        cfg.password[sizeof(cfg.password) - 1] = '\0';
        ConfigStore::save(cfg);

        // Confirmation screen
        display_.clear();
        display_.print(0, "  WiFi Saved!");
        display_.print(2, ssid.c_str());
        display_.print(4, "Restart device");
        display_.print(5, "to connect.");
        vTaskDelay(pdMS_TO_TICKS(3000));
        render(); }));

    netm->addChild(std::make_unique<MenuItem>("Matter Pair", [this]()
                                              {
        show_matter_pairing_screen(display_, matter_pin_, matter_disc_,
                                   matter_code_, input_poll_fn_,
                                   encoder_ok_, nullptr);
        render(); }));

    // ── Lights ────────────────────────────────────────────────────────────────
    // s_led_tgt persists across menu interactions (static local).
    static LedManager::Target s_led_tgt = LedManager::Target::BOTH;

    auto lights = std::make_unique<MenuItem>("Lights");

    lights->addChild(std::make_unique<MenuItem>("Next Effect", [&leds]()
                                                { leds.next_effect(s_led_tgt); }));

    lights->addChild(std::make_unique<MenuItem>("Bright +", [&leds]()
                                                {
        for (int i = 0; i < LedManager::STRIP_COUNT; i++) {
            uint8_t b = leds.get_brightness(i);
            leds.set_brightness(s_led_tgt, b > 230 ? 255 : b + 25);
        } }));

    lights->addChild(std::make_unique<MenuItem>("Bright -", [&leds]()
                                                {
        for (int i = 0; i < LedManager::STRIP_COUNT; i++) {
            uint8_t b = leds.get_brightness(i);
            leds.set_brightness(s_led_tgt, b < 25 ? 0 : b - 25);
        } }));

    lights->addChild(std::make_unique<MenuItem>("White", [&leds]()
                                                { leds.set_color(s_led_tgt, 255, 255, 255); }));

    lights->addChild(std::make_unique<MenuItem>("Warm White", [&leds]()
                                                { leds.set_color(s_led_tgt, 255, 180, 80); }));

    lights->addChild(std::make_unique<MenuItem>("Red", [&leds]()
                                                { leds.set_color(s_led_tgt, 255, 0, 0); }));

    lights->addChild(std::make_unique<MenuItem>("Blue", [&leds]()
                                                { leds.set_color(s_led_tgt, 0, 80, 255); }));

    lights->addChild(std::make_unique<MenuItem>("Green", [&leds]()
                                                { leds.set_color(s_led_tgt, 0, 220, 50); }));

    lights->addChild(std::make_unique<MenuItem>("Purple", [&leds]()
                                                { leds.set_color(s_led_tgt, 180, 0, 255); }));

    lights->addChild(std::make_unique<MenuItem>("-> Both", []()
                                                {
        s_led_tgt = LedManager::Target::BOTH;
        ESP_LOGI(TAG, "LED target: Both"); }));

    lights->addChild(std::make_unique<MenuItem>("-> Strip 1", []()
                                                {
        s_led_tgt = LedManager::Target::STRIP_1;
        ESP_LOGI(TAG, "LED target: Strip 1"); }));

    lights->addChild(std::make_unique<MenuItem>("-> Strip 2", []()
                                                {
        s_led_tgt = LedManager::Target::STRIP_2;
        ESP_LOGI(TAG, "LED target: Strip 2"); }));

    // ── System ────────────────────────────────────────────────────────────────
    auto sys = std::make_unique<MenuItem>("System");

    sys->addChild(std::make_unique<MenuItem>("Uptime", [this, &net]()
                                             { show_info_screen(net); }));

    // ── System → Update ───────────────────────────────────────────────────────
    if (ota_) {
        auto upd = std::make_unique<MenuItem>("Update");

        upd->addChild(std::make_unique<MenuItem>("Check Now", [this]()
            { show_ota_screen(); }));

        upd->addChild(std::make_unique<MenuItem>("Auto Update", [this]()
        {
            if (!ota_) return;
            bool now_en = !ota_->is_auto_update_enabled();
            ota_->set_auto_update(now_en);
            display_.clear();
            display_.print(0, " Auto Update   ");
            display_.print(3, now_en ? "    Enabled    " : "    Disabled   ");
            vTaskDelay(pdMS_TO_TICKS(1500));
            render();
        }));

        sys->addChild(std::move(upd));
    }

    // ── Assemble ──────────────────────────────────────────────────────────────
    root->addChild(std::move(stat));
    root->addChild(std::move(netm));
    root->addChild(std::move(lights));
    root->addChild(std::move(sys));

    root_menu_ = std::move(root);
    current_menu_ = root_menu_.get();
    current_selection_ = 0;
    display_start_ = 0;

    ESP_LOGI(TAG, "Menu built with %zu top-level items",
             current_menu_->getChildren().size());
}
