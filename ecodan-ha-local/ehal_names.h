#pragma once

// Display names of the Home Assistant entities (German / English).
//
// Terms from the EHST20D/EHPT series installation manual:
//   - Menu tree   EN "Main Controller Menu Tree" / DE "Menuebaum Hauptsteuerung"
//     (Forced DHW / Vorrang TWW-Bereitung, Energy monitor / Energiemonitoring,
//      Consumed electrical energy / Verbrauchte elektr. Energie, Delivered energy / Produzierte Energie,
//      DHW max. temp. / Trinkwarmwasser Max., DHW max. temp. drop / TWW Nacherwaermungs-Diff.,
//      Zone1 / HK 1, Legionella / Legionellen)
//   - Table "Thermistor inputs" / "Eingaenge Temperaturfuehler" (THW1, THW2, THW6, THW7, THW10, ...)
//   - Glossary: Target flow/room temperature / Sollvorlauf-/Sollraumtemperatur, Holiday mode / Urlaubsmodus,
//     Defrosting / Abtaubetrieb, Outdoor ambient temp. / Aussentemperatur
// Compressor values: outdoor unit service manual OCH694A (request codes 2/3/16, English only;
// German "Verdichter" as in the manual, OUT15 "Verdichter-ON Signal").
//
// Key = technical name (without "_<UniqueId>"). Spaces are replaced by '_' before the lookup.

namespace ehal::names
{
    struct EntityName
    {
        const char* key;
        const char* de;
        const char* en;
    };

    static const EntityName ENTITY_NAMES[] = {
        // Control
        {"climate_control",            "HK 1 Heizen",                                  "Zone1 heating"},
        {"climate_control_z2",         "HK 2 Heizen",                                  "Zone2 heating"},
        {"dhw_water_heater",           "Trinkwarmwasser (TWW)",                        "Domestic hot water (DHW)"},
        {"force_dhw",                  "Vorrang TWW-Bereitung",                        "Forced DHW"},
        {"turn_on_off_hp",             "Ein/Aus",                                      "Power"},
        {"sh_mode",                    "HK 1 Heizbetrieb",                             "Zone1 heating mode"},
        {"z1_flow_temp_target_set",    "HK 1 Sollvorlauftemperatur",                   "Zone1 target flow temp."},

        // Temperatures (thermistor names as in the "Thermistor inputs" table)
        {"hp_feed_temp",               "Vorlauftemperatur (THW1)",                     "Flow water temp. (THW1)"},
        {"hp_return_temp",             "Rücklauftemperatur (THW2)",                   "Return water temp. (THW2)"},
        {"boiler_flow_temp",           "HK 1 Vorlauftemperatur (THW6)",                "Zone1 flow water temp. (THW6)"},
        {"boiler_return_temp",         "HK 1 Rücklauftemperatur (THW7)",              "Zone1 return water temp. (THW7)"},
        {"buffer_temp",                "Pufferspeicher Tankwassertemperatur (THW10)",  "Mixing tank water temp. (THW10)"},
        {"dhw_temp",                   "Trinkwarmwasserspeicher Temperatur",           "DHW tank water temp."},
        {"outside_temp",               "Außentemperatur",                             "Outdoor ambient temp."},
        {"z1_room_temp",               "HK 1 Raumtemperatur",                          "Zone1 room temp."},
        {"z2_room_temp",               "HK 2 Raumtemperatur",                          "Zone2 room temp."},
        {"z1_room_temp_target",        "HK 1 Sollraumtemperatur",                      "Zone1 target room temp."},
        {"z2_room_temp_target",        "HK 2 Sollraumtemperatur",                      "Zone2 target room temp."},
        {"z1_flow_temp_target",        "HK 1 Sollvorlauftemperatur (Status)",          "Zone1 target flow temp. (state)"},
        {"z2_flow_temp_target",        "HK 2 Sollvorlauftemperatur (Status)",          "Zone2 target flow temp. (state)"},
        {"sh_flow_temp_target",        "Sollvorlauftemperatur aktuell",                "Current target flow temp."},
        {"dhw_flow_temp_target",       "Trinkwarmwasser Max.",                         "DHW max. temp."},
        {"dhw_temp_drop",              "TWW Nacherwärmungs-Diff.",                    "DHW max. temp. drop"},
        {"legionella_prevention_temp", "Legionellen Warmwasser Temperatur",            "Legionella hot water temp."},

        // Operation
        {"mode_power",                 "Betriebszustand",                              "Power state"},
        {"mode_operation",             "Betriebsart",                                  "Operation mode"},
        {"mode_heating_cooling",       "HK 1 Heizbetrieb (Status)",                    "Zone1 heating mode (state)"},
        {"mode_dhw",                   "TWW Normal/Eco",                               "DHW Normal/Eco"},
        {"mode_dhw_forced",            "Vorrang TWW-Bereitung aktiv",                  "Forced DHW active"},
        {"mode_defrost",               "Abtaubetrieb",                                 "Defrosting"},
        {"flow_rate",                  "Durchfluss",                                   "Flow rate"},
        {"output_pwr",                 "Ausgangsleistung",                             "Output power"},

        // Compressor (OCH694A)
        {"compressor_frequency",       "Verdichter Frequenz",                          "Compressor operating frequency"},
        {"compressor_on",              "Verdichter EIN",                               "Compressor ON"},
        {"compressor_runtime",         "Verdichter Betriebsstunden",                   "Compressor accumulated operating time"},
        {"compressor_starts",          "Verdichter Anzahl Starts",                     "Compressor number of operation times"},
        {"compressor_avg_run",         "Verdichter Laufzeit pro Start",                "Compressor run time per start"},

        // Energy monitor
        {"heating_consumed",           "Heizen verbrauchte elektr. Energie",           "Heating consumed electrical energy"},
        {"heating_delivered",          "Heizen produzierte Energie",                   "Heating delivered energy"},
        {"cooling_consumed",           "Kühlen verbrauchte elektr. Energie",          "Cooling consumed electrical energy"},
        {"cooling_delivered",          "Kühlen produzierte Energie",                  "Cooling delivered energy"},
        {"dhw_consumed",               "TWW verbrauchte elektr. Energie",              "DHW consumed electrical energy"},
        {"dhw_delivered",              "TWW produzierte Energie",                      "DHW delivered energy"},
        {"sh_cop",                     "Heizen COP",                                   "Heating COP"},
        {"cool_cop",                   "Kühlen COP",                                  "Cooling COP"},
        {"dhw_cop",                    "TWW COP",                                      "DHW COP"},

        // Diagnostics
        {"Heat_pump_connection_state", "Verbindung Wärmepumpe",                       "Heat pump connection"},
        {"Wifi_signal",                "WLAN-Signal",                                  "WiFi signal"},
        {"Wifi_SSID",                  "WLAN-SSID",                                    "WiFi SSID"},
        {"IP_address",                 "IP-Adresse",                                   "IP address"},
        {"MAC_address",                "MAC-Adresse",                                  "MAC address"},
    };

    enum class Language : uint8_t
    {
        TECHNICAL = 0, // original behaviour: technical name incl. UniqueId
        GERMAN = 1,
        ENGLISH = 2
    };
} // namespace ehal::names
