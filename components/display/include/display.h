#pragma once
#include "ssd1306.h"
#include "driver/i2c_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <stdint.h>
#include <stdbool.h>
#include <vector>
#include <string>

#define MAX_LINES       20
#define MAX_LINE_LENGTH 32

enum class ScrollMode {
    NONE,
    HARDWARE_RIGHT,
    HARDWARE_LEFT,
    HARDWARE_UP,
    HARDWARE_DOWN,
    PAGE_UP,
    PAGE_DOWN
};

class Display {
public:
    Display();
    ~Display();

    bool init(i2c_port_t port   = I2C_NUM_0,
              uint8_t   addr    = 0x3C,
              int       sda_pin = GPIO_NUM_8,
              int       scl_pin = GPIO_NUM_9);

    void clear();
    void update();

    // Text management
    void print(int line, const char* text);
    void setSelectedLine(int line);
    void clearSelectedLine();
    void clearLine(int line);

    /**
     * writeLines — called by Menu::render().
     * Writes up to MAX_VISIBLE_ITEMS strings into the display buffer
     * and highlights the line at highlight_line (0-based within the
     * visible window).  Pass highlight_line = -1 for no highlight.
     */
    void writeLines(const std::vector<std::string>& lines, int highlight_line);

    // Software scrolling
    void scrollUp();
    void scrollDown();
    void setScrollPosition(int top_line);

    // Hardware scrolling
    void startHardwareScroll(ScrollMode mode, ssd1306_scroll_frames_t frame_speed);
    void startHardwareScroll(ScrollMode mode, uint8_t speed);
    void stopHardwareScroll();
    bool isHardwareScrolling() const { return m_hardware_scrolling; }

    // Batch updates
    void beginBatch();
    void endBatch();

    // Accessors
    int  getDisplayOffset() const { return m_display_offset; }
    int  getTotalLines()    const { return MAX_LINES; }

    /**
     * Returns the i2c_master_bus_handle_t created during init().
     * Use this to add the Seesaw encoder as a second device on the
     * same bus — avoids creating a conflicting second I2C peripheral.
     * Returns nullptr if init() has not been called yet.
     */
    i2c_master_bus_handle_t getBusHandle() const { return m_bus_handle; }

    /**
     * @brief Re-probe the bus to settle its internal state after all
     *        existing device transactions have completed.
     *
     * The new i2c_master driver tracks a bus->status field.  After
     * ssd1306_init() and subsequent display writes, the status may not
     * be I2C_STATUS_DONE from the bus object's perspective, even though
     * the wire is idle.  Calling i2c_master_probe() on any address resets
     * it.  Call this ONCE just before registering a second device
     * (e.g. the Seesaw encoder) on the same bus handle.
     *
     * @param addr    Address to probe (use the target device's address).
     * @param timeout_ms  Probe timeout in milliseconds.
     * @return ESP_OK if device ACKed, ESP_ERR_NOT_FOUND if no ACK (bus
     *         state is still reset either way).
     */
    esp_err_t probe_bus(uint8_t addr, uint32_t timeout_ms = 50) {
        if (!m_bus_handle) return ESP_ERR_INVALID_STATE;
        // Drain all pending transactions first, then probe to reset bus->status.
        i2c_master_bus_wait_all_done(m_bus_handle, 200);
        esp_err_t r = i2c_master_probe(m_bus_handle, addr, (int)timeout_ms);
        i2c_master_bus_wait_all_done(m_bus_handle, 100);
        vTaskDelay(pdMS_TO_TICKS(20));
        return r;
    }

    /**
     * Returns the mutex that guards all I2C transactions on this bus.
     * Any task that touches the bus must take this mutex for the transaction.
     * Timeout: portMAX_DELAY is fine since display operations are short
     * (<2ms per page write).
     */
    SemaphoreHandle_t getBusMutex() const { return m_bus_mutex; }

    /**
     * Render a single character inverted (white-on-black) at a specific
     * page and column without redrawing the whole display.  Call after
     * writeLines() / print() to overlay a cursor on the grid.
     *
     * @param page  Display page 0-7 (one page = 8 pixels high)
     * @param col   Character column 0-15 (one column = 8 pixels wide)
     * @param c     ASCII character to render
     */
    void render_char_inverted(int page, int col, char c);

    /**
     * Invert a horizontal run of character cells at pixel-level y position.
     * Used for cursor overlays with 9-px line spacing where characters span
     * two hardware pages.  Modifies the internal page buffer and flushes.
     *
     * @param y_px      Top pixel of the character row (0..63)
     * @param col_start First character column to invert (0..15)
     * @param num_cols  Number of columns to invert
     */
    void invert_char_cells(int y_px, int col_start, int num_cols);

    // Debug
    void debug_display_info();

private:
    static constexpr int SCREEN_WIDTH  = 128;
    static constexpr int SCREEN_HEIGHT = 64;
    static constexpr int CHAR_HEIGHT   = 8;   // font glyph height in pixels
    static constexpr int LINE_HEIGHT   = 9;   // glyph (8) + 1px inter-line gap
    static constexpr int VISIBLE_LINES = 7;   // floor(64 / 9) = 7 lines visible

    ssd1306_handle_t          m_dev;
    i2c_master_bus_handle_t   m_bus_handle;
    SemaphoreHandle_t         m_bus_mutex;
    bool  m_initialized;
    int   m_selected_line;
    int   m_display_offset;
    bool  m_batch_mode;
    bool  m_dirty;
    bool  m_hardware_scrolling;
    char  m_lines[MAX_LINES][MAX_LINE_LENGTH];

    void      renderLine(int line_num, int y_pos, bool is_selected);
    esp_err_t init_i2c_bus(int sda_pin, int scl_pin);
    void      refresh_display();
    ssd1306_scroll_types_t convertScrollMode(ScrollMode mode);
};