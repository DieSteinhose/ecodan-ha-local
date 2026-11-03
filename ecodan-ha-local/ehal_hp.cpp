#include "ehal.h"
#include "ehal_config.h"
#include "ehal_diagnostics.h"
#include "ehal_hp.h"
#include "ehal_proto.h"

#include <HardwareSerial.h>
#include <freertos/task.h>
#include <esp_pthread.h>

#include <atomic>
#include <deque>
#include <mutex>
#include <thread>

namespace ehal::hp
{
    HardwareSerial port = Serial1;
    uint64_t rxMsgCount = 0;
    uint64_t txMsgCount = 0;
    std::atomic<uint32_t> statusCycleCount{0};

    // Last raw response per GET type (for /cn105_probe and web UI)
    struct RawResponse
    {
        uint8_t data[PAYLOAD_SIZE];
        uint32_t when = 0; // millis(), 0 = never received
    };
    RawResponse rawResponses[256];
    std::mutex rawResponseMutex;

    // DIP switches (GET 0x11)
    uint8_t dipSwitches[7] = {};
    std::atomic<bool> dipValid{false};

    TaskHandle_t serialRxTaskHandle = nullptr;
    std::thread serialRxThread;

    // ---------------------------------------------------------------------------------
    // Command scheduling
    //  * There is ALWAYS at most one request to the FTC in flight (awaitingResponse).
    //    Upstream sent immediately on every SET as well -> several requests
    //    on the bus at once, lost responses, stuck chain.
    //  * SET commands (user) take priority over the GET status cycle and are
    //    NO longer cleared by the status cycle.
    //  * SETs of the same kind in the queue are replaced (only the last value counts).
    //  * Response timeout: SETs are retried once, GETs skipped; after
    //    several timeouts in a row the connection is re-established.
    // ---------------------------------------------------------------------------------
    struct PendingCmd
    {
        Message msg;
        uint8_t attempts = 0;
    };

    std::deque<PendingCmd> setQueue;
    std::deque<PendingCmd> getQueue;
    std::mutex cmdQueueMutex;

    bool awaitingResponse = false;
    PendingCmd inflight;
    MsgType expectedResponse = MsgType::GET_RES;
    uint8_t expectedPayloadType = 0;
    std::chrono::steady_clock::time_point lastTxTime;
    uint8_t consecutiveTimeouts = 0;

    bool statusRefreshPending = false;
    std::chrono::steady_clock::time_point statusRefreshDue;
    bool statusEverRequested = false;
    std::chrono::steady_clock::time_point lastStatusUpdate;

    const auto RESPONSE_TIMEOUT = std::chrono::milliseconds(2000);  // 2x22 Byte @2400 8E1 ~ 200 ms
    const auto STATUS_INTERVAL = std::chrono::seconds(60);
    const auto STATUS_REFRESH_AFTER_SET = std::chrono::seconds(5);
    const uint8_t MAX_SET_ATTEMPTS = 2;
    const uint8_t MAX_CONSECUTIVE_TIMEOUTS = 5;
    const size_t MAX_SET_QUEUE = 8;

    Status status;
    float temperatureStep = 0.5f;
    std::atomic<bool> connected{false};

    bool serial_tx(Message& msg)
    {
        if (!port)
        {
            log_web_ratelimit(F("Serial connection unavailable for tx"));
            return false;
        }

        if (port.availableForWrite() < msg.size())
        {
            log_web(F("Serial tx buffer size: %u"), port.availableForWrite());
            return false;
        }

        msg.set_checksum();
        port.write(msg.buffer(), msg.size());
        port.flush();

        auto& config = config_instance();
        if (config.DumpPackets)
        {
            msg.debug_dump_packet();
        }

        ++txMsgCount;
        return true;
    }

    void clear_command_queue()
    {
        std::lock_guard<std::mutex> lock{cmdQueueMutex};

        setQueue.clear();
        getQueue.clear();
        awaitingResponse = false;
    }

    void resync_rx()
    {
        // Only discard the receive buffer. The command queue is kept:
        // a pending request runs into its timeout and is retried if needed.
        while (port.available() > 0)
            port.read();
    }

