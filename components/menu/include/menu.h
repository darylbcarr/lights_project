#pragma once
#include <string>
#include <vector>
#include <functional>
#include <memory>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

class Display;

class MenuItem {
public:
    using Callback = std::function<void()>;

    explicit MenuItem(const std::string& name);
    MenuItem(const std::string& name, Callback callback);

    void addChild(std::unique_ptr<MenuItem> child);

    const std::string& getName()     const { return name_; }
    MenuItem*          getParent()   const { return parent_; }
    const std::vector<std::unique_ptr<MenuItem>>& getChildren() const { return children_; }
    bool               hasChildren() const { return !children_.empty(); }

    void execute() const;
    void setCallback(Callback callback) { callback_ = callback; }

private:
    std::string name_;
    MenuItem*   parent_;
    std::vector<std::unique_ptr<MenuItem>> children_;
    Callback    callback_;

    friend class Menu;
};

// ── Forward declarations for all system components ────────────────────────────
class Networking;
class LedManager;
class OtaManager;

class Menu {
public:
    static constexpr uint8_t MAX_VISIBLE_ITEMS = 7;

    /**
     * Callback that returns true when the user has signalled "dismiss"
     * (i.e. button pressed and released).  Injected via set_dismiss_fn()
     * so that Menu doesn't depend on the encoder directly.
     */
    using DismissFn = std::function<bool()>;

    /**
     * Raw hardware state snapshot polled during blocking screens
     * (text input, info screens with encoder interaction).
     */
    struct InputEvent {
        int8_t delta   = 0;      ///< Encoder rotation since last poll (+CW, -CCW)
        bool   enc_btn = false;  ///< Encoder button held
        bool   btnA    = false;  ///< GPIO button A held
        bool   btnB    = false;  ///< GPIO button B held
    };
    using InputPollFn = std::function<InputEvent()>;

    explicit Menu(Display& display);
    ~Menu() = default;

    Menu(const Menu&)            = delete;
    Menu& operator=(const Menu&) = delete;

    /**
     * @brief Inject the button-poll function used by info screens to wait
     *        for a dismiss press.  Call once from main before build().
     *        The function should return true exactly once per button press
     *        (edge-detected, not level).
     */
    void set_dismiss_fn(DismissFn fn)  { dismiss_fn_     = fn; }

    /**
     * @brief Inject the hardware-poll function used by text-input screens.
     *        Call once from main before build().
     *        The function should read encoder delta + button states and return
     *        them without any debouncing — Menu handles edge detection internally.
     */
    void set_input_fn(InputPollFn fn)  { input_poll_fn_  = fn; }

    /**
     * @brief Inform the menu whether a rotary encoder is present.
     *        Affects the hint line shown on the Matter pairing screen.
     */
    void set_encoder_ok(bool ok)       { encoder_ok_     = ok; }

    /**
     * @brief Inject the OtaManager pointer so the menu can expose firmware
     *        update features under System → Update.  Call before build().
     */
    void set_ota(OtaManager* ota)      { ota_            = ota; }

    // ── Navigation ────────────────────────────────────────────────────────────
    void next();
    void previous();
    void select();
    void back();
    void render();

    /**
     * @brief Animate a hardware scroll (UP when going_next, DOWN otherwise)
     *        then render the new state.  Call instead of render() after
     *        next() / previous() / back() for a slide effect.
     */
    void render_scrolled(bool going_next);

    /**
     * @brief Advance horizontal scroll on the selected item (if its name
     *        exceeds 16 chars).  Call every ~300 ms from encoder_task.
     */
    void tick_h_scroll();

    // ── Wiring ────────────────────────────────────────────────────────────────
    /**
     * @brief Build the full menu tree and wire all callbacks.
     *        Call once after all system components are initialised.
     */
    void build(Networking& net, LedManager& leds);

    /**
     * @brief Store the Matter commissioning info for display on the Matter Pair
     *        screen and the first-run setup screen.  Call from app_main after
     *        MatterBridge::start() succeeds.
     */
    void set_matter_pairing_info(uint32_t pin, uint16_t disc,
                                 const std::string& code);

