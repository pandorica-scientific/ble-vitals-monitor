# Baby Sensor Relax: unofficial passive monitor and logger

> **Unofficial, independent project.** Not affiliated with, sponsored by, or endorsed by the
> manufacturer of the Baby Sensor Relax. Not a medical device.

An **unofficial, receive-only** monitor for the *Baby Sensor Relax* wristband. It passively
listens to the Bluetooth Low Energy (BLE) advertisements the wristband already broadcasts, decodes
the vitals (heart rate, SpO₂, skin temperature), and displays and logs them **without ever
connecting to the wristband or its base station**, so the official system keeps working untouched.

Two ways to use it:

- **Standalone device.** An inexpensive ESP32 "Cheap Yellow Display" (CYD) shows the live vitals,
  logs every reading to a microSD card as timestamped CSV, draws 24-hour touch charts, raises a
  full-screen alarm for a sustained critical heart rate, and can serve its logs to a phone as a
  report. No phone, no cloud, no internet; WiFi is used only to set the clock.
- **On a Mac.** Small Swift command-line tools print the live vitals, for reverse engineering or
  quick checks without any extra hardware.

![The monitor showing live vitals](docs/images/monitor-live.jpg)

![The monitor showing live vitals](docs/images/monitor-live_2.jpg)

---

## Important: read this first

- **This is NOT a medical device and NOT a safety monitor.** It is a hobby and educational
  project. Do not rely on it to keep a baby safe. Keep using the official monitor and follow your
  paediatrician's guidance.
