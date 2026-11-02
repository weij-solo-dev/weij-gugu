// console.h — the serial test console (USB-Serial/JTAG on the device, stdin in the simulator):
// provisioning without the parent page (Wi-Fi, keys), driving calls and replays, inspecting state.
// Development and acceptance tooling; it ships but is only reachable over the debug port.
#pragma once
#include "esp_err.h"
esp_err_t console_init(void);
