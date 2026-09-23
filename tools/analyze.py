#!/usr/bin/env python3
"""Per-byte analysis of a BLE manufacturer-data capture.

Reads a CSV with the columns ``t_ms,name,rssi,hex`` (one advertisement per row, as written by the
capture tools) and, for every device name, reports which byte positions are constant and which
change, their ranges, and which look like a counter or a vital sign. Receive-only data; nothing
here talks to a device.

Usage: tools/analyze.py [capture.csv]
"""

import collections
import csv
import sys


def load(path):
    """Return {device name: [(t_ms, rssi, payload bytes), ...]} in file order."""
    rows = collections.defaultdict(list)
    with open(path, newline="") as f:
        for row in csv.DictReader(f):
            try:
                payload = bytes.fromhex(row["hex"])
            except (KeyError, ValueError):
                continue
            rows[row["name"]].append((int(row["t_ms"]), int(row["rssi"]), payload))
    return rows


def describe_column(values, sample_count):
    """One line of classification for a single byte position across all samples."""
    unique = set(values)
    lo, hi = min(values), max(values)
    if len(unique) == 1:
        return f"CONST 0x{lo:02X} ({lo}d)"
    tag = f"VAR  range 0x{lo:02X}-0x{hi:02X} ({lo}-{hi}d) uniq={len(unique)}"
    # A counter steps by exactly one (modulo 256) between most consecutive samples.
    steps_of_one = sum(1 for a, b in zip(values, values[1:]) if (b - a) & 0xFF == 1)
    if steps_of_one >= 0.6 * (sample_count - 1):
        tag += "  [COUNTER +1]"
    elif hi - lo <= 12 and 40 <= lo <= 200:
        tag += "  [vital-like: small drift in a physiological range]"
    return tag


def report(name, samples):
    n = len(samples)
    length = len(samples[0][2])
    duration_s = (samples[-1][0] - samples[0][0]) / 1000.0
    print(f"\n===== {name}  ({n} adverts, {length}B payload) =====")
    print(f"span {duration_s:.1f}s, mean interval {duration_s / max(1, n - 1):.3f}s")
    for i in range(length):
        column = [s[2][i] for s in samples if i < len(s[2])]
        print(f"  b[{i:2d}] first=0x{column[0]:02X} last=0x{column[-1]:02X}  "
              f"{describe_column(column, n)}")
    print("  samples:")
    for j in sorted({0, n // 4, n // 2, 3 * n // 4, n - 1}):
        t, rssi, payload = samples[j]
        print(f"    t={t / 1000:6.1f}s rssi={rssi} {payload.hex().upper()}")


def main(argv):
    path = argv[1] if len(argv) > 1 else "capture.csv"
    for name, samples in load(path).items():
        if samples:
            report(name, samples)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
