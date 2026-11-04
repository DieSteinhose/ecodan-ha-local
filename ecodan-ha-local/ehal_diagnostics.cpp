#include <cmath>
#include "ehal_diagnostics.h"
#include "ehal_thirdparty.h"
#include "esp_err.h"
#include "psram_alloc.h"
#include <chrono>

#include "time.h"

#include <cstdio>
#include <deque>
#include <memory>
#include <mutex>

#if ARDUINO_ARCH_ESP32
#include <esp_task_wdt.h>
#include <esp_chip_info.h>
#endif

#if CONFIG_IDF_TARGET_ESP32S2 || CONFIG_IDF_TARGET_ESP32S3 || CONFIG_IDF_TARGET_ESP32C3
#include <driver/temperature_sensor.h>
#endif

namespace ehal
{
    std::mutex diagnosticRingbufferLock;
    psram::deque diagnosticRingbuffer;

#define MAX_MESSAGE_LENGTH 255U
#define MAX_NUM_ELEMENTS 32U

    void log_web(const __FlashStringHelper* fmt, ...)
    {
        std::unique_ptr<char[]> buffer{ new char[MAX_MESSAGE_LENGTH] };

        va_list args;
        va_start(args, fmt);

        // Include timestamp in diagnostic log message.
        time_t now = time(nullptr);
        struct tm t = *localtime(&now);
        strftime(buffer.get(), MAX_MESSAGE_LENGTH, "[%T] ", &t);
        const size_t offset = 11; // "[14:55:02] "

        // Format remainder of log message into buffer.
        vsnprintf_P(buffer.get() + offset, MAX_MESSAGE_LENGTH - offset, (PGM_P)fmt, args);

        va_end(args);

        // Block on the lock instead of busy-waiting. The busy-wait
        // (up to 10 s) could starve all lower-priority tasks including idle on the single-core ESP32-S2
        // through priority inversion (RX thread prio 5 vs. loopTask prio 1)
        // -> task watchdog. The FreeRTOS mutex passes its priority on to the holder.
        std::lock_guard<std::mutex> lock(diagnosticRingbufferLock);

        if (diagnosticRingbuffer.size() > MAX_NUM_ELEMENTS)
            diagnosticRingbuffer.pop_front();

        diagnosticRingbuffer.push_back(buffer.get());
    }

    void log_web_ratelimit(const __FlashStringHelper* fmt, ...)
    {
        static std::chrono::steady_clock::time_point last_log = std::chrono::steady_clock::now();

        std::chrono::steady_clock::time_point log_time = std::chrono::steady_clock::now();
        if (log_time - last_log > std::chrono::seconds(1))
        {
            last_log = log_time;
            std::unique_ptr<char[]> buffer{ new char[MAX_MESSAGE_LENGTH] };

            va_list args;
            va_start(args, fmt);

            // Include timestamp in diagnostic log message.
            time_t now = time(nullptr);
            struct tm t = *localtime(&now);
            strftime(buffer.get(), MAX_MESSAGE_LENGTH, "[%T] ", &t);
            const size_t offset = 11; // "[14:55:02] "

            // Format remainder of log message into buffer.
            vsnprintf_P(buffer.get() + offset, MAX_MESSAGE_LENGTH - offset, (PGM_P)fmt, args);

            va_end(args);

            std::lock_guard<std::mutex> lock(diagnosticRingbufferLock);

            if (diagnosticRingbuffer.size() > MAX_NUM_ELEMENTS)
                diagnosticRingbuffer.pop_front();

            diagnosticRingbuffer.push_back(buffer.get());
        }
    }

    String logs_as_json()
    {
        JsonDocument doc;        
        JsonArray msg = doc["messages"].to<JsonArray>();

        {
            std::lock_guard<std::mutex> lock(diagnosticRingbufferLock);

            psram::deque::const_iterator end = std::end(diagnosticRingbuffer);
            for (psram::deque::const_iterator it = std::begin(diagnosticRingbuffer); it != end; ++it)
            {
                msg.add(*it);
            }
        }

        String jsonOut;
        serializeJson(doc, jsonOut);

        return jsonOut;
    }

