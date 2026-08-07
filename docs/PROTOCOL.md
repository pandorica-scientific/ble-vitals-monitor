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
 11-12  16-bit     PPG / perfusion signal (pulse-derived)
 13     spo2       SpO2 (%)                                                            ★CONFIRMED
 14-15  16-bit     PPG / perfusion signal
 16-21  MAC        wristband MAC address, little-endian (e.g. 33 05 6C 4E CD F7)
 22     A4         checksum / constant
```

The base's advertisement is a **9-byte identity beacon** only
(`F5 04` + MAC little-endian + `01`) — no vitals.

## Decoding rules (verified against the official app)

- **Heart rate** = `byte[10]` (bpm). Verified: captured values matched the app's HR history
  exactly across the full range (118–148 bpm).
- **SpO2** = `byte[13]` (%). Verified against the app (96–99%).
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

- **Heart rate**: new value ~every 20 s (each bumps the byte-2 counter).
- **SpO2 / temperature**: the wristband broadcasts continuously, but the cloud/app only
  *commits* a stabilized value ~every 15 min, and skips a commit when the reading is invalid.

## Reception notes

- The wristband uses **BLE legacy advertising** with a small payload (flags + 23-byte
  manufacturer data + short name in the scan response). It fits the 31-byte legacy limit, so a
  classic **ESP32 (BLE 4.2)** can receive it — confirmed on an ESP32-D0WD-V3.
  (Note: a different device, the Owlet Dream Sock, uses BLE 5 *extended* advertising and needs
  a BLE 5 receiver; this one does not.)
- macOS CoreBluetooth does not expose peripheral MAC addresses, so on the Mac the device is
  matched by its advertised local name (`BS01`) and the `F5 03` manufacturer-data prefix.
