#!/usr/bin/env python3
"""Objective metrics for the deterministic Speex PLC harness.

The core metrics only need the Python standard library.  If the optional
``pesq`` package is installed, ITU-T P.862 PESQ is reported as well.
"""

import argparse
import array
import json
import math
import os
import statistics
import sys


def read_pcm(path):
    with open(path, "rb") as f:
        data = f.read()
    if len(data) % 2:
        raise ValueError(f"{path}: odd number of bytes")
    samples = array.array("h")
    samples.frombytes(data)
    if sys.byteorder != "little":
        samples.byteswap()
    return samples


def read_mask(path):
    with open(path, "rb") as f:
        return list(f.read())


def rms(samples, start, end):
    total = 0
    for x in samples[start:end]:
        total += x * x
    return math.sqrt(total / max(1, end - start))


def seg_snr(ref, deg, frame_size):
    values = []
    for start in range(0, min(len(ref), len(deg)) - frame_size + 1, frame_size):
        end = start + frame_size
        signal = sum(x * x for x in ref[start:end])
        if rms(ref, start, end) < 80.0:
            continue
        noise = sum((a - b) * (a - b) for a, b in zip(ref[start:end], deg[start:end]))
        snr = 10.0 * math.log10((signal + 1.0) / (noise + 1.0))
        values.append(max(-10.0, min(50.0, snr)))
    return statistics.fmean(values) if values else float("nan")


def energy_error_db(ref, deg, frame_size):
    errors = []
    for start in range(0, min(len(ref), len(deg)) - frame_size + 1, frame_size):
        end = start + frame_size
        ref_rms = rms(ref, start, end)
        if ref_rms < 80.0:
            continue
        deg_rms = max(1.0, rms(deg, start, end))
        errors.append(abs(20.0 * math.log10(deg_rms / ref_rms)))
    errors.sort()
    if not errors:
        return float("nan"), float("nan")
    p95_index = min(len(errors) - 1, int(math.ceil(0.95 * len(errors))) - 1)
    return statistics.median(errors), errors[p95_index]


def local_mad(values, index, radius):
    lo = max(0, index - radius)
    hi = min(len(values), index + radius + 1)
    sample = sorted(values[lo:hi])
    med = sample[len(sample) // 2]
    return statistics.median(abs(v - med) for v in sample)


def click_metrics(samples, mask, frame_size, pop_threshold):
    diffs = [0]
    diffs.extend(abs(samples[i] - samples[i - 1]) for i in range(1, len(samples)))
    boundary_jumps = []
    for frame in range(1, len(mask)):
        if mask[frame] != mask[frame - 1]:
            pos = frame * frame_size
            if pos < len(samples):
                boundary_jumps.append(diffs[pos])

    events = []
    last_event = -10**9
    radius = max(8, frame_size // 2)
    for i, value in enumerate(diffs):
        if value < pop_threshold or i - last_event < frame_size // 2:
            continue
        mad = local_mad(diffs, i, radius)
        if value > max(pop_threshold, 8.0 * (mad + 1.0)):
            events.append(i)
            last_event = i

    return {
        "boundary_count": len(boundary_jumps),
        "boundary_max_jump": max(boundary_jumps, default=0),
        "boundary_pops": sum(1 for x in boundary_jumps if x >= pop_threshold),
        "max_jump": max(diffs, default=0),
        "click_count": len(events),
        "click_threshold": pop_threshold,
    }


def pesq_score(ref, deg, fs):
    try:
        from pesq import pesq
    except Exception:
        return None
    if fs not in (8000, 16000):
        return None
    n = min(len(ref), len(deg))
    ref_f = [x / 32768.0 for x in ref[:n]]
    deg_f = [x / 32768.0 for x in deg[:n]]
    mode = "nb" if fs == 8000 else "wb"
    return float(pesq(fs, ref_f, deg_f, mode))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("reference")
    parser.add_argument("clean")
    parser.add_argument("lossy")
    parser.add_argument("mask")
    parser.add_argument("--fs", type=int, default=8000)
    parser.add_argument("--frame-size", type=int, default=160)
    parser.add_argument("--pop-threshold", type=int, default=6000)
    args = parser.parse_args()

    ref = read_pcm(args.reference)
    clean = read_pcm(args.clean)
    lossy = read_pcm(args.lossy)
    mask = read_mask(args.mask)
    energy_median, energy_p95 = energy_error_db(clean, lossy, args.frame_size)

    result = {
        "samples": len(lossy),
        "lost_frames": sum(mask),
        "loss_percent": 100.0 * sum(mask) / max(1, len(mask)),
        "seg_snr_db": seg_snr(clean, lossy, args.frame_size),
        "energy_mae_db_median": energy_median,
        "energy_mae_db_p95": energy_p95,
        "clean": click_metrics(clean, mask, args.frame_size, args.pop_threshold),
        "lossy": click_metrics(lossy, mask, args.frame_size, args.pop_threshold),
    }
    clean_pesq = pesq_score(ref, clean, args.fs)
    lossy_pesq = pesq_score(ref, lossy, args.fs)
    if clean_pesq is not None and lossy_pesq is not None:
        result["pesq_clean"] = clean_pesq
        result["pesq_lossy"] = lossy_pesq
        result["pesq_drop"] = clean_pesq - lossy_pesq

    print(json.dumps(result, indent=2, sort_keys=True))


if __name__ == "__main__":
    main()