    bool serial_rx(Message& msg)
    {
        if (!port)
        {
            log_web_ratelimit(F("Serial connection unavailable for rx"));
            return false;
        }

        if (port.available() < HEADER_SIZE)
        {
            const TickType_t maxBlockingTime = pdMS_TO_TICKS(1000);
            ulTaskNotifyTakeIndexed(0, pdTRUE, maxBlockingTime);

            // We were woken by an interrupt, but there's not enough data available
            // yet on the serial port for us to start processing it as a packet.
            if (port.available() < HEADER_SIZE)
                return false;
        }

        // Scan for the start of an Ecodan packet.
        if (port.peek() != HEADER_MAGIC_A)
        {
            log_web_ratelimit(F("Dropping serial data, header magic mismatch"));
            resync_rx();
            return false;
        }

        if (port.readBytes(msg.buffer(), HEADER_SIZE) < HEADER_SIZE)
        {
            log_web(F("Serial port header read failure!"));
            resync_rx();
            return false;
        }

        msg.increment_write_offset(HEADER_SIZE);

        if (!msg.verify_header())
        {
            log_web(F("Serial port message appears invalid, skipping payload wait..."));
            resync_rx();
            return false;
        }

        // 17 bytes @2400 baud 8E1 take ~80 ms. Upstream waited up to 30 s here without a
        // watchdog ping (= watchdog timeout 30 s!) -> limited to 2 s, watchdog is fed.
        size_t remainingBytes = msg.payload_size() + CHECKSUM_SIZE;
        auto startTime = std::chrono::steady_clock::now();
        while (port.available() < remainingBytes)
        {
            delay(5);
            ehal::ping_watchdog();

            if (std::chrono::steady_clock::now() - startTime > std::chrono::seconds(2))
            {
                log_web(F("Serial port message could not be received within 2s (got %u / %u bytes)"), port.available(), remainingBytes);
                resync_rx();
                return false;
            }
        }

        if (port.readBytes(msg.payload(), remainingBytes) < remainingBytes)
        {
            log_web(F("Serial port payload read failure!"));
            resync_rx();
            return false;
        }

        msg.increment_write_offset(msg.payload_size()); // Don't count checksum byte.

        if (!msg.verify_checksum())
        {
            resync_rx();
            return false;
        }

        auto& config = config_instance();
        if (config.DumpPackets)
        {
            msg.debug_dump_packet();
        }

        ++rxMsgCount;
        return true;
    }

    bool begin_connect()
    {
        Message cmd{MsgType::CONNECT_CMD};
        char payload[2] = {0xCA, 0x01};
        cmd.write_payload(payload, sizeof(payload));

        if (!serial_tx(cmd))
        {
            log_web(F("Failed to tx CONNECT_CMD!"));
            return false;
        }

        return true;
    }

    // Sends the next command, but only if none is waiting for a response.
    bool dispatch_next_cmd()
    {
        Message msg;
        {
            std::lock_guard<std::mutex> lock{cmdQueueMutex};

            if (awaitingResponse || !connected)
                return true;

            std::deque<PendingCmd>* queue = !setQueue.empty() ? &setQueue : (!getQueue.empty() ? &getQueue : nullptr);
            if (queue == nullptr)
                return true;

            inflight = std::move(queue->front());
            queue->pop_front();
            inflight.attempts++;

            msg = inflight.msg;
            expectedResponse = (msg.type() == MsgType::SET_CMD) ? MsgType::SET_RES : MsgType::GET_RES;
            expectedPayloadType = msg[0];
            awaitingResponse = true;
            lastTxTime = std::chrono::steady_clock::now();
        }

        if (!serial_tx(msg))
        {
            log_web(F("Unable to dispatch command, flushing queued requests..."));

            clear_command_queue();

            connected = false;
            return false;
        }

        return true;
    }

    // Called by the RX thread on SET_RES/GET_RES. Returns true if the
    // response matches the pending request (late responses are ignored).
    bool complete_inflight(MsgType resType, uint8_t payloadType)
    {
        std::lock_guard<std::mutex> lock{cmdQueueMutex};

        if (!awaitingResponse || resType != expectedResponse)
            return false;

        if (resType == MsgType::GET_RES && payloadType != expectedPayloadType)
            return false;

        awaitingResponse = false;
        consecutiveTimeouts = 0;
        return true;
    }