- The displayed numbers can be wrong, delayed or missing (weak signal, poor sensor contact, and
  the wristband's own failure modes described in [`docs/PROTOCOL.md`](docs/PROTOCOL.md)).
- This project is **not affiliated with or endorsed by** the manufacturer of the Baby Sensor Relax.
  Product names are used only to describe compatibility (see *Trademark* below).
- **It is strictly receive-only.** It never transmits to, connects to, pairs with or modifies the
  wristband or base. It cannot interfere with the official system.

### Legal and reverse engineering

Provided as general context, **not legal advice**:

- **Owned device, passive method.** The protocol was reverse-engineered by passively receiving
  the *unencrypted* BLE advertisements a device **I own** already broadcasts to the room. No
  encryption was decrypted and **no technological protection measure was circumvented**, so
  anti-circumvention laws (for example US DMCA §1201) do not apply. Nothing is ever transmitted to
  the wristband or base.
- **Interoperability.** In the EU (where this was built) reverse engineering a lawfully owned
  product for interoperability is permitted (Software Directive 2009/24/EC; Trade Secrets
  Directive 2016/943), and publishing the resulting interoperability facts is likewise permitted.
- **Trademark.** *Baby Sensor Relax* is a trademark of its respective owner. It is used here
  **only descriptively** (nominative fair use) to state what this project is compatible with. This
  project is **independent and not affiliated with, sponsored by, or endorsed by** the
  manufacturer, and contains **none** of their firmware, code, images or branding.
- **If you accepted a licence for the official app**, check it for any reverse-engineering clause
  before publishing derivatives.

See [`docs/PROTOCOL.md`](docs/PROTOCOL.md) for the findings.

---

## How it works

The wristband is a **connectionless BLE broadcaster**: it puts the current vitals into the
manufacturer-data field of its advertisement, about every 1.5 s, in the clear. Anyone in radio range
can read them. The base station is the *intended* receiver (and relays to the cloud), but it is one
listener among many; a passive receiver is another, and does not affect it.

```
 wristband --(BLE broadcast, unencrypted)--> base --(WiFi/cellular)--> cloud --> phone app
     |
     +--(same broadcast)--> THIS project (passive, receive-only)
```

Decoded fields (all verified against the official app):

| Field | Source byte(s) | Notes |
|-------|----------------|-------|
| Heart rate | `b10` | bpm, a new value about every 20 s |
| Beat interval | `b11..b12` | big-endian uint16, milliseconds; used to check the heart-rate byte |
| SpO₂ | `b13` | %, commits about every 15 min |
| Skin temperature | `((b6<<8) \| b7) / 10` | °C, 0.1° resolution (big-endian uint16); updates about every 15 min |
| Signal quality | `b4` | rough contact indicator |

Room temperature is measured by the **base**, not the wristband, so it is **not** available over
BLE. Full details in [`docs/PROTOCOL.md`](docs/PROTOCOL.md); how the firmware is organised is in
[`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md).

---

## Hardware (standalone device)

- **ESP32-2432S028 "Cheap Yellow Display" (CYD)**: 2.8" 320×240 touchscreen, about €10.
  - This unit's panel is marked **`TPM408-2.8`** and is an **ILI9342 (320×240 native)**. Note this
    differs from the common CYD's ILI9341 (240×320). The firmware is configured for the ILI9342 in
    `firmware/cyd_vitals/display.h`; if your board is the ILI9341 type, adjust the panel class and
    size there.
  - **Panel colour quirk:** this unit shows the 16-bit complement of what is written, with red and
    blue exchanged, so the library's `TFT_BLACK` appears white and `TFT_RED` appears yellow. The
    firmware names colours by what actually appears (`PANEL_RED`, `PANEL_YELLOW` and so on in
    `display.h`). If your panel shows wrong colours, that file is the first thing to look at.
  - Chip: classic **ESP32-D0WD (BLE 4.2)**. Sufficient, because the wristband uses BLE *legacy*
    advertising.
- **microSD card**, **FAT32** formatted (cards of 32 GB or less are FAT32 by default), for logging.
- A 2.4 GHz WiFi network, used only at boot to set the real-time clock over NTP.

---

## Build, test and flash

Uses [`arduino-cli`](https://arduino.github.io/arduino-cli/) and the ESP32 Arduino core.

```bash
# one-time setup
arduino-cli core update-index
arduino-cli core install esp32:esp32@3.3.11
arduino-cli lib install "LovyanGFX@1.2.26"

# native tests: the firmware logic (C++17) and the report page (Node.js 18+)
sh tests/run.sh

# compile and flash the main firmware over USB (adjust the serial port)
PORT=/dev/cu.usbserial-XXXX
arduino-cli compile -b esp32:esp32:esp32:PartitionScheme=no_fs firmware/cyd_vitals
arduino-cli upload  -b esp32:esp32:esp32:PartitionScheme=no_fs,UploadSpeed=115200 -p $PORT firmware/cyd_vitals
```

Notes:
- **`PartitionScheme=no_fs` is required.** It gives the firmware two 1.98 MB slots, which is what
  over-the-air updates need, and no SPIFFS, which the firmware never used. Boards flashed with the
  earlier `huge_app` layout must be flashed over USB once more; after that, updates can go over the
  air (see *Updating the firmware over the air* below).
- If flashing fails at high speed, keep `UploadSpeed=115200`. Some USB-serial adapters are
  unreliable at 921600.
- On Apple Silicon Macs the ESP32 toolchain needs Rosetta (`softwareupdate --install-rosetta`).

### Configuration

Everything tunable is in **`firmware/cyd_vitals/config.h`**: the name shown in the header of the
live view (`DISPLAY_NAME`), the warning and alarm thresholds, the timezone and NTP servers, the site
coordinates for the sunset schedule, the backlight levels, the scan duty, the access-point
credentials for export mode, the hostname and timeouts for over-the-air updates, and every pin.
Change values there rather than in the code that uses them.

---

## First-time setup (WiFi, contacts and SD)

The firmware keeps three independent WiFi credential slots on the SD card:

- **`/wifi.txt`**: primary network;
- **`/wifi_backup.txt`**: first fallback network;
- **`/wifi_backup2.txt`**: second fallback network, such as another phone hotspot.

Each file contains the SSID on line 1 and the password on line 2. The networks are used **only** to
sync the clock over Network Time Protocol (NTP) at boot; WiFi is then fully turned off so BLE runs
cleanly.

The SD card is wired to the ESP32, so you cannot drop a file on it from a computer without removing
the card. Two options:

1. **Provision from the ESP32** (no card reader needed). Flash `firmware/provision_wifi`, open a
   115200-baud serial monitor, choose `primary`, `backup`, `backup2`, `contacts` or `ota`, and
   enter the values at its prompts. A credential slot is replaced through a verified temporary
   file, tested against the network for up to 15 seconds, and never embedded in or printed by the
   sketch. Repeat it for the other slots, then flash `cyd_vitals`.
2. **Card reader.** Create the files directly on the card in the same two-line format.

Enable a fallback phone hotspot before resetting the monitor away from home; boot-time attempts are
intentionally finite and WiFi is never retried while live Bluetooth monitoring is running.

The timezone is **Europe/Warsaw** (CET/CEST with daylight saving); change `TZ_INFO` in `config.h`
for your region.

**Boot behaviour.** On a cold power-up without a valid time, the device tries the primary network
for up to 15 seconds and, once connected, NTP for up to 15 seconds. If that slot does not obtain the
time, it gives `backup`, then `backup2`, the same bounded attempt. Success causes one intentional
**soft reboot** into BLE-only mode, because WiFi and BLE coexistence severely throttles reception
and a WiFi shutdown does not fully undo it. If all three slots fail, monitoring starts without
clock-based CSV logging or night mode, and a later reset retries. The worst case before monitoring
starts is about 90 seconds when every network connects slowly or NTP times out.

---

## Using the device

**Live view.** Big heart rate and SpO₂ up top, skin temperature small next to the name, and a
one-hour BPM and SpO₂ sparkline (with yellow caution and red danger lines) along the bottom; time
and signal in the header. A value turns yellow past the (configurable, non-medical) warning
thresholds and a warning bar appears along the bottom. A heart rate shown in orange came from the
band's beat interval rather than its heart-rate byte, with both numbers noted under it (see the
alarm section below).

The header reports what the receiver can actually distinguish:

- **`scan`**: the 30-second scan-start grace period;
- the normal **`sig`** value: wristband packets are arriving;
- **`BAND / RANGE`**: other Bluetooth traffic is arriving, but the wristband is not;
- **`RADIO RETRY`**: all Bluetooth traffic stopped and the rate-limited scanner restart is active.

**24-hour charts.** Tap the **HEART** or **OXYGEN** column (number or sparkline) to open a
full-screen 0:00 to 24:00 plot of the **mean ± standard deviation** per time bin. Tap the plot to
cycle the bin size **1 h → 30 min → 15 min**; it **returns to the live view after 10 s** of no touch.
On boot the firmware reloads the day's CSV, so the charts survive a power cycle.

**Night mode.** The schedule follows **sunset and sunrise** for the configured location rather than
two fixed clock times, so it tracks the seasons without anyone editing constants twice a year. From
the night edge the backlight **fades over an hour** from full to about a tenth, and mirrors that
fade over the hour after the morning edge. The display also switches to a **dark theme**: the
background and the grey chrome (labels, name, clock, separators, chart grid) flip to their inverse,
while the HEART, OXYGEN and temperature colours stay as they are by day. The theme cannot fade, so
it flips in one step where the evening fade begins.

Pure solar times are wrong at the solstices (at this latitude the sun rises at 04:14 in late June),
so the solar result is **clamped**: dimming never starts before 19:00 or after 21:00, and
brightening never starts before 07:00. In practice sunset drives the evening edge through spring
and autumn and the clamp holds it at the solstices, while sunrise drives the morning edge for
roughly the winter half of the year.

The site coordinates, the three clamps, the two brightness levels and the fade length are in
`config.h`, as is `FORCE_NIGHT` for checking the dark theme without waiting for sunset. The chrome
palette is in `display.h`. Until the clock is set the display stays in the daytime look. No network
is involved: the solar maths runs on the device from the date and the timezone offset already in
effect, so daylight saving needs no special handling.

### Critical heart-rate alarm

Above the ordinary warning colours there is one alarm that takes the whole screen. It exists for a
**sustained** heart rate outside the safe band in either direction: at or above **200 bpm**, or at
or below **80 bpm**.

**What triggers it.** A reading at or above 200 starts a one-minute confirmation window. Any
reading below 180 during that minute cancels it; the rate came down and nothing happened. At the
end of the minute the alarm fires if the latest reading is at or above **190** *and* either two
readings reached 200, **or** the rate stopped falling. That second path matters: `205, 195, 185` is
a rate coming down and stays quiet, while `205, 185, 195` dipped and came back and does not.

**The slow side is the same machine with the thresholds read the other way up.** A reading at or
below **80** arms it, anything above **100** cancels it, and it fires if the last reading of the
minute is at or below **90** and either two readings reached 80 or the rate stopped climbing back.
One state machine serves both, so there is a single confirmation window, a single latch and a
single snooze to reason about. Two differences are deliberate:

- **A heart rate of zero is thrown away before it reaches the slow alarm.** An idle or charging
  wristband broadcasts zero every couple of seconds, and zero is *no reading*, not a very slow
  heart.
- **The implausible-collapse rule stays on the fast side only.** Its mirror image, a very slow
  rate jumping to a very fast one, has no such justification, and against a wristband that will
  manufacture numbers off bedding it would wake the house for an artifact.

**When the wristband contradicts itself, the recent past decides what is shown, and the fast alarm
hears the faster decode.** Both of the band's heart-rate decodes, its heart-rate byte and its beat
interval, intermittently lock onto every second beat, independently of each other: in 18 days of
logs the byte halved 368 times and the interval doubled 389 times. When the two disagree, the
screen, the plots, the log and the **slow** alarm take whichever candidate is nearer the median of
the last five minutes of agreeing readings (the byte, until there are three of them). The **fast**
alarm additionally hears the interval whenever it is the faster candidate, so a tachycardia that
starts abruptly while the byte halves is still counted. A number that came from the interval is one
step further from the sensor, so a window containing any such reading needs **three readings across
two minutes** rather than two across one, and such a reading can never trigger the collapse rule.
An episode confirmed this way is titled **HIGH RATE (beat)** and logged as `CONFIRMED_HIGH_BEAT`,
so it is always clear which decode raised it. See `docs/PROTOCOL.md`.

Dismissing silences both sides at once: one hold, one snooze.

Because the minute is counted rather than thrown away, the timer already reads `01:00` when the
alarm appears. If the rate oscillates either side of 200 the window restarts without cancelling, so
confirmation can take longer; the timer counts from the first crossing, so that delay is visible
rather than hidden.

Two things also raise it immediately, without waiting for the window:

- **the signal dies while armed**: silence after a reading at or above 200 is not a drop, and
  silence after one at or below 80 is not a recovery;
- **an implausible collapse**, a step from 170-plus to 60-or-less, which is either the heart-rate
  byte wrapping past 255 or a genuine emergency. The firmware cannot tell those apart and does not
  try: both alarm.

**What it shows.** Current rate, peak, oxygen **with its age**, elapsed time since onset, a
high-resolution trace of the last 10 minutes (widening to 30 and 60 to keep the onset on screen),
and your emergency phone numbers. Dropouts in the trace are drawn as shaded bands rather than a
line across the gap, because a line across four missing minutes reads as a steady rate for four
minutes. Only the border flashes, so the numbers stay readable.

**Clearing it.** Press and hold anywhere for **three seconds**, watching the countdown. It latches
until you do: no reading clears it, no dropout clears it, and it survives a reboot via RTC memory
and `/alerts.csv`. After a dismissal it stays quiet for ten minutes, then returns if the rate is
still high, so it cannot be silenced and forgotten. Swiping up to export is refused while it is up.

**Test it.** Hold the **HEART** cell for three seconds to run a one-minute `TEST` alarm: real
brightness, real flashing, no log entry. **Run it from where you actually sleep, with the lights as
they normally are.** It is the only way to find out whether the alarm reaches you.

**`/contacts.txt`** on the SD card holds the numbers: up to two lines of up to 30 characters,
rendered exactly as written, so numbers sharing a prefix can go on one line:

```
224 432 969 / 931 / 970 / 973
```

The repository ships no default and contains nobody's real numbers; a stranger who builds this must
not get someone else's hospital on their screen. Without the file the alarm still works, just
without numbers.

**`/alerts.csv`** records every episode, append-only, so a power cut mid-episode still leaves a
readable file:

```csv
timestamp,event,hr_bpm,spo2_pct,detail
2026-08-10 03:14:22,ONSET,214,94,CONFIRMED_HIGH
2026-08-10 03:19:41,RESOLVED,176,93,
2026-08-10 03:20:58,DISMISS,171,93,dismissed
2026-09-02 15:47:10,ONSET,221,96,CONFIRMED_HIGH_BEAT band=110 beat=271ms
```

`hr_bpm` here is the rate the alarm acted on, which for the fast alarm can be the beat interval's
rate even while the screen kept the band's byte. When that rate came from the interval, the detail
column keeps the band's own number and the interval behind it, so the episode can be checked later
against the daily log.

A six-minute episode that resolves on its own at three in the morning leaves a record worth showing
a clinician. Neither this file nor `/contacts.txt` is served by export mode, which whitelists
`/vitals_*.csv` only; pull the card to read them.

Thresholds and timings are the `HR_CRIT` / `HR_SUSTAIN` / `HR_CANCEL` block in `config.h`.

> **The alarm is light only.** The board has no speaker and does not use the network, so it reaches
> someone in the same room and nobody through a closed door. Whether it wakes anyone is a question
> about where the board sits, and no firmware setting changes that.
>
> **Silence is not an all-clear.** The wristband loses contact when the baby moves, and a distressed
> baby moves. The board can also brown out, fill its card, or hang. Any of these produce no alarm.
>
> **The rate can read low when it is high.** The heart rate arrives as a single byte and cannot
> express more than 255. The collapse rule above defends against a wrap but cannot rule it out.
>
> **This is not a medical device** and does not decide whether to seek care.

### Power draw

The board runs off 5 V USB and the on-board AMS1117 is a **linear** regulator, so roughly a third of
the input energy becomes heat before anything useful happens. That sets the floor; the two knobs
above it are the radio and the backlight.

| Setting | Why it is where it is |
|---|---|
| `SCAN_INTERVAL_MS 100` / `SCAN_WINDOW_MS 100` | Continuous scan. Not for more data (the ceiling is one new measurement per 14 to 20 s however hard the board listens) but because this runs from a powerbank, and many powerbanks switch themselves off when the load drops below a minimum. A receiver that never sleeps keeps the draw above that floor. It also buys frame-loss margin; see the measurements below. |
| `BRIGHT_DAY_LEVEL 255` | Full brightness by day. Backlight current tracks the PWM duty closely, so this is deliberate for the same powerbank reason as the scan duty. |
| `BRIGHT_NIGHT_LEVEL 26` | About a tenth of full: readable across a dark room at a glance without lighting it up. |
| `BRIGHT_RAMP_MIN 60` | The fade spans an hour at both edges. The loop refreshes the backlight every 10 s, so the 229 PWM levels are covered in 360 refreshes and no single step is more than one level, below the threshold where a change reads as a flicker. |

**Do not lower the scan window without re-measuring.** Reception degrades much faster than the duty
ratio suggests, because rendering and SD writes compete with the radio. Worst gap between decoded
advertisements, measured on this board with the full firmware running, over about 3 minutes each:

| Window / interval | Duty | Adverts per 30 s | Worst gap | Verdict |
|---|---|---|---|---|
| 100 / 100 | 100 % | about 51 | none seen | current setting |
| 500 / 1000 | 50 % | about 29 | **2.0 s** | acceptable if power is not a concern |
| 300 / 1000 | 30 % | about 15 | **13.0 s** | too close to `STALE_MS`; drops to `--` |

> Use a stable, regulated 5 V USB supply. The firmware adds no dummy workload for a power bank. If a
> battery supply is required, choose one whose output is explicitly documented as always-on at low
> current.
>
> **`/boot.log` on the SD card** records why each restart happened, so you can tell these apart
> without a current meter: `POWERON` records a cold boot or supply interruption but does not
> identify its cause; `BROWNOUT` means the ESP32 supply fell too low; `PANIC` and `TASK_WDT`
> indicate a firmware failure. The last column is the firmware slot that booted.

### Getting the data off the board

**Swipe up** on the live view and the board stops scanning, becomes its own WiFi access point and
serves the logged CSVs to a phone. The screen shows everything you need (network name, password and
address), so there is nothing to configure and nothing to remember. **Swipe down** to go back to
monitoring, or just wait: it returns on its own after 10 minutes.

Because the board *is* the network, this works anywhere: a doctor's office, a car, a basement. No
home WiFi, no phone hotspot, no internet, and the SD card never leaves the slot.

| | |
|---|---|
| Network | `BabyVitals` (WPA2, password `babyvitals`; both in `config.h`) |
| Address | `http://192.168.4.1` |
| Returns to monitoring | swipe down, or automatically after 10 min |

> **Monitoring is paused the whole time the access point is up**, and the screen says so in a
> warning bar. WiFi and BLE cannot share this radio without wrecking reception (the same reason
> the firmware reboots after its NTP sync), so export mode stops scanning outright rather than
> quietly degrading it. The swipe has to cross more than half the screen and be clearly vertical,
> so brushing the display while moving the board will not trigger it.

The page itself does the work; the ESP32 only ships bytes, so the charts can be interactive without
costing the board anything. Pick a range (7, 14 or 30 days, or everything) and it shows the median
and daily range for heart rate and SpO₂, a tappable day-by-day breakdown at a selectable resolution
(10 s to 15 min), per-day coverage, and time spent outside the thresholds. **Save to phone** bundles
it all into a single self-contained `.html` file (about 275 KB for a month) that opens later with no
board and no network, which is the version to show at an appointment.

![The report page: 30-day summary with median and daily range for heart rate and SpO₂](docs/images/report-overview.jpg)

Tap any day in either chart and it expands hour by hour, with the full table underneath:

![Single-day view: the selected day hour by hour, above the per-day detail table](docs/images/report-single-day.jpg)

> Screenshots use synthetic sample data, not a real baby's readings.

> The report states plainly that this is a home-built receiver rather than a medical device, and
> shows a **coverage** figure for every day. A wrist sensor drops out when the baby moves, so low
> readings are often motion artefacts; coverage is what tells you how much to trust a given day.

Only `/vitals_*.csv` files are ever served. That is a deliberate whitelist rather than a path
lookup, because `/wifi.txt` on the same card holds your **home** WiFi password in plain text and
anyone in the room can join the access point while it is up.

**CSV logs**: one file per day on the SD card, for example `/vitals_2026-08-07.csv`:

```csv
timestamp,hr_bpm,spo2_pct,skin_c,beat_ms,hr_eff
2026-08-07 15:59:21,134,96,,458,134
2026-08-07 15:59:38,143,96,35.0,419,143
2026-09-02 15:47:10,110,96,35.1,271,221
```

`hr_bpm` is the wristband's own heart-rate byte and `hr_eff` is the rate the board actually used;
the two differ on the third row, where the byte had halved and the beat interval was believed
instead. `beat_ms` is the evidence for that decision. Files written before these columns existed
have four columns and are still read correctly; the report page plots `hr_eff` where a row has one
and `hr_bpm` otherwise.

Pull the card any time to browse the full history on a computer. `tools/analyze.py` gives a quick
per-column summary of a raw capture.

### Updating the firmware over the air

Once a board runs a firmware built with the `no_fs` layout, later builds can be installed without
touching the USB cable. **Swipe down** on the live view and the board stops scanning, joins your home
network using the same three credential slots as the clock sync, and waits up to ten minutes for an
upload. The screen shows the network, the address and the hostname, and says in a warning bar that
monitoring is paused. **Swipe down** again, or let it time out, and it reboots into monitoring.

It accepts an upload only if **`/ota.txt`** on the SD card holds a password (one line, at least
eight characters; the `ota` choice in `provision_wifi` writes it). Without the file the screen says
so and nothing is accepted. From a computer on the same network:

```bash
arduino-cli compile -b esp32:esp32:esp32:PartitionScheme=no_fs --output-dir build firmware/cyd_vitals
python3 ~/Library/Arduino15/packages/esp32/hardware/esp32/3.3.11/tools/espota.py \
  -i <board address> -a '<password from /ota.txt>' -f build/cyd_vitals.ino.bin -r
```

`espota.py` ships with the ESP32 core (the path above is macOS; on Linux it is under
`~/.arduino15`). It is used directly rather than through `arduino-cli upload --protocol network`,
which only accepts boards it has discovered over mDNS and has no way to pass this core a password.
The board also answers to `babyvitals.local` on networks that pass mDNS. It connects back to your
computer to fetch the image, so macOS will ask once to allow incoming connections for Python. A progress bar tracks the transfer; when it completes the board reboots into
the new firmware, and on any error it shows why and reboots into the firmware it was already
running.

**Rollback.** A freshly installed firmware boots on probation and is confirmed only after its
Bluetooth scan has started and it has run for a minute. A crash or watchdog reset before that
makes the bootloader fall back to the previous firmware on the next start. `/boot.log` records
which of the two slots (`app0`, `app1`) each boot ran from, so a silent rollback is visible. A
deliberate reboot (leaving export or maintenance mode) confirms the firmware first, so it never
counts as a failure.

> Maintenance mode is refused while an alarm is up, for the same reason export mode is. Anyone on
> your home network who knows the password can install firmware on the board while the mode is
> active; the mode is entered by hand, lasts ten minutes, and the password never leaves the SD
> card.

---

## Mac tools (no extra hardware)

Built and run with the system Swift toolchain (macOS, CoreBluetooth):

```bash
swiftc -O mac/vitals_reader.swift -o vitals_reader
./vitals_reader               # live: HR | beat | SpO2 | skin | signal | RSSI

swiftc -O mac/passive_scan.swift -o passive_scan
./passive_scan 60             # 60 s raw advertisement survey (reverse engineering)
```

Both are strictly receive-only (scan, never connect). Grant the terminal Bluetooth permission in
*System Settings → Privacy & Security → Bluetooth* on first run.

---

## Repository layout

```
firmware/
  cyd_vitals/           the monitor firmware (see docs/ARCHITECTURE.md)
    cyd_vitals.ino        glue: readings, radio, clock, storage, export, views, setup/loop
    config.h              every tunable and every pin
    band_protocol.h       frame decoding and the heart-rate correction rules   (pure, tested)
    critical_alarm.h      confirmation state machine for both alarms          (pure, tested)
    day_night.h           solar dimming schedule and the backlight ramp        (pure, tested)
    history.h             the last hour of readings                            (pure, tested)
    day_bins.h            24 hours in fifteen-minute bins                      (pure, tested)
    gesture.h             tap and swipe recognition                            (pure, tested)
    hold_gesture.h        three-second hold with countdown                     (pure, tested)
    alert_log.h           /alerts.csv rows and boot reconciliation             (pure, tested)
    vitals_csv.h          daily log rows, written and read back                (pure, tested)
    contacts.h            /contacts.txt parsing                                (pure, tested)
    radio_health.h        what the receiver can say about its own reception    (pure, tested)
    wifi_failover.h       the three credential slots and their order           (pure, tested)
    trace_window.h        alarm trace windowing and dropout gaps               (pure, tested)
    live_render_key.h     change detection for the live view                   (pure, tested)
    display.h             panel driver, layout, palette
    live_render.h         the live view
    plot_render.h         the 24-hour chart
    export_render.h       the export-mode screen
    maintenance_render.h  the over-the-air update screen
    alarm_render.h        the alarm screen
    firmware_health.h     rollback protection for over-the-air updates
    ota_password.h        /ota.txt parsing                                     (pure, tested)
    app_html.h            the report page served in export mode
  reader_serial/        minimal ESP32 reader: decoded vitals over serial, no display
  bandsniff/            reception test: confirms a classic ESP32 can hear the wristband
  provision_wifi/       writes the WiFi slots, /contacts.txt and /ota.txt to the SD card over serial
mac/
  vitals_reader.swift   live decoded vitals on macOS
  passive_scan.swift    raw BLE advertisement survey
tests/
  run.sh                runs both native suites
  firmware_logic_test.cpp   the pure headers, natively
  report_html_test.mjs      the report page's data handling, in Node.js
tools/
  analyze.py            per-byte analysis of a raw capture
docs/
  PROTOCOL.md           the reverse-engineered BLE protocol and the evidence behind the rules
  ARCHITECTURE.md       how the firmware is organised and where a change belongs
  images/
```

Continuous integration runs the native tests and compiles every sketch for the ESP32 on each push
and pull request (`.github/workflows/tests.yml`).

---

## Contributing

**Code and documentation pull requests are paused.** Bug reports, protocol observations from your
own hardware, and build reports are very welcome; open an issue.

Once contributions reopen they will require agreement to the
[Contributor Licence Agreement](CLA.md) and a `Signed-off-by` line on every commit. You keep your
copyright; the agreement exists so the project's licence can still be changed later without hunting
down every past contributor. A DCO sign-off alone would not achieve that.

Please do not send optical geometry, wavelength handling, PPG pipeline design, sensor-fusion design,
schematics, or wearable mechanical design to this repository; that material is deliberately
unpublished. Read [`CONTRIBUTING.md`](CONTRIBUTING.md) before opening anything.

---

## License and trademarks

Code and documentation: [PolyForm Noncommercial 1.0.0](LICENSE). Provided as-is, with no warranty.
Not a medical device.

**Free for parents.** Build it, flash it, change it, share it, for your own family or for any other
noncommercial purpose. Hospitals, universities, charities and public research or health
organisations are covered explicitly. What the licence does not allow is selling this software or
devices running it, or building it into a paid product or service. If you want to do that, ask me.
See [`NOTICE`](NOTICE) for the details.

This is *source available*, not OSI open source: a deliberate choice, so the work stays open to the
people it was written for without being commercialised out from under them.

*Baby Sensor Relax* and any related names are trademarks of their respective owner and are used here
only for descriptive and compatibility purposes. This project is independent and unofficial.