    float get_cpu_temperature()
    {
#if CONFIG_IDF_TARGET_ESP32S2 || CONFIG_IDF_TARGET_ESP32S3 || CONFIG_IDF_TARGET_ESP32C3
        static temperature_sensor_handle_t handle = nullptr;

        if (!handle)
        {
            temperature_sensor_config_t config = {
                .range_min = 40,
                .range_max = 80
            };
            ESP_ERROR_CHECK(temperature_sensor_install(&config, &handle));

            ESP_ERROR_CHECK(temperature_sensor_enable(handle)); 
        }

        float temp;
        temperature_sensor_get_celsius(handle, &temp);

        return temp;
#else
        return 0.0f;
#endif
    }

    void init_watchdog()
    {
#if ARDUINO_ARCH_ESP32
        esp_chip_info_t info = {};
        esp_chip_info(&info);

        // Reset the board if the watchdog timer isn't reset every 30s.
        esp_task_wdt_config_t config = {};
        config.timeout_ms = 30000;
        config.idle_core_mask = ((1 << info.cores) - 1);
        config.trigger_panic = true;
        
        esp_err_t ret = esp_task_wdt_init(&config);
        if (ret == ESP_ERR_INVALID_STATE)
            ret = esp_task_wdt_reconfigure(&config);
        
        if (ret == ESP_OK)
            ehal::log_web(F("Watchdog initialized."));
        else
            ehal::log_web(F("Watchdog initialization failed!"));
#endif
    }

    void add_thread_to_watchdog()
    {
#if ARDUINO_ARCH_ESP32
        esp_task_wdt_add(nullptr);
#endif
    }

    void ping_watchdog()
    {
#if ARDUINO_ARCH_ESP32
        esp_task_wdt_reset();
#endif
    }
} // namespace ehal
// ---------------------------------------------------------------------------------------------
// WiFi/reset diagnostics
// ---------------------------------------------------------------------------------------------
#include <WiFi.h>
#include <esp_attr.h>
#include <esp_system.h>

namespace ehal
{
    namespace
    {
        const uint32_t NETDIAG_MAGIC = 0xEC0DA116;

        struct RtcNetDiag
        {
            uint32_t magic;
            uint32_t uptimeS;        // last recorded uptime
            uint32_t disconnects;    // WiFi disconnects during this boot
            uint16_t lastReason;     // last disconnect reason (wifi_err_reason_t)
            uint8_t wifiReboot;      // 1 = restarted by the WiFi watchdog
            uint8_t pad;
        };

        RTC_NOINIT_ATTR RtcNetDiag rtcNet;
        RtcNetDiag prevBoot{};
        bool prevValid = false;
        esp_reset_reason_t resetReason = ESP_RST_UNKNOWN;

        volatile uint32_t wifiDisconnects = 0;
        volatile uint16_t wifiLastReason = 0;
        volatile uint32_t wifiLastDisconnectMs = 0;
        volatile uint32_t wifiLastConnectMs = 0;
        volatile bool wifiEventPending = false;

        const char* reset_reason_name(esp_reset_reason_t r)
        {
            switch (r)
            {
            case ESP_RST_POWERON: return "power-on";
            case ESP_RST_EXT: return "external pin";
            case ESP_RST_SW: return "software restart";
            case ESP_RST_PANIC: return "PANIC (crash)";
            case ESP_RST_INT_WDT: return "interrupt watchdog";
            case ESP_RST_TASK_WDT: return "task watchdog";
            case ESP_RST_WDT: return "other watchdog";
            case ESP_RST_DEEPSLEEP: return "deep sleep";
            case ESP_RST_BROWNOUT: return "BROWNOUT (supply voltage)";
            case ESP_RST_USB: return "USB";
            case ESP_RST_PWR_GLITCH: return "power glitch";
            case ESP_RST_CPU_LOCKUP: return "CPU lockup";
            default: return "unknown";
            }
        }

        String duration_text(uint32_t s)
        {
            char b[32];
            if (s >= 86400)
                snprintf(b, sizeof(b), "%lud %luh", (unsigned long)(s / 86400), (unsigned long)((s % 86400) / 3600));
            else if (s >= 3600)
                snprintf(b, sizeof(b), "%luh %lumin", (unsigned long)(s / 3600), (unsigned long)((s % 3600) / 60));
            else
                snprintf(b, sizeof(b), "%lumin %lus", (unsigned long)(s / 60), (unsigned long)(s % 60));
            return String(b);
        }

