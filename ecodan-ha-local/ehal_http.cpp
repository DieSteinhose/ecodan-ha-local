#include "ehal_config.h"
#include "ehal_css_gz.h"
#include "ehal_ota.h"
#include "ehal_diagnostics.h"
#include "ehal_hp.h"
#include "ehal_html.h"
#include "ehal_http.h"
#include "ehal_js.h"
#include "ehal_mqtt.h"
#include "ehal_thirdparty.h"
#include "ehal.h"

#include <DNSServer.h>
#include <Update.h>
#include <WebServer.h>
#include <WiFi.h>

#include <chrono>
#include <thread>

namespace ehal::http
{
    WebServer server(80);
    std::unique_ptr<DNSServer> dnsServer;
    String loginCookie;
    uint8_t failedLoginCount = 0;

    String bool_to_emoji(bool value)
    {
        if (value)
            return F("&#x2705;");
        else
            return F("&#x274C;");
    }

    String uint64_to_string(uint64_t value)
    {
        char buffer[20] = {};
        snprintf(buffer, sizeof(buffer), "%lld", value);
        return buffer;
    }

    String configuration_status()
    {
        Config& config = config_instance();

        if (config.WifiSsid.isEmpty() || config.WifiPassword.isEmpty())
            return F("&#x26A0; WiFi configuration is incomplete!");

        if (config.MqttServer.isEmpty() || config.MqttPort == 0 || config.MqttUserName.isEmpty() || config.MqttPassword.isEmpty() || config.MqttTopic.isEmpty())
            return F("&#x26A0; MQTT configuration is incomplete!");

        if (config.DevicePassword.isEmpty())
            return F("&#x1F513; Setting a Device Password is recommended!");

        if (config.HostName != WiFi.getHostname())
            return F("&#x26A0; Configured Hostname is not being used!");

        return F("&#x2705;");
    }

    String generate_login_cookie()
    {
        String payload = config_instance().DevicePassword + String(xTaskGetTickCount());
        uint8_t sha256[32];

        mbedtls_md_context_t ctx;

        mbedtls_md_init(&ctx);
        mbedtls_md_setup(&ctx, mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), 0);
        mbedtls_md_starts(&ctx);
        mbedtls_md_update(&ctx, (const unsigned char*)payload.c_str(), payload.length());
        mbedtls_md_finish(&ctx, sha256);
        mbedtls_md_free(&ctx);

        String cookie = "";
        char hex[3] = {};
        for (int i = 0; i < sizeof(sha256); ++i)
        {
            snprintf(hex, sizeof(hex), "%02x", sha256[i]);
            cookie += hex;
        }