    /**
     * @brief First-time setup screen — call from app_main when no WiFi
     *        credentials are stored.  Blocks until the user completes setup.
     *
     *  User picks one of two paths:
     *   - "Matter"     → shows commissioning info (PIN + discriminator);
     *                    user pairs via Home/Alexa app; presses A+B when done.
     *   - "Setup WiFi" → character-by-character SSID + password entry;
     *                    credentials are saved to NVS.
     *
     * @return true  if WiFi credentials were entered and saved (reload NetCfg).
     *         false if Matter path was chosen (Matter manages WiFi).
     *
     * Call set_matter_pairing_info() before this to populate the Matter screen.
     */
    enum class SetupResult { WiFiSaved, MatterChosen, Pending };

    /**
     * Runs the first-time setup choice screen and (if WiFi chosen) the SSID/
     * password entry.  Returns:
     *   WiFiSaved    — credentials saved to NVS; reload NetCfg.
     *   MatterChosen — user selected Matter; caller must start Matter stack
     *                  and then call show_matter_pairing_screen_standalone().
     *   Pending      — should not occur (loop never exits without a choice).
     */
    SetupResult first_time_setup();

    /**
     * Shows the Matter pairing screen and blocks until:
     *   - commissioned_fn() returns true  → returns true  (proceed to main menu)
     *   - user presses back               → returns false (return to choice screen)
     * Call this from app_main after starting the Matter stack.
     */
    bool show_matter_pairing_standalone(std::function<bool()> commissioned_fn = nullptr);

    /**
     * @brief Blocking character-by-character text entry using the encoder + buttons.
     *        Returns the entered string when the user confirms (both GPIO buttons).
     *        Returns empty string if cancelled or input_poll_fn_ is not set.
     *
     * Controls:
     *   Encoder rotate  → scroll through character palette
     *   Encoder press   → add selected character
     *   Encoder hold    → delete last character (backspace)
     *   Button A        → jump to next character group (a-z / A-Z / 0-9 / !@#)
     *   Button B        → add space
     *   Both A+B        → confirm and return
     *
     * Both fields are entered on one screen.
     * Returns true = confirmed (ssid non-empty), false = cancelled.
     */
    bool show_wifi_credentials(std::string &ssid, std::string &pw);

    // ── Display blanking ──────────────────────────────────────────────────────
    /**
     * @brief Must be called from the encoder poll loop on any encoder event.
     *        Resets the blank timer and wakes the display if it was blanked.
     */
    void wake();

    /**
     * @brief Call periodically (e.g. every second) to enforce the blank timeout.
     */
    void tick_blank_timer();

    bool is_blanked() const { return blanked_; }

    MenuItem* getCurrentMenu()      const { return current_menu_; }
    size_t    getCurrentSelection()  const { return current_selection_; }

private:
    Display&                    display_;
    LedManager*                 leds_             = nullptr;
    std::unique_ptr<MenuItem>   root_menu_;
    MenuItem*                   current_menu_     = nullptr;
    size_t                      current_selection_ = 0;
    size_t                      display_start_     = 0;
    DismissFn                   dismiss_fn_        = nullptr;
    InputPollFn                 input_poll_fn_     = nullptr;
    bool                        encoder_ok_        = false;
    OtaManager*                 ota_               = nullptr;

    // Matter commissioning info (set via set_matter_pairing_info())
    uint32_t    matter_pin_  = 0;
    uint16_t    matter_disc_ = 0;
    std::string matter_code_;

    // Blank timer
    static constexpr uint32_t BLANK_TIMEOUT_S = 300;  // 5 minutes
    uint32_t                  idle_seconds_    = 0;
    bool                      blanked_         = false;

    void                        updateDisplayStart();
    std::vector<std::string>    getVisibleItems() const;

    // Horizontal scroll state for the currently selected item
    int h_scroll_offset_ = 0;
    int h_scroll_dir_    = 1;

    // Sub-screens (blocking, return to menu on button)
    void show_info_screen(Networking& net);
    void show_net_status(Networking& net);
    void show_ota_screen();   // firmware update check + optional install

    // Blocks until dismiss_fn_ fires or 30s timeout
    void wait_for_dismiss();

    // Async action dispatch — posts cb to action_task so encoder_task stays free.
    // If an action is already queued or running the new request is silently dropped.
    void post_action(MenuItem::Callback cb);
    static void action_task_fn(void* arg);

    SemaphoreHandle_t action_sem_         = nullptr;  // 1 = slot free
    QueueHandle_t     action_queue_       = nullptr;  // depth 1
    TaskHandle_t      action_task_handle_ = nullptr;
};