        String reason_text(uint16_t reason)
        {
            String s = String(reason);
            if (reason != 0)
            {
                s += F(" (");
                s += WiFi.STA.disconnectReasonName((wifi_err_reason_t)reason);
                s += F(")");
            }
            return s;
        }
    } // namespace

    void netdiag_init()
    {
        resetReason = esp_reset_reason();

        // RTC RAM survives software reset/panic/watchdog, but not power-on/brownout.
        if (rtcNet.magic == NETDIAG_MAGIC && resetReason != ESP_RST_POWERON && resetReason != ESP_RST_BROWNOUT)
        {
            prevBoot = rtcNet;
            prevValid = true;
        }

        rtcNet = RtcNetDiag{};
        rtcNet.magic = NETDIAG_MAGIC;

        WiFi.onEvent([](arduino_event_id_t event, arduino_event_info_t info) {
            if (event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED)
            {
                uint16_t reason = info.wifi_sta_disconnected.reason;
                // Only count the connected -> disconnected transition (not failed reconnects).
                if (wifiLastConnectMs != 0)
                {
                    ++wifiDisconnects;
                    wifiLastDisconnectMs = millis();
                }
                else if (wifiLastDisconnectMs == 0)
                {
                    wifiLastDisconnectMs = millis(); // first connection attempt at boot
                }
                // ASSOC_LEAVE (8) is caused by our own disconnect() during reconnect -> keep the original reason
                if (reason != WIFI_REASON_ASSOC_LEAVE || wifiLastReason == 0)
                    wifiLastReason = reason;
                wifiLastConnectMs = 0;
                wifiEventPending = true;
            }
            else if (event == ARDUINO_EVENT_WIFI_STA_GOT_IP)
            {
                wifiLastConnectMs = millis();
                wifiEventPending = true;
            }
        });
    }

    void netdiag_loop()
    {
        static uint32_t lastUpdate = 0;
        static bool loggedBoot = false;
        static bool wasConnected = false;
        uint32_t nowMs = millis();

        if (!loggedBoot)
        {
            loggedBoot = true;
            log_web(F("Reset reason: %s"), reset_diag_text().c_str());
        }

        if (wifiEventPending)
        {
            wifiEventPending = false;
            bool connected = wifiLastConnectMs != 0;
            if (connected && !wasConnected && wifiLastDisconnectMs != 0)
                log_web(F("WiFi reconnected after %lu s (last disconnect reason %s)"),
                        (unsigned long)((wifiLastConnectMs - wifiLastDisconnectMs) / 1000), reason_text(wifiLastReason).c_str());
            else if (!connected && wasConnected)
                log_web(F("WiFi disconnected, reason %s"), reason_text(wifiLastReason).c_str());
            wasConnected = connected;
        }

        if (nowMs - lastUpdate >= 1000)
        {
            lastUpdate = nowMs;
            rtcNet.uptimeS = nowMs / 1000;
            rtcNet.disconnects = wifiDisconnects;
            rtcNet.lastReason = wifiLastReason;
        }
    }

    void netdiag_note_wifi_reboot()
    {
        rtcNet.uptimeS = millis() / 1000;
        rtcNet.disconnects = wifiDisconnects;
        rtcNet.lastReason = wifiLastReason;
        rtcNet.wifiReboot = 1;
    }

    String reset_diag_text()
    {
        String s = reset_reason_name(resetReason);
        if (prevValid)
        {
            if (prevBoot.wifiReboot)
                s += F(" - WiFi watchdog restart");
            s += F("; previous run: ");
            s += duration_text(prevBoot.uptimeS);
            s += F(", ");
            s += String(prevBoot.disconnects);
            s += F(" WiFi disconnects, last reason ");
            s += reason_text(prevBoot.lastReason);
        }
        return s;
    }

    String wifi_diag_text()
    {
        String s = String((unsigned long)wifiDisconnects);
        s += F(" disconnects since boot");
        if (wifiLastReason != 0)
        {
            s += F(", last reason ");
            s += reason_text(wifiLastReason);
            if (wifiLastDisconnectMs != 0)
            {
                s += F(", ");
                s += duration_text((millis() - wifiLastDisconnectMs) / 1000);
                s += F(" ago");
            }
        }
        return s;
    }
} // namespace ehal
