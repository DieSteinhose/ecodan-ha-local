#pragma once

#include <Arduino.h>

// Safe OTA updates ("test mode").
//
// A firmware installed via OTA starts in the PENDING_VERIFY state. It is only marked as valid
// once it has proven to work (uptime + status cycles with the FTC + MQTT).
// If it crashes / hangs (watchdog) / is restarted before that, the bootloader restores the
// previous firmware on the next boot. Without confirmation within the
// time limit the firmware triggers the rollback itself.
namespace ehal::ota
{
    void initialize();      // call in setup() (logs the state)
    void handle_loop();     // call in loop() (confirmation / timeout)

    bool is_pending_verify();
    String state_description();     // for the diagnostics page

    // Manually switch back to the firmware in the other OTA partition (reboots).
    // Returns false if there is no valid firmware there.
    bool rollback_to_previous(String& error);
} // namespace ehal::ota
