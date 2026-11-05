#include "ehal.h"
#include "ehal_config.h"
#include "ehal_diagnostics.h"
#include "ehal_hp.h"
#include "ehal_mqtt.h"
#include "ehal_names.h"
#include "ehal_thirdparty.h"

#include <WiFi.h>
#include <WiFiClient.h>

#include <chrono>
#include <cmath>
#include <deque>
#include <string>
#include <thread>

namespace ehal::mqtt
{
#define SENSOR_STATE_TIMEOUT (300) // If we update HP state once a minute, expiring HA states after 300s seems appropriate.

    bool publish_climate_status();
    bool publish_z2_climate_status();
    bool publish_binary_sensor_status(const String& name, bool on);
    template <typename T>
    bool publish_sensor_status(const String& name, T value);
    bool connect_mqtt();
    void invalidate_published_states();

    std::mutex statusUpdateMtx;
    bool needsAutoDiscover = true;

    // Delta publishing. For each state topic a hash of the last sent payload
    // is stored; unchanged values are not resent. Full refresh every
    // 2 min (< exp_aft 300 s!), after (re)connect and when Home Assistant restarts (homeassistant/status).
    struct PublishedState
    {
        uint32_t topicHash;
        uint32_t payloadHash;
    };
    PublishedState publishedStates[64]; // ~50 state topics; fixed table instead of a hash map (smaller code)
    size_t publishedStateCount = 0;
    std::chrono::steady_clock::time_point lastFullStateRefresh = std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point statePublishHoldoff = std::chrono::steady_clock::now();
    // MUST be shorter than SENSOR_STATE_TIMEOUT (exp_aft = 300 s), otherwise HA sets unchanged
    // values to "unavailable" (with 10 min, sensors were unavailable 5 out of 10 min).
    // All values every 2 min (robust against briefly lost messages / connection drops).
    const auto FULL_STATE_REFRESH_INTERVAL = std::chrono::seconds(120);
    static_assert(120 + 10 < SENSOR_STATE_TIMEOUT, "full refresh (+tick) must happen before exp_aft");
    // 5 s (THW6/frequency now arrive every 20 s; delta publishing only sends changes)
    const auto STATE_PUBLISH_INTERVAL = std::chrono::seconds(5);
    const auto MQTT_RECONNECT_INTERVAL = std::chrono::seconds(30);
    // Max. age of outdoor unit values that are still sent (rotation: fast ~30 s, slow 5 min, hourly)
    const uint32_t RC_MAX_AGE_FAST_S = 120;
    const uint32_t RC_MAX_AGE_SLOW_S = 420;
    const uint32_t RC_MAX_AGE_HOURLY_S = 3900;
    const auto DISCOVERY_SETTLE_TIME = std::chrono::seconds(5); // HA needs a moment to create the entities
    WiFiClient espClient;
    MQTTClient mqttClient(4096);

    enum class SensorType
    {
        POWER,
        LIVE_POWER,
        FREQUENCY,
        TEMPERATURE,
        FLOW_RATE,
        DURATION_HOURS,
        DURATION_MINUTES,
        COUNT,
        CURRENT,
        KELVIN_DIFF,    // temperature difference (superheat/subcool)
        COP,
        CONNECTIVITY,
        WIFI_SIGNAL,
        WIFI_SSID,
        IP_ADDRESS,
        MAC_ADDRESS
    };

    // https://arduinojson.org/v6/how-to/configure-the-serialization-of-floats/#how-to-reduce-the-number-of-decimal-places
    double round2(double value)
    {
        return (int)(value * 100 + 0.5) / 100.0;
    }