        return cookie;
    }

    bool show_login_if_required()
    {
        if (requires_first_time_configuration())
        {
            log_web(F("Skipping login, as first time configuration is required"));
            return false;
        }

        if (config_instance().DevicePassword.isEmpty())
        {
            log_web(F("Skipping login, as device password is unset."));
            return false;
        }

        if (!loginCookie.isEmpty())
        {
            String clientCookie = server.header(F("Cookie"));
            if (clientCookie.indexOf(String(F("login-cookie=")) + loginCookie) != -1)
            {
                return false;
            }

            log_web(F("Client cookie mismatch, redirecting to login page"));
        }
        else
        {
            log_web(F("Login cookie is unset, redirecting to login page"));
        }

        String page{FPSTR(PAGE_TEMPLATE)};
        page.replace(F("{{PAGE_SCRIPT}}"), "");
        page.replace(F("{{PAGE_BODY}}"), FPSTR(BODY_TEMPLATE_LOGIN));
        server.send(200, F("text/html"), page);
        return true;
    }

    void async_restart()
    {
        std::thread asyncRestart([]()
        {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            ESP.restart();
        });

        asyncRestart.detach();
    }

    void handle_root()
    {
        if (show_login_if_required())
            return;

        String page{F(PAGE_TEMPLATE)};
        page.replace(F("{{PAGE_SCRIPT}}"), "");
        page.replace(F("{{PAGE_BODY}}"), F(BODY_TEMPLATE_HOME));
        page.replace(F("{{hp_conn}}"), bool_to_emoji(hp::is_connected()));
        page.replace(F("{{mqtt_conn}}"), bool_to_emoji(mqtt::is_connected()));
        page.replace(F("{{config}}"), configuration_status());
        server.send(200, "text/html", page);
    }

    void handle_configure()
    {
        if (show_login_if_required())
            return;

        String page{F(PAGE_TEMPLATE)};
        page.replace(F("{{PAGE_SCRIPT}}"), F("src='/configuration.js'"));
        page.replace(F("{{PAGE_BODY}}"), F(BODY_TEMPLATE_CONFIG));

        Config& config = config_instance();
        page.replace(F("{{device_pw}}"), config.DevicePassword);
        page.replace(F("{{serial_rx}}"), String(config.SerialRxPort));
        page.replace(F("{{serial_tx}}"), String(config.SerialTxPort));
        page.replace(F("{{status_led}}"), String(config.StatusLed));

        if (config.DumpPackets)
            page.replace(F("{{dump_pkt}}"), F("checked"));
        else
            page.replace(F("{{dump_pkt}}"), "");

        if (config.WifiReset)
            page.replace(F("{{wifi_reset}}"), F("checked"));
        else
            page.replace(F("{{wifi_reset}}"), "");

        // Heat pump config
        if (config.CoolEnabled)
            page.replace(F("{{cool_enabled}}"), F("checked"));
        else
            page.replace(F("{{cool_enabled}}"), "");

        if (config.UniqueId.length() > 0)
            page.replace(F("{{unique_id}}"), config.UniqueId);
        else
            page.replace(F("{{unique_id}}"), device_mac());

        page.replace(F("{{wifi_ssid}}"), config.WifiSsid);
        page.replace(F("{{wifi_pw}}"), config.WifiPassword);
        page.replace(F("{{hostname}}"), config.HostName);
        page.replace(F("{{mqtt_server}}"), config.MqttServer);
        page.replace(F("{{mqtt_port}}"), String(config.MqttPort));
        page.replace(F("{{mqtt_user}}"), config.MqttUserName);
        page.replace(F("{{mqtt_pw}}"), config.MqttPassword);
        page.replace(F("{{mqtt_topic}}"), config.MqttTopic);

        server.send(200, F("text/html"), page);
    }

    void handle_configuration_js()
    {
        String js{F(SCRIPT_CONFIGURATION_PAGE)};

        Config& config = config_instance();
        js.replace(F("{{wifi_ssid}}"), config.WifiSsid);

        server.send(200, F("text/javascript"), js);
    }

    void handle_save_configuration()
    {
        Config config;
        config.DevicePassword = server.arg(F("device_pw"));
        config.SerialRxPort = server.arg(F("serial_rx")).toInt();
        config.SerialTxPort = server.arg(F("serial_tx")).toInt();
        config.StatusLed = server.arg(F("status_led")).toInt();

        if (server.hasArg(F("dump_pkt")))
            config.DumpPackets = true;
        else
            config.DumpPackets = false;

        if (server.hasArg(F("cool_enabled")))
            config.CoolEnabled = true;
        else
            config.CoolEnabled = false;

        if (server.hasArg(F("wifi_reset")))
            config.WifiReset = true;
        else
            config.WifiReset = false;

        config.UniqueId = server.arg(F("unique_id"));

        config.WifiSsid = server.arg(F("wifi_ssid"));
        config.WifiPassword = server.arg(F("wifi_pw"));

        config.HostName = server.arg(F("hostname"));
        config.MqttServer = server.arg(F("mqtt_server"));
        config.MqttPort = server.arg(F("mqtt_port")).toInt();
        config.MqttUserName = server.arg(F("mqtt_user"));
        config.MqttPassword = server.arg(F("mqtt_pw"));
        config.MqttTopic = server.arg(F("mqtt_topic"));
        save_configuration(config);

        String page{F(PAGE_TEMPLATE)};
        page.replace(F("{{PAGE_SCRIPT}}"), F("defer src='/reboot.js'"));
        page.replace(F("{{PAGE_BODY}}"), F(BODY_TEMPLATE_CONFIG_SAVED));
        server.sendHeader("Connection", "close");
        server.send(200, F("text/html"), page);

        async_restart();
    }

    void handle_reboot_js()
    {
        String js{F(SCRIPT_WAIT_REBOOT)};
        server.send(200, F("text/javascript"), js);
    }

    void handle_clear_config()
    {
        clear_configuration();

        String page{F(PAGE_TEMPLATE)};
        page.replace(F("{{PAGE_SCRIPT}}"), F("defer src='/reboot.js'"));
        page.replace(F("{{PAGE_BODY}}"), F(BODY_TEMPLATE_CONFIG_CLEARED));
        server.sendHeader(F("Connection"), F("close"));
        server.send(200, F("text/html"), page);

        async_restart();
    }

    void handle_query_ssid_list()
    {
        int16_t result = WiFi.scanComplete();

        if (result == WIFI_SCAN_RUNNING)
        {
            log_web(F("WiFi scan in progres..."));
            server.send(202, F("text/plain"), "");
        }
        else if (result == WIFI_SCAN_FAILED)
        {
            log_web(F("Starting WiFi scan..."));
            WiFi.scanNetworks(/* async = */ true);
            server.send(202, F("text/plain"), "");
        }
        else if (result >= 0)
        {
            log_web(F("Wifi Scan Result: %u"), result);

            JsonDocument doc;
            JsonObject json = doc.to<JsonObject>();
            JsonArray wifi = json["wifi"].to<JsonArray>();
            String jsonOut;

            for (int i = 0; i < result; ++i)
            {
                log_web(F("SSID: %s"), WiFi.SSID(i).c_str());
                JsonObject obj = wifi.add<JsonObject>();
                obj[F("ssid")] = WiFi.SSID(i);
                obj[F("rssi")] = WiFi.RSSI(i);
            }

            serializeJson(json, jsonOut);

            WiFi.scanDelete();
            server.send(200, F("text/plain"), jsonOut);
        }
        else
        {
            log_web(F("Unexpected WIFI scan result: %u"), result);
            server.send(500, F("text/plain"), "");
        }
    }

    void handle_query_life()
    {
        server.send(200, F("text/plain"), F("alive"));
    }

    void handle_query_diagnostic_logs()
    {
        server.send(200, F("text/plain"), logs_as_json());
    }

    void handle_diagnostics()
    {
        if (show_login_if_required())
            return;

        String page{FPSTR(PAGE_TEMPLATE)};
        page.replace(F("{{PAGE_SCRIPT}}"), F("src='/diagnostic.js'"));
        page.replace(F("{{PAGE_BODY}}"), F(BODY_TEMPLATE_DIAGNOSTICS));

        char deviceMac[19] = {};
        snprintf_P(deviceMac, sizeof(deviceMac), (PGM_P)F("%#llx"), ESP.getEfuseMac());

        page.replace(F("{{sw_ver}}"), get_software_version() + F(" [") + ota::state_description() + F("]"));
        page.replace(F("{{device_mac}}"), deviceMac);
        page.replace(F("{{device_cpus}}"), String(ESP.getChipCores()));
        page.replace(F("{{device_cpu_freq}}"), String(ESP.getCpuFreqMHz()));
        page.replace(F("{{device_free_heap}}"), String(ESP.getFreeHeap()));
        page.replace(F("{{device_total_heap}}"), String(ESP.getHeapSize()));
        page.replace(F("{{device_min_heap}}"), String(ESP.getMinFreeHeap()));
        page.replace(F("{{device_free_psram}}"), String(ESP.getFreePsram()));
        page.replace(F("{{device_total_psram}}"), String(ESP.getPsramSize()));
        page.replace(F("{{device_cpu_temp}}"), String(get_cpu_temperature()));

        page.replace(F("{{wifi_hostname}}"), WiFi.getHostname());
        page.replace(F("{{wifi_ip}}"), WiFi.localIP().toString());
        page.replace(F("{{wifi_gateway_ip}}"), WiFi.gatewayIP().toString());
        page.replace(F("{{wifi_mac}}"), WiFi.macAddress());
        page.replace(F("{{wifi_mac}}"), WiFi.macAddress());
        page.replace(F("{{wifi_rssi}}"), String(WiFi.RSSI()));
        page.replace(F("{{device_boot_time}}"), ehal::config_instance().BootTime);

        page.replace(F("{{ha_hp_entity}}"), String(F("climate.")) + ehal::mqtt::unique_entity_name(F("climate_control")));
        page.replace(F("{{ha_hp_z2_entity}}"), String(F("climate.")) + ehal::mqtt::unique_entity_name(F("climate_control_z2")));

        page.replace(F("{{hp_tx_count}}"), uint64_to_string(hp::get_tx_msg_count()));
        page.replace(F("{{hp_rx_count}}"), uint64_to_string(hp::get_rx_msg_count()));

        server.send(200, F("text/html"), page);
    }

    void handle_diagnostic_js()
    {
        String js{F(SCRIPT_UPDATE_DIAGNOSTIC_LOGS)};
        server.send(200, F("text/javascript"), js);
    }

    // Raw response of a GET type as hex string ("?" if never received)
    String raw_response_hex(uint8_t type)
    {
        uint8_t buf[16];
        uint32_t age = 0;
        if (!hp::get_last_response(type, buf, age))
            return F("?");
        char raw[16 * 3];
        for (size_t i = 0; i < 16; ++i)
            snprintf(raw + i * 3, 4, i < 15 ? "%02X " : "%02X", buf[i]);
        return String(raw);
    }

    void handle_heat_pump()
    {
        if (show_login_if_required())
            return;

        String page{F(PAGE_TEMPLATE)};
        page.replace(F("{{PAGE_SCRIPT}}"), "");
        page.replace(F("{{PAGE_BODY}}"), F(BODY_TEMPLATE_HEAT_PUMP));

        {
            auto& status = hp::get_status();
            std::lock_guard<hp::Status> lock{status};

            page.replace(F("{{z1_room_temp}}"), String(status.Zone1RoomTemperature, 1));
            page.replace(F("{{z1_set_temp}}"), String(status.Zone1SetTemperature, 1));
            page.replace(F("{{z2_room_temp}}"), String(status.Zone2RoomTemperature, 1));
            page.replace(F("{{z2_set_temp}}"), String(status.Zone2SetTemperature, 1));
            page.replace(F("{{dhw_temp}}"), String(status.DhwTemperature, 1));
            page.replace(F("{{dhw_set_temp}}"), String(status.DhwFlowTemperatureSetPoint, 1));
            page.replace(F("{{outside_temp}}"), String(status.OutsideTemperature, 1));

            // Hydraulics + THW10 candidates (GET 0x0E) in the web UI as well
            page.replace(F("{{hp_feed_temp}}"), String(status.DhwFeedTemperature, 1));
            page.replace(F("{{hp_return_temp}}"), String(status.DhwReturnTemperature, 1));
            page.replace(F("{{boiler_flow_temp}}"), String(status.BoilerFlowTemperature, 1));
            page.replace(F("{{boiler_return_temp}}"), String(status.BoilerReturnTemperature, 1));
            page.replace(F("{{z1_flow_target}}"), String(status.Zone1FlowTemperatureSetPoint, 1));
            page.replace(F("{{sh_flow_target}}"), String(status.RadiatorFlowTemperatureSetPoint, 1));
            page.replace(F("{{flow_rate}}"), String(status.FlowRate));
            page.replace(F("{{compressor_freq}}"), String(status.CompressorFrequency));
            // THW10, compressor
            page.replace(F("{{buffer_temp}}"), status.BufferTemperatureValid ? String(status.BufferTemperature, 1) : String(F("?")));
            page.replace(F("{{compressor_on}}"), status.CompressorRuntimeValid ? bool_to_emoji(status.CompressorOn) : String(F("?")));
            page.replace(F("{{compressor_runtime}}"), status.CompressorRuntimeValid ? String(status.CompressorRuntimeHours, 0) : String(F("?")));

            page.replace(F("{{sh_consumed}}"), String(status.EnergyConsumedHeating));
            page.replace(F("{{sh_delivered}}"), String(status.EnergyDeliveredHeating));
            if (status.EnergyConsumedHeating > 0.0f)
                page.replace(F("{{sh_cop}}"), String(status.EnergyDeliveredHeating / status.EnergyConsumedHeating));
            else
                page.replace(F("{{sh_cop}}"), "0.00");

            page.replace(F("{{cool_consumed}}"), String(status.EnergyConsumedCooling));
            page.replace(F("{{cool_delivered}}"), String(status.EnergyDeliveredCooling));
            if (status.EnergyConsumedCooling > 0.0f)
                page.replace(F("{{cool_cop}}"), String(status.EnergyDeliveredCooling / status.EnergyConsumedCooling));
            else
                page.replace(F("{{cool_cop}}"), "0.00");

            page.replace(F("{{dhw_consumed}}"), String(status.EnergyConsumedDhw));
            page.replace(F("{{dhw_delivered}}"), String(status.EnergyDeliveredDhw));
            if (status.EnergyConsumedDhw > 0.0f)
                page.replace(F("{{dhw_cop}}"), String(status.EnergyDeliveredDhw / status.EnergyConsumedDhw));
            else
                page.replace(F("{{dhw_cop}}"), "0.00");

            page.replace(F("{{out_pwr}}"), String(status.OutputPower));

            page.replace(F("{{mode_pwr}}"), status.power_as_string());
            page.replace(F("{{mode_op}}"), status.operation_as_string());
            page.replace(F("{{mode_hol}}"), bool_to_emoji(status.HolidayMode));
            page.replace(F("{{defrost}}"), bool_to_emoji(status.DefrostActive));
            page.replace(F("{{dhw_forced}}"), bool_to_emoji(status.DhwForcedActive));
            page.replace(F("{{mode_dhw_timer}}"), bool_to_emoji(status.DhwTimerMode));
            page.replace(F("{{mode_heating_cooling}}"), status.hp_mode_as_string());
            page.replace(F("{{mode_dhw}}"), status.dhw_mode_as_string());

            page.replace(F("{{min_flow_temp}}"), String(status.MinimumFlowTemperature));
            page.replace(F("{{max_flow_temp}}"), String(status.MaximumFlowTemperature));
        }

        // Configuration & raw data (outside the status lock)
        {
            int8_t z2 = hp::zone2_present();
            page.replace(F("{{zone2}}"), z2 < 0 ? String(F("unknown (DIP switches not read yet)")) : (z2 ? String(F("present")) : String(F("not present - zone 2 entities disabled"))));
            page.replace(F("{{dip_switches}}"), hp::dip_switches_as_string());
            page.replace(F("{{raw_0f}}"), String(F("0F: ")) + raw_response_hex(0x0F));
            page.replace(F("{{raw_13}}"), String(F("13: ")) + raw_response_hex(0x13));
            page.replace(F("{{raw_0e}}"), String(F("0E: ")) + raw_response_hex(0x0E));
        }

        server.send(200, F("text/html"), page);
    }

    // Track the upload result. Upstream ALWAYS answered HTTP 200 and rebooted,
    // even if Update.begin/write/end had failed -> a failure was only visible from the version.
    struct FirmwareUploadState
    {
        bool started = false;
        String error;
        size_t bytes = 0;
    } fwUpload;

    void fail_firmware_upload(const String& error)
    {
        if (fwUpload.error.isEmpty())
            fwUpload.error = error;
        if (Update.isRunning())
            Update.abort();
        log_web(F("Firmware update FAILED: %s"), fwUpload.error.c_str());
    }

    void handle_firmware_update()
    {
        String page{F(PAGE_TEMPLATE)};
        server.sendHeader(F("Connection"), F("close"));

        String error = fwUpload.error;
        if (!fwUpload.started)
            error = F("no firmware file received (form field 'update' missing?)");
        else if (error.isEmpty() && !Update.isFinished())
            error = F("upload incomplete");

        if (!error.isEmpty())
        {
            // No restart: the running firmware stays active.
            page.replace(F("{{PAGE_SCRIPT}}"), "");
            page.replace(F("{{PAGE_BODY}}"), String(F("<h1>Firmware update FAILED</h1><p>")) + error +
                         F("</p><p>The running firmware stays active, nothing was changed. <a href=\"/configuration\">Back</a></p>"));
            server.send(500, F("text/html"), page);
            fwUpload = FirmwareUploadState{};
            return;
        }

        log_web(F("Firmware update OK (%u bytes), restarting into test mode..."), (unsigned)fwUpload.bytes);
        page.replace(F("{{PAGE_SCRIPT}}"), F("defer src='/reboot.js'"));
        page.replace(F("{{PAGE_BODY}}"), String(F("<p>Firmware update OK (")) + String((unsigned)fwUpload.bytes) +
                     F(" bytes). Restarting - the new firmware runs in TEST MODE and confirms itself after ~3 min "
                       "(see Diagnostics), otherwise the previous firmware is restored automatically. "
                       "<span id=\"reboot_progress\"></span> <a href=\"/\">Home</a></p>"));
        server.send(200, F("text/html"), page);
        fwUpload = FirmwareUploadState{};

        async_restart();
    }

    void handle_firmware_update_handler()
    {
        HTTPUpload& upload = server.upload();
        switch (upload.status)
        {
        case UPLOAD_FILE_START:
        {
            fwUpload = FirmwareUploadState{};
            fwUpload.started = true;
            log_web(F("Firmware upload started: %s"), upload.filename.c_str());
            if (!Update.begin(UPDATE_SIZE_UNKNOWN))
            {
                fail_firmware_upload(String(F("Update.begin: ")) + Update.errorString());
            }
        }
        break;

        case UPLOAD_FILE_WRITE:
        {
            ehal::ping_watchdog(); // the upload can take > 30 s (task watchdog)
            if (!fwUpload.error.isEmpty())
                break; // discard the rest of the upload

            if (Update.write(upload.buf, upload.currentSize) != upload.currentSize)
            {
                fail_firmware_upload(String(F("Update.write at byte ")) + String((unsigned)fwUpload.bytes) + F(": ") + Update.errorString());
                break;
            }
            fwUpload.bytes += upload.currentSize;
        }
        break;

        case UPLOAD_FILE_END:
        {
            if (!fwUpload.error.isEmpty())
                break;

            if (fwUpload.bytes == 0)
            {
                fail_firmware_upload(F("empty file"));
            }
            else if (!Update.end(true))
            {
                // end() verifies the image (header, hash, ...) and only then sets the boot partition.
                fail_firmware_upload(String(F("Update.end (image check): ")) + Update.errorString());
            }
        }
        break;

        case UPLOAD_FILE_ABORTED:
        {
            fail_firmware_upload(F("upload aborted (connection lost)"));
        }
        break;
        }
    }

    void handle_redirect()
    {
        String page{F(PAGE_TEMPLATE)};
        page.replace(F("{{PAGE_SCRIPT}}"), F("src='/redirect.js'"));
        page.replace(F("{{PAGE_BODY}}"), F(BODY_TEMPLATE_REDIRECT));
        server.send(200, F("text/html"), page);
    }

    void handle_redirect_js()
    {
        String js{F(SCRIPT_REDIRECT)};
        js.replace(F("{{uri}}"), "/");
        server.send(200, F("text/javascript"), js);
    }

    void handle_verify_login()
    {
        if (server.arg(F("device_pw")) == config_instance().DevicePassword)
        {
            if (loginCookie.isEmpty())
            {
                loginCookie = generate_login_cookie();
            }

            log_web(F("Successful device login, authorising client."));

            server.sendHeader(F("Set-Cookie"), String(F("login-cookie=")) + loginCookie);

            // Reset brute-force mitigation count, to avoid upsetting legitimate users.
            failedLoginCount = 0;
        }
        else
        {
            log_web(F("Device password mismatch! Login attempted with '%s'"), server.arg(F("device_pw")).c_str());

            // Mitigate password brute-force.
            if (++failedLoginCount > 5)
            {
                std::this_thread::sleep_for(std::chrono::seconds(5));
                failedLoginCount = 10; // Prevent overflow of counter.
            }
        }

        handle_redirect();
    }

    // Manual switch back to the firmware in the other OTA partition (login required).
    void handle_firmware_rollback()
    {
        if (show_login_if_required())
            return;

        String error;
        if (!ota::rollback_to_previous(error))
        {
            log_web(F("Manual firmware rollback refused: %s"), error.c_str());
            server.send(409, F("text/plain"), String(F("Rollback not possible: ")) + error);
            return;
        }

        server.sendHeader(F("Connection"), F("close"));
        server.send(200, F("text/plain"), F("Switching to previous firmware, restarting..."));
        async_restart();
    }

    // Diagnostics. GET /cn105_probe            -> all raw responses received so far
    //               GET /cn105_probe?type=0x13  -> send one GET (whitelist only, read-only), wait up to 3 s
    void handle_cn105_probe()
    {
        if (show_login_if_required())
            return;

        String out;
        if (server.hasArg(F("type")))
        {
            uint8_t type = static_cast<uint8_t>(strtoul(server.arg(F("type")).c_str(), nullptr, 0));
            if (!hp::is_probe_type_allowed(type))
            {
                server.send(400, F("text/plain"), F("type not allowed (only read-only GET types 0x01..0x2F, 0xA1, 0xA2)"));
                return;
            }

            const uint32_t requestedAt = millis();
            if (!hp::request_probe(type))
            {
                server.send(503, F("text/plain"), F("heat pump not connected"));
                return;
            }

            // Wait for a response received AFTER sending (max. 3 s).
            bool got = false;
            while (!got && millis() - requestedAt < 3000)
            {
                delay(20);
                ehal::ping_watchdog();
                uint8_t buf[16];
                uint32_t age = 0;
                if (hp::get_last_response(type, buf, age) && age <= millis() - requestedAt)
                    got = true;
            }

            char hdr[48];
            snprintf(hdr, sizeof(hdr), "GET 0x%02X: ", type);
            out += hdr;
            out += got ? raw_response_hex(type) : String(F("no response within 3 s"));
            out += F("\n\n");
        }

        out += F("Last raw responses (payload bytes 0..15, byte 0 = type):\n");
        for (int t = 0; t < 256; ++t)
        {
            uint8_t buf[16];
            uint32_t age = 0;
            if (!hp::get_last_response(static_cast<uint8_t>(t), buf, age))
                continue;
            char line[96];
            snprintf(line, sizeof(line), "0x%02X (%lus ago): ", t, (unsigned long)(age / 1000));
            out += line;
            out += raw_response_hex(static_cast<uint8_t>(t));
            out += '\n';
        }

        server.send(200, F("text/plain"), out);
    }

    void handle_milligram_css()
    {
        // gzip-compressed from flash (10 KB -> 2.4 KB), no heap copy; browsers may cache it for 1 day.
        server.sendHeader(F("Content-Encoding"), F("gzip"));
        server.sendHeader(F("Cache-Control"), F("max-age=86400"));
        server.send_P(200, PSTR("text/css"), reinterpret_cast<const char*>(PAGE_CSS_GZ), PAGE_CSS_GZ_LEN);
    }

    void do_common_initialization()
    {
        // Common pages
        server.on(F("/diagnostics"), handle_diagnostics);
        server.on(F("/configuration"), handle_configure);
        server.on(F("/heat_pump"), handle_heat_pump);

        // Forms / magic URIs for buttons.
        server.on(F("/verify_login"), handle_verify_login);
        server.on(F("/save"), handle_save_configuration);
        server.on(F("/clear_config"), handle_clear_config);
        server.on(F("/update"), HTTP_POST, handle_firmware_update, handle_firmware_update_handler);
        server.on(F("/firmware_rollback"), HTTP_POST, handle_firmware_rollback);
        server.on(F("/cn105_probe"), HTTP_GET, handle_cn105_probe);

        // XMLHTTPRequest / Javascript / CSS
        server.on(F("/query_ssid"), handle_query_ssid_list);
        server.on(F("/query_life"), handle_query_life);
        server.on(F("/query_diagnostic_logs"), handle_query_diagnostic_logs);
        server.on(F("/configuration.js"), handle_configuration_js);
        server.on(F("/reboot.js"), handle_reboot_js);
        server.on(F("/diagnostic.js"), handle_diagnostic_js);
        server.on(F("/redirect.js"), handle_redirect_js);
        server.on(F("/milligram.css"), handle_milligram_css);

        const char* headers[] = {"Cookie"};
        server.collectHeaders(headers, sizeof(headers) / sizeof(char*));
        server.begin();
    }

    bool initialize_default()
    {
        ehal::log_web(F("Regular startup mode, initializing web-server..."));

        server.on("/", handle_root);
        server.onNotFound(handle_root);
        do_common_initialization();
        return true;
    }

    bool initialize_captive_portal()
    {
        dnsServer.reset(new DNSServer());
        if (!dnsServer)
        {
            log_web(F("Failed to allocate DNS server!"));
            return false;
        }

        if (!dnsServer->start(/*port =*/53, "*", WiFi.softAPIP()))
        {
            log_web(F("Failed to start DNS server!"));
            return false;
        }

        log_web(F("Initialized DNS server for captive portal."));

        server.on("/", handle_configure);
        server.onNotFound(handle_configure);
        do_common_initialization();
        return true;
    }

    void handle_loop()
    {
        if (dnsServer && requires_first_time_configuration())
        {
            dnsServer->processNextRequest();
        }

        server.handleClient();
    }
} // namespace ehal::http