    void check_response_timeout()
    {
        bool timedOut = false;
        bool retried = false;
        bool giveUp = false;
        uint8_t type = 0;
        {
            std::lock_guard<std::mutex> lock{cmdQueueMutex};

            if (!awaitingResponse || std::chrono::steady_clock::now() - lastTxTime < RESPONSE_TIMEOUT)
                return;

            timedOut = true;
            awaitingResponse = false;
            type = expectedPayloadType;

            if (++consecutiveTimeouts >= MAX_CONSECUTIVE_TIMEOUTS)
            {
                giveUp = true;
                consecutiveTimeouts = 0;
            }
            else if (inflight.msg.type() == MsgType::SET_CMD && inflight.attempts < MAX_SET_ATTEMPTS)
            {
                setQueue.push_front(inflight);
                retried = true;
            }
        }

        if (!timedOut)
            return;

        if (giveUp)
        {
            log_web(F("Heat pump did not answer %u requests in a row, reconnecting..."), MAX_CONSECUTIVE_TIMEOUTS);
            {
                // Keep user commands, only drop the status cycle.
                std::lock_guard<std::mutex> lock{cmdQueueMutex};
                getQueue.clear();
            }
            connected = false;
            return;
        }

        log_web(F("No response for command type %#x within %ums, %s"), type, (unsigned)RESPONSE_TIMEOUT.count(),
                retried ? "retrying" : "skipping");
        dispatch_next_cmd();
    }

    bool enqueue_set_cmd(Message&& cmd)
    {
        bool accepted = false;
        {
            std::lock_guard<std::mutex> lock{cmdQueueMutex};

            // Same kind of command (type, flags, zone, control mode if any) already queued?
            // -> only send the newest value (e.g. HA sliders, automations with several calls).
            const bool zoneCmd = (cmd[1] & SET_SETTINGS_FLAG_ZONE_TEMPERATURE) != 0;
            for (auto& pending : setQueue)
            {
                Message& m = pending.msg;
                if (m[0] == cmd[0] && m[1] == cmd[1] && m[2] == cmd[2] && (!zoneCmd || m[6] == cmd[6]))
                {
                    m = std::move(cmd);
                    pending.attempts = 0;
                    accepted = true;
                    break;
                }
            }

            if (!accepted && setQueue.size() < MAX_SET_QUEUE)
            {
                PendingCmd pending;
                pending.msg = std::move(cmd);
                setQueue.push_back(std::move(pending));
                accepted = true;
            }
        }

        if (!accepted)
        {
            log_web(F("Command queue full (%u), dropping command!"), MAX_SET_QUEUE);
            return false;
        }

        return dispatch_next_cmd();
    }

    bool begin_get_status()
    {
        size_t dropped = 0;
        {
            std::lock_guard<std::mutex> lock{cmdQueueMutex};

            // Only restart the GET cycle; pending SET commands are kept.
            dropped = getQueue.size();
            getQueue.clear();

            const GetType types[] = {
                GetType::DEFROST_STATE,
                GetType::COMPRESSOR_FREQUENCY,
                GetType::FORCED_DHW_STATE,
                GetType::HEATING_POWER,
                GetType::TEMPERATURE_CONFIG,
                GetType::SH_TEMPERATURE_STATE,
                GetType::DHW_TEMPERATURE_STATE_A,
                GetType::DHW_TEMPERATURE_STATE_B,
                // GetType::ACTIVE_TIME,
                GetType::FLOW_RATE,
                GetType::TEMPERATURE_STATE_D, // THW10 (instead of 0x0E)
                GetType::ACTIVE_TIME,         // compressor on / runtime
                GetType::MODE_FLAGS_A,
                GetType::MODE_FLAGS_B,
                GetType::ENERGY_USAGE,
                GetType::ENERGY_DELIVERY,
            };

            if (!dipValid)
            {
                PendingCmd dip;
                dip.msg = Message{MsgType::GET_CMD, GetType::DIP_SWITCHES};
                getQueue.push_back(std::move(dip));
            }

            for (auto t : types)
            {
                PendingCmd pending;
                pending.msg = Message{MsgType::GET_CMD, t};
                getQueue.push_back(std::move(pending));
            }
        }

        if (dropped > 0)
            log_web(F("status query restarted, %u requests of previous cycle were still pending"), dropped);

        return dispatch_next_cmd();
    }

    String get_device_model()
    {
        return F("Ecodan Air Source Heat Pump");
    }

    Status& get_status()
    {
        return status;
    }

    float get_temperature_step()
    {
        return temperatureStep;
    }

    float get_min_thermostat_temperature()
    {
        return 8.0f;
    }

    float get_max_thermostat_temperature()
    {
        return 28.0f;
    }

