# Baby Sensor Relax — unofficial passive monitor & logger

> **Unofficial, independent project.** Not affiliated with, sponsored by, or endorsed by the
> manufacturer of the Baby Sensor Relax. Not a medical device.

An **unofficial, receive-only** monitor for the *Baby Sensor Relax* wristband. It passively
listens to the Bluetooth Low Energy advertisements the wristband already broadcasts, decodes
the vitals (heart rate, SpO₂, skin temperature), and displays and logs them — **without ever
connecting to the wristband or its base station**, so the official system keeps working
untouched.

Two ways to use it:

- **Standalone device** — an inexpensive ESP32 "Cheap Yellow Display" (CYD) shows the live
  vitals, logs every reading to a microSD card as timestamped CSV, and draws 24-hour touch
  charts. No phone, no cloud, no internet (WiFi is used only to set the clock).
- **On a Mac** — small Swift command-line tools print the live vitals, for reverse-engineering
  or quick checks without any extra hardware.

![The monitor showing live vitals](docs/images/monitor-live.jpg)

![The monitor showing live vitals](docs/images/monitor-live_2.jpg)
---

## ⚠️ Important — read this first

- **This is NOT a medical device and NOT a safety monitor.** It is a hobby / educational
  project. Do not rely on it to keep a baby safe. Keep using the official monitor and follow
  your pediatrician's guidance.
- The displayed numbers can be wrong, delayed, or missing (weak signal, poor sensor contact).
- **"Est. Body" is an estimate** (skin temperature + 2 °C), clearly labeled as such — it is not
  a measured core temperature.
- This project is **not affiliated with or endorsed by** the manufacturer of the Baby Sensor
  Relax. Product names are used only to describe compatibility (see *Trademark* below).
- **It is strictly receive-only.** It never transmits to, connects to, pairs with, or modifies
  the wristband or base. It cannot interfere with the official system.

### Legal & reverse engineering

Provided as general context, **not legal advice**:

- **Owned device, passive method.** The protocol was reverse-engineered by passively receiving
  the *unencrypted* BLE advertisements a device **I own** already broadcasts to the room. No
  encryption was decrypted and **no technological protection measure was circumvented**, so
  anti-circumvention laws (e.g. US DMCA §1201) do not apply. Nothing is ever transmitted to the
  wristband or base.
- **Interoperability.** In the EU (where this was built) reverse engineering a lawfully-owned
  product for interoperability is permitted (Software Directive 2009/24/EC; Trade Secrets
  Directive 2016/943), and publishing the resulting interoperability facts is likewise permitted.
- **Trademark.** *Baby Sensor Relax* is a trademark of its respective owner. It is used here
  **only descriptively** (nominative fair use) to state what this project is compatible with.
  This project is **independent and not affiliated with, sponsored by, or endorsed by** the
  manufacturer, and contains **none** of their firmware, code, images, or branding.
- **If you accepted a licence for the official app**, check it for any reverse-engineering clause
  before publishing derivatives.

See [`docs/PROTOCOL.md`](docs/PROTOCOL.md) for the findings.

---

## How it works

The wristband is a **connectionless BLE broadcaster** — it puts the current vitals into the
manufacturer-data field of its advertisement, ~every 1.5 s, in the clear. Anyone in radio range
can read them. The base station is the *intended* receiver (and relays to the cloud), but it is
just one listener among many — a passive sniffer is another, and doesn't affect it.

```
 wristband ──(BLE broadcast, unencrypted)──► base ──(WiFi/cellular)──► cloud ──► phone app
     │
     └──(same broadcast)──► THIS project (passive, receive-only)
```

Decoded fields (all verified against the official app):

| Field | Source byte(s) | Notes |
|-------|----------------|-------|
| Heart rate | `b10` | bpm, updates ~20 s |
| SpO₂ | `b13` | %, commits ~15 min |
| Skin temp | `((b6<<8) \| b7) / 10` | °C, 0.1° res (big-endian uint16); updates ~every 15 min |
| Signal quality | `b4` | rough contact indicator |

