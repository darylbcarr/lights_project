#pragma once

/**
 * @file console_commands.h
 * @brief Simple UART command shell
 *
 * Commands (115200 baud, UART0):
 *
 *   net-status              Show full network status (IP, geo, RSSI, etc.)
 *   time [<fmt>]            Print current time (strftime format)
 *   i2c-scan                Scan I2C bus and print responding addresses
 *   matter-pair             Reopen BLE commissioning window (fast advertising)
 *   check-ota               Check GitHub for a firmware update now
 *   clear-wifi              Erase stored WiFi credentials and restart
 *   help                    List all commands
 */

#include "networking.h"
#include "matter_bridge.h"
#include "ota_manager.h"
#include "driver/i2c_master.h"
#include "freertos/semphr.h"

/**
 * @brief Start the UART command shell task.
 * @param net         Pointer to the shared Networking instance.
 * @param matter      Pointer to the shared MatterBridge.
 * @param ota         Pointer to the shared OtaManager.
 * @param bus_mutex   The I2C bus mutex owned by Display.
 * @param bus_handle  The I2C master bus handle (for i2c-scan).
 */
void console_start(Networking*             net,
                   MatterBridge*           matter,
                   OtaManager*             ota,
                   SemaphoreHandle_t       bus_mutex,
                   i2c_master_bus_handle_t bus_handle);