    bool set_z1_target_temperature(float newTemp)
    {
        if (newTemp > get_max_thermostat_temperature())
        {
            log_web(F("Thermostat setting exceeds maximum allowed!"));
            return false;
        }

        if (newTemp < get_min_thermostat_temperature())
        {
            log_web(F("Thermostat setting is lower than minimum allowed!"));
            return false;
        }

        Message cmd{MsgType::SET_CMD, SetType::BASIC_SETTINGS};
        cmd[1] = SET_SETTINGS_FLAG_ZONE_TEMPERATURE;
        cmd[2] = static_cast<uint8_t>(SetZone::ZONE_1);
        cmd.set_float16(newTemp, 10);

        if (!enqueue_set_cmd(std::move(cmd)))
        {
            log_web(F("command dispatch failed for z1 temperature setting!"));
            return false;
        }

        return true;
    }

    bool set_z2_target_temperature(float newTemp)
    {
        if (newTemp > get_max_thermostat_temperature())
        {
            log_web(F("Thermostat setting exceeds maximum allowed!"));
            return false;
        }

        if (newTemp < get_min_thermostat_temperature())
        {
            log_web(F("Thermostat setting is lower than minimum allowed!"));
            return false;
        }

        // Using SetZone::ZONE_2 seems to reset Zone1 to zero therefore we set both at the same time.
        // SetZone::ZONE_2 is labelled as (Probably) in the CN105 docs so we dont trust it
        Message cmd{MsgType::SET_CMD, SetType::BASIC_SETTINGS};
        cmd[1] = SET_SETTINGS_FLAG_ZONE_TEMPERATURE;
        cmd[2] = static_cast<uint8_t>(SetZone::BOTH);
        cmd.set_float16(status.Zone1SetTemperature, 10);
        cmd.set_float16(newTemp, 12);

        if (!enqueue_set_cmd(std::move(cmd)))
        {
            log_web(F("command dispatch failed for z2 temperature setting!"));
            return false;
        }

        return true;
    }

    // From FTC6 installation manual ("DHW max. temp.")
    float get_min_dhw_temperature()
    {
        return 40.0f;
    }

    float get_max_dhw_temperature()
    {
        return 60.0f;
    }

    // From FTC6 installation manual ("Zone heating/cooling min. temp.")
    float get_min_flow_target_temperature(String mode)
    {
        String coolMode = "Cool Flow Temperature";
        return (coolMode == mode) ? 5.0f : 20.0f;
    }

    float get_max_flow_target_temperature(String mode)
    {
        String coolMode = "Cool Flow Temperature";
        return (coolMode == mode) ? 25.0f : 60.0f;
    }

    bool set_z1_flow_target_temperature(float newTemp)
    {
        if (newTemp > get_max_flow_target_temperature(status.hp_mode_as_string()))
        {
            log_web(F("Z1 flow temperature setting exceeds maximum allowed (%s)!"), String(get_max_flow_target_temperature(status.hp_mode_as_string())).c_str());
            return false;
        }

        if (newTemp < get_min_flow_target_temperature(status.hp_mode_as_string()))
        {
            log_web(F("Z1 flow temperature setting is lower than minimum allowed (%s)!"), String(get_min_flow_target_temperature(status.hp_mode_as_string())).c_str());
            return false;
        }

        Message cmd{MsgType::SET_CMD, SetType::BASIC_SETTINGS};
        cmd[1] = SET_SETTINGS_FLAG_ZONE_TEMPERATURE;
        cmd[2] = static_cast<uint8_t>(SetZone::ZONE_1);
        cmd[6] = static_cast<uint8_t>(SetHpMode::FLOW_CONTROL_MODE);
        cmd.set_float16(newTemp, 10);

        if (!enqueue_set_cmd(std::move(cmd)))
        {
            log_web(F("command dispatch failed for Z1 flow target temperature setting!"));
            return false;
        }

        return true;
    }

