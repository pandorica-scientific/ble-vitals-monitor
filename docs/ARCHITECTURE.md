# Architecture

How the firmware is put together, and where a change belongs.

## The three layers

The monitor firmware (`firmware/cyd_vitals/`) is one Arduino sketch plus a set of headers, split
into three layers by what they may depend on:

| Layer | Files | May include | Tested by |
|---|---|---|---|
| **Pure logic** | `band_protocol.h`, `critical_alarm.h`, `day_night.h`, `history.h`, `day_bins.h`, `gesture.h`, `hold_gesture.h`, `alert_log.h`, `vitals_csv.h`, `contacts.h`, `radio_health.h`, `wifi_failover.h`, `trace_window.h`, `live_render_key.h`, `ota_password.h` | the C standard library and `config.h` | `tests/firmware_logic_test.cpp`, natively |
| **Rendering** | `display.h`, `live_render.h`, `plot_render.h`, `export_render.h`, `maintenance_render.h`, `alarm_render.h` | LovyanGFX, `config.h`, pure-logic headers | compiling for the ESP32 |
| **Device services** | `firmware_health.h` | the ESP-IDF OTA API | compiling for the ESP32 |
| **Glue** | `cyd_vitals.ino` | everything, plus the ESP32 BLE, WiFi, SD, web-server and ArduinoOTA libraries | running on the board |

The rule that keeps this useful: **any decision that can be wrong belongs in the pure layer**, where
a test can pin it down. The sketch reads sensors and clocks, hands the values to pure functions, and
draws what comes back. `config.h` holds every tunable and is included by all three layers.

### Pure logic

Each header is self-contained, header-only, and uses free `inline` functions over plain structs.
State is passed in and out explicitly; nothing reads `millis()` or the clock. That is what lets the
native test drive the alarm through a two-minute window in a few microseconds, or replay a day of
readings through the rate-correction rules.

The two headers that matter most:

- `band_protocol.h` decodes the wristband's 23-byte frame and decides which of the band's two
  heart-rate decodes to believe when they disagree. The evidence behind every constant is in
  [`PROTOCOL.md`](PROTOCOL.md).
- `critical_alarm.h` is the confirmation state machine for both the fast and the slow alarm. It
  is plain-old-data so the latched state survives a soft reboot in RTC memory.

### Rendering

The render headers take a `Screen` (the panel driver, the layout and the current chrome palette,
defined in `display.h`) and whatever data they draw. They hold no state of their own. Each screen
repaints only the regions whose inputs changed; the sketch decides *when* by comparing a render key
(`LiveRenderKey`, `AlarmRenderKey`) built from the current inputs.

The panel on this board shows the 16-bit complement of what is written, with red and blue
exchanged. `display.h` names the colours by what actually appears (`PANEL_RED`, `PANEL_YELLOW`,
`PANEL_WHITE`, `PANEL_BLACK`); use those rather than the library constants.

### Glue

`cyd_vitals.ino` is organised in sections in the order the data flows: readings, radio, clock,
storage, backlight, touch, export, maintenance, views, alarms, then `setup()` and `loop()`.
Sketch-local types are defined before the first function because the Arduino preprocessor inserts
generated prototypes ahead of it.

Export mode and maintenance mode share one shape: hand the radio from BLE to WiFi without a reboot,
own the loop while they last, and reboot to leave, because a reboot is the only reliable way back
to a full-speed BLE scan after WiFi has run.

## Data flow

```
BLE task                                   loop()
--------                                   ------
advertisement -> decodeBandFrame()         readSnapshot()
             -> mergeBandReading()  ---->  serviceRadioRecovery()      radio_health.h
                (band_protocol.h,          serviceAlarms()             critical_alarm.h
                 under a spinlock)         renderLive() / serviceAlarmView()
                                           recordNewReading()  -> SD CSV, DayBins, History
```

`mergeBandReading()` runs once per received frame and makes the heart-rate decision once per
measurement, so the display, the plots, the log and both alarms all see the same number.

## Files on the SD card

| Path | Written by | Read by |
|---|---|---|
| `/vitals_YYYY-MM-DD.csv` | `logVitalsRow()`, one row per measurement | `loadTodayFromCsv()` at boot; export mode; the report page |
| `/alerts.csv` | `logAlert()`, append-only | `reconcileOpenEpisode()` at boot |
| `/boot.log` | `logBoot()` | a person, when the board restarted overnight |
| `/wifi.txt`, `/wifi_backup.txt`, `/wifi_backup2.txt` | `provision_wifi` | `syncTimeOverWifi()` on a cold boot |
| `/contacts.txt` | `provision_wifi` | `loadContacts()` at boot |
| `/ota.txt` | `provision_wifi` | `enterMaintenance()`, to gate firmware uploads |

Export mode serves `/vitals_*.csv` only, by whitelist, because the credential files sit on the same
card.

## Boot sequence

1. Latch the reset reason (before the soft reboot below can overwrite it).
2. Initialise the panel and the SD card.
3. If the clock is unset and this power cycle has not synced yet: connect to WiFi, sync NTP, then
   soft-reboot. WiFi and BLE coexistence throttles reception, and a WiFi de-init does not fully
   restore it, so the second boot runs BLE only with the clock already set.
4. Log the boot, apply brightness and theme, reload today's CSV into the charts, load contacts,
   and re-enter any alarm episode that has an `ONSET` without a `DISMISS`.
5. Start the passive BLE scan.
6. A minute later, if this is a freshly installed firmware, confirm it (`firmware_health.h`).
   Until then a crash rolls back to the previous slot.

## Firmware slots

The board is built with the `no_fs` partition layout: two 1.98 MB application slots and no SPIFFS.
A USB flash always writes the first slot; an over-the-air upload writes whichever slot is not
running and switches the boot pointer. `/boot.log` records the slot each boot ran from.

## Other sketches and tools

- `firmware/reader_serial/` decodes the band to the serial port on a bare ESP32, sharing
  `band_protocol.h` with the monitor.
- `firmware/bandsniff/` lists every advertising device it hears, to confirm a classic ESP32 can
  receive the band at all.
- `firmware/provision_wifi/` writes the credential slots and `/contacts.txt` to the card over a
  serial prompt, so the card never has to leave the board.
- `mac/` holds two Swift command-line tools for the same passive reception on macOS.
- `tools/analyze.py` classifies the byte positions of a raw capture.

## Adding something

- A new rule about readings: a pure header (or `band_protocol.h`), plus a test.
- A new tunable: `config.h`, and a line in the README if a builder should know about it.
- A new screen: a `*_render.h` header taking a `Screen`, and a `View` value in the sketch.
- A new file on the card: a constant for its path in the storage section of the sketch, and a row
  in the table above.
