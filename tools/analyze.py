#!/usr/bin/env python3
"""Per-byte analysis of BS01/BG01N manufacturer-data time series.
Reads capture.csv (t_ms,name,rssi,hex). Reports which byte columns are constant vs
changing, their ranges, and flags likely vitals. Receive-only data — no device contact."""
import csv, sys, collections

path = sys.argv[1] if len(sys.argv) > 1 else "capture.csv"
rows = collections.defaultdict(list)   # name -> list of (t_ms, rssi, bytes)
with open(path) as f:
    r = csv.DictReader(f)
    for row in r:
        try:
            b = bytes.fromhex(row["hex"])
        except Exception:
            continue
        rows[row["name"]].append((int(row["t_ms"]), int(row["rssi"]), b))

for name, samples in rows.items():
    if not samples:
        continue
    n = len(samples)
    length = len(samples[0][2])
    print(f"\n===== {name}  ({n} adverts, {length}B payload) =====")
    dur = (samples[-1][0] - samples[0][0]) / 1000.0
    print(f"span {dur:.1f}s, mean interval {dur/max(1,n-1):.3f}s")
    # per-byte column stats
    for i in range(length):
        col = [s[2][i] for s in samples if i < len(s[2])]
        vals = set(col)
        mn, mx = min(col), max(col)
        first, last = col[0], col[-1]
        is_const = len(vals) == 1
        # count monotonic +1 steps (counter signature)
        inc1 = sum(1 for a, b in zip(col, col[1:]) if (b - a) & 0xFF == 1)
        tag = ""
        if is_const:
            tag = f"CONST 0x{mn:02X} ({mn}d)"
        else:
            spread = mx - mn
            tag = f"VAR  range 0x{mn:02X}-0x{mx:02X} ({mn}-{mx}d) uniq={len(vals)}"
            if inc1 >= 0.6 * (n - 1):
                tag += "  [COUNTER +1]"
            elif spread <= 12 and 40 <= mn <= 200:
                tag += "  [<-- vital-like: small drift in physiological range]"
        print(f"  b[{i:2d}] first=0x{first:02X} last=0x{last:02X}  {tag}")
    # show a few raw frames spread across time
    print("  samples:")
    idxs = sorted(set([0, n//4, n//2, 3*n//4, n-1]))
    for j in idxs:
        t, rssi, b = samples[j]
        print(f"    t={t/1000:6.1f}s rssi={rssi} {b.hex().upper()}")