    bool set_z2_flow_target_temperature(float newTemp)
    {
        if (newTemp > get_max_flow_target_temperature(status.hp_mode_as_string()))
        {
            log_web(F("Z2 flow temperature setting exceeds maximum allowed (%s)!"), String(get_max_flow_target_temperature(status.hp_mode_as_string())).c_str());
            return false;
        }

        if (newTemp < get_min_flow_target_temperature(status.hp_mode_as_string()))
        {
            log_web(F("Z2 flow temperature setting is lower than minimum allowed (%s)!"), String(get_min_flow_target_temperature(status.hp_mode_as_string())).c_str());
            return false;
        }

        Message cmd{MsgType::SET_CMD, SetType::BASIC_SETTINGS};
        cmd[1] = SET_SETTINGS_FLAG_ZONE_TEMPERATURE;
        cmd[2] = static_cast<uint8_t>(SetZone::ZONE_2);
        cmd[6] = static_cast<uint8_t>(SetHpMode::FLOW_CONTROL_MODE);
        cmd.set_float16(newTemp, 10);

        if (!enqueue_set_cmd(std::move(cmd)))
        {
            log_web(F("command dispatch failed for Z2 flow target temperature setting!"));
            return false;
        }

        return true;
    }

    bool set_dhw_target_temperature(float newTemp)
    {
        if (newTemp > get_max_dhw_temperature())
        {
            log_web(F("DHW setting exceeds maximum allowed (%s)!"), String(get_max_dhw_temperature()).c_str());
            return false;
        }

        if (newTemp < get_min_dhw_temperature())
        {
            log_web(F("DHW setting is lower than minimum allowed (%s)!"), String(get_min_dhw_temperature()).c_str());
            return false;
        }

        Message cmd{MsgType::SET_CMD, SetType::BASIC_SETTINGS};
        cmd[1] = SET_SETTINGS_FLAG_DHW_TEMPERATURE;
        cmd.set_float16(newTemp, 8);

        if (!enqueue_set_cmd(std::move(cmd)))
        {
            log_web(F("command dispatch failed for DHW temperature setting!"));
            return false;
        }

        return true;
    }

    bool set_dhw_mode(String mode)
    {
        Status::DhwMode dhwMode = Status::DhwMode::NORMAL;

        if (mode == "off")
            return set_dhw_force(false);
        else if (mode == "performance")
            dhwMode = Status::DhwMode::NORMAL;
        else if (mode == "eco")
            dhwMode = Status::DhwMode::ECO;
        else
            return false;

        Message cmd{MsgType::SET_CMD, SetType::BASIC_SETTINGS};
        cmd[1] = SET_SETTINGS_FLAG_DHW_MODE;
        cmd[5] = static_cast<uint8_t>(dhwMode);

        if (!enqueue_set_cmd(std::move(cmd)))
        {
            log_web(F("command dispatch failed for DHW mode setting!"));
            return false;
        }

        return true;
    }

    bool set_dhw_force(bool on)
    {
        Message cmd{MsgType::SET_CMD, SetType::DHW_SETTING};
        cmd[1] = SET_SETTINGS_FLAG_MODE_TOGGLE;
        cmd[3] = on ? 1 : 0; // bit[3] of payload is DHW force, bit[2] is Holiday mode.

        if (!enqueue_set_cmd(std::move(cmd)))
        {
            log_web(F("command dispatch failed for DHW force setting!"));
            return false;
        }

        return true;
    }

    bool set_power_mode(bool on)
    {
        Message cmd{MsgType::SET_CMD, SetType::BASIC_SETTINGS};
        cmd[1] = SET_SETTINGS_FLAG_MODE_TOGGLE;
        cmd[3] = on ? 1 : 0;
        if (!enqueue_set_cmd(std::move(cmd)))
        {
            log_web(F("command dispatch failed for power mode setting!"));
            return false;
        }

        return true;
    }

    bool set_hp_mode(uint8_t mode)
    {
        Message cmd{MsgType::SET_CMD, SetType::BASIC_SETTINGS};
        cmd[1] = SET_SETTINGS_FLAG_HP_MODE;
        cmd[6] = mode;

        if (!enqueue_set_cmd(std::move(cmd)))
        {
            log_web(F("command dispatch failed for heat pump mode setting!"));
            return false;
        }

        return true;
    }

    void handle_set_response(Message& res)
    {
        if (res.type() != MsgType::SET_RES)
        {
            log_web(F("Unexpected set response type: %#x"), static_cast<uint8_t>(res.type()));
        }

        if (complete_inflight(MsgType::SET_RES, res[0]))
        {
            // Re-read the status shortly after a change (several SETs -> one refresh).
            std::lock_guard<std::mutex> lock{cmdQueueMutex};
            statusRefreshPending = true;
            statusRefreshDue = std::chrono::steady_clock::now() + STATUS_REFRESH_AFTER_SET;
        }

        // Upstream sent nothing after SET_RES -> the chain got stuck.
        if (!dispatch_next_cmd())
        {
            log_web(F("Failed to dispatch next command!"));
        }
    }

