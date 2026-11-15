# Changelog

All notable changes of this fork are listed here. The fork is based on
[rbroker/ecodan-ha-local](https://github.com/rbroker/ecodan-ha-local) v0.2.3.

## v0.3.1 - 2026-11-15

Bug-fix release. Installing it over v0.3.0 with the web update keeps all settings and
Home Assistant entities.

### Fixed

- **Energy statistics in Home Assistant were too low.** The heat pump reports the total of the
  previous day. The six energy sensors (heating, cooling, DHW; consumed and delivered) used
  `state_class: total_increasing`, so whenever a day was higher than the day before, Home Assistant
  only added the difference to its long-term statistics and the energy dashboard (measured:
  37.9 kWh instead of 73.6 kWh in nine days). They now use `state_class: total` with a `last_reset`
  that follows the date of the value, so every day is added in full.
- **The web UI accepted changes without a login**, even with a device password set: saving the
  configuration, resetting it, uploading a firmware, reading the log and scanning for WiFi networks.
  All of these now require the login.
- Failed logins wrote the attempted password to the log.
- The configuration page broke if a password or SSID contained a quote, `<` or `&`, and saving the
  page then stored a truncated value.
- "Heat pump connection state" could never report "disconnected", because nothing was published
  while the heat pump did not answer. MQTT was not reconnected in that state either.
- The "Rebooting..." page after saving or updating got stuck instead of returning to the home page.
- "Device Boot Time" on the diagnostics page always showed 1970.
- Set points were truncated instead of rounded (20.3 was sent as 20.29).
- If an MQTT subscription failed, the bridge stayed connected and silently ignored those commands.
- A failed outdoor unit query was only repeated after its full interval; for the hourly values the
  sensor could be unavailable for almost an hour.

### Changed

- The MQTT payload of the six energy state topics is now JSON, for example
  `{"value":8.41,"date":"2026-11-14T00:00:00+00:00"}`. Home Assistant handles this through
  discovery; anything else that reads these topics directly has to be adapted.
- Resetting the configuration (`/clear_config`) and saving it (`/save`) are POST requests only.
- Requests that are refused because of a missing login answer with HTTP 401
  (page views still show the login page).
- The login cookie is `HttpOnly` and `SameSite=Lax`.
- Saving the configuration keeps every field that is not part of the request. Before, a script that
  left a field out cleared it. Checkboxes can be set from a script with `name=on` / `name=off`;
  leaving them out keeps their value.
- The diagnostic log JSON has a second list `packets` for serial packet dumps; `messages` only
  contains regular messages.
- Heat pump values are published only after a full status cycle was read since the last (re)connect,
  so there are no zeros after a restart and no stale values after a reconnect. The status is read
  right after a reconnect instead of up to a minute later.
- WiFi signal strength is sampled once a minute instead of being published on every change.
- The heat pump page shows the date the efficiency values belong to, and "not present" for zone 2
  if the system has none.
- The diagnostics page shows the Home Assistant unique ID of the climate entities instead of a
  guessed entity ID.

### Internal

- MQTT publishing and the heat pump page work on a copy of the status instead of holding its lock.
  The lock blocked the serial receive thread during network I/O, which could cause false timeouts
  with a slow broker.
- Separate ring buffers for regular log messages (48) and packet dumps (32).
- Numbers in the configuration form are validated; an empty unique ID falls back to the device ID.
- Version number in one place, HTML fixes, removed unused code, no compiler warnings.
- README: "Auto-Reset WiFi Settings" is on by default and erases the WiFi credentials after
  10 minutes without a connection at start (the documentation said off / ~20 minutes).

### Upgrade notes

- Statistics recorded before the update are not corrected. At the switch Home Assistant may add
  one extra daily value per energy sensor once.
- Scripts that read `/query_diagnostic_logs` or post to `/save` and `/update` have to log in first
  (`POST /verify_login`, field `device_pw`) and send the cookie. The cookie is valid until the next
  restart of the bridge.

## v0.3.0 - 2026-11-09

First release of the fork. Drop-in replacement for upstream v0.2.3: it can be installed with the
web update, settings and Home Assistant entities are kept.

### Added

- **Safe firmware updates.** A new firmware runs in a test mode for about 3 minutes. If it crashes,
  hangs, or cannot talk to the heat pump and MQTT, the previous firmware is restored automatically.
  The previous firmware can also be restored manually (`POST /firmware_rollback`).
- New values in Home Assistant:
  - Buffer / mixing tank temperature (THW10)
  - Compressor: on/off, operating hours, number of starts, average run time per start
  - Outdoor unit (request codes): target frequency, frequency limitation, thermo ON time, discharge
    temperature (TH4), compressor current, discharge superheat, sub-cool, ambient temperature (TH7),
    actuators, control state, operating hours
- THW6/THW7 and the compressor frequency are updated every 20 seconds (other values every 60 seconds).
- Zone 2 detection from the DIP switches: zone 2 entities are only created if the system has zone 2,
  and commands for a missing zone 2 are ignored.
- Optional German or English entity names (configuration page). Entity IDs do not change.
- Heat pump page: hydraulics, buffer tank, compressor, outdoor unit values, DIP switches and raw data.
- Diagnostics page: reason of the last reset, WiFi disconnects, state of the running firmware.
- Diagnostic endpoints to read raw responses (`/cn105_probe`) and outdoor unit request codes
  (`/cn105_rc`). Only documented read-only codes can be sent.
- Builds with PlatformIO for the LOLIN S2 Mini and generic ESP32 dev kits (4 MB flash or more).
  GitHub Actions builds the web update files and factory images for USB and publishes them
  with checksums.

### Fixed

- Crashes and restarts (task watchdog) when several commands arrived at once, for example from
  automations or sliders. Only one request at a time is sent to the heat pump; commands are queued,
  repeated once after a timeout and no longer dropped by the status cycle.
- Boot loop with newer ESP32 cores: the stack of the serial receive thread was too small.
- The bridge did not come back after a short WiFi dropout, because it restarted the connection
  attempt in every loop.
- Setting the DHW temperature also changed the Zone 1 room set point.
- Switching the water heater entity on or off wrote to a state topic instead of sending a command.
- Sensor types in Home Assistant: output power was rejected and shown as unavailable; device and
  state classes for temperatures, frequency, energy and durations (long-term statistics).
- A failed firmware upload was reported as successful and restarted the bridge. Upload errors are
  now shown and the running firmware stays active.
- Watchdog resets during long uploads, while waiting for serial data and during MQTT reconnects.

### Changed

- Much less MQTT traffic: only changed values are sent, plus a full refresh every 2 minutes.
  Discovery messages are retained and only sent after a (re)connect or a Home Assistant restart,
  not on every update.
- MQTT messages use QoS 1 instead of QoS 2; incoming commands are processed outside the MQTT callback.
- The compressor frequency is read from the FTC only.
- The image is built for size (`-Os`), and the USB serial console is off on the LOLIN S2 Mini, so the
  image fits into the update partition. USB flashing on that board needs BOOT + RST.
