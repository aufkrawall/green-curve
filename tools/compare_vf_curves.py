#!/usr/bin/env python3
"""Compare two VF curve JSON dumps (--json output) and report differences."""

import json
import sys
import os


def load_curve(path):
    with open(path, "r", encoding="utf-8") as f:
        return json.load(f)


def fmt_mhz(freq_khz):
    """Display MHz from kHz."""
    return f"{freq_khz} MHz"


def fmt_khz(val):
    return f"{val:+,d} kHz"


def main():
    if len(sys.argv) < 3:
        print(f"Usage: {sys.argv[0]} <baseline.json> <comparison.json>")
        sys.exit(1)

    a = load_curve(sys.argv[1])
    b = load_curve(sys.argv[2])

    print("=" * 72)
    print("VF CURVE COMPARISON REPORT")
    print("=" * 72)

    # Summary header
    print(f"\nBaseline   : {sys.argv[1]}")
    print(f"Comparison : {sys.argv[2]}")
    print(f"Baseline GPU   : {a.get('gpu', '?')}")
    print(f"Comparison GPU : {b.get('gpu', '?')}")
    print()

    # Global settings comparison
    print("--- GLOBAL SETTINGS ---")
    for key in ("gpu_offset_mhz", "mem_offset_mhz", "power_limit_pct", "fan"):
        va = a.get(key, "?")
        vb = b.get(key, "?")
        marker = " *DIFF*" if va != vb else ""
        print(f"  {key:25s}: baseline={str(va):>12s}  comparison={str(vb):>12s}{marker}")
    print()

    # Fans comparison
    afans = a.get("fans", [])
    bfans = b.get("fans", [])
    nfans = max(len(afans), len(bfans))
    print(f"--- FANS ({nfans} fans) ---")
    for i in range(nfans):
        fa = afans[i] if i < len(afans) else {}
        fb = bfans[i] if i < len(bfans) else {}
        for key in ("percent", "rpm", "policy", "signal", "target"):
            va = fa.get(key, "?")
            vb = fb.get(key, "?")
            marker = " *DIFF*" if va != vb else ""
            if marker:
                print(f"  fan[{i}].{key:10s}: baseline={str(va):>6s}  comparison={str(vb):>6s}{marker}")
    if not any(fa != fb for fa, fb in zip(afans, bfans)):
        print("  (all fans identical)")
    print()

    # VF curve comparison
    apoints = {p["index"]: p for p in a.get("points", [])}
    bpoints = {p["index"]: p for p in b.get("points", [])}

    print("--- VF CURVE POINT COMPARISON ---")
    print(f"  {'Idx':>4s}  {'Freq_a':>8s}  {'Freq_b':>8s}  {'Freq_d':>8s}  "
          f"{'Volt_a':>6s}  {'Volt_b':>6s}  {'Volt_d':>6s}  "
          f"{'Offs_a':>10s}  {'Offs_b':>10s}  {'Offs_d':>10s}")
    print("  " + "-" * 88)

    total_freq_drift = 0
    count_freq_diff = 0
    max_freq_drift = 0
    max_freq_drift_idx = -1

    for idx in range(128):
        pa = apoints.get(idx, {"freq_mhz": 0, "volt_mv": 0, "offset_khz": 0})
        pb = bpoints.get(idx, {"freq_mhz": 0, "volt_mv": 0, "offset_khz": 0})

        fa = pa.get("freq_mhz", 0)
        fb = pb.get("freq_mhz", 0)
        va = pa.get("volt_mv", 0)
        vb = pb.get("volt_mv", 0)
        oa = pa.get("offset_khz", 0)
        ob = pb.get("offset_khz", 0)

        fd = fb - fa
        vd = vb - va
        od = ob - oa

        marker = ""
        if abs(fd) >= 2:
            marker = " <--"
            total_freq_drift += abs(fd)
            count_freq_diff += 1
            if abs(fd) > max_freq_drift:
                max_freq_drift = abs(fd)
                max_freq_drift_idx = idx

        if marker or idx in (74, 75, 76) or (idx >= 76 and idx <= 126 and False):
            # Show all points with diffs, plus key anchor points
            print(f"  {idx:4d}  {fa:8d}  {fb:8d}  {fd:+8d}  "
                  f"{va:6d}  {vb:6d}  {vd:+6d}  "
                  f"{oa:+10d}  {ob:+10d}  {od:+10d}{marker}")

    print()
    print(f"--- TAIL ANALYSIS (points 76-126) ---")
    # Tail points
    tail_a = [apoints.get(i, {}).get("freq_mhz", 0) for i in range(76, 127)]
    tail_b = [bpoints.get(i, {}).get("freq_mhz", 0) for i in range(76, 127)]

    def tail_stats(tail, label):
        non_zero = [f for f in tail if f > 0]
        if not non_zero:
            print(f"  {label}: (empty)")
            return
        min_f = min(non_zero)
        max_f = max(non_zero)
        avg_f = sum(non_zero) / len(non_zero)
        lock_target = non_zero[0] if non_zero else 0
        drifted = [f for f in non_zero if abs(f - lock_target) > 2]
        print(f"  {label}:")
        print(f"    Lock target: {non_zero[0]} MHz (point 76)")
        print(f"    Range: {min_f} - {max_f} MHz (spread: {max_f - min_f} MHz)")
        print(f"    Average: {avg_f:.0f} MHz")
        print(f"    Drifted points: {len(drifted)}/{len(non_zero)} (>{'2'} MHz from target)")

    tail_stats(tail_a, "Baseline")
    tail_stats(tail_b, "Comparison")

    # Detect tail drift pattern
    if len(tail_a) == len(tail_b) == 51:
        drift_a = [tail_a[i] - tail_a[0] for i in range(len(tail_a))]
        drift_b = [tail_b[i] - tail_b[0] for i in range(len(tail_b))]
        print()

        # Check which has less drift
        avg_drift_a = sum(abs(d) for d in drift_a) / len(drift_a)
        avg_drift_b = sum(abs(d) for d in drift_b) / len(drift_b)
        print(f"  Average absolute tail drift from lock point:")
        print(f"    Baseline:   {avg_drift_a:.1f} MHz")
        print(f"    Comparison: {avg_drift_b:.1f} MHz")

        if avg_drift_b < avg_drift_a:
            print(f"  >>> Comparison has {avg_drift_a - avg_drift_b:.1f} MHz LESS tail drift (BETTER)")
        elif avg_drift_b > avg_drift_a:
            print(f"  >>> Comparison has {avg_drift_b - avg_drift_a:.1f} MHz MORE tail drift (WORSE)")
        else:
            print(f"  >>> Tail drift is equivalent")

    print()
    print("--- PRE-TAIL EDITS (points 74, 75) ---")
    for idx in (74, 75):
        pa = apoints.get(idx, {})
        pb = bpoints.get(idx, {})
        fa = pa.get("freq_mhz", 0)
        fb = pb.get("freq_mhz", 0)
        oa = pa.get("offset_khz", 0)
        ob = pb.get("offset_khz", 0)
        fd = fb - fa
        od = ob - oa
        print(f"  Point {idx}: baseline={fa} MHz (offset={oa:+d})  comparison={fb} MHz (offset={ob:+d})  d_freq={fd:+d}  d_offset={od:+d}")

    print()
    print("--- SUMMARY ---")
    print(f"  VF points with frequency diff >= 2 MHz: {count_freq_diff}/128")
    print(f"  Total absolute frequency drift: {total_freq_drift} MHz")
    if max_freq_drift_idx >= 0:
        print(f"  Max individual point drift: {max_freq_drift} MHz at index {max_freq_drift_idx}")
    print()

    # Monotonicity check
    print("--- MONOTONICITY CHECK ---")
    for label, points in [("Baseline", apoints), ("Comparison", bpoints)]:
        prev_freq = -1
        violations = []
        for idx in range(128):
            p = points.get(idx, {})
            freq = p.get("freq_mhz", 0)
            if freq > 0:
                if freq < prev_freq:
                    violations.append((idx, prev_freq, freq))
                prev_freq = freq
        if violations:
            print(f"  {label}: {len(violations)} non-monotonic transitions detected")
            for idx, prev, curr in violations[:5]:
                print(f"    Point {idx}: {curr} MHz < previous {prev} MHz")
        else:
            print(f"  {label}: Fully monotonic OK")

    print()
    print("=" * 72)


if __name__ == "__main__":
    main()