    void handle_get_response(Message& res)
    {
        {
            // Store the raw response for diagnostics
            std::lock_guard<std::mutex> lock{rawResponseMutex};
            RawResponse& r = rawResponses[res.payload_type<uint8_t>()];
            memcpy(r.data, res.payload(), PAYLOAD_SIZE);
            uint32_t now = millis();
            r.when = now == 0 ? 1 : now;
        }

        {
            std::lock_guard<Status> lock{status};

            switch (res.payload_type<GetType>())
            {
            case GetType::DEFROST_STATE:
                status.DefrostActive = res[3] != 0;
                break;
            case GetType::COMPRESSOR_FREQUENCY:
                status.CompressorFrequency = res[1];
                break;
            case GetType::FORCED_DHW_STATE:
                status.DhwForcedActive = res[7] != 0;
                break;
            case GetType::HEATING_POWER:
                status.OutputPower = res[6];
                break;
            case GetType::TEMPERATURE_CONFIG:
                status.Zone1SetTemperature = res.get_float16(1);
                status.Zone2SetTemperature = res.get_float16(3);
                status.Zone1FlowTemperatureSetPoint = res.get_float16(5);
                status.Zone2FlowTemperatureSetPoint = res.get_float16(7);
                status.LegionellaPreventionSetPoint = res.get_float16(9);
                status.DhwTemperatureDrop = res.get_float8_v2(11);
                status.MaximumFlowTemperature = res.get_float8_v3(12);
                status.MinimumFlowTemperature = res.get_float8_v3(13);
                break;
            case GetType::SH_TEMPERATURE_STATE:
                status.Zone1RoomTemperature = res.get_float16(1);
                if (res.get_u16(3) != 0xF0C4) // 0xF0C4 seems to be a sentinel value for "not reported in the current system"
                    status.Zone2RoomTemperature = res.get_float16(3);
                else
                    status.Zone2RoomTemperature = 0.0f;
                status.OutsideTemperature = res.get_float8(11);
                break;
            case GetType::DHW_TEMPERATURE_STATE_A:
                status.DhwFeedTemperature = res.get_float16(1);
                status.DhwReturnTemperature = res.get_float16(4);
                status.DhwTemperature = res.get_float16(7);
                break;
            case GetType::DHW_TEMPERATURE_STATE_B:
                status.BoilerFlowTemperature = res.get_float16(1);
                status.BoilerReturnTemperature = res.get_float16(4);
                break;
            case GetType::ACTIVE_TIME:
                status.CompressorOn = res[1] != 0;
                // gekkekoe get_float24_v2(3): byte 3 = units/remainder, bytes 4..5 = hundreds
                status.CompressorRuntimeHours = float((uint16_t(res[4]) << 8) | res[5]) * 100.0f + res[3];
                status.CompressorRuntimeValid = true;
                break;
            case GetType::FLOW_RATE:
                status.FlowRate = res[12];
                break;
            case GetType::THERMISTOR_STATE:
                break; // boiler flow/return, usually unused (still available via probe)
            case GetType::TEMPERATURE_STATE_D:
                status.BufferTemperature = res.get_float16(1);
                status.BufferTemperatureValid = true;
                break;
            case GetType::DIP_SWITCHES:
                for (size_t i = 0; i < 7; ++i)
                    dipSwitches[i] = res[1 + i * 2];
                if (!dipValid)
                {
                    dipValid = true;
                    log_web(F("DIP switches read: %s -> zone 2 %s"), dip_switches_as_string().c_str(), zone2_present() == 1 ? "present" : "not present");
                }
                break;
            case GetType::MODE_FLAGS_A:
                status.set_power_mode(res[3]);
                status.set_operation_mode(res[4]);
                status.set_dhw_mode(res[5]);
                status.set_heating_cooling_mode(res[6]);
                status.DhwFlowTemperatureSetPoint = res.get_float16(8);
                status.RadiatorFlowTemperatureSetPoint = res.get_float16(12);
                break;
            case GetType::MODE_FLAGS_B:
                status.HolidayMode = res[4] > 0;
                status.DhwTimerMode = res[5] > 0;
                break;
            case GetType::ENERGY_USAGE:
                status.EnergyConsumedHeating = res.get_float24(4);
                status.EnergyConsumedCooling = res.get_float24(7);
                status.EnergyConsumedDhw = res.get_float24(10);
                break;
            case GetType::ENERGY_DELIVERY:
                status.EnergyDeliveredHeating = res.get_float24(4);
                status.EnergyDeliveredCooling = res.get_float24(7);
                status.EnergyDeliveredDhw = res.get_float24(10);
                ++statusCycleCount; // last type in the cycle
                {
                    // Log the RX thread's stack headroom once per boot.
                    static bool stackLogged = false;
                    if (!stackLogged)
                    {
                        stackLogged = true;
                        log_web(F("RX thread stack: %u bytes never used (of 6144)"), (unsigned)uxTaskGetStackHighWaterMark(nullptr));
                    }
                }
                break;
            default:
                log_web(F("Unknown response type received on serial port: %u"), static_cast<uint8_t>(res.payload_type<GetType>()));
                break;
            }
        }

        if (!complete_inflight(MsgType::GET_RES, res.payload_type<uint8_t>()))
        {
            log_web(F("Ignoring unexpected/late GET response type %#x"), res.payload_type<uint8_t>());
            return;
        }

        if (!dispatch_next_cmd())
        {
            log_web(F("Failed to dispatch status update command!"));
        }
    }