Room temperature is measured by the **base**, not the wristband, so it is **not** available
over BLE. Full details in [`docs/PROTOCOL.md`](docs/PROTOCOL.md).

---

## Hardware (standalone device)

- **ESP32-2432S028 "Cheap Yellow Display" (CYD)** — 2.8" 320×240 touchscreen, ~€10.
  - This unit's panel is marked **`TPM408-2.8`** and is an **ILI9342 (320×240 native)** — note
    this differs from the common CYD's ILI9341 (240×320). The firmware is configured for the
    ILI9342; if your board is the ILI9341 type, adjust the panel class/size in `cyd_vitals.ino`.
  - **Panel colour quirk:** this unit is **BGR** (`rgb_order = true`) *and* renders `TFT_YELLOW`/
    `TFT_RED` swapped, so the warning-line colour constants are deliberately crossed in
    `miniPlot()`. If your panel shows wrong colours, that's the first thing to flip.
  - Chip: classic **ESP32-D0WD (BLE 4.2)** — sufficient, because the wristband uses BLE *legacy*
    advertising.
- **microSD card**, **FAT32** formatted (≤32 GB is FAT32 by default), for CSV logging.
- A 2.4 GHz WiFi network (used only at boot to set the real-time clock via NTP).

---

## Build & flash

