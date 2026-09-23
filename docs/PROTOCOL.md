# Baby Sensor Relax: BLE advertisement protocol

Reverse-engineered **passively** (receive-only) from a device I own, by observing the Bluetooth Low
Energy advertisements the wristband already broadcasts to the room. No connection is ever made to
the wristband or base, no pairing, no encryption is broken; the data is transmitted in the clear.

Findings marked **CONFIRMED** were checked against the official app or against a controlled
experiment. Everything else is labelled as a candidate or unknown, and the firmware does not act on
it.

Contents:

1. [System overview](#1-system-overview)
2. [Advertisement frame](#2-advertisement-frame)
3. [Decoding rules](#3-decoding-rules)
4. [Update cadence](#4-update-cadence)
5. [Heart rate: when the band contradicts itself](#5-heart-rate-when-the-band-contradicts-itself)
6. [Bytes 14 to 15: an optical return level, not a perfusion index](#6-bytes-14-to-15-an-optical-return-level-not-a-perfusion-index)
7. [Idle and charging frame](#7-idle-and-charging-frame)
8. [Reception notes](#8-reception-notes)

## 1. System overview

The product is a two-part system:

| Part | What it does | Identifiers (example unit) |
|------|--------------|----------------------------|
| **Base** (`BG01N` + `WIFI_V016`) | BLE receiver near the crib; relays to the cloud over WiFi or cellular | WiFi MAC `48:9D:24:…`, BLE MAC `CF:56:65:…` |
| **Wristband** (`BS01`) | Worn by the baby; measures the vitals | BLE MAC `F7:CD:4E:…` |

Data flow: **wristband → (BLE broadcast) → base → (WiFi/cellular) → cloud → app.**

The wristband is a **connectionless broadcaster**: it puts the current vitals into the
manufacturer-specific data field of its BLE advertisement, roughly every 1.5 s. This is why a third
party in radio range can read it passively without touching the base link.

Room temperature is measured by the **base**, not the wristband, and is **not** present in any BLE
advertisement; it is only available via the base's cloud upload. The base's own advertisement is a
**9-byte identity beacon** (`F5 04` + MAC little-endian + `01`) with no vitals.

## 2. Advertisement frame

Wristband, 23 bytes of manufacturer data:

```
offset  value      field
  0     F5         marker (constant)
  1     03         device type: 03 = wristband (base uses 04)
  2     seq        measurement counter, +1 per new reading (~every 20 s)
  3     state      bit field. 0x03 = measuring; 0x00 = awake but no valid measurement;
                   0x80 = idle/charging (no measurement at all); +0x08 = new temperature
                   committed; +0x04 = new SpO2 committed.                        CONFIRMED
  4     0..~77     signal-quality hint
  5     --         constant 40 while worn, 60 on the charger: candidate battery percent
  6-7   u16 BE     SKIN TEMPERATURE:  skin_C = ((b6<<8)|b7) / 10   (0.1 C resolution)  CONFIRMED
  8-9   u16 BE     secondary thermal value (u16 BE / 10); meaning NOT confirmed
 10     hr         HEART RATE (bpm)                                                    CONFIRMED
 11-12  u16 BE     INTER-BEAT INTERVAL in milliseconds (beat_ms)                        CONFIRMED
 13     spo2       SpO2 (%)                                                            CONFIRMED
 14-15  u16 BE     optical return level, NOT a perfusion index (section 6)
 16-21  MAC        wristband MAC address, little-endian (e.g. 33 05 6C 4E CD F7)
 22     A4         checksum or constant
```

## 3. Decoding rules

### Heart rate: `byte[10]`, bpm. CONFIRMED

Captured values matched the app's heart-rate history exactly across the full range seen
(118 to 148 bpm). The byte is a **windowed average**, not a single beat (see the interval below).
It can be wrong in two specific ways, both handled by the firmware and described in section 5.

### SpO2: `byte[13]`, percent. CONFIRMED

Verified against the app (96 to 99 %).

### Inter-beat interval: big-endian `uint16` of bytes 11 to 12, milliseconds. CONFIRMED

So `bpm_from_interval = 60000 / beat_ms`. Established from a 60-minute passive capture
(182 measurements, heart rate 112 to 167 bpm):

- Fitting `beat_ms = k / hr` gives **k = 60208, bootstrap 95 % CI [59699, 60716]**. That interval
  contains exactly **60000** (milliseconds) and **excludes 61440**, which rules out 1/1024-second
  ticks, the only other unit that fit a shorter sample.
- The two estimates agree on average: mean `byte[10]` = 133.10 bpm, mean `60000/beat_ms` =
  132.60 bpm. Mean disagreement −0.50 bpm, so the interval is unbiased.
- **It is not a restatement of `byte[10]`.** Across 32 heart-rate values that recurred during the
  capture, the interval was constant in **0 of 32** (median spread 52 ms, maximum 113 ms). A
  reported 123 bpm carried 16 distinct intervals from 435 to 520 ms.
- The two disagree per measurement (sd 8.7 bpm, 95 % limits of agreement −17.6 to +16.6 bpm)
  because they measure different things: averaging `60000/beat_ms` over a wider window moves it
  steadily toward `byte[10]` (rmse 8.74 → 5.52 → 4.60 → 3.80 bpm over 1, 3, 5 and 9 measurements).
  The consistent reading is that **`byte[10]` is a windowed average and bytes 11 to 12 are a recent
  single-beat interval**, so the pair carries beat-to-beat variability the bpm byte alone does not.

> A plain least-squares fit of `beat_ms` on `60000/hr` returns a slope of 0.83, not 1.0. That is an
> errors-in-variables artifact, not evidence against the interval reading: regressing the other way
> gives an implied slope of 1.40, the product of the two slopes is 0.59 (it would be 1.0 with no
> scatter), and the geometric-mean slope is 1.08. Both one-directional fits are attenuated by the
> scatter over a narrow range. The single-parameter scale fit above has no intercept to trade
> against and is the estimator to trust.

### Skin temperature: big-endian `uint16` of bytes 6 to 7, divided by 10. CONFIRMED

Degrees Celsius at 0.1° resolution. Verified against the app and independent captures:

| bytes | value | app shows |
|---|---|---|
| `01 5D` | 0x015D = 349 → **34.9 °C** | 35 |
| `01 64` | 0x0164 = 356 → **35.6 °C** | 36 |
| `01 57` | 0x0157 = 343 → **34.3 °C** | 34 |

The app displays the whole-degree rounded value; keep the decimal internally.

> **Decoding pitfall.** The temperature is a **16-bit** field. Reading only the *low* byte (byte 7
> or byte 9) makes it look like an 8-bit value with a shifting offset, and it will not track the
> real temperature. Always combine the high byte with the low byte.

Off the body the band reads **28.1 °C**, which passes the firmware's plausibility gate of 28.0 °C.
An unworn band in a warm room can therefore log a "valid" skin temperature; the gate is a sanity
check, not a wear detector.

### Bytes 8 to 9: a second `uint16 / 10` thermal value. NOT confirmed

It changes at the same moments as the skin temperature. Its meaning (ambient, corrected, or
something else) is not established; do not label it.

### Byte 5: candidate battery percentage. NOT confirmed

Constant 40 across both worn captures and 60 on the charger. That is a two-point observation from
different days, and it did not move over five minutes of charging: a candidate, not a finding.

### Byte 3: state flags. CONFIRMED

`0x80` set means the band is not measuring (section 7). The packet that *introduces* a new
temperature has **`byte[3] & 0x08`** set, checked across three captures: the flag was set 6 times,
the temperature changed 6 times, and it never changed without the flag. The same test promotes
**`byte[3] & 0x04` as a new SpO2 commit**: 6 flags, and SpO2 never changed without one. The flag can
be set while the committed value repeats the previous one, so read it as "a value was committed",
not "the value differs".

### No encryption

Everything is in the clear.

## 4. Update cadence

- **Heart rate:** a new value about every 20 s, each bumping the byte-2 counter. Over a 60-minute
  capture: median **20.1 s**, range 13.0 to 26.8 s.
- **SpO2 and temperature:** the wristband broadcasts continuously, but the cloud and app only
  *commit* a stabilised value about every 15 min, and skip a commit when the reading is invalid.
  The exact same bytes 6 to 7 repeat across all the intervening packets, so do not create a new
  temperature data point per packet; compare the raw value or watch the flag.
- **The counter stalls rather than zeroing on contact loss.** Gaps of 79, 131 and 188 s against a
  20 s median: losing contact shows up as *no new measurement*, while the last frame keeps being
  rebroadcast. A receiver must treat frame age, not frame content, as the staleness signal.

### Nothing changes between counter bumps

Across 182 measurements (384 deduplicated frames, 60-minute capture), the number of sequences in
which **any** field changed while `byte[2]` held steady was **zero**: not the heart rate, not the
beat interval, not SpO2, not the state or signal-quality bytes. Between ticks the band rebroadcasts
a byte-for-byte identical payload roughly every 1.5 s.

The practical consequences:

- **Scanning harder does not buy more data.** A Mac capture scanning continuously (the 100 %-duty
  case) measured a median inter-frame gap of **1.52 s**, which is simply the band's own advertising
  period. Receiving every advertisement still yields one *new* measurement per ~20 s. A higher scan
  duty cycle buys margin against frame loss, nothing else.
- **Beat-level PPG is not recoverable.** One frame per ~1.5 s is an effective sample rate of
  **0.66 Hz**. An infant heart runs 2 to 3 Hz, so reconstructing a waveform would need more than
  6 Hz. The beat interval in bytes 11 to 12 is a *value the band computed*, not a waveform.
- **Deduplicate on `byte[2]`** before logging or feeding an alarm. Treating repeated advertisements
  as repeated readings inflates the sample count about 13× with copies of one number.

## 5. Heart rate: when the band contradicts itself

The most important findings in this document. Both concern the heart-rate byte, and both are
detectable because the band carries two independent decodes of the same quantity: `byte[10]` and
`60000 / beat_ms`.

### 5.1 The band invents a heart rate from bedding. CONFIRMED

Controlled run on 2026-08-26 (adult volunteer, 11.5 min; the sensor moved through charger, air,
hand, air, hand, a bright laptop screen, and a dark room against a mattress).

Left in a dark room facing a mattress, with **no pulse anywhere near it**, the band did not fall
silent. It produced three fresh measurements, about 20 s apart, reporting **90, 93 and 96 bpm**:
values a parent would read as a calmly sleeping baby.

They are detectably false, because the band contradicts itself. Byte 10 said 90, 93 and 96 bpm
while its own beat interval implied 136, 149 and 155 bpm:

| condition | byte 10 | 60000 / interval | disagreement |
|---|---|---|---|
| adult hand, settled | 68 to 69 | 71 to 74 | **−4 bpm** |
| fabric in the dark | 90 to 96 | 136 to 155 | **−54 bpm** |

Against the 60-minute infant capture, where the two decodes agree to a mean of +0.5 bpm with an sd
of 8.8 (95 % of readings inside ±17 bpm), **not one of 182 genuine readings disagreed by more than
40 bpm**. Every fabric reading did, by 46 to 59.

So there is a clean validity test with no false positives in the data available:

> **A reading is doubtful when `|byte10 − 60000/interval| > 40 bpm`.**
> Flags 0 of 182 genuine infant readings; catches all 3 fabric readings.
> At 30 bpm it would still only flag 1.1 % (2 of 182) of genuine readings.

This matters more than any labelling question: a band that has come off and is lying in the cot
reports a plausible, reassuring heart rate rather than an obvious fault. Silence is a visible
failure; an invented 93 bpm is not.

Implemented in `band_protocol.h` as `bandReadingDisagrees()`. The reading is never dropped; which
decode is shown is decided by the rule in 5.2, and whenever the two disagree both numbers stay on
the screen ("band X / beat Y" under the value, or "reading issue" when the interval is unusable),
because hiding a doubtful number would trade one silent failure for another.

### 5.2 Both decodes halve, independently. CONFIRMED

Bedding is not the only way byte 10 lies. Worn, on a real wrist, it intermittently locks onto every
second beat and reports half the true rate. A first pass over 33 days of raw logs (2026-08-07 to
2026-09-02, about 115,000 readings) found 70 such episodes, typically ~160 → ~80 bpm for 40 s to
5 min, and a highest reading of 191 in the whole period. From 2026-09-02 21:54 the board logged the
beat interval beside the byte, and 18 days of that (64,633 readings, to 2026-09-20) settle which
decode to believe when they disagree. The answer is **neither, on its own**:

| | |
|---|---|
| readings where the two decodes disagree by more than 40 bpm | **757** (1.2 %) |
| … with the interval the higher of the two: the byte had halved | 368 |
| … with the interval the lower of the two: the interval had doubled | 389 |
| longest run of a doubled interval | 43 readings, 14 min (2026-09-20 06:36: byte 148, interval 73) |
| a clean run of a halved byte | 17 readings, 6 min (2026-09-19 12:59: byte 83, interval 167) |

The interval is not stale when it fails; it keeps changing, at twice the true length, so no "stuck
value" test catches it. It also throws a single wild reading (280, 305 or 365 ms, that is 214, 197
or 164 bpm) about every fifteen minutes: across 230 single-reading disagreements the most common
spacing is exactly 15 min, and the SpO2 value changes at the same reading 22 % of the time against
1.6 % at a random reading. That is the band's scheduled SpO2 measurement disturbing its beat
detector.

What separates the two decodes in every case is the recent past. The one that has lost the beat
sits at half or double the rate of the last few minutes; the other is continuous with it. Replaying
four rules over the 18 days, scoring a disagreement as right when the chosen number lands within
25 bpm of the surrounding agreeing readings:

| rule | right | low-alarm runs | high-alarm runs |
|---|---|---|---|
| interval always wins (the firmware of 2026-09-02) | 22 % | 37 | 2 |
| nearer the last shown value | 78 % | 23 | 0 |
| **nearer the median of the last 5 min of agreeing readings** (`band_protocol.h`, replayed as the sketch feeds it) | **80 %** | **11** | **2** |
| raw byte, never correct | 70 % | 19 | 0 |

A "run" is three or more readings within 120 s at or below 80, or at or above 200: what would reach
an alarm's confirmation window. The first rule was worse than doing nothing: it put a false 60 to
76 bpm on the screen for 13 to 14 minutes at a stretch three times in two days. What defeats the
chosen rule is a long messy stretch where both decodes drop beats in turn, so that the context
itself fills with halved readings (2026-09-19 18:30 to 19:20 is the example, and most of the
residual 20 %).

**The rule, as implemented** (`resolveHeartRate()` in `band_protocol.h`): when the two disagree by
more than 40 bpm and the interval is inside 150 to 2000 ms, the candidate nearer the median of the
agreeing readings from the last five minutes wins (at least three of them, kept in `RateContext`).
A tie, or no reference yet, keeps the byte. A zero heart rate stays zero. The decision is made once
per measurement, not once per rebroadcast.

The chosen rate is what the screen, the plots, the daily log (`hr_eff`) and the **slow** alarm use.
When the interval is shown the number is orange with "band X / beat Y" under it; when the byte is
kept despite a disagreement the same note appears under an ordinary number.

**The fast alarm hears more** (`highAlarmHeartRate()`). Continuity has one blind spot: a
tachycardia that begins abruptly, which is how supraventricular tachycardia begins, while the byte
halves at the same moment. The halved byte is then the candidate nearer the recent past, and the
screen keeps it. So the fast alarm is fed the faster decode whenever the band contradicts itself,
flagged as corrected so that the longer confirmation window applies: **three critical readings
across 120 s** rather than two across 60 s, and never the implausible-collapse rule. The
fifteen-minute glitch arms a window that the next ordinary reading abandons. Over the 18 days this
input would have confirmed twice: 2026-09-16 20:07 (byte 78 to 95 against 205 to 250 bpm on the
interval for a minute, the previous five minutes at 166; plausibly real) and 2026-09-09 13:50 (byte
85 to 96 against 204 to 250 for two minutes, the previous five minutes at 125 and the next at 150;
doubtful). That is the accepted price of not missing a halved onset. An episode confirmed this way
is logged and titled `CONFIRMED_HIGH_BEAT`, so it is always clear which decode raised it. The slow
alarm never hears the interval: a doubled interval must not pose as bradycardia.

The daily CSV carries `beat_ms` and `hr_eff` alongside the raw `hr_bpm` for exactly this reason:
the decision has to remain auditable after the fact. Rows written by the 2026-09-02 firmware carry
the `hr_eff` of the interval-always-wins rule, false lows included.

## 6. Bytes 14 to 15: an optical return level, not a perfusion index

Pulse-derived and varying per measurement (143 distinct values in 182), but the meaning is **not** a
vital sign. It is not an interval (treating it as one implies ~62 bpm against a true 133). It
correlates only weakly and with everything at once: signal-quality byte +0.49, SpO2 +0.42, heart
rate +0.43, bytes 11 to 12 −0.35.

"Perfusion index" is the first guess anyone makes about a pulse-derived 16-bit field, so it was
tested properly. Re-analysed over the same 60-minute capture (182 measurements, deduplicated by the
byte-2 counter).

Reading it as **big-endian** is right: little-endian spreads the same data over 1028 to 65282,
big-endian gives a compact 686 to 1195. The **top nibble of byte 14 is always 0**, so the payload
is effectively **12-bit**, the natural width of an optical front end's converter, not of a
percentage.

What points *toward* perfusion:

- It drifts rather than jumps. Lag-1 autocorrelation +0.67, lag-5 +0.56; a 7-minute moving average
  still holds 56 % of its variance. Perfusion moves on that timescale.
- It rises with the signal-quality byte (+0.49 raw, +0.22 partial, p = 0.003).
- With the hour's drift removed from both series, it rises with **skin temperature** (+0.23,
  p = 0.003). Warmer periphery, more perfusion: the right sign.
- It is mostly its own quantity: heart rate, SpO2, beat interval, skin temperature and signal
  quality together explain only **R² = 0.30** of it.

What argues *against* it:

- **The dynamic range is far too narrow.** 686 to 1195 over an hour, a factor of **1.74**.
  Perfusion index is defined as AC/DC and published ranges span 0.02 to 20 %, three orders of
  magnitude; even a still subject moves several-fold.
- **The distribution is wrong.** Skew −0.27, kurtosis −0.32: symmetric, faintly left-leaning. A
  perfusion index is strongly right-skewed, near log-normal.
- **No scaling makes sense as a percentage.** ÷100 gives 6.9 to 12.0 % (a plausible band, but
  implausibly stable for a whole hour); ÷10 gives 73 to 117 %, which is not a perfusion index at
  all.
- It shows no relationship with beat-to-beat interval scatter (r = −0.05, p = 0.49), which a
  genuine pulse-amplitude measure would be expected to show.

**Settled by the controlled run of 2026-08-26.** Bytes 14 to 15 by what was actually under the
sensor:

| under the sensor | n | bytes 14 to 15 | |
|---|---|---|---|
| nothing (awake, no contact) | 4 | 391 to 423 | |
| charger, idle beacon | 2 | 439 to 456 | canned value |
| **adult hand, settled** | 2 | **603 to 653** | a real pulse, hr 68 to 69 |
| **fabric in the dark** | 3 | **712 to 757** | no pulse at all |
| infant wrist, 60 min | 182 | 686 to 1195 | |

**A perfusion index cannot read higher against a mattress than against living tissue.** Fabric in
the dark (712 to 757) outscores a settled adult hand with a genuine, internally consistent pulse
(603 to 653). That is fatal to the perfusion reading and it is not a marginal difference.

What the field behaves like is an **optical return level**: how much light comes back to the
detector. Pale fabric held close reflects more infrared than skin does, which is exactly the
observed ordering; bare air (~400) sits at the bottom, and the infant wrist, strapped tight, at the
top. It is also not a free-running light meter: through the 188-second stall while the sensor faced
a bright laptop screen the value never updated once, because **the field is only refreshed when the
band produces a measurement.**

Conclusion: a signal-strength number attached to each measurement attempt, useful at most as a
contact indicator, never to be presented as a vital sign. A perfusion index also cannot be *derived*
from the broadcast: it is the ratio of the pulsatile to the steady part of the optical signal, and
the advertisement carries neither (no waveform, one scalar per ~20 s).

## 7. Idle and charging frame. CONFIRMED

Off the wrist (on the charger, at least) the wristband still advertises, roughly every 2.1 s
rather than the ~1.5 s it uses while measuring, and the payload is static:

- `byte[3] == 0x80` (bit 7 set) instead of `0x03`. **Treat bit `0x80` as "not measuring".**
- `byte[2]` (measurement counter) frozen; it does not advance while idle.
- Heart rate, SpO2, beat interval, skin temperature and the signal-quality byte are all **0**.
- Bytes 14 to 15 hold `0x01B7` (439), see section 6.
- `byte[5]` reads **60** here, against a constant **40** across both worn captures (section 3).

A receiver must therefore not treat a zero heart rate as a reading. `band_protocol.h` decodes these
frames but leaves the vitals at zero, and both the alert mask and the critical-alarm state machine
guard on `hr > 0`, so an idle band cannot raise a low-heart-rate alarm.

## 8. Reception notes

- The wristband uses **BLE legacy advertising** with a small payload (flags + 23-byte manufacturer
  data + short name in the scan response). It fits the 31-byte legacy limit, so a classic **ESP32
  (BLE 4.2)** can receive it; confirmed on an ESP32-D0WD-V3. (A different device, the Owlet Dream
  Sock, uses BLE 5 *extended* advertising and needs a BLE 5 receiver; this one does not.)
- macOS CoreBluetooth does not expose peripheral MAC addresses, so on the Mac the device is matched
  by its advertised local name (`BS01`) and the `F5 03` manufacturer-data prefix.
- The firmware scans passively (no scan requests), so it never sees the scan response and matches
  on the manufacturer-data prefix alone. Everything it decodes is in the advertisement itself.