    void handle_connect_response(Message& res)
    {
        log_web(F("connection reply received from heat pump"));

        {
            std::lock_guard<std::mutex> lock{cmdQueueMutex};
            awaitingResponse = false;
            consecutiveTimeouts = 0;
        }

        connected = true;

        // Send any commands that arrived while disconnected.
        dispatch_next_cmd();
    }

    void handle_ext_connect_response(Message& res)
    {
        log_web(F("Unexpected extended connection response!"));
    }

    void IRAM_ATTR serial_rx_isr()
    {
        BaseType_t higherPriorityTaskWoken = pdFALSE;
        vTaskNotifyGiveIndexedFromISR(serialRxTaskHandle, 0, &higherPriorityTaskWoken);
#if CONFIG_IDF_TARGET_ESP32C3
        portEND_SWITCHING_ISR(higherPriorityTaskWoken);
#else
        portYIELD_FROM_ISR(higherPriorityTaskWoken);
#endif
    }

    void serial_rx_thread()
    {
        ehal::add_thread_to_watchdog();

        // Wake the serial RX thread when the serial RX GPIO pin changes (this may occur during or after packet receipt)
        serialRxTaskHandle = xTaskGetCurrentTaskHandle();

        {
            auto& config = config_instance();
            attachInterrupt(digitalPinToInterrupt(config.SerialRxPort), serial_rx_isr, FALLING);
        }

        while (true)
        {
            try
            {
                ehal::ping_watchdog();

                Message res;
                if (!serial_rx(res))
                {
                    continue;
                }

                switch (res.type())
                {
                case MsgType::SET_RES:
                    handle_set_response(res);
                    break;
                case MsgType::GET_RES:
                    handle_get_response(res);
                    break;
                case MsgType::CONNECT_RES:
                    handle_connect_response(res);
                    break;
                case MsgType::EXT_CONNECT_RES:
                    handle_ext_connect_response(res);
                    break;
                default:
                    log_web(F("Unknown serial message type received: %#x"), static_cast<uint8_t>(res.type()));
                    break;
                }
            }
            catch (std::exception const& ex)
            {
                ehal::log_web(F("Exception occurred on serial rx thread: %s"), ex.what());
                std::this_thread::sleep_for(std::chrono::seconds(1));
            }
        }
    }

    bool initialize()
    {
        auto& config = config_instance();

        log_web(F("Initializing HeatPump with serial rx: %d, tx: %d"), (int8_t)config.SerialRxPort, (int8_t)config.SerialTxPort);

        pinMode(config.SerialRxPort, INPUT_PULLUP);
        pinMode(config.SerialTxPort, OUTPUT);

        delay(25); // There seems to be a window after setting the pin modes where trying to use the UART can be flaky, so introduce a short delay

        port.begin(2400, SERIAL_8E1, config.SerialRxPort, config.SerialTxPort);

        // std::thread only gets a 2048 B stack in the current Arduino core (IDF 5.5)
        // (CONFIG_PTHREAD_TASK_STACK_SIZE_DEFAULT). With packet dump enabled (log_web -> vsnprintf/strftime)
        // the RX thread overflowed its stack -> panic/boot loop (confirmed by core dump). Hence 6 KB.
        {
            esp_pthread_cfg_t cfg = esp_pthread_get_default_config();
            cfg.stack_size = 6144;
            cfg.prio = 5;
            cfg.thread_name = "ecodan_rx";
            esp_pthread_set_cfg(&cfg);
        }
        serialRxThread = std::thread{serial_rx_thread};

        if (!begin_connect())
        {
            log_web(F("Failed to start heatpump connection proceedure..."));
        }

        return true;
    }

