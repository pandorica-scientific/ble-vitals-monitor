# Baby Sensor Relax — BLE advertisement protocol

Reverse-engineered **passively** (receive-only) from a device I own, by observing the
Bluetooth Low Energy advertisements the wristband already broadcasts to the room. No
connection is ever made to the wristband or base, no pairing, no encryption is broken —
the data is transmitted in the clear.

## System overview

The product is a two-part system:

| Part | What it does | Identifiers (example unit) |
|------|--------------|----------------------------|
| **Base** (`BG01N` + `WIFI_V016`) | BLE receiver near the crib; relays to the cloud over WiFi/cellular | WiFi MAC `48:9D:24:…`, BLE MAC `CF:56:65:…` |
| **Wristband** (`BS01`) | Worn by the baby; measures the vitals | BLE MAC `F7:CD:4E:…` |

Data flow: **wristband → (BLE broadcast) → base → (WiFi/cellular) → cloud → app.**

The wristband is a **connectionless broadcaster**: it puts the current vitals into the
manufacturer-specific data field of its BLE advertisement, roughly every 1.5 s. This is why
a third party in radio range can read it passively without touching the base link.

Room temperature is measured by the **base**, not the wristband, and is **not** present in any
BLE advertisement — it is only available via the base's cloud upload.

## Advertisement frame (wristband, 23 bytes of manufacturer data)

```
offset  value      field
  0     F5         marker (constant)
  1     03         device type: 03 = wristband (base uses 04)
  2     seq        measurement counter, +1 per new reading (~every 20 s)
  3     state      normally 0x03; the packet that introduces a NEW temperature has bit 0x08 set
                   (0x0B). Treat 0x08 as a *candidate* "temperature updated" flag (unconfirmed).
  4     0..~77     signal-quality hint
  5     --         state/session-related
  6-7   u16 BE     SKIN TEMPERATURE:  skin_C = ((b6<<8)|b7) / 10   (0.1 C resolution)  ★CONFIRMED
  8-9   u16 BE     secondary thermal value (u16 BE / 10); meaning NOT confirmed — do not label
 10     hr         HEART RATE (bpm)                                                    ★CONFIRMED
 11-12  u16 BE     INTER-BEAT INTERVAL in milliseconds (beat_ms)                        ★CONFIRMED
 13     spo2       SpO2 (%)                                                            ★CONFIRMED
 14-15  16-bit     pulse-derived; meaning NOT confirmed — do not label
 16-21  MAC        wristband MAC address, little-endian (e.g. 33 05 6C 4E CD F7)
 22     A4         checksum / constant
```

The base's advertisement is a **9-byte identity beacon** only
(`F5 04` + MAC little-endian + `01`) — no vitals.

## Decoding rules (verified against the official app)

- **Heart rate** = `byte[10]` (bpm). Verified: captured values matched the app's HR history
  exactly across the full range (118–148 bpm).
- **SpO2** = `byte[13]` (%). Verified against the app (96–99%).
- **Inter-beat interval** = **big-endian `uint16` of bytes 11–12**, in **milliseconds**. So
  `bpm_from_interval = 60000 / beat_ms`. Established from a 60-minute passive capture
  (182 measurements, HR 112–167 bpm):
  - Fitting `beat_ms = k / hr` gives **k = 60208, bootstrap 95% CI [59699, 60716]**. That
    interval contains exactly **60000** (milliseconds) and **excludes 61440**, which rules out
    1/1024-second ticks — the only other unit that fit the 10-minute sample.
  - The two estimates agree on average: mean `byte[10]` = 133.10 bpm, mean `60000/beat_ms` =
    132.60 bpm. Mean disagreement −0.50 bpm, i.e. unbiased.
  - **It is not a restatement of `byte[10]`.** Across 32 heart-rate values that recurred during
    the capture, the interval was constant in **0 of 32** — median spread 52 ms, max 113 ms. A
    reported 123 bpm carried 16 distinct intervals from 435 to 520 ms.
  - The two disagree per-measurement (sd 8.7 bpm, 95% limits of agreement −17.6…+16.6 bpm)
    because they measure different things: averaging `60000/beat_ms` over a wider window moves it
    steadily toward `byte[10]` (rmse 8.74 → 5.52 → 4.60 → 3.80 bpm over 1/3/5/9 measurements).
    The consistent reading is that **`byte[10]` is a windowed average and bytes 11–12 are a
    recent single-beat interval** — so the pair carries beat-to-beat variability that the bpm
    byte alone does not.

  > A plain OLS fit of `beat_ms` on `60000/hr` returns a slope of 0.83, not 1.0. That is an
  > errors-in-variables artifact, not evidence against the interval reading: regressing the other
  > direction gives an implied slope of 1.40, the product of the two slopes is 0.59 (it would be
  > 1.0 with no scatter), and the geometric-mean slope is 1.08. Both one-directional fits are
  > attenuated by the scatter over a narrow x-range. The single-parameter scale fit above has no
  > intercept to trade against and is the estimator to trust.