Uses [`arduino-cli`](https://arduino.github.io/arduino-cli/) and the ESP32 Arduino core.

```bash
# one-time setup
arduino-cli core update-index
arduino-cli core install esp32:esp32
arduino-cli lib install "LovyanGFX"

# compile + flash the main firmware (adjust the serial port)
PORT=/dev/cu.usbserial-XXXX
arduino-cli compile -b esp32:esp32:esp32:PartitionScheme=huge_app firmware/cyd_vitals
arduino-cli upload  -b esp32:esp32:esp32:PartitionScheme=huge_app,UploadSpeed=115200 -p $PORT firmware/cyd_vitals
```

Notes:
- **`PartitionScheme=huge_app` is required** — WiFi + BLE + graphics don't fit the default app
  partition.
- If flashing fails at high speed, keep `UploadSpeed=115200` (some USB-serial adapters are
  unreliable at 921600).

---

## First-time setup (WiFi & SD)

The firmware reads WiFi credentials from **`/wifi.txt`** on the SD card (line 1 = SSID, line 2 =
password) and uses them **only** to sync the clock over NTP, then turns WiFi off so BLE runs
clean.

The SD card is wired to the ESP32, not to your computer, so you can't drop the file on it via a
card reader unless you remove the card. Two options:

1. **Provision from the ESP32** (no card reader needed): open
   `firmware/provision_wifi/provision_wifi.ino`, put your SSID/password in the two `REDACTED`
   fields, flash it once (it writes and verifies `/wifi.txt`), then flash `cyd_vitals` and
   **clear your credentials back out of the sketch**.
2. **Card reader**: create `/wifi.txt` on the card directly.

Timezone is set to **Europe/Warsaw** (CET/CEST with DST) in `cyd_vitals.ino` — change the
`setenv("TZ", ...)` / `configTzTime(...)` strings for your region.

**Boot behavior (normal):** on a cold power-up the device connects to WiFi, sets its clock via
NTP, then **soft-reboots itself once** and comes up in BLE-only mode. This is intentional — the
ESP32's WiFi/BLE radio *coexistence* severely throttles BLE reception, so the firmware uses WiFi
only briefly for the clock (which survives the reboot) and then runs Bluetooth at full strength.
Expect a ~15 s boot with one automatic restart.

---

## Using the device

**Live view** — big Heart rate and SpO₂ up top, skin temperature small next to the name, and a
1-hour BPM and SpO₂ sparkline (with yellow/red warning lines) along the bottom; time and signal in
the header. Values turn red past (configurable, non-medical) alert thresholds.

**24-hour charts** — tap the **HEART** or **OXYGEN** column (number or sparkline) to open a
full-screen 0:00→24:00 plot showing the **average ± standard deviation** per time-bin. Tap the
plot to cycle the bin size **1 h → 30 min → 15 min**; it **auto-returns to the live view after
10 s** of no touch. On boot the firmware reloads the day's CSV so charts survive a power cycle.

**Night dimming** — the backlight drops to its **lowest step between 22:00 and 07:00** and runs at 100 % the
rest of the day, so the monitor stays readable in a dark room without lighting it up. Change the
window or the levels with `NIGHT_START_MIN` / `NIGHT_END_MIN` / `BRIGHT_NIGHT` at the top of
`cyd_vitals.ino`. Until the clock is set the display stays at full brightness.

> **Powering it from a USB power bank?** At the lowest backlight steps the whole board can draw
> less than the ~50–100 mA most power banks treat as "nothing is plugged in", so the bank switches
> itself off after an hour or so. The firmware counters this with a short current burst every
> `KEEPALIVE_EVERY_MS` while dimmed (CPU spin + read-only SD traffic — silent, and no wear on the
> card). If your bank still cuts out, either raise `BRIGHT_NIGHT`, set `KEEPALIVE_BRIGHT` to add a
> visible backlight flash to the burst, or just use a mains USB charger — chargers have no
> low-load cutoff, which is the only fix that costs nothing.

**CSV logs** — one file per day on the SD card, e.g. `/vitals_2026-08-07.csv`:

```csv
timestamp,hr_bpm,spo2_pct,skin_c
2026-08-07 15:59:21,134,96,
2026-08-07 15:59:38,143,96,35.0
```

Pull the card any time to browse the full history on a computer. `tools/analyze.py` gives a
quick per-column summary of a capture/log.

---

## Mac tools (no extra hardware)

Built and run with the system Swift toolchain (macOS, CoreBluetooth):

```bash
swiftc -O mac/babysensor_reader.swift -o babysensor_reader
./babysensor_reader           # live: HR | SpO2 | skin | signal | RSSI

swiftc -O mac/passive_scan.swift -o passive_scan
./passive_scan 60             # 60 s raw advertisement survey (reverse-engineering)
```

Both are strictly receive-only (scan, never connect). Grant the terminal Bluetooth permission
in *System Settings → Privacy & Security → Bluetooth* on first run.

---

## Repository layout

```
firmware/
  cyd_vitals/       main firmware: display + BLE + WiFi/NTP + SD CSV + 24h touch plots
  reader_serial/    minimal ESP32 reader — decoded vitals over serial (no display)
  bandsniff/        reception test — confirms a classic ESP32 can hear the wristband
  provision_wifi/   one-time helper to write /wifi.txt to the SD card
mac/
  babysensor_reader.swift   live decoded vitals on macOS
  passive_scan.swift        raw BLE advertisement survey
tools/
  analyze.py        per-byte / per-column analysis of captures & CSV logs
docs/
  PROTOCOL.md       the reverse-engineered BLE protocol
  images/
```

---

## License & trademarks

Code and documentation: [PolyForm Noncommercial 1.0.0](LICENSE). Provided as-is, with no
warranty. Not a medical device.

**Free for parents.** Build it, flash it, change it, share it — for your own family, or for
any other noncommercial purpose. Hospitals, universities, charities and public research or
health organisations are covered explicitly. What the licence does not allow is selling this
software or devices running it, or building it into a paid product or service. If you want to
do that, ask me. See [`NOTICE`](NOTICE) for the details.

This is *source available*, not OSI open source — a deliberate choice, so the work stays
open to the people it was written for without being commercialised out from under them.

*Baby Sensor Relax* and any related names are trademarks of their respective owner and are used
here only for descriptive/compatibility purposes. This project is independent and unofficial.