    String get_mode_status_template()
    {
        String tpl(F(R"(
{% if (value_json is defined and value_json.mode is defined) %}
{{ value_json.mode }}
{% else %}
off
{% endif %})"));

        tpl.replace("\n", "");
        tpl.trim();
        return tpl;
    }

    String get_action_status_template()
    {
        String tpl(F(R"(
{% if (value_json is defined and value_json.mode is defined) %}
{{ value_json.action }}
{% else %}
off
{% endif %})"));

        tpl.replace("\n", "");
        tpl.trim();
        return tpl;
    }

    String get_temperature_status_template()
    {
        String tpl(F(R"(
{% if (value_json is defined and value_json.temperature is defined) %}
{% if (value_json.temperature|int >= {{min_temp}} and value_json.temperature|int <= {{max_temp}}) %}
{{ value_json.temperature }}
{% elif (value_json.temperature|int < {{min_temp}}) %}
{{min_temp}}
{% elif (value_json.temperature|int > {{max_temp}}) %}
{{max_temp}}
{% endif %}
{% else %}
21
{% endif %})"));

        auto& status = hp::get_status();
        std::lock_guard<hp::Status> lock{status};

        tpl.replace("\n", "");
        tpl.replace(F("{{min_temp}}"), String(hp::get_min_thermostat_temperature()));
        tpl.replace(F("{{max_temp}}"), String(hp::get_max_thermostat_temperature()));
        tpl.trim();

        return tpl;
    }

    String get_current_temperature_status_template()
    {
        String tpl(F(R"(
{% if (value_json is defined and value_json.curr_temp is defined) %}
{{ value_json.curr_temp }}
{% else %}
0
{% endif %})"));

        tpl.replace("\n", "");
        tpl.trim();

        return tpl;
    }

    void on_z1_temperature_set_command(const String& payload)
    {
        if (payload.isEmpty())
        {
            return;
        }

        float setTemperature = payload.toFloat();

        if (!hp::set_z1_target_temperature(setTemperature))
        {
            log_web(F("Failed to set z1 target temperature!"));
        }
        else
        {
            {
                auto& status = hp::get_status();
                std::lock_guard<hp::Status> lock{status};
                status.Zone1SetTemperature = setTemperature;
            }

            // Ensure lock is released, because publish_climate_status will attempt
            // to acquire it internally
            publish_climate_status();
        }
    }

    void on_z1_flow_target_temperature_set_command(const String& payload)
    {
        if (payload.isEmpty())
        {
            return;
        }

        float setTemperature = payload.toFloat();

        if (!hp::set_z1_flow_target_temperature(setTemperature))
        {
            log_web(F("Failed to set Z1 flow target temperature!"));
        }
        else
        {
            {
                auto& status = hp::get_status();
                std::lock_guard<hp::Status> lock{status};
                status.Zone1FlowTemperatureSetPoint = setTemperature;
            }

            publish_sensor_status<float>(F("z1_flow_temp_target"), setTemperature);
        }
    }

    void on_z2_temperature_set_command(const String& payload)
    {
        if (payload.isEmpty())
        {
            return;
        }

        float setTemperature = payload.toFloat();

        if (!hp::set_z2_target_temperature(setTemperature))
        {
            log_web(F("Failed to set z2 target temperature!"));
        }
        else
        {
            {
                auto& status = hp::get_status();
                std::lock_guard<hp::Status> lock{status};
                status.Zone2SetTemperature = setTemperature;
            }

            // Ensure lock is released, because publish_z2_climate_status will attempt
            // to acquire it internally
            publish_z2_climate_status();
        }
    }

    void on_z2_flow_target_temperature_set_command(const String& payload)
    {
        if (payload.isEmpty())
        {
            return;
        }

        float setTemperature = payload.toFloat();

        if (!hp::set_z2_flow_target_temperature(setTemperature))
        {
            log_web(F("Failed to set Z2 flow target temperature!"));
        }
        else
        {
            {
                auto& status = hp::get_status();
                std::lock_guard<hp::Status> lock{status};
                status.Zone2FlowTemperatureSetPoint = setTemperature;
            }

            publish_sensor_status<float>(F("z2_flow_temp_target"), setTemperature);
        }
    }

    void on_dhw_temperature_set_command(const String& payload)
    {
        if (payload.isEmpty())
        {
            return;
        }

        float setTemperature = payload.toFloat();

        if (!hp::set_dhw_target_temperature(setTemperature))
        {
            log_web(F("Failed to set DHW target temperature!"));
        }
        else
        {
            {
                // Upstream bug: wrote the DHW setpoint into Zone1SetTemperature (Z1 room setpoint)!
                // Result: the Z1 thermostat showed e.g. 50 °C, and set_z2_target_temperature
                // sent this value to the FTC as the Z1 setpoint.
                auto& status = hp::get_status();
                std::lock_guard<hp::Status> lock{status};
                status.DhwFlowTemperatureSetPoint = setTemperature;
            }

            publish_sensor_status<float>(F("dhw_flow_temp_target"), setTemperature);
        }
    }

    void on_mode_set_command(const String& payload)
    {
        uint8_t mode = -1;

        if (payload == "Heat Target Temperature")
        {
            mode = static_cast<uint8_t>(hp::Status::HpMode::HEAT_ROOM_TEMP);
        }
        else if (payload == "Heat Flow Temperature")
        {
            mode = static_cast<uint8_t>(hp::Status::HpMode::HEAT_FLOW_TEMP);
        }
        else if (payload == "Heat Compensation Curve")
        {
            mode = static_cast<uint8_t>(hp::Status::HpMode::HEAT_COMPENSATION_CURVE);
        }
        else if (payload == "Cool Target Temperature")
        {
            mode = static_cast<uint8_t>(hp::Status::HpMode::COOL_ROOM_TEMP);
        }
        else if (payload == "Cool Flow Temperature")
        {
            mode = static_cast<uint8_t>(hp::Status::HpMode::COOL_FLOW_TEMP);
        }
        else
        {
            log_web(F("Unexpected mode requested: %s"), payload.c_str());
            return;
        }

        if (!hp::set_hp_mode(mode))
        {
            log_web(F("Failed to set hp heating coling operation mode!"));
        }
        else
        {
            String modeStr;
            {
                auto& status = hp::get_status();
                std::lock_guard<hp::Status> lock{status};
                status.set_heating_cooling_mode(mode);
                modeStr = status.hp_mode_as_string();
            }

            publish_sensor_status<String>(F("mode_heating_cooling"), modeStr);
        }
    }

    void on_dhw_mode_set_command(const String& payload)
    {
        if (!hp::set_dhw_mode(payload))
        {
            log_web(F("Failed to set DHW mode!"));
        }
        else
        {
            String modeStr;
            {
                auto& status = hp::get_status();
                std::lock_guard<hp::Status> lock{status};
                if (payload == "eco")
                    status.HotWaterMode = hp::Status::DhwMode::ECO;
                else if (payload == "performance")
                    status.HotWaterMode = hp::Status::DhwMode::NORMAL;
                else if (payload == "off")
                    status.Operation = hp::Status::OperationMode::OFF;
                modeStr = status.dhw_mode_as_string();
            }

            publish_sensor_status<String>(F("mode_dhw"), modeStr);
        }
    }

    void on_force_dhw_command(const String& payload)
    {
        bool forced = payload == "ON";

        if (!hp::set_dhw_force(forced))
        {
            log_web(F("Failed to force DHW: %s"), payload.c_str());
        }
        else
        {
            {
                auto& status = hp::get_status();
                std::lock_guard<hp::Status> lock{status};
                status.DhwForcedActive = forced;
            }
            publish_binary_sensor_status(F("mode_dhw_forced"), forced);
        }
    }

    void on_turn_on_off_command(const String& payload)
    {
      bool turnON = payload == "ON";

      if (!hp::set_power_mode(turnON))
        {
            log_web(F("Failed to set power mode!"));
        }
        else
        {
            String powerStr;
            {
                auto& status = hp::get_status();
                std::lock_guard<hp::Status> lock{status};
                status.Power = turnON ? hp::Status::PowerMode::ON : hp::Status::PowerMode::STANDBY;
                powerStr = status.power_as_string();
            }
            publish_sensor_status<String>(F("mode_power"), powerStr);
        }
    }

    // Incoming commands are only buffered in the callback
    // and processed after mqttClient.loop(). Upstream published directly in the callback
    // (QoS2). lwmqtt reads further packets during publish() and calls the
    // callback recursively -> not reentrant: acks get lost, 10 s timeouts,
    // reconnect loops with delay() -> task watchdog reset on commands in quick
    // succession.
    std::deque<std::pair<String, String>> pendingCommands;
    const size_t MAX_PENDING_COMMANDS = 16;

    void mqtt_callback(String& topic, String& payload)
    {
        // Same topic already pending -> keep only the newest value.
        for (auto& cmd : pendingCommands)
        {
            if (cmd.first == topic)
            {
                cmd.second = payload;
                return;
            }
        }

        if (pendingCommands.size() >= MAX_PENDING_COMMANDS)
        {
            log_web(F("MQTT command backlog full, dropping: %s"), topic.c_str());
            return;
        }

        pendingCommands.emplace_back(topic, payload);
    }

    void handle_mqtt_command(const String& topic, const String& payload)
    {
        if (topic == F("homeassistant/status"))
        {
            if (payload == F("online"))
            {
                log_web(F("Home Assistant (re)started, re-publishing discovery and states"));
                needsAutoDiscover = true;
                invalidate_published_states();
            }
            return;
        }

        try
        {
            auto& config = config_instance();
            String z1ClimateEntity = unique_entity_name(F("climate_control"));
            String z2ClimateEntity = unique_entity_name(F("climate_control_z2"));
            String z1TempCmdTopic = config.MqttTopic + "/" + z1ClimateEntity + F("/temp_cmd");
            String z1FlowTargetCmdTopic = config.MqttTopic + "/" + unique_entity_name(F("z1_flow_temp_target")) + F("/set");
            String z2TempCmdTopic = config.MqttTopic + "/" + z2ClimateEntity + F("/temp_cmd");
            String z2FlowTargetCmdTopic = config.MqttTopic + "/" + unique_entity_name(F("z2_flow_temp_target")) + F("/set");
            String dhwForceCmdTopic = config.MqttTopic + "/" + unique_entity_name(F("force_dhw")) + F("/set");
            String turnOnOffCmdTopic = config.MqttTopic + "/" + unique_entity_name(F("turn_on_off_hp")) + F("/set");
            String dhwTempCmdTopic = config.MqttTopic + "/" + unique_entity_name(F("dhw_water_heater")) + F("/set");
            String dhwModeCmdTopic = config.MqttTopic + "/" + unique_entity_name(F("dhw_mode")) + F("/set");
            String shModeCmdTopic = config.MqttTopic + "/" + unique_entity_name(F("sh_mode")) + F("/set");

            log_web(F("MQTT topic received: %s: '%s'"), topic.c_str(), payload.c_str());

            if (z1TempCmdTopic == topic)
            {
                on_z1_temperature_set_command(payload);
            }
            else if (z1FlowTargetCmdTopic == topic)
            {
                on_z1_flow_target_temperature_set_command(payload);
            }
            else if ((z2TempCmdTopic == topic || z2FlowTargetCmdTopic == topic) && hp::zone2_present() == 0)
            {
                log_web(F("Ignoring zone 2 command, no zone 2 configured (DIP switches)"));
            }
            else if (z2TempCmdTopic == topic)
            {
                on_z2_temperature_set_command(payload);
            }
            else if (z2FlowTargetCmdTopic == topic)
            {
                on_z2_flow_target_temperature_set_command(payload);
            }
            else if (dhwTempCmdTopic == topic)
            {
                on_dhw_temperature_set_command(payload);
            }
            else if (shModeCmdTopic == topic)
            {
                on_mode_set_command(payload);
            }
            else if (dhwModeCmdTopic == topic)
            {
                on_dhw_mode_set_command(payload);
            }
            else if (dhwForceCmdTopic == topic)
            {
                on_force_dhw_command(payload);
            }
            else if (turnOnOffCmdTopic == topic)
            {
                on_turn_on_off_command(payload);
            }
        }
        catch (std::exception const& ex)
        {
            log_web(F("Exception on MQTT callback: %s"), ex.what());
        }
    }

    String get_connection_error_string()
    {
        switch (mqttClient.lastError())
        {
        case LWMQTT_NETWORK_FAILED_CONNECT:
            return F("LWMQTT_NETWORK_FAILED_CONNECT");
        case LWMQTT_NETWORK_TIMEOUT:
            return F("LWMQTT_NETWORK_TIMEOUT");
        case LWMQTT_NETWORK_FAILED_READ:
            return F("LWMQTT_NETWORK_FAILED_READ");
        case LWMQTT_NETWORK_FAILED_WRITE:
            return F("LWMQTT_NETWORK_FAILED_WRITE");
        case LWMQTT_MISSING_OR_WRONG_PACKET:
            return F("LWMQTT_MISSING_OR_WRONG_PACKET");
        case LWMQTT_CONNECTION_DENIED:
            return F("LWMQTT_CONNECTION_DENIED");
        case LWMQTT_FAILED_SUBSCRIPTION:
            return F("LWMQTT_FAILED_SUBSCRIPTION");
        case LWMQTT_PONG_TIMEOUT:
            return F("LWMQTT_PONG_TIMEOUT");
        default:
            return F("Unknown");
        }
    }

    bool is_configured()
    {
        Config& config = config_instance();
        if (config.MqttServer.isEmpty())
            return false;

        if (config.MqttTopic.isEmpty())
            return false;

        return true;
    }

    bool periodic_update_tick()
    {
        if (!is_configured())
            return false;

        auto now = std::chrono::steady_clock::now();
        static auto last_attempt = now + std::chrono::seconds(5);

        // No more periodic re-discovery (upstream never reset needsAutoDiscover
        // -> 52 discovery JSONs in EVERY tick). Discovery configs are now
        // retained; they are only resent on (re)connect and HA restart (birth message).
        if ((now - last_attempt < STATE_PUBLISH_INTERVAL))
            return false;

        last_attempt = now;

        return true;
    }

    // Display name for HA depending on the setting (technical / German / English).
    // key = technical name without UniqueId; fallback = previous name.
    String entity_display_name_for_key(const String& key, const String& fallback)
    {
        const auto lang = static_cast<names::Language>(config_instance().EntityNameLanguage);
        if (lang != names::Language::GERMAN && lang != names::Language::ENGLISH)
            return fallback;

        String k = key;
        k.replace(" ", "_");
        for (const auto& entry : names::ENTITY_NAMES)
        {
            if (k == entry.key)
                return String(lang == names::Language::GERMAN ? entry.de : entry.en);
        }
        return fallback; // unknown key -> technical name
    }

    String entity_display_name(const String& uniqueName)
    {
        // "<key>_<UniqueId>" -> "<key>"
        String suffix = String("_") + config_instance().UniqueId;
        String key = uniqueName;
        if (key.endsWith(suffix))
            key.remove(key.length() - suffix.length());
        return entity_display_name_for_key(key, uniqueName);
    }

    String unique_entity_name(const String& name)
    {
        String stringName = name;
        stringName.replace(" ", "_");
        return  stringName + "_" + config_instance().UniqueId;
    }

    void add_discovery_device_object(JsonObject obj)
    {
        JsonObject device = obj["device"].to<JsonObject>();
        JsonArray identifiers = device["ids"].to<JsonArray>();
        identifiers.add(device_mac());

        device[F("name")] = F("Mitsubishi A2W Heat Pump");
        device[F("sw")] = get_software_version();
        device[F("mdl")] = hp::get_device_model();
        device[F("mf")] = F("MITSUBISHI ELECTRIC");
        device[F("cu")] = String(F("http://")) + WiFi.localIP().toString() + F("/configuration");
    }

    bool publish_mqtt(const String& topic, const String& payload, bool retain = false)
    {
        const int RETRY_COUNT = 2;
        for (int i = 0; i < RETRY_COUNT; ++i)
        {
            ehal::ping_watchdog();

            // QoS1 instead of QoS2: one round trip per message instead of two, much shorter
            // blocking time of the loopTask (state values are re-sent periodically anyway).
            if (mqttClient.publish(topic, payload, retain, static_cast<int>(LWMQTT_QOS1)))
            {
                return true;
            }
            else
            {
                log_web(F("MQTT publishing failure: '%s': %d"), topic.c_str(), mqttClient.lastError());
            }

            if (!mqttClient.connected())
            {
                log_web(F("MQTT network disconnection detected trying to publish: '%s' attempting to re-connect: %d/%d"), topic.c_str(), i+1, RETRY_COUNT);
                connect_mqtt();
            }
        }

        needsAutoDiscover = true;
        return false;
    }

    bool publish_mqtt(const String& topic, const JsonDocument& json, bool retain = false)
    {
        String output;
        serializeJson(json, output);
        return publish_mqtt(topic, output, retain);
    }

    uint32_t fnv1a(const String& s)
    {
        uint32_t h = 2166136261u;
        for (size_t i = 0; i < s.length(); ++i)
        {
            h ^= static_cast<uint8_t>(s[i]);
            h *= 16777619u;
        }
        return h;
    }

    void invalidate_published_states()
    {
        publishedStateCount = 0;
        lastFullStateRefresh = std::chrono::steady_clock::now();
    }

    // Sends a (non-retained) state only if it changed since it was last sent.
    bool publish_state(const String& topic, const String& payload)
    {
        const uint32_t topicHash = fnv1a(topic);
        const uint32_t payloadHash = fnv1a(payload);

        PublishedState* entry = nullptr;
        for (size_t i = 0; i < publishedStateCount; ++i)
        {
            if (publishedStates[i].topicHash == topicHash)
            {
                entry = &publishedStates[i];
                break;
            }
        }

        if (entry != nullptr && entry->payloadHash == payloadHash)
            return true;

        if (!publish_mqtt(topic, payload))
        {
            if (entry != nullptr)
                entry->payloadHash = 0; // resend on the next tick
            return false;
        }

        if (entry == nullptr && publishedStateCount < sizeof(publishedStates) / sizeof(publishedStates[0]))
        {
            entry = &publishedStates[publishedStateCount++];
            entry->topicHash = topicHash;
        }

        if (entry != nullptr)
            entry->payloadHash = payloadHash;

        return true;
    }

    bool publish_state(const String& topic, const JsonDocument& json)
    {
        String output;
        serializeJson(json, output);
        return publish_state(topic, output);
    }

    // Remove an entity from HA (empty retained discovery message). Errors are not critical.
    void remove_ha_entity(const String& component, const String& name)
    {
        String topic = String(F("homeassistant/")) + component + "/" + unique_entity_name(name) + F("/config");
        mqttClient.publish(topic, String(), /* retain =*/true, static_cast<int>(LWMQTT_QOS1));
        ehal::ping_watchdog();
    }

    bool publish_ha_climate_auto_discover()
    {
        // https://www.home-assistant.io/integrations/climate.mqtt/
        String uniqueName = unique_entity_name(F("climate_control"));

        const auto& config = config_instance();
        String discoveryTopic = String(F("homeassistant/climate/")) + uniqueName + F("/config");
        String stateTopic = config.MqttTopic + "/" + uniqueName + F("/state");
        String tempCmdTopic = config.MqttTopic + "/" + uniqueName + F("/temp_cmd");

        JsonDocument doc;
        JsonObject payloadJson = doc.to<JsonObject>();
        payloadJson[F("name")] = entity_display_name(uniqueName);
        payloadJson[F("unique_id")] = uniqueName;
        payloadJson[F("icon")] = F("mdi:heat-pump-outline");

        add_discovery_device_object(payloadJson);

        payloadJson[F("mode_stat_t")] = stateTopic;
        payloadJson[F("mode_stat_tpl")] = get_mode_status_template();
        payloadJson[F("act_t")] = stateTopic;
        payloadJson[F("act_tpl")] = get_action_status_template();
        payloadJson[F("temp_stat_t")] = stateTopic;
        payloadJson[F("temp_stat_tpl")] = get_temperature_status_template();
        payloadJson[F("curr_temp_t")] = stateTopic;
        payloadJson[F("curr_temp_tpl")] = get_current_temperature_status_template();
        payloadJson[F("temp_cmd_t")] = tempCmdTopic;
        payloadJson[F("temp_cmd_tpl")] = F("{{ value|float }}");
        payloadJson[F("temp_cmd_tpl")] = F("{{ value }}");

        {
            auto& status = hp::get_status();
            std::lock_guard<hp::Status> lock{status};

            payloadJson[F("initial")] = status.Zone1SetTemperature;
            payloadJson[F("min_temp")] = hp::get_min_thermostat_temperature();
            payloadJson[F("max_temp")] = hp::get_max_thermostat_temperature();
            payloadJson[F("temp_unit")] = "C";
            payloadJson[F("temp_step")] = hp::get_temperature_step();
        }

        JsonArray modes = payloadJson["modes"].to<JsonArray>();
        modes.add(F("heat"));
        modes.add(F("off"));

        if (!publish_mqtt(discoveryTopic, doc, /* retain =*/true))
        {
            log_web(F("Failed to publish homeassistant climate entity auto-discover"));
            return false;
        }

        return true;
    }

    bool publish_ha_climate_z2_auto_discover()
    {
        // https://www.home-assistant.io/integrations/climate.mqtt/
        String uniqueName = unique_entity_name(F("climate_control_z2"));

        const auto& config = config_instance();
        String discoveryTopic = String(F("homeassistant/climate/")) + uniqueName + F("/config");
        String stateTopic = config.MqttTopic + "/" + uniqueName + F("/state");
        String tempCmdTopic = config.MqttTopic + "/" + uniqueName + F("/temp_cmd");

        JsonDocument doc;
        JsonObject payloadJson = doc.to<JsonObject>();
        payloadJson[F("name")] = entity_display_name(uniqueName);
        payloadJson[F("unique_id")] = uniqueName;
        payloadJson[F("icon")] = F("mdi:heat-pump-outline");

        add_discovery_device_object(payloadJson);

        payloadJson[F("mode_stat_t")] = stateTopic;
        payloadJson[F("mode_stat_tpl")] = get_mode_status_template();
        payloadJson[F("act_t")] = stateTopic;
        payloadJson[F("act_tpl")] = get_action_status_template();
        payloadJson[F("temp_stat_t")] = stateTopic;
        payloadJson[F("temp_stat_tpl")] = get_temperature_status_template();
        payloadJson[F("curr_temp_t")] = stateTopic;
        payloadJson[F("curr_temp_tpl")] = get_current_temperature_status_template();
        payloadJson[F("temp_cmd_t")] = tempCmdTopic;
        payloadJson[F("temp_cmd_tpl")] = F("{{ value|float }}");
        payloadJson[F("temp_cmd_tpl")] = F("{{ value }}");

        {
            auto& status = hp::get_status();
            std::lock_guard<hp::Status> lock{status};

            payloadJson[F("initial")] = status.Zone2SetTemperature;
            payloadJson[F("min_temp")] = hp::get_min_thermostat_temperature();
            payloadJson[F("max_temp")] = hp::get_max_thermostat_temperature();
            payloadJson[F("temp_unit")] = "C";
            payloadJson[F("temp_step")] = hp::get_temperature_step();
        }

        JsonArray modes = payloadJson["modes"].to<JsonArray>();
        modes.add(F("heat"));
        modes.add(F("off"));

        if (!publish_mqtt(discoveryTopic, doc, /* retain =*/true))
        {
            log_web(F("Failed to publish homeassistant climate entity auto-discover"));
            return false;
        }

        return true;
    }

    bool publish_ha_force_dhw_auto_discover()
    {
        // https://www.home-assistant.io/integrations/switch.mqtt/
        String uniqueName = unique_entity_name(F("force_dhw"));

        const auto& config = config_instance();
        String discoveryTopic = String(F("homeassistant/switch/")) + uniqueName + F("/config");
        String stateTopic = config.MqttTopic + "/" + unique_entity_name(F("mode_dhw_forced")) + F("/state");
        String cmdTopic = config.MqttTopic + "/" + uniqueName + F("/set");

        JsonDocument doc;
        JsonObject payloadJson = doc.to<JsonObject>();
        payloadJson[F("name")] = entity_display_name(uniqueName);
        payloadJson[F("unique_id")] = uniqueName;
        payloadJson[F("icon")] = F("mdi:toggle-switch-variant");

        add_discovery_device_object(payloadJson);

        payloadJson[F("stat_t")] = stateTopic;
        payloadJson[F("stat_t_tpl")] = F("{{ value }}");
        payloadJson[F("stat_on")] = F("on");
        payloadJson[F("stat_off")] = F("off");
        payloadJson[F("cmd_t")] = cmdTopic;
        payloadJson[F("cmd_tpl")] = F("{{ value }}");

        if (!publish_mqtt(discoveryTopic, doc, /* retain =*/true))
        {
            log_web(F("Failed to publish homeassistant force DHW entity auto-discover"));
            return false;
        }

        return true;
    }

    bool publish_ha_set_z1_flow_target_auto_discover()
    {
        // https://www.home-assistant.io/integrations/number.mqtt/
        String uniqueName = unique_entity_name(F("z1_flow_temp_target"));

        const auto& config = config_instance();
        auto& status = hp::get_status();
        String discoveryTopic = String(F("homeassistant/number/")) + uniqueName + F("/config");
        String stateTopic = config.MqttTopic + "/" + unique_entity_name(F("z1_flow_temp_target")) + F("/state");
        String cmdTopic = config.MqttTopic + "/" + uniqueName + F("/set");

        JsonDocument doc;
        JsonObject payloadJson = doc.to<JsonObject>();
        payloadJson[F("name")] = entity_display_name_for_key(F("z1_flow_temp_target_set"), uniqueName); // number entity != status sensor
        payloadJson[F("unique_id")] = uniqueName;

        add_discovery_device_object(payloadJson);

        payloadJson[F("stat_t")] = stateTopic;
        payloadJson[F("stat_t_tpl")] = F("{{ value }}");
        payloadJson[F("cmd_t")] = cmdTopic;
        payloadJson[F("cmd_tpl")] = F("{{ value }}");
        payloadJson[F("min")] = String(ehal::hp::get_min_flow_target_temperature(status.hp_mode_as_string()));
        payloadJson[F("max")] = String(ehal::hp::get_max_flow_target_temperature(status.hp_mode_as_string()));
        payloadJson[F("step")] = 1;
        payloadJson[F("dev_cla")] = F("temperature");
        payloadJson[F("unit_of_meas")] = F("°C");
        payloadJson[F("icon")] = String("mdi:thermometer-water");

        if (!publish_mqtt(discoveryTopic, doc, /* retain =*/true))
        {
            log_web(F("Failed to publish homeassistant Z1 flow temperature set entity auto-discover"));
            return false;
        }

        return true;
    }

    bool publish_ha_turn_on_off_auto_discover()
    {
        // https://www.home-assistant.io/integrations/switch.mqtt/
        String uniqueName = unique_entity_name(F("turn_on_off_hp"));

        const auto& config = config_instance();
        String discoveryTopic = String(F("homeassistant/switch/")) + uniqueName + F("/config");
        String stateTopic = config.MqttTopic + "/" + unique_entity_name(F("mode_power")) + F("/state");
        String cmdTopic = config.MqttTopic + "/" + uniqueName + F("/set");

        JsonDocument doc;
        JsonObject payloadJson = doc.to<JsonObject>();
        payloadJson[F("name")] = entity_display_name(uniqueName);
        payloadJson[F("unique_id")] = uniqueName;
        payloadJson[F("icon")] = F("mdi:power");

        add_discovery_device_object(payloadJson);

        payloadJson[F("stat_t")] = stateTopic;
        payloadJson[F("stat_t_tpl")] = F("{{ value }}");
        payloadJson[F("stat_on")] = F("On");
        payloadJson[F("stat_off")] = F("Standby");
        payloadJson[F("cmd_t")] = cmdTopic;
        payloadJson[F("cmd_tpl")] = F("{{ value }}");

        if (!publish_mqtt(discoveryTopic, doc, /* retain =*/true))
        {
            log_web(F("Failed to publish homeassistant turn On/Off HP entity auto-discover"));
            return false;
        }

        return true;
    }

    bool publish_ha_set_dhw_temp_auto_discover()
    {
        // https://www.home-assistant.io/integrations/water_heater.mqtt/
        String uniqueName = unique_entity_name(F("dhw_water_heater"));

        const auto& config = config_instance();
        String discoveryTopic = String(F("homeassistant/water_heater/")) + uniqueName + F("/config");

        JsonDocument doc;
        JsonObject payloadJson = doc.to<JsonObject>();
        payloadJson[F("name")] = entity_display_name(uniqueName);
        payloadJson[F("unique_id")] = uniqueName;

        add_discovery_device_object(payloadJson);

        payloadJson[F("curr_temp_t")] = config.MqttTopic + "/" + unique_entity_name(F("dhw_temp")) + F("/state");
        payloadJson[F("temp_cmd_t")] = config.MqttTopic + "/" + uniqueName + F("/set");
        payloadJson[F("temp_stat_t")] = config.MqttTopic + "/" + unique_entity_name(F("dhw_flow_temp_target")) + F("/state");
        payloadJson[F("mode_stat_t")] = config.MqttTopic + "/" + unique_entity_name(F("mode_dhw")) + F("/state");
        payloadJson[F("mode_stat_tpl")] = "{% if value==\"Eco\" %} eco {% elif value==\"Normal\" %} performance {% else %} off {% endif %}";
        // (fork VonLatvala 73d5a0f): must be a topic the firmware subscribes to. Previously pointed to the
        // STATE topic of mode_dhw_forced -> on/off on the water_heater only corrupted the DHW boost state.
        payloadJson[F("power_cmd_t")] = config.MqttTopic + "/" + unique_entity_name(F("force_dhw")) + F("/set");
        payloadJson[F("mode_cmd_t")] = config.MqttTopic + "/" + unique_entity_name(F("dhw_mode")) + F("/set");
        payloadJson[F("min_temp")] = String(ehal::hp::get_min_dhw_temperature());
        payloadJson[F("max_temp")] = String(ehal::hp::get_max_dhw_temperature());
        JsonArray modes = payloadJson["modes"].to<JsonArray>();
        modes.add("off");
        modes.add("eco");
        modes.add("performance");
        payloadJson[F("temp_unit")] = "C";
        payloadJson[F("precision")] = 0.5f;

        if (!publish_mqtt(discoveryTopic, doc, /* retain =*/true))
        {
            log_web(F("Failed to publish homeassistant DHW temperature set entity auto-discover"));
            return false;
        }

        return true;
    }

    bool publish_ha_set_sh_mode_auto_discover()
    {
        // https://www.home-assistant.io/integrations/select.mqtt/
        String uniqueName = unique_entity_name(F("sh_mode"));

        const auto& config = config_instance();
        String discoveryTopic = String(F("homeassistant/select/")) + uniqueName + F("/config");

        JsonDocument doc;
        JsonObject payloadJson = doc.to<JsonObject>();
        payloadJson[F("name")] = entity_display_name(uniqueName);
        payloadJson[F("unique_id")] = uniqueName;

        add_discovery_device_object(payloadJson);

        payloadJson[F("stat_t")] = config.MqttTopic + "/" + unique_entity_name(F("mode_heating_cooling")) + F("/state");
        payloadJson[F("cmd_t")] = config.MqttTopic + "/" + unique_entity_name(F("sh_mode")) + F("/set");
        JsonArray options = payloadJson["options"].to<JsonArray>();
        options.add("Heat Target Temperature");
        options.add("Heat Flow Temperature");
        options.add("Heat Compensation Curve");
        if (config.CoolEnabled) {
          options.add("Cool Target Temperature");
          options.add("Cool Flow Temperature");
        }

        if (!publish_mqtt(discoveryTopic, doc, /* retain =*/true))
        {
            log_web(F("Failed to publish homeassistant SH mode entity auto-discover"));
            return false;
        }

        return true;
    }

    bool publish_ha_binary_sensor_auto_discover(const String& name)
    {
        const auto& config = config_instance();
        String uniqueName = unique_entity_name(name);
        String discoveryTopic = String(F("homeassistant/binary_sensor/")) + uniqueName + F("/config");
        String stateTopic = config.MqttTopic + "/" + uniqueName + F("/state");

        // https://www.home-assistant.io/integrations/binary_sensor.mqtt/
        JsonDocument doc;
        JsonObject payloadJson = doc.to<JsonObject>();
        payloadJson[F("name")] = entity_display_name(uniqueName);
        payloadJson[F("unique_id")] = uniqueName;

        add_discovery_device_object(payloadJson);

        payloadJson[F("stat_t")] = stateTopic;
        payloadJson[F("payload_off")] = F("off");
        payloadJson[F("payload_on")] = F("on");
        payloadJson[F("exp_aft")] = SENSOR_STATE_TIMEOUT;

        if (!publish_mqtt(discoveryTopic, doc, /* retain =*/true))
        {
            log_web(F("Failed to publish homeassistant %s entity auto-discover"), uniqueName.c_str());
            return false;
        }

        return true;
    }

    bool publish_ha_float_sensor_auto_discover(const String& name, SensorType type)
    {
        const auto& config = config_instance();
        String uniqueName = unique_entity_name(name);
        String discoveryTopic = String(F("homeassistant/sensor/")) + uniqueName + F("/config");
        String stateTopic = config.MqttTopic + "/" + uniqueName + F("/state");

        // https://www.home-assistant.io/integrations/sensor.mqtt/
        JsonDocument doc;
        JsonObject payloadJson = doc.to<JsonObject>();
        payloadJson[F("name")] = entity_display_name(uniqueName);
        payloadJson[F("unique_id")] = uniqueName;

        add_discovery_device_object(payloadJson);

        payloadJson[F("stat_t")] = stateTopic;
        payloadJson[F("val_tpl")] = F("{{ value|float }}");
        payloadJson[F("exp_aft")] = SENSOR_STATE_TIMEOUT;

        switch (type)
        {
        // device/state classes from fork VonLatvala 73d5a0f (+ output_pwr fix from HA log).
        case SensorType::POWER:
            payloadJson[F("unit_of_meas")] = F("kWh");
            payloadJson[F("icon")] = F("mdi:lightning-bolt");
            // Daily values (0xA1/0xA2 contain a date) are reset every day -> total_increasing,
            // otherwise HA would count the reset as negative energy.
            payloadJson[F("stat_cla")] = F("total_increasing");
            payloadJson[F("dev_cla")] = F("energy");
            break;

        case SensorType::LIVE_POWER:
            payloadJson[F("unit_of_meas")] = F("kW");
            payloadJson[F("icon")] = F("mdi:lightning-bolt");
            payloadJson[F("stat_cla")] = F("measurement");
            payloadJson[F("dev_cla")] = F("power"); // was "energy" -> HA rejected the sensor (unavailable)
            break;

        case SensorType::FREQUENCY:
            payloadJson[F("unit_of_meas")] = F("Hz");
            payloadJson[F("icon")] = F("mdi:fan");
            payloadJson[F("stat_cla")] = F("measurement");
            payloadJson[F("dev_cla")] = F("frequency");
            break;

        case SensorType::FLOW_RATE:
            payloadJson[F("unit_of_meas")] = F("L/min");
            payloadJson[F("icon")] = F("mdi:pump");
            payloadJson[F("stat_cla")] = F("measurement");
            // payloadJson[F("dev_cla")] = F("water");
            break;

        case SensorType::TEMPERATURE:
            payloadJson[F("unit_of_meas")] = F("°C");
            payloadJson[F("stat_cla")] = F("measurement");
            payloadJson[F("dev_cla")] = F("temperature");
            break;

        case SensorType::DURATION_MINUTES:
            payloadJson[F("unit_of_meas")] = F("min");
            payloadJson[F("icon")] = F("mdi:timer-sand");
            payloadJson[F("stat_cla")] = F("measurement");
            payloadJson[F("dev_cla")] = F("duration");
            break;

        case SensorType::CURRENT:
            payloadJson[F("unit_of_meas")] = F("A");
            payloadJson[F("stat_cla")] = F("measurement");
            payloadJson[F("dev_cla")] = F("current");
            break;

        case SensorType::KELVIN_DIFF:
            payloadJson[F("unit_of_meas")] = F("K");
            payloadJson[F("icon")] = F("mdi:thermometer-lines");
            payloadJson[F("stat_cla")] = F("measurement");
            break;

        case SensorType::COUNT:
            payloadJson[F("icon")] = F("mdi:counter");
            payloadJson[F("stat_cla")] = F("total_increasing");
            break;

        case SensorType::DURATION_HOURS:
            payloadJson[F("unit_of_meas")] = F("h");
            payloadJson[F("icon")] = F("mdi:timer-outline");
            payloadJson[F("stat_cla")] = F("total_increasing");
            payloadJson[F("dev_cla")] = F("duration");
            break;

        case SensorType::COP:
            payloadJson[F("icon")] = F("mdi:heat-pump-outline");
            payloadJson[F("unit_of_meas")] = F("COP");
            payloadJson[F("stat_cla")] = F("measurement");
            break;

        default:
            break;
        }

        if (!publish_mqtt(discoveryTopic, doc, /* retain =*/true))
        {
            log_web(F("Failed to publish homeassistant %s entity auto-discover"), uniqueName.c_str());
            return false;
        }

        return true;
    }

    bool publish_ha_string_sensor_auto_discover(const String& name, const String& icon = "")
    {
        const auto& config = config_instance();
        String uniqueName = unique_entity_name(name);
        String discoveryTopic = String(F("homeassistant/sensor/")) + uniqueName + F("/config");
        String stateTopic = config.MqttTopic + "/" + uniqueName + F("/state");

        // https://www.home-assistant.io/integrations/sensor.mqtt/
        JsonDocument doc;
        JsonObject payloadJson = doc.to<JsonObject>();
        payloadJson[F("name")] = entity_display_name(uniqueName);
        payloadJson[F("unique_id")] = uniqueName;

        add_discovery_device_object(payloadJson);

        payloadJson[F("stat_t")] = stateTopic;
        payloadJson[F("val_tpl")] = F("{{ value }}");
        payloadJson[F("exp_aft")] = SENSOR_STATE_TIMEOUT;

        if (!icon.isEmpty())
        {
            payloadJson[F("icon")] = icon;
        }

        if (!publish_mqtt(discoveryTopic, doc, /* retain =*/true))
        {
            log_web(F("Failed to publish homeassistant %s entity auto-discover"), uniqueName.c_str());
            return false;
        }

        return true;
    }

    bool publish_ha_diagnostic_sensor_auto_discover(const String& name, SensorType type)
    {
        const auto& config = config_instance();
        String uniqueName = unique_entity_name(name);
        String discoveryTopic = String(F("homeassistant/sensor/")) + uniqueName + F("/config");
        String stateTopic = config.MqttTopic + "/" + uniqueName + F("/state");

        // https://www.home-assistant.io/integrations/sensor.mqtt/
        JsonDocument doc;
        JsonObject payloadJson = doc.to<JsonObject>();
        payloadJson[F("name")] = entity_display_name_for_key(name, name);
        payloadJson[F("unique_id")] = uniqueName;

        add_discovery_device_object(payloadJson);

        payloadJson[F("entity_category")] = "diagnostic";
        payloadJson[F("stat_t")] = stateTopic;
        payloadJson[F("val_tpl")] = F("{{ value }}");
        payloadJson[F("exp_aft")] = SENSOR_STATE_TIMEOUT;

        switch (type)
        {
        case SensorType::CONNECTIVITY:
            discoveryTopic = String(F("homeassistant/binary_sensor/")) + uniqueName + F("/config");
            payloadJson[F("payload_off")] = F("off");
            payloadJson[F("payload_on")] = F("on");
            payloadJson[F("dev_cla")] = F("connectivity");
            break;
        case SensorType::WIFI_SIGNAL:
            payloadJson[F("unit_of_meas")] = F("dBm");
            payloadJson[F("icon")] = F("mdi:wifi");
            payloadJson[F("dev_cla")] = F("signal_strength");
            break;
        case SensorType::WIFI_SSID:
            payloadJson[F("icon")] = F("mdi:eye");
            payloadJson[F("enabled_by_default")] = bool(false);
            break;
        case SensorType::IP_ADDRESS:
            payloadJson[F("icon")] = F("mdi:ip");
            payloadJson[F("enabled_by_default")] = bool(false);
            break;
        case SensorType::MAC_ADDRESS:
            payloadJson[F("icon")] = F("mdi:eye");
            payloadJson[F("enabled_by_default")] = bool(false);
            break;
        default:
            break;
        }

        if (!publish_mqtt(discoveryTopic, doc, /* retain =*/true))
        {
            log_web(F("Failed to publish homeassistant %s entity auto-discover"), uniqueName.c_str());
            return false;
        }

        return true;
    }

    bool publish_homeassistant_auto_discover_all()
    {
        // https://www.home-assistant.io/integrations/mqtt/
        if (!publish_ha_climate_auto_discover())
            return false;

        if (hp::zone2_present() == 1)
        {
            if (!publish_ha_climate_z2_auto_discover())
                return false;
        }
        else if (hp::zone2_present() == 0)
        {
            remove_ha_entity(F("climate"), F("climate_control_z2"));
        }

        if (!publish_ha_set_z1_flow_target_auto_discover())
            return false;

        if (!publish_ha_force_dhw_auto_discover())
            return false;

        if (!publish_ha_turn_on_off_auto_discover())
            return false;

        if (!publish_ha_set_dhw_temp_auto_discover())
            return false;

        if (!publish_ha_set_sh_mode_auto_discover())
            return false;

        if (!publish_ha_binary_sensor_auto_discover(F("mode_defrost")))
            return false;

        if (!publish_ha_float_sensor_auto_discover(F("compressor_frequency"), SensorType::FREQUENCY))
            return false;

        if (!publish_ha_float_sensor_auto_discover(F("flow_rate"), SensorType::FLOW_RATE))
            return false;

        // 0x0E candidates (aux_temp_1..5, thermistor_raw) were boiler flow/return -> remove the entities.
        for (size_t i = 1; i <= 5; ++i)
            remove_ha_entity(F("sensor"), String(F("aux_temp_")) + String(i));
        remove_ha_entity(F("sensor"), F("thermistor_raw"));

        // THW10 (buffer) from GET 0x0F, compressor from GET 0x13
        if (!publish_ha_float_sensor_auto_discover(F("buffer_temp"), SensorType::TEMPERATURE))
            return false;

        if (!publish_ha_binary_sensor_auto_discover(F("compressor_on")))
            return false;

        if (!publish_ha_float_sensor_auto_discover(F("compressor_runtime"), SensorType::DURATION_HOURS))
            return false;

        // Compressor starts (outdoor unit request code 3, resolution 100) + avg. runtime per start
        if (!publish_ha_float_sensor_auto_discover(F("compressor_starts"), SensorType::COUNT))
            return false;

        if (!publish_ha_float_sensor_auto_discover(F("compressor_avg_run"), SensorType::DURATION_MINUTES))
            return false;

        // Outdoor unit (request codes OCH694A)
        if (!publish_ha_float_sensor_auto_discover(F("outdoor_target_freq"), SensorType::FREQUENCY))
            return false;
        // Remove the former outdoor unit actual frequency sensor (code 16) - frequency comes via 0x04 (FTC)
        remove_ha_entity(F("sensor"), F("outdoor_actual_freq"));
        if (!publish_ha_string_sensor_auto_discover(F("freq_limitation"), F("mdi:speedometer-slow")))
            return false;
        if (!publish_ha_binary_sensor_auto_discover(F("freq_limited")))
            return false;
        if (!publish_ha_float_sensor_auto_discover(F("thermo_on_time"), SensorType::DURATION_MINUTES))
            return false;
        if (!publish_ha_float_sensor_auto_discover(F("discharge_temp"), SensorType::TEMPERATURE))
            return false;
        if (!publish_ha_float_sensor_auto_discover(F("compressor_current"), SensorType::CURRENT))
            return false;
        if (!publish_ha_float_sensor_auto_discover(F("discharge_superheat"), SensorType::KELVIN_DIFF))
            return false;
        if (!publish_ha_float_sensor_auto_discover(F("subcool"), SensorType::KELVIN_DIFF))
            return false;
        if (!publish_ha_float_sensor_auto_discover(F("outdoor_ambient_temp"), SensorType::TEMPERATURE))
            return false;
        if (!publish_ha_string_sensor_auto_discover(F("outdoor_actuators"), F("mdi:electric-switch")))
            return false;
        if (!publish_ha_string_sensor_auto_discover(F("outdoor_control_state"), F("mdi:state-machine")))
            return false;
        if (!publish_ha_float_sensor_auto_discover(F("outdoor_compressor_hours"), SensorType::DURATION_HOURS))
            return false;

        if (!publish_ha_binary_sensor_auto_discover(F("mode_dhw_forced")))
            return false;

        if (!publish_ha_float_sensor_auto_discover(F("output_pwr"), SensorType::LIVE_POWER))
            return false;

        if (!publish_ha_float_sensor_auto_discover(F("legionella_prevention_temp"), SensorType::TEMPERATURE))
            return false;

        if (!publish_ha_float_sensor_auto_discover(F("dhw_temp_drop"), SensorType::TEMPERATURE))
            return false;

        if (!publish_ha_float_sensor_auto_discover(F("outside_temp"), SensorType::TEMPERATURE))
            return false;

        if (!publish_ha_float_sensor_auto_discover(F("hp_feed_temp"), SensorType::TEMPERATURE))
            return false;

        if (!publish_ha_float_sensor_auto_discover(F("hp_return_temp"), SensorType::TEMPERATURE))
            return false;

        if (!publish_ha_float_sensor_auto_discover(F("boiler_flow_temp"), SensorType::TEMPERATURE))
            return false;

        if (!publish_ha_float_sensor_auto_discover(F("boiler_return_temp"), SensorType::TEMPERATURE))
            return false;

        if (!publish_ha_float_sensor_auto_discover(F("dhw_flow_temp_target"), SensorType::TEMPERATURE))
            return false;

        if (!publish_ha_float_sensor_auto_discover(F("sh_flow_temp_target"), SensorType::TEMPERATURE))
            return false;

        if (!publish_ha_string_sensor_auto_discover(F("mode_power")))
            return false;

        if (!publish_ha_string_sensor_auto_discover(F("mode_operation")))
            return false;

        if (!publish_ha_string_sensor_auto_discover(F("mode_dhw")))
            return false;

        if (!publish_ha_string_sensor_auto_discover(F("mode_heating_cooling")))
            return false;

        if (!publish_ha_float_sensor_auto_discover(F("heating_consumed"), SensorType::POWER))
            return false;

        if (!publish_ha_float_sensor_auto_discover(F("heating_delivered"), SensorType::POWER))
            return false;

        if (!publish_ha_float_sensor_auto_discover(F("cooling_consumed"), SensorType::POWER))
            return false;

        if (!publish_ha_float_sensor_auto_discover(F("cooling_delivered"), SensorType::POWER))
            return false;

        if (!publish_ha_float_sensor_auto_discover(F("dhw_consumed"), SensorType::POWER))
            return false;

        if (!publish_ha_float_sensor_auto_discover(F("dhw_delivered"), SensorType::POWER))
            return false;

        if (!publish_ha_float_sensor_auto_discover(F("z1_room_temp"), SensorType::TEMPERATURE))
            return false;

        if (!publish_ha_float_sensor_auto_discover(F("z1_flow_temp_target"), SensorType::TEMPERATURE))
            return false;

        if (!publish_ha_float_sensor_auto_discover(F("z1_room_temp_target"), SensorType::TEMPERATURE))
            return false;

        if (hp::zone2_present() == 1)
        {
            if (!publish_ha_float_sensor_auto_discover(F("z2_room_temp"), SensorType::TEMPERATURE))
                return false;

            if (!publish_ha_float_sensor_auto_discover(F("z2_flow_temp_target"), SensorType::TEMPERATURE))
                return false;

            if (!publish_ha_float_sensor_auto_discover(F("z2_room_temp_target"), SensorType::TEMPERATURE))
                return false;
        }
        else if (hp::zone2_present() == 0)
        {
            remove_ha_entity(F("sensor"), F("z2_room_temp"));
            remove_ha_entity(F("sensor"), F("z2_flow_temp_target"));
            remove_ha_entity(F("sensor"), F("z2_room_temp_target"));
        }

        if (!publish_ha_float_sensor_auto_discover(F("dhw_temp"), SensorType::TEMPERATURE))
            return false;

        if (!publish_ha_float_sensor_auto_discover(F("dhw_cop"), SensorType::COP))
            return false;

        if (!publish_ha_float_sensor_auto_discover(F("sh_cop"), SensorType::COP))
            return false;

        if (!publish_ha_float_sensor_auto_discover(F("cool_cop"), SensorType::COP))
            return false;

        // Diagnostic sensors
        if (!publish_ha_diagnostic_sensor_auto_discover(F("Heat pump connection state"), SensorType::CONNECTIVITY))
            return false;

        if (!publish_ha_diagnostic_sensor_auto_discover(F("Wifi signal"), SensorType::WIFI_SIGNAL))
            return false;

        if (!publish_ha_diagnostic_sensor_auto_discover(F("Wifi SSID"), SensorType::WIFI_SSID))
            return false;

        if (!publish_ha_diagnostic_sensor_auto_discover(F("IP address"), SensorType::IP_ADDRESS))
            return false;

        if (!publish_ha_diagnostic_sensor_auto_discover(F("MAC address"), SensorType::MAC_ADDRESS))
            return false;

        return true;
    }

    void publish_homeassistant_auto_discover()
    {
        if (!needsAutoDiscover || !mqttClient.connected())
            return;

        log_web(F("Publishing Home Assistant auto-discovery..."));
        if (publish_homeassistant_auto_discover_all())
        {
            needsAutoDiscover = false; // upstream never reset this
        }

        // Give HA a moment to create the entities, then send all states once.
        invalidate_published_states();
        statePublishHoldoff = std::chrono::steady_clock::now() + DISCOVERY_SETTLE_TIME;
    }

    bool publish_climate_status()
    {
        JsonDocument doc;
        JsonObject json = doc.to<JsonObject>();


        {
            auto& status = hp::get_status();
            std::lock_guard<hp::Status> lock{status};

            json[F("temperature")] = round2(status.Zone1SetTemperature);
            json[F("curr_temp")] = round2(status.Zone1RoomTemperature);
            json[F("mode")] = status.ha_mode_as_string();
            json[F("action")] = status.ha_action_as_string();
        }

        const auto& config = config_instance();
        String stateTopic = config.MqttTopic + "/" + unique_entity_name(F("climate_control")) + F("/state");
        if (!publish_state(stateTopic, doc))
        {
            log_web(F("Failed to publish MQTT state for: %s"), unique_entity_name(F("climate_control")).c_str());
            return false;
        }

        return true;
    }

    bool publish_z2_climate_status()
    {
        JsonDocument doc;
        JsonObject json = doc.to<JsonObject>();


        {
            auto& status = hp::get_status();
            std::lock_guard<hp::Status> lock{status};

            json[F("temperature")] = round2(status.Zone2SetTemperature);
            json[F("curr_temp")] = round2(status.Zone2RoomTemperature);
            json[F("mode")] = status.ha_mode_as_string();
            json[F("action")] = status.ha_action_as_string();
        }

        const auto& config = config_instance();
        String stateTopic = config.MqttTopic + "/" + unique_entity_name(F("climate_control_z2")) + F("/state");
        if (!publish_state(stateTopic, doc))
        {
            log_web(F("Failed to publish MQTT state for: %s"), unique_entity_name(F("climate_control_z2")).c_str());
            return false;
        }

        return true;
    }

    bool publish_binary_sensor_status(const String& name, bool on)
    {
        String state = on ? F("on") : F("off");
        const auto& config = config_instance();
        String stateTopic = config.MqttTopic + "/" + unique_entity_name(name) + F("/state");
        if (!publish_state(stateTopic, state))
        {
            log_web(F("Failed to publish MQTT state for: %s"), unique_entity_name(name).c_str());
            return false;
        }

        return true;
    }

    template <typename T>
    bool publish_sensor_status(const String& name, T value)
    {
        const auto& config = config_instance();
        String stateTopic = config.MqttTopic + "/" + unique_entity_name(name) + F("/state");
        if (!publish_state(stateTopic, String(value)))
        {
            log_web(F("Failed to publish MQTT state for: %s"), unique_entity_name(name).c_str());
            return false;
        }

        return true;
    }

    void publish_entity_state_updates()
    {
        if (!mqttClient.connected())
            return;

        if (!publish_climate_status())
            return;

        if (hp::zone2_present() == 1 && !publish_z2_climate_status())
            return;

        auto& status = hp::get_status();
        std::lock_guard<hp::Status> lock{status};
        if (!publish_binary_sensor_status(F("mode_defrost"), status.DefrostActive))
            return;

        if (!publish_sensor_status<float>(F("compressor_frequency"), status.CompressorFrequency))
            return;

        if (!publish_sensor_status<float>(F("flow_rate"), status.FlowRate))
            return;

        // THW10 + compressor (only sent once received)
        if (status.BufferTemperatureValid && !publish_sensor_status<float>(F("buffer_temp"), status.BufferTemperature))
            return;

        if (status.CompressorRuntimeValid)
        {
            if (!publish_binary_sensor_status(F("compressor_on"), status.CompressorOn))
                return;

            if (!publish_sensor_status<float>(F("compressor_runtime"), status.CompressorRuntimeHours))
                return;
        }

        // Prefer operating hours from the outdoor unit (code 2, x10 h); the FTC counter 0x13 can read lower
        float outdoorHours = 0.0f;
        {
            uint16_t v = 0;
            uint32_t age = 0;
            if (hp::get_request_code_value(2, v, age) && age <= RC_MAX_AGE_HOURLY_S)
                outdoorHours = float(v) * 10.0f;
        }

        if (status.CompressorStartsValid && status.CompressorStarts > 0)
        {
            if (!publish_sensor_status<float>(F("compressor_starts"), float(status.CompressorStarts)))
                return;

            const float hours = outdoorHours > 0.0f ? outdoorHours : (status.CompressorRuntimeValid ? status.CompressorRuntimeHours : 0.0f);
            if (hours > 0.0f &&
                !publish_sensor_status<float>(F("compressor_avg_run"), hours * 60.0f / float(status.CompressorStarts)))
                return;
        }

        // Outdoor unit values - only sent while fresh (otherwise the HA sensor goes unavailable via exp_aft)
        {
            uint16_t v = 0;
            uint32_t age = 0;
            auto fresh = [&](int16_t code, uint32_t maxAge) { return hp::get_request_code_value(code, v, age) && age <= maxAge; };

            if (fresh(17, RC_MAX_AGE_FAST_S) && !publish_sensor_status<float>(F("outdoor_target_freq"), float(v)))
                return;
            if (fresh(52, RC_MAX_AGE_FAST_S))
            {
                if (!publish_sensor_status<String>(F("freq_limitation"), hp::request_code_status_text(52, v)))
                    return;
                if (!publish_binary_sensor_status(F("freq_limited"), v != 0))
                    return;
            }
            if (fresh(48, RC_MAX_AGE_FAST_S) && !publish_sensor_status<float>(F("thermo_on_time"), float(v)))
                return;
            if (fresh(4, RC_MAX_AGE_SLOW_S) && !publish_sensor_status<float>(F("discharge_temp"), float(int16_t(v))))
                return;
            if (fresh(1, RC_MAX_AGE_SLOW_S) && !publish_sensor_status<float>(F("compressor_current"), float(v)))
                return;
            if (fresh(12, RC_MAX_AGE_SLOW_S) && !publish_sensor_status<float>(F("discharge_superheat"), float(int16_t(v))))
                return;
            if (fresh(13, RC_MAX_AGE_SLOW_S) && !publish_sensor_status<float>(F("subcool"), float(int16_t(v))))
                return;
            if (fresh(9, RC_MAX_AGE_SLOW_S) && !publish_sensor_status<float>(F("outdoor_ambient_temp"), float(int16_t(v))))
                return;
            if (fresh(54, RC_MAX_AGE_SLOW_S) && !publish_sensor_status<String>(F("outdoor_actuators"), hp::request_code_status_text(54, v)))
                return;
            if (fresh(51, RC_MAX_AGE_SLOW_S) && !publish_sensor_status<String>(F("outdoor_control_state"), hp::request_code_status_text(51, v)))
                return;
            if (outdoorHours > 0.0f && !publish_sensor_status<float>(F("outdoor_compressor_hours"), outdoorHours))
                return;
        }

        if (!publish_binary_sensor_status(F("mode_dhw_forced"), status.DhwForcedActive))
            return;

        if (!publish_sensor_status<float>(F("output_pwr"), status.OutputPower))
            return;

        if (!publish_sensor_status<float>(F("legionella_prevention_temp"), status.LegionellaPreventionSetPoint))
            return;

        if (!publish_sensor_status<float>(F("dhw_temp_drop"), status.DhwTemperatureDrop))
            return;

        if (!publish_sensor_status<float>(F("outside_temp"), status.OutsideTemperature))
            return;

        if (!publish_sensor_status<float>(F("hp_feed_temp"), status.DhwFeedTemperature))
            return;

        if (!publish_sensor_status<float>(F("hp_return_temp"), status.DhwReturnTemperature))
            return;

        if (!publish_sensor_status<float>(F("boiler_flow_temp"), status.BoilerFlowTemperature))
            return;

        if (!publish_sensor_status<float>(F("boiler_return_temp"), status.BoilerReturnTemperature))
            return;

        if (!publish_sensor_status<float>(F("dhw_flow_temp_target"), status.DhwFlowTemperatureSetPoint))
            return;

        if (!publish_sensor_status<float>(F("sh_flow_temp_target"), status.RadiatorFlowTemperatureSetPoint))
            return;

        if (!publish_sensor_status<String>(F("mode_power"), status.power_as_string()))
            return;

        if (!publish_sensor_status<String>(F("mode_operation"), status.operation_as_string()))
            return;

        if (!publish_sensor_status<String>(F("mode_dhw"), status.dhw_mode_as_string()))
            return;

        if (!publish_sensor_status<String>(F("mode_heating_cooling"), status.hp_mode_as_string()))
            return;

        if (!publish_sensor_status<float>(F("heating_consumed"), status.EnergyConsumedHeating))
            return;

        if (!publish_sensor_status<float>(F("heating_delivered"), status.EnergyDeliveredHeating))
            return;

        if (!publish_sensor_status<float>(F("cooling_consumed"), status.EnergyConsumedCooling))
            return;

        if (!publish_sensor_status<float>(F("cooling_delivered"), status.EnergyDeliveredCooling))
            return;

        if (!publish_sensor_status<float>(F("dhw_consumed"), status.EnergyConsumedDhw))
            return;

        if (!publish_sensor_status<float>(F("dhw_delivered"), status.EnergyDeliveredDhw))
            return;

        if (!publish_sensor_status<float>(F("z1_room_temp"), status.Zone1RoomTemperature))
            return;

        if (!publish_sensor_status<float>(F("z1_flow_temp_target"), status.Zone1FlowTemperatureSetPoint))
            return;

        if (!publish_sensor_status<float>(F("z1_room_temp_target"), status.Zone1SetTemperature))
            return;

        if (hp::zone2_present() == 1)
        {
            if (!publish_sensor_status<float>(F("z2_room_temp"), status.Zone2RoomTemperature))
                return;

            if (!publish_sensor_status<float>(F("z2_flow_temp_target"), status.Zone2FlowTemperatureSetPoint))
                return;

            if (!publish_sensor_status<float>(F("z2_room_temp_target"), status.Zone2SetTemperature))
                return;
        }

        if (!publish_sensor_status<float>(F("dhw_temp"), status.DhwTemperature))
            return;

        if (!publish_sensor_status<float>(F("dhw_cop"), status.EnergyConsumedDhw > 0.0f ? status.EnergyDeliveredDhw / status.EnergyConsumedDhw : 0.0f))
            return;

        if (!publish_sensor_status<float>(F("sh_cop"), status.EnergyConsumedHeating > 0.0f ? status.EnergyDeliveredHeating / status.EnergyConsumedHeating : 0.0f))
            return;
        
        if (!publish_sensor_status<float>(F("cool_cop"), status.EnergyConsumedCooling > 0.0f ? status.EnergyDeliveredCooling / status.EnergyConsumedCooling : 0.0f))
            return;

        // Diagnostic
        if (!publish_binary_sensor_status(F("Heat pump connection state"), hp::is_connected()))
            return;

        if (!publish_sensor_status<int>(F("Wifi signal"), int(WiFi.RSSI())))
            return;

        if (!publish_sensor_status<String>(F("Wifi SSID"), WiFi.SSID()))
            return;

        if (!publish_sensor_status<String>(F("IP address"), WiFi.localIP().toString()))
            return;

        if (!publish_sensor_status<String>(F("MAC address"), WiFi.macAddress()))
            return;
    }

    bool connect_mqtt()
    {
        if (mqttClient.connected())
            return true;

        // Exactly one connection attempt. Upstream tried up to 11x with
        // delay(1000) + up to 10 s timeout each, without a watchdog ping (watchdog = 30 s).
        // Retries happen via the periodic tick (every 30 s).
        Config& config = config_instance();
        ehal::ping_watchdog();
        bool ok;
        if (!config.MqttPassword.isEmpty() && !config.MqttUserName.isEmpty())
        {
            log_web(F("MQTT user '%s' has configured password, connecting with credentials..."), config.MqttUserName.c_str());
            ok = mqttClient.connect(WiFi.localIP().toString().c_str(), config.MqttUserName.c_str(), config.MqttPassword.c_str());
        }
        else
        {
            log_web(F("MQTT username/password not configured, connecting as anonymous user..."));
            ok = mqttClient.connect(WiFi.localIP().toString().c_str());
        }
        ehal::ping_watchdog();

        if (!ok)
        {
            log_web(F("MQTT connection failure: '%s'"), get_connection_error_string().c_str());
            return false;
        }

        if (mqttClient.connected())
        {
            needsAutoDiscover = true;
            invalidate_published_states();

            // HA birth message -> resend discovery + all states when HA restarts.
            if (!mqttClient.subscribe(F("homeassistant/status")))
            {
                log_web(F("Failed to subscribe to homeassistant/status!"));
            }

            String tempCmdTopic = config.MqttTopic + "/" + unique_entity_name(F("climate_control")) + F("/temp_cmd");
            if (!mqttClient.subscribe(tempCmdTopic))
            {
                log_web(F("Failed to subscribe to temperature command topic!"));
                return false;
            }

            String z2TempCmdTopic = config.MqttTopic + "/" + unique_entity_name(F("climate_control_z2")) + F("/temp_cmd");
            if (!mqttClient.subscribe(z2TempCmdTopic))
            {
                log_web(F("Failed to subscribe to Z2 temperature command topic!"));
                return false;
            }

            if (!mqttClient.subscribe(config.MqttTopic + "/" + unique_entity_name(F("force_dhw")) + F("/set")))
            {
                log_web(F("Failed to subscribe to boost DHW command topic!"));
                return false;
            }

            if (!mqttClient.subscribe(config.MqttTopic + "/" + unique_entity_name(F("turn_on_off_hp")) + F("/set")))
            {
                log_web(F("Failed to subscribe to turn ON/OFF command topic!"));
                return false;
            }

            if (!mqttClient.subscribe(config.MqttTopic + "/" + unique_entity_name(F("dhw_water_heater")) + F("/set")))
            {
                log_web(F("Failed to subscribe to DHW temperature topic!"));
                return false;
            }

            if (!mqttClient.subscribe(config.MqttTopic + "/" + unique_entity_name(F("z1_flow_temp_target")) + F("/set")))
            {
                log_web(F("Failed to subscribe to Z1 flow target temperature command topic!"));
                return false;
            }

            if (!mqttClient.subscribe(config.MqttTopic + "/" + unique_entity_name(F("dhw_mode")) + F("/set")))
            {
                log_web(F("Failed to subscribe to DHW mode command topic!"));
                return false;
            }

            if (!mqttClient.subscribe(config.MqttTopic + "/" + unique_entity_name(F("sh_mode")) + F("/set")))
            {
                log_web(F("Failed to subscribe to SH mode command topic!"));
                return false;
            }

            log_web(F("Successfully established MQTT client connection!"));
        }

        return true;
    }

    bool initialize()
    {
        log_web(F("Initializing MQTT..."));

        if (!is_configured())
        {
            log_web(F("Unable to initialize MQTT, server is not configured."));
            return false;
        }


        auto& config = config_instance();
        
        const int KEEPALIVE_TIME_SECONDS = 90;
        const bool USE_CLEAN_SESSION = false; // Persistent MQTT session
        const int COMMAND_TIMEOUT_MILLISECONDS = 5000; // LAN: 10 s was unnecessarily long (blocks loopTask)
        mqttClient.setOptions(KEEPALIVE_TIME_SECONDS, USE_CLEAN_SESSION, COMMAND_TIMEOUT_MILLISECONDS);
        mqttClient.onMessage(mqtt_callback);
        mqttClient.begin(config.MqttServer.c_str(), config.MqttPort, espClient);

        if (connect_mqtt())
        {
            publish_homeassistant_auto_discover();
        }

        return true;
    }

    void handle_loop()
    {
        if (hp::is_connected())
        {
            if (periodic_update_tick())
            {
                auto now = std::chrono::steady_clock::now();

                // Re-establish MQTT connection if we need to (at most every 30 s, blocks up to 5 s).
                if (!mqttClient.connected())
                {
                    static auto lastReconnect = now - MQTT_RECONNECT_INTERVAL;
                    if (now - lastReconnect >= MQTT_RECONNECT_INTERVAL)
                    {
                        lastReconnect = now;
                        log_web(F("MQTT disconnect detected during periodic update check!"));
                        needsAutoDiscover = true;
                        connect_mqtt();
                    }
                }

                // Publish homeassistant auto-discovery messages if we need to.
                publish_homeassistant_auto_discover();

                // Periodic full refresh (in case HA missed something).
                if (now - lastFullStateRefresh > FULL_STATE_REFRESH_INTERVAL)
                {
                    invalidate_published_states();
                }

                // Update entity statuses (changed values only).
                if (now >= statePublishHoldoff)
                {
                    publish_entity_state_updates();
                }
            }
        }

        // Once the DIP switches are read (zone 2 known), resend discovery once.
        {
            static int8_t lastZone2 = -2;
            int8_t zone2 = hp::zone2_present();
            if (zone2 != lastZone2)
            {
                if (lastZone2 != -2)
                    needsAutoDiscover = true;
                lastZone2 = zone2;
            }
        }

        mqttClient.loop();

        // Process buffered commands outside the lwmqtt callback.
        // (Commands arriving during publish() are appended to the queue.)
        while (!pendingCommands.empty())
        {
            auto cmd = std::move(pendingCommands.front());
            pendingCommands.pop_front();
            handle_mqtt_command(cmd.first, cmd.second);
            ehal::ping_watchdog();
        }
    }

    bool is_connected()
    {
        return mqttClient.connected();
    }
} // namespace ehal::mqtt