    void handle_loop()
    {
        if (!is_connected() && !port.available())
        {
            static auto last_attempt = std::chrono::steady_clock::now();
            auto now = std::chrono::steady_clock::now();
            if (now - last_attempt > std::chrono::seconds(10))
            {
                last_attempt = now;
                if (!begin_connect())
                {
                    log_web(F("Failed to start heatpump connection proceedure..."));
                }
            }

            return;
        }
        else if (is_connected())
        {
            check_response_timeout();

            auto now = std::chrono::steady_clock::now();
            bool refresh = false;
            {
                std::lock_guard<std::mutex> lock{cmdQueueMutex};
                if (statusRefreshPending && now >= statusRefreshDue && setQueue.empty())
                {
                    statusRefreshPending = false;
                    refresh = true;
                }
            }

            // Update status the first time we enter this condition, then every 60s thereafter
            // (and additionally shortly after a SET command).
            if (refresh || !statusEverRequested || now - lastStatusUpdate > STATUS_INTERVAL)
            {
                statusEverRequested = true;
                lastStatusUpdate = now;
                if (!begin_get_status())
                {
                    log_web(F("Failed to begin heatpump status update!"));
                }
            }
        }
    }

    bool is_connected()
    {
        return connected;
    }

    uint64_t get_tx_msg_count()
    {
        return txMsgCount;
    }

    uint64_t get_rx_msg_count()
    {
        return rxMsgCount;
    }

    uint32_t get_status_cycle_count()
    {
        return statusCycleCount;
    }

    int8_t zone2_present()
    {
        if (!dipValid)
            return -1;

        // Logic from gekkekoe (status.h has_2zones), bit 0 = switch x-1:
        //  SW3-6 on and SW2-7 off -> 2 zones with shared flow (2-zone valve)
        //  SW2-6 on and SW2-7 on  -> 2 zones with separate flow circuits (mixing tank)
        auto isOn = [](uint8_t v, uint8_t n) { return (v & (1U << n)) != 0; };
        const uint8_t sw2 = dipSwitches[1];
        const uint8_t sw3 = dipSwitches[2];
        if (isOn(sw3, 5) && !isOn(sw2, 6))
            return 1;
        if (isOn(sw2, 5) && isOn(sw2, 6))
            return 1;
        return 0;
    }

    String dip_switches_as_string()
    {
        if (!dipValid)
            return F("unknown");

        String s;
        for (size_t i = 0; i < 7; ++i)
        {
            s += F("SW");
            s += String(i + 1);
            s += ' ';
            for (uint8_t b = 0; b < 8; ++b)
                s += (dipSwitches[i] & (1U << b)) ? '1' : '0';
            if (i < 6)
                s += ' ';
        }
        return s;
    }

    bool is_probe_type_allowed(uint8_t type)
    {
        // Only GET types without parameters. Excluded: 0xA3 (service request code - also includes
        // reset/forced operation codes!) and 0xC9 (different message type).
        return (type >= 0x01 && type <= 0x2F) || type == 0xA1 || type == 0xA2;
    }

    bool request_probe(uint8_t type)
    {
        if (!is_probe_type_allowed(type) || !connected)
            return false;

        {
            std::lock_guard<std::mutex> lock{cmdQueueMutex};
            PendingCmd pending;
            pending.msg = Message{MsgType::GET_CMD, static_cast<GetType>(type)};
            getQueue.push_front(std::move(pending));
        }

        return dispatch_next_cmd();
    }

    bool get_last_response(uint8_t type, uint8_t out[16], uint32_t& ageMs)
    {
        std::lock_guard<std::mutex> lock{rawResponseMutex};
        const RawResponse& r = rawResponses[type];
        if (r.when == 0)
            return false;
        memcpy(out, r.data, PAYLOAD_SIZE);
        ageMs = millis() - r.when;
        return true;
    }
} // namespace ehal::hp
