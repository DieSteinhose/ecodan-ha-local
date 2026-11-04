#pragma once

#include <Arduino.h>

namespace ehal
{
    void log_web(const __FlashStringHelper* fmt, ...);
    void log_web_ratelimit(const __FlashStringHelper* fmt, ...);

    float get_cpu_temperature();

    String logs_as_json();

    void init_watchdog();
    void ping_watchdog();
    void add_thread_to_watchdog();

    // WiFi/reset diagnostics (survives software resets via RTC RAM)
    void netdiag_init();                 // call before WiFi.begin()
    void netdiag_loop();                 // call in loop()
    void netdiag_note_wifi_reboot();     // before ESP.restart() due to WiFi loss
    String reset_diag_text();
    String wifi_diag_text();
} // namespace ehal