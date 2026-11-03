#include "ehal_ota.h"
#include "ehal_diagnostics.h"
#include "ehal_hp.h"
#include "ehal_mqtt.h"

#include <Preferences.h>
#include <esp_ota_ops.h>

#include <chrono>

// Arduino core: by default initArduino() marks a new image as valid right at startup
// (before setup() even runs) -> a crash afterwards does NOT cause a rollback.
// With this override ehal::ota handles the confirmation itself.
extern "C" bool verifyRollbackLater()
{
    return true;
}

namespace ehal::ota
{
    namespace
    {
        const auto MIN_UPTIME = std::chrono::minutes(3);
        const uint32_t MIN_STATUS_CYCLES = 2;
        const auto VERIFY_TIMEOUT = std::chrono::minutes(10);

        bool pendingVerify = false;
        bool confirmed = false;
        esp_ota_img_states_t bootState = ESP_OTA_IMG_UNDEFINED;
        std::chrono::steady_clock::time_point bootTime;

        const char* state_name(esp_ota_img_states_t s)
        {
            switch (s)
            {
            case ESP_OTA_IMG_NEW: return "NEW";
            case ESP_OTA_IMG_PENDING_VERIFY: return "PENDING_VERIFY";
            case ESP_OTA_IMG_VALID: return "VALID";
            case ESP_OTA_IMG_INVALID: return "INVALID";
            case ESP_OTA_IMG_ABORTED: return "ABORTED";
            default: return "UNDEFINED";
            }
        }

#ifdef EHAL_OTA_CRASH_TEST
        // Test build: crashes ONCE on purpose in test mode to prove the automatic rollback
        // of the bootloader. The NVS flag prevents a boot loop in case the
        // bootloader unexpectedly cannot roll back.
        void maybe_crash_for_test()
        {
            static bool checked = false;
            if (checked || !pendingVerify)
                return;
            if (std::chrono::steady_clock::now() - bootTime < std::chrono::seconds(45))
                return;
            checked = true;

            Preferences prefs;
            prefs.begin("ehal_ota", false);
            bool alreadyCrashed = prefs.getBool("crashtest", false);
            if (!alreadyCrashed)
                prefs.putBool("crashtest", true);
            prefs.end();

            if (alreadyCrashed)
            {
                log_web(F("OTA crash test: already crashed once -> bootloader rollback did NOT happen!"));
                return;
            }

            log_web(F("OTA crash test: crashing deliberately now (expect rollback to previous firmware)"));
            delay(500);
            abort();
        }
#endif
    } // namespace

    void initialize()
    {
        bootTime = std::chrono::steady_clock::now();

        const esp_partition_t* running = esp_ota_get_running_partition();
        if (running == nullptr || esp_ota_get_state_partition(running, &bootState) != ESP_OK)
        {
            log_web(F("OTA: could not determine firmware state (no OTA data?)"));
            return;
        }

        pendingVerify = (bootState == ESP_OTA_IMG_PENDING_VERIFY);

        log_web(F("OTA: running from %s, state %s"), running->label, state_name(bootState));

        if (pendingVerify)
        {
            log_web(F("OTA: NEW FIRMWARE IN TEST MODE - confirms itself after %u min, >= %u status cycles and MQTT connected; otherwise automatic rollback (at the latest after %u min)"),
                    (unsigned)MIN_UPTIME.count(), (unsigned)MIN_STATUS_CYCLES, (unsigned)VERIFY_TIMEOUT.count());
        }
        else if (bootState == ESP_OTA_IMG_NEW)
        {
            // Bootloader did not set the state to PENDING_VERIFY -> rollback not supported.
            log_web(F("OTA: WARNING - state NEW at boot, bootloader apparently does not support rollback!"));
        }
    }

    void handle_loop()
    {
#ifdef EHAL_OTA_CRASH_TEST
        maybe_crash_for_test();
#endif

        if (!pendingVerify || confirmed)
            return;

        auto uptime = std::chrono::steady_clock::now() - bootTime;
        uint32_t cycles = hp::get_status_cycle_count();

        if (uptime >= MIN_UPTIME && cycles >= MIN_STATUS_CYCLES && mqtt::is_connected())
        {
            if (esp_ota_mark_app_valid_cancel_rollback() == ESP_OK)
            {
                confirmed = true;
                pendingVerify = false;
                bootState = ESP_OTA_IMG_VALID;
                log_web(F("OTA: firmware CONFIRMED after %u status cycles - test mode finished"), (unsigned)cycles);

                Preferences prefs;
                prefs.begin("ehal_ota", false);
                prefs.remove("crashtest");
                prefs.end();
            }
            else
            {
                log_web(F("OTA: marking firmware as valid FAILED"));
            }
            return;
        }

        if (uptime >= VERIFY_TIMEOUT)
        {
            log_web(F("OTA: test mode timeout (status cycles: %u, MQTT: %s) -> rolling back to previous firmware"),
                    (unsigned)cycles, mqtt::is_connected() ? "ok" : "not connected");
            delay(500);
            esp_ota_mark_app_invalid_rollback_and_reboot();
        }
    }

    bool is_pending_verify()
    {
        return pendingVerify;
    }

    String state_description()
    {
        const esp_partition_t* running = esp_ota_get_running_partition();
        String s = running ? String(running->label) : String(F("?"));
        s += F(", ");
        s += state_name(bootState);
        if (pendingVerify)
        {
            auto left = std::chrono::duration_cast<std::chrono::seconds>(MIN_UPTIME - (std::chrono::steady_clock::now() - bootTime)).count();
            s += F(" (TEST MODE: ");
            s += String((unsigned)hp::get_status_cycle_count());
            s += F("/");
            s += String((unsigned)MIN_STATUS_CYCLES);
            s += F(" cycles");
            if (left > 0)
            {
                s += F(", ");
                s += String((long)left);
                s += F(" s");
            }
            s += F(")");
        }
        return s;
    }

    bool rollback_to_previous(String& error)
    {
        const esp_partition_t* other = esp_ota_get_next_update_partition(nullptr);
        if (other == nullptr)
        {
            error = F("no other OTA partition");
            return false;
        }

        // Never switch back to a firmware that already crashed or was rejected.
        esp_ota_img_states_t otherState;
        if (esp_ota_get_state_partition(other, &otherState) == ESP_OK &&
            (otherState == ESP_OTA_IMG_INVALID || otherState == ESP_OTA_IMG_ABORTED))
        {
            error = String(F("firmware in ")) + other->label + F(" is marked ") + state_name(otherState);
            return false;
        }

        esp_app_desc_t desc;
        if (esp_ota_get_partition_description(other, &desc) != ESP_OK)
        {
            error = String(F("no valid firmware in ")) + other->label;
            return false;
        }

        // esp_ota_set_boot_partition verifies the full image (header + hash) before switching.
        esp_err_t err = esp_ota_set_boot_partition(other);
        if (err != ESP_OK)
        {
            error = String(F("firmware in ")) + other->label + F(" is invalid: ") + esp_err_to_name(err);
            return false;
        }

        log_web(F("OTA: manual switch to %s (%s %s), restarting..."), other->label, desc.project_name, desc.version);
        return true;
    }
} // namespace ehal::ota