- **Bytes 14–15**: pulse-derived and varying per measurement (143 distinct values in 182), but
  the meaning is **not** established. It is not an interval (treating it as one implies ~62 bpm
  against a true 133). It correlates only weakly and with everything at once — signal-quality
  byte +0.49, SpO2 +0.42, heart rate +0.43, bytes 11–12 −0.35 — which is what a perfusion or
  amplitude measure would look like, but that is a guess. Do not label it.
- **Skin temperature** = **big-endian `uint16` of bytes 6–7, divided by 10** (°C, 0.1° resolution).
  Verified against the app and independent captures:
  - `01 5D` = 0x015D = 349 → **34.9 °C**  (app shows 35)
  - `01 64` = 0x0164 = 356 → **35.6 °C**  (app shows 36)
  - `01 57` = 0x0157 = 343 → **34.3 °C**  (app shows 34)
  The app displays the whole-degree rounded value; keep the decimal internally.
- **No encryption**: everything is in the clear.

> **Decoding pitfall (learned the hard way):** temperature is a **16-bit** field. Reading only
> the *low* byte (byte 7 or byte 9) makes it look like an 8-bit value with a shifting offset,
> and it will not track the real temperature. Always combine the high byte (`byte 6`) with the
> low byte (`byte 7`). Byte 8–9 is a *second* 16-bit `/10` value that also changes at the
> temperature-update boundaries — its meaning is **not** established; do not label it (ambient,
> room, corrected, etc.) without more evidence.

### Temperature update timing

Skin temperature changes only **~every 15 minutes**; the exact same bytes 6–7 repeat across
all the intervening ~20 s packets. The packet that *introduces* a new temperature had
**`byte[3] == 0x0B`** (i.e. `0x03 | 0x08`) in every observed case, so `byte[3] & 0x08` is a
plausible **"new temperature measurement"** flag — treat it as a research candidate, not a
guarantee. Do **not** create a new temperature datapoint per packet just because a new
advertisement arrived; compare the raw value (and/or watch the flag) instead.

## Update cadence

- **Heart rate**: new value ~every 20 s (each bumps the byte-2 counter). Measured over a
  60-minute capture: median **20.1 s**, range 13.0–26.8 s.
- **SpO2 / temperature**: the wristband broadcasts continuously, but the cloud/app only
  *commits* a stabilized value ~every 15 min, and skips a commit when the reading is invalid.

### Nothing changes between counter bumps

Across 182 measurements (384 deduplicated frames, 60-minute capture), the number of sequences in
which **any** field changed while `byte[2]` held steady was **zero** — not heart rate, not the
beat interval, not SpO2, not the state or signal-quality bytes. Between ticks the band
re-broadcasts a byte-for-byte identical 23-byte payload roughly every 1.5 s.

The practical consequences, because this question keeps coming back:

- **Scanning harder does not buy more data.** A Mac capture scanning continuously — the 100%-duty
  case — measured a median inter-frame gap of **1.52 s**, which is simply the band's own
  advertising period. Receiving every single advertisement still yields one *new* measurement per
  ~20 s. A higher scan duty cycle buys margin against frame loss, nothing else.
- **Beat-level PPG is not recoverable.** One frame per ~1.5 s is an effective sample rate of
  **0.66 Hz**. An infant heart runs 2–3 Hz, so reconstructing a waveform would need >6 Hz. The
  beat interval in bytes 11–12 is a *value the band computed*, not a waveform we could derive.
- Deduplicate on `byte[2]` before logging or feeding an alarm. Treating repeated advertisements as
  repeated readings inflates the sample count ~13× with copies of one number, which will quietly
  break any logic sized against a per-measurement count.

## Reception notes

- The wristband uses **BLE legacy advertising** with a small payload (flags + 23-byte
  manufacturer data + short name in the scan response). It fits the 31-byte legacy limit, so a
  classic **ESP32 (BLE 4.2)** can receive it — confirmed on an ESP32-D0WD-V3.
  (Note: a different device, the Owlet Dream Sock, uses BLE 5 *extended* advertising and needs
  a BLE 5 receiver; this one does not.)
- macOS CoreBluetooth does not expose peripheral MAC addresses, so on the Mac the device is
  matched by its advertised local name (`BS01`) and the `F5 03` manufacturer-data prefix.
