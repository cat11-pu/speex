#!/usr/bin/env python3
"""Differential test runner for Speex fixed-point and floating-point builds.

The legal corpus contains 3000 encoded streams.  Each stream is decoded from the
same bytes by a fixed-point and a floating-point build.  The malformed corpus
contains 1000 independently mutated packets.  Results are written as JSON,
JSONL and CSV under --out-dir.
"""

from __future__ import annotations

import argparse
import collections
import dataclasses
import datetime as _dt
import json
import math
import os
import random
import shutil
import struct
import subprocess
import sys
import tempfile
from pathlib import Path
from typing import Dict, Iterable, List, Optional, Sequence, Tuple


CORPUS_MAGIC = b"SPDIFF1\0"
RESULT_MAGIC = b"SPDRES1\0"
PARAM_NAMES = (
    "id",
    "mode",
    "quality",
    "complexity",
    "vbr",
    "vbr_quality",
    "highpass",
    "signal",
    "amplitude",
    "frequency",
    "switch_every",
    "switch_kind",
    "frames",
    "seed",
    "enh",
)
PARAM_DISPLAY = {
    "mode": {0: "NB", 1: "WB", 2: "UWB"},
    "signal": {
        0: "silence",
        1: "sine",
        2: "noise",
        3: "sweep",
        4: "mixed",
    },
    "switch_kind": {
        0: "quality",
        1: "vbr",
        2: "complexity",
        3: "highpass",
        4: "quality+vbr",
        5: "quality-extremes",
    },
}
ERROR_BINS = (
    (0, 0),
    (1, 1),
    (2, 3),
    (4, 7),
    (8, 15),
    (16, 31),
    (32, 63),
    (64, 127),
    (128, 255),
    (256, 511),
    (512, 1 << 30),
)
RATE_BY_MODE = (8000, 16000, 32000)


@dataclasses.dataclass
class DecodedCase:
    case_id: int
    mode: int
    enh: int
    frames: List[Tuple[int, int, Tuple[int, ...]]]


@dataclasses.dataclass
class Comparison:
    decode_errors: List[dict]
    sample_error_count: int
    max_abs_error: int
    sample_count: int
    first_error: Optional[dict]
    abs_hist: collections.Counter


def log(message: str) -> None:
    print(f"[diff] {message}", flush=True)


def run_command(cmd: Sequence[str], timeout: int = 120) -> subprocess.CompletedProcess:
    env = os.environ.copy()
    proc = subprocess.run(
        list(cmd),
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        timeout=timeout,
        env=env,
    )
    if proc.returncode != 0:
        joined = " ".join(cmd)
        raise RuntimeError(
            f"command failed ({proc.returncode}): {joined}\n"
            f"stdout:\n{proc.stdout}\nstderr:\n{proc.stderr}"
        )
    return proc


def write_cases(path: Path, specs: Sequence[dict]) -> None:
    with path.open("w", encoding="ascii") as f:
        f.write("# id mode quality complexity vbr vbr_quality highpass signal "
                "amplitude frequency switch_every switch_kind frames seed enh\n")
        for spec in specs:
            f.write(" ".join(str(int(spec[name])) for name in PARAM_NAMES))
            f.write("\n")


def parse_corpus(path: Path) -> List[dict]:
    data = path.read_bytes()
    if data[:8] != CORPUS_MAGIC:
        raise ValueError(f"{path}: bad corpus magic")
    count = struct.unpack_from("<I", data, 8)[0]
    pos = 12
    cases = []
    for _ in range(count):
        case_id, mode, enh, frames = struct.unpack_from("<IBBH", data, pos)
        pos += 8
        packets = []
        for _ in range(frames):
            size = struct.unpack_from("<I", data, pos)[0]
            pos += 4
            packets.append(data[pos:pos + size])
            pos += size
        cases.append(
            {"id": case_id, "mode": mode, "enh": enh, "packets": packets}
        )
    if pos != len(data):
        raise ValueError(f"{path}: {len(data) - pos} trailing bytes")
    return cases


def write_corpus(path: Path, cases: Sequence[dict]) -> None:
    with path.open("wb") as f:
        f.write(CORPUS_MAGIC)
        f.write(struct.pack("<I", len(cases)))
        for case in cases:
            packets = list(case["packets"])
            f.write(
                struct.pack(
                    "<IBBH",
                    int(case["id"]),
                    int(case["mode"]),
                    int(case["enh"]),
                    len(packets),
                )
            )
            for packet in packets:
                f.write(struct.pack("<I", len(packet)))
                f.write(packet)


def parse_results(path: Path) -> Dict[int, DecodedCase]:
    data = path.read_bytes()
    if data[:8] != RESULT_MAGIC:
        raise ValueError(f"{path}: bad result magic")
    count = struct.unpack_from("<I", data, 8)[0]
    pos = 12
    cases: Dict[int, DecodedCase] = {}
    for _ in range(count):
        case_id, mode, enh, frames = struct.unpack_from("<IBBH", data, pos)
        pos += 8
        decoded_frames = []
        for expected_frame in range(frames):
            frame_id, frame_index, ret, overflow, _, _, n_samples = struct.unpack_from(
                "<IHibBHI", data, pos
            )
            pos += 18
            if frame_id != case_id or frame_index != expected_frame:
                raise ValueError(f"{path}: inconsistent frame record")
            samples = struct.unpack_from(f"<{n_samples}h", data, pos)
            pos += 2 * n_samples
            decoded_frames.append((ret, overflow, samples))
        cases[case_id] = DecodedCase(case_id, mode, enh, decoded_frames)
    if pos != len(data):
        raise ValueError(f"{path}: {len(data) - pos} trailing result bytes")
    return cases


def meson_build(root: Path, build_dir: Path, fixed: bool) -> None:
    expected = "fixed" if fixed else "float"
    marker = build_dir / "meson-private"
    args = [
        "meson",
        "setup",
        str(build_dir),
        f"-Dfixed-point={'enabled' if fixed else 'disabled'}",
        "-Dfloat-api=enabled",
        "-Dtools=disabled",
        "-Dtest-binaries=disabled",
        "-Dvbr=enabled",
        "--buildtype=release",
    ]
    if marker.exists():
        args.insert(2, "--reconfigure")
    run_command(args, timeout=180)
    run_command(["meson", "compile", "-C", str(build_dir)], timeout=180)
    log(f"{expected} build ready at {build_dir}")


def compile_tool(root: Path, build_dir: Path, out: Path) -> None:
    source = root / "tests" / "differential" / "speex_diff_tool.c"
    include_dirs = [
        root,
        root / "include",
        build_dir,
        build_dir / "include",
        build_dir / "include" / "speex",
    ]
    cmd = [
        os.environ.get("CC", "cc"),
        "-O2",
        "-Wall",
        "-Wextra",
        "-DHAVE_CONFIG_H=1",
    ]
    for include_dir in include_dirs:
        cmd.extend(["-I", str(include_dir)])
    cmd.extend(
        [
            str(source),
            "-o",
            str(out),
            "-L",
            str(build_dir / "libspeex"),
            f"-Wl,-rpath,{build_dir / 'libspeex'}",
            "-lspeex",
            "-lm",
        ]
    )
    run_command(cmd, timeout=180)


def ensure_tools(root: Path, build_fixed: Path, build_float: Path,
                 skip_build: bool) -> Tuple[Path, Path]:
    if not skip_build:
        meson_build(root, build_fixed, fixed=True)
        meson_build(root, build_float, fixed=False)
    fixed_tool = build_fixed / "speex_diff_tool"
    float_tool = build_float / "speex_diff_tool"
    if not skip_build or not fixed_tool.exists() or not float_tool.exists():
        compile_tool(root, build_fixed, fixed_tool)
        compile_tool(root, build_float, float_tool)
    fixed_info = json.loads(run_command([str(fixed_tool), "info"]).stdout)
    float_info = json.loads(run_command([str(float_tool), "info"]).stdout)
    if fixed_info.get("arithmetic") != "fixed":
        raise RuntimeError(f"{fixed_tool} is not a fixed-point build: {fixed_info}")
    if float_info.get("arithmetic") != "float":
        raise RuntimeError(f"{float_tool} is not a floating-point build: {float_info}")
    return fixed_tool, float_tool


def tool_for_producer(producer: int, fixed_tool: Path, float_tool: Path) -> Path:
    return fixed_tool if producer == 0 else float_tool


def generate_and_decode(specs: Sequence[dict], fixed_tool: Path, float_tool: Path,
                        work_dir: Path, tag: str,
                        timeout: int = 180) -> Tuple[Dict[int, DecodedCase], Dict[int, DecodedCase]]:
    """Generate specs with their selected encoder, then decode by both builds."""
    if not specs:
        return {}, {}
    grouped: Dict[int, List[dict]] = {0: [], 1: []}
    for spec in specs:
        grouped[int(spec.get("producer", 0))].append(spec)

    fixed_results: Dict[int, DecodedCase] = {}
    float_results: Dict[int, DecodedCase] = {}
    for producer, group in grouped.items():
        if not group:
            continue
        cases_path = work_dir / f"{tag}.p{producer}.cases.txt"
        corpus_path = work_dir / f"{tag}.p{producer}.corpus.bin"
        write_cases(cases_path, group)
        gen_tool = tool_for_producer(producer, fixed_tool, float_tool)
        run_command([str(gen_tool), "gen", str(cases_path), str(corpus_path)],
                    timeout=timeout)

        for name, decoder_tool, target in (
            ("fixed", fixed_tool, fixed_results),
            ("float", float_tool, float_results),
        ):
            result_path = work_dir / f"{tag}.p{producer}.{name}.result.bin"
            run_command(
                [str(decoder_tool), "decode", str(corpus_path), str(result_path)],
                timeout=timeout,
            )
            target.update(parse_results(result_path))
    return fixed_results, float_results


def make_legal_specs(total: int = 3000, seed: int = 3697) -> List[dict]:
    rng = random.Random(seed)
    specs = []
    for i in range(total):
        if i < total * 20 // 100:
            signal = 0
        elif i < total * 45 // 100:
            signal = 1
        elif i < total * 60 // 100:
            signal = 2
        elif i < total * 75 // 100:
            signal = 3
        else:
            signal = 4

        mode = i % 3
        quality = (i // 3) % 11
        complexity = (i // 33) % 11
        vbr = (i // 7) % 2
        highpass = (i // 11) % 2
        enh = i % 2
        producer = i % 2

        if signal == 0:
            amplitude = 0
            frequency = 440
        elif signal == 1:
            amplitude = (100, 500, 2000, 8000, 16000, 30000)[(i // 5) % 6]
            frequency = (220, 440, 1000, 2000, 3000, 4000)[(i // 13) % 6]
        elif signal == 2:
            amplitude = (100, 500, 2000, 8000, 16000)[(i // 5) % 5]
            frequency = 0
        elif signal == 3:
            amplitude = (500, 2000, 8000, 16000)[(i // 5) % 4]
            frequency = 400
        else:
            amplitude = (1000, 4000, 8000, 16000)[(i // 5) % 4]
            frequency = (440, 1000, 2000)[(i // 13) % 3]

        switch_every = 0
        switch_kind = 0
        if (i // 4) % 2 == 0:
            switch_every = (1, 2, 3, 4, 6)[(i // 9) % 5]
            switch_kind = (i // 17) % 6

        frame_choices = (4, 6, 8, 12, 16, 20, 24)
        frames = frame_choices[(i // 19) % len(frame_choices)]
        spec = {
            "id": i,
            "mode": mode,
            "quality": quality,
            "complexity": complexity,
            "vbr": vbr,
            "vbr_quality": rng.randrange(0, 11),
            "highpass": highpass,
            "signal": signal,
            "amplitude": amplitude,
            "frequency": frequency,
            "switch_every": switch_every,
            "switch_kind": switch_kind,
            "frames": frames,
            "seed": (i * 2654435761 + seed) & 0x7FFFFFFF,
            "enh": enh,
            "producer": producer,
        }
        specs.append(spec)
    return specs


def compare_decodes(fixed: DecodedCase, floating: DecodedCase) -> Comparison:
    decode_errors = []
    if len(fixed.frames) != len(floating.frames):
        decode_errors.append(
            {
                "kind": "frame_count",
                "fixed": len(fixed.frames),
                "float": len(floating.frames),
            }
        )
        return Comparison(decode_errors, 0, 0, 0, None, collections.Counter())

    hist: collections.Counter = collections.Counter()
    sample_count = 0
    sample_error_count = 0
    max_abs_error = 0
    first_error = None
    for frame_index, (fix_frame, flt_frame) in enumerate(zip(fixed.frames, floating.frames)):
        fix_ret, fix_overflow, fix_samples = fix_frame
        flt_ret, flt_overflow, flt_samples = flt_frame
        if fix_ret != flt_ret:
            decode_errors.append(
                {
                    "kind": "status_mismatch",
                    "frame": frame_index,
                    "fixed_ret": fix_ret,
                    "float_ret": flt_ret,
                }
            )
        if fix_ret != 0 or flt_ret != 0:
            decode_errors.append(
                {
                    "kind": "decode_error",
                    "frame": frame_index,
                    "fixed_ret": fix_ret,
                    "float_ret": flt_ret,
                }
            )
            continue
        if fix_overflow or flt_overflow:
            decode_errors.append(
                {
                    "kind": "overflow",
                    "frame": frame_index,
                    "fixed_overflow": fix_overflow,
                    "float_overflow": flt_overflow,
                }
            )
        if len(fix_samples) != len(flt_samples):
            decode_errors.append(
                {
                    "kind": "sample_count",
                    "frame": frame_index,
                    "fixed": len(fix_samples),
                    "float": len(flt_samples),
                }
            )
            continue
        for sample_index, (fix_sample, flt_sample) in enumerate(
            zip(fix_samples, flt_samples)
        ):
            diff = abs(fix_sample - flt_sample)
            sample_count += 1
            hist[diff] += 1
            if diff > 1:
                sample_error_count += 1
                if first_error is None:
                    first_error = {
                        "frame": frame_index,
                        "sample": sample_index,
                        "fixed": fix_sample,
                        "float": flt_sample,
                        "abs_error": diff,
                    }
                if diff > max_abs_error:
                    max_abs_error = diff
            elif diff > max_abs_error:
                max_abs_error = diff
    return Comparison(
        decode_errors,
        sample_error_count,
        max_abs_error,
        sample_count,
        first_error,
        hist,
    )


def nonzero_summary(samples: Sequence[int]) -> dict:
    nonzero = 0
    first = None
    max_abs = 0
    for index, value in enumerate(samples):
        if value:
            nonzero += 1
            max_abs = max(max_abs, abs(value))
            if first is None:
                first = {"sample": index, "value": value}
    return {"nonzero": nonzero, "first": first, "max_abs": max_abs}


def click_events(spec: dict, samples: Sequence[int]) -> List[dict]:
    if spec["switch_every"] <= 0 or spec["signal"] not in (1, 3):
        return []
    total = len(samples)
    if total < 8:
        return []
    frame_size = total // int(spec["frames"])
    if frame_size < 2:
        return []
    events = []
    for switch_index in range(
        int(spec["switch_every"]), int(spec["frames"]), int(spec["switch_every"])
    ):
        boundary = switch_index * frame_size
        if boundary < 4 or boundary + 4 >= total:
            continue
        lo = max(2, boundary - frame_size)
        hi = min(total - 1, boundary + frame_size)
        second_diff = []
        for pos in range(lo, hi):
            second_diff.append(abs(samples[pos] - 2 * samples[pos - 1] + samples[pos - 2]))
        if not second_diff:
            continue
        ordered = sorted(second_diff)
        median = ordered[len(ordered) // 2]
        local = []
        for pos in range(max(2, boundary - 3), min(total - 1, boundary + 4)):
            local.append(abs(samples[pos] - 2 * samples[pos - 1] + samples[pos - 2]))
        peak = max(local) if local else 0
        threshold = max(256, 8 * median + 64)
        if peak > threshold:
            events.append(
                {
                    "switch_index": switch_index,
                    "sample": boundary,
                    "peak_second_diff": peak,
                    "median_second_diff": median,
                    "threshold": threshold,
                }
            )
    return events


def all_samples(decoded: DecodedCase) -> Tuple[int, ...]:
    values: List[int] = []
    for ret, _, samples in decoded.frames:
        if ret == 0:
            values.extend(samples)
    return tuple(values)


def case_params(spec: dict) -> dict:
    params = {name: int(spec[name]) for name in PARAM_NAMES}
    params["producer"] = "fixed" if int(spec.get("producer", 0)) == 0 else "float"
    return params


def evaluate_specs(specs: Sequence[dict], fixed_tool: Path, float_tool: Path,
                   work_dir: Path, tag: str) -> Dict[int, Comparison]:
    fixed_results, float_results = generate_and_decode(
        specs, fixed_tool, float_tool, work_dir, tag
    )
    by_id = {int(spec["id"]): spec for spec in specs}
    comparisons: Dict[int, Comparison] = {}
    for case_id, spec in by_id.items():
        fixed = fixed_results.get(case_id)
        floating = float_results.get(case_id)
        if fixed is None or floating is None:
            raise RuntimeError(f"missing decode result for case {case_id}")
        comparisons[case_id] = compare_decodes(fixed, floating)
    return comparisons


def quantile_from_hist(hist: collections.Counter, total: int, q: float) -> int:
    if total <= 0:
        return 0
    target = max(1, int(math.ceil(q * total)))
    cumulative = 0
    for value in sorted(hist):
        cumulative += hist[value]
        if cumulative >= target:
            return value
    return max(hist) if hist else 0


def histogram_bins(hist: collections.Counter, total: int) -> List[dict]:
    bins = []
    for lo, hi in ERROR_BINS:
        if hi >= 1 << 29:
            count = sum(value_count for value, value_count in hist.items() if value >= lo)
        else:
            count = sum(hist[value] for value in range(lo, hi + 1))
        bins.append(
            {
                "lo": lo,
                "hi": None if hi >= 1 << 29 else hi,
                "count": count,
                "percent": (100.0 * count / total) if total else 0.0,
            }
        )
    return bins


def dimension_candidates(spec: dict, dimension: str) -> List[int]:
    current = int(spec[dimension])
    if dimension == "frames":
        return list(range(1, current))
    if dimension == "switch_every":
        return list(range(0, current))
    if dimension == "switch_kind":
        return list(range(0, current))
    if dimension == "signal":
        return [value for value in range(0, current)]
    if dimension == "amplitude":
        values = [0, 64, 128, 256, 512, 1000, 2000, 4000, 8000, 12000, 16000, 24000, 30000]
        return [value for value in values if value < current]
    if dimension == "frequency":
        priority = [440, 220, 1000, 2000, 3000, 4000, 6000, 8000, 12000]
        return [value for value in priority if value != current]
    if dimension == "mode":
        return list(range(0, current))
    if dimension == "quality":
        return list(range(0, current))
    if dimension == "complexity":
        return list(range(0, current))
    if dimension == "vbr":
        return [0] if current == 1 else []
    if dimension == "vbr_quality":
        return list(range(0, current))
    if dimension == "highpass":
        return [0] if current == 1 else []
    if dimension == "enh":
        return [0] if current == 1 else []
    if dimension == "seed":
        return [0]
    if dimension == "producer":
        return [0] if current == 1 else []
    return []


def case_rank(spec: dict) -> tuple:
    frequency_priority = [440, 220, 1000, 2000, 3000, 4000, 6000, 8000, 12000]
    frequency = int(spec["frequency"])
    frequency_rank = (
        frequency_priority.index(frequency)
        if frequency in frequency_priority
        else len(frequency_priority) + frequency
    )
    return (
        int(spec["frames"]),
        int(spec["switch_every"]),
        int(spec["switch_kind"]),
        int(spec["signal"]),
        int(spec["amplitude"]),
        frequency_rank,
        int(spec["mode"]),
        int(spec["quality"]),
        int(spec["complexity"]),
        int(spec["vbr"]),
        int(spec["vbr_quality"]),
        int(spec["highpass"]),
        int(spec["enh"]),
        int(spec["seed"]),
        int(spec.get("producer", 0)),
    )


def shrink_cases(failing: Sequence[dict], fixed_tool: Path, float_tool: Path,
                 work_dir: Path) -> Dict[int, dict]:
    """Batch coordinate-descent minimisation of failing case parameters."""
    if not failing:
        return {}
    current = {int(spec["id"]): dict(spec) for spec in failing}
    dimensions = (
        "frames",
        "switch_every",
        "switch_kind",
        "signal",
        "amplitude",
        "frequency",
        "mode",
        "quality",
        "complexity",
        "vbr",
        "vbr_quality",
        "highpass",
        "enh",
        "seed",
        "producer",
    )
    next_temp_id = 1000000
    round_no = 0
    while True:
        changed_any = False
        round_no += 1
        for dimension in dimensions:
            candidate_map: Dict[int, List[Tuple[int, dict]]] = {}
            candidates: List[dict] = []
            for parent_id, spec in sorted(current.items()):
                values = dimension_candidates(spec, dimension)
                if not values:
                    continue
                entries = []
                for value in values:
                    candidate = dict(spec)
                    candidate[dimension] = value
                    if case_rank(candidate) >= case_rank(spec):
                        continue
                    candidate["id"] = next_temp_id
                    next_temp_id += 1
                    entries.append((candidate["id"], candidate))
                    candidates.append(candidate)
                candidate_map[parent_id] = entries
            if not candidates:
                continue
            comparisons = evaluate_specs(
                candidates, fixed_tool, float_tool, work_dir,
                f"shrink-r{round_no}-{dimension}",
            )
            for parent_id, entries in candidate_map.items():
                chosen = None
                for candidate_id, candidate in entries:
                    if comparisons[candidate_id].sample_error_count > 0:
                        chosen = candidate
                        break
                if chosen is not None:
                    current[parent_id] = chosen
                    changed_any = True
        log(
            f"shrink round {round_no}: {len(current)} cases, "
            f"{'changed' if changed_any else 'stable'}"
        )
        if not changed_any:
            break

    for case_id, spec in list(current.items()):
        spec["id"] = case_id
    return current


def mutate_packets(legal_specs: Sequence[dict], legal_corpus: Sequence[dict],
                   total: int, seed: int) -> List[dict]:
    rng = random.Random(seed)
    specs_by_id = {int(spec["id"]): spec for spec in legal_specs}
    candidates = [
        case for case in legal_corpus
        if case["packets"] and len(case["packets"][0]) >= 4
    ]
    malformed = []
    for i in range(total):
        source = candidates[rng.randrange(len(candidates))]
        source_id = int(source["id"])
        packet = bytearray(source["packets"][rng.randrange(len(source["packets"]))])
        mutation_class = i % 10
        description = ""
        invalid_class = "mutated_legal"
        guaranteed_invalid = False
        if i < total * 50 // 100:
            # m=9..12 in a narrowband block is rejected by nb_decode as corrupt.
            mode = 9 + rng.randrange(0, 4)
            packet = bytearray([(mode << 3) | rng.randrange(0, 8)])
            description = f"invalid-nb-submode:{mode}"
            invalid_class = "invalid_submode"
            guaranteed_invalid = True
        elif i < total * 75 // 100:
            # A wideband block with skip-table id 5..7 is rejected as corrupt.
            mode = 5 + rng.randrange(0, 3)
            packet = bytearray([0x80 | (mode << 3) | rng.randrange(0, 8)])
            description = f"invalid-wb-skip:{mode}"
            invalid_class = "invalid_wideband_skip"
            guaranteed_invalid = True
        elif i < total * 90 // 100:
            packet = bytearray()
            description = "empty-packet"
            invalid_class = "empty_packet"
            guaranteed_invalid = True
        elif mutation_class < 3:  # truncation
            new_len = rng.randrange(0, len(packet) + 1)
            packet = packet[:new_len]
            description = f"truncate:{new_len}/{len(source['packets'][0])}"
        elif mutation_class < 6:  # bit flips
            flips = rng.randrange(1, 9)
            positions = []
            for _ in range(flips):
                bit = rng.randrange(0, max(1, len(packet) * 8))
                byte_index = bit // 8
                bit_index = bit % 8
                if byte_index < len(packet):
                    packet[byte_index] ^= 1 << bit_index
                    positions.append(bit)
            description = f"bitflip:{positions}"
        elif mutation_class == 6:  # random bytes
            packet = bytearray(rng.randrange(0, 128) for _ in range(rng.randrange(0, 96)))
            description = f"random:{len(packet)}"
        elif mutation_class == 7:  # delete span
            if packet:
                start = rng.randrange(0, len(packet))
                end = min(len(packet), start + rng.randrange(1, 17))
                del packet[start:end]
                description = f"delete:{start}:{end}"
            else:
                description = "empty-source"
        elif mutation_class == 8:  # append garbage
            garbage = bytes(rng.randrange(0, 256) for _ in range(rng.randrange(1, 33)))
            packet.extend(garbage)
            description = f"append:{len(garbage)}"
        else:  # structural corruption of the first byte
            if packet:
                packet[0] = rng.randrange(0, 256)
            description = "first-byte-corrupt"

        mode = source["mode"] if i % 3 else rng.randrange(0, 3)
        enh = source["enh"] if i % 2 else rng.randrange(0, 2)
        malformed.append(
            {
                "id": 3000 + i,
                "mode": mode,
                "enh": enh,
                "packets": [bytes(packet)],
                "source_id": source_id,
                "mutation": description,
                "invalid_class": invalid_class,
                "guaranteed_invalid": guaranteed_invalid,
                "source_params": case_params(specs_by_id[source_id]),
            }
        )
    return malformed


def decode_corpus_pair(corpus_path: Path, fixed_tool: Path, float_tool: Path,
                       work_dir: Path, tag: str) -> Tuple[Dict[int, DecodedCase], Dict[int, DecodedCase]]:
    results = {}
    for name, tool in (("fixed", fixed_tool), ("float", float_tool)):
        out = work_dir / f"{tag}.{name}.result.bin"
        run_command([str(tool), "decode", str(corpus_path), str(out)], timeout=300)
        results[name] = parse_results(out)
    return results["fixed"], results["float"]


def decode_malformed(malformed: Sequence[dict], fixed_tool: Path, float_tool: Path,
                     work_dir: Path) -> Dict[int, dict]:
    """Decode malformed cases in chunks and isolate any crashing input."""
    output: Dict[int, dict] = {}
    chunk_size = 100
    for base in range(0, len(malformed), chunk_size):
        chunk = malformed[base:base + chunk_size]
        corpus = work_dir / f"malformed-{base}.corpus.bin"
        write_corpus(corpus, chunk)
        try:
            fixed, floating = decode_corpus_pair(
                corpus, fixed_tool, float_tool, work_dir, f"malformed-{base}"
            )
            for case in chunk:
                case_id = int(case["id"])
                if case_id not in fixed or case_id not in floating:
                    raise RuntimeError(f"missing malformed result {case_id}")
                output[case_id] = {
                    "fixed": fixed[case_id],
                    "float": floating[case_id],
                    "crash": None,
                }
        except Exception as exc:  # isolate a crashing case
            log(f"malformed chunk {base} failed, isolating: {exc}")
            for case in chunk:
                case_id = int(case["id"])
                one_corpus = work_dir / f"malformed-{case_id}.corpus.bin"
                write_corpus(one_corpus, [case])
                try:
                    fixed, floating = decode_corpus_pair(
                        one_corpus, fixed_tool, float_tool, work_dir,
                        f"malformed-{case_id}",
                    )
                    output[case_id] = {
                        "fixed": fixed[case_id],
                        "float": floating[case_id],
                        "crash": None,
                    }
                except Exception as isolated_exc:
                    output[case_id] = {
                        "fixed": None,
                        "float": None,
                        "crash": str(isolated_exc),
                    }
    return output


def verdict(ret: int, overflow: int) -> str:
    if ret == 0 and not overflow:
        return "accept"
    if ret == -2:
        return "corrupt"
    if ret == -1:
        return "truncated"
    if ret < 0:
        return f"reject({ret})"
    return f"accept+overflow({ret})"


def severity(ret: int, overflow: int) -> int:
    if ret >= 0 and not overflow:
        return 0
    if ret < 0:
        return -ret
    return 1


def analyze_malformed(malformed: Sequence[dict], decoded: Dict[int, dict]) -> dict:
    counts = collections.Counter()
    examples = []
    for case in malformed:
        case_id = int(case["id"])
        item = decoded[case_id]
        if item["crash"]:
            counts["crash"] += 1
            examples.append(
                {
                    "kind": "malformed_crash",
                    "case_id": case_id,
                    "mutation": case["mutation"],
                    "source_id": case["source_id"],
                    "error": item["crash"],
                }
            )
            continue
        fixed = item["fixed"].frames[0]
        floating = item["float"].frames[0]
        fix_ret, fix_overflow = fixed[0], fixed[1]
        flt_ret, flt_overflow = floating[0], floating[1]
        fix_verdict = verdict(fix_ret, fix_overflow)
        flt_verdict = verdict(flt_ret, flt_overflow)
        if fix_ret == flt_ret and fix_overflow == flt_overflow:
            counts["exact_match"] += 1
        elif fix_ret == flt_ret:
            counts["same_status_overflow_diff"] += 1
        elif fix_ret < flt_ret:
            counts["fixed_stricter"] += 1
        else:
            counts["float_stricter"] += 1

        if fix_ret >= 0 and flt_ret < 0:
            counts["fixed_accepts_float_rejects"] += 1
        elif fix_ret < 0 and flt_ret >= 0:
            counts["float_accepts_fixed_rejects"] += 1
        elif fix_ret < 0 and flt_ret < 0 and fix_ret != flt_ret:
            counts["different_rejection"] += 1
        elif fix_ret >= 0 and flt_ret >= 0:
            counts["both_accept"] += 1
        if fix_ret < 0 and flt_ret < 0:
            counts["both_reject"] += 1
        if case.get("guaranteed_invalid"):
            counts["guaranteed_invalid"] += 1
            if fix_ret < 0 and flt_ret < 0:
                counts["guaranteed_invalid_rejected_both"] += 1
            elif fix_ret >= 0 and flt_ret >= 0:
                counts["guaranteed_invalid_accepted_both"] += 1

        if severity(fix_ret, fix_overflow) > severity(flt_ret, flt_overflow):
            counts["fixed_overflow_stricter"] += 1
        if severity(flt_ret, flt_overflow) > severity(fix_ret, fix_overflow):
            counts["float_overflow_stricter"] += 1

        mismatch = (
            (fix_ret >= 0 and flt_ret < 0)
            or (fix_ret < 0 and flt_ret >= 0)
            or fix_overflow != flt_overflow
        )
        if mismatch:
            examples.append(
                {
                    "kind": "malformed_verdict_mismatch",
                    "case_id": case_id,
                    "mutation": case["mutation"],
                    "source_id": case["source_id"],
                    "mode": case["mode"],
                    "enh": case["enh"],
                    "invalid_class": case["invalid_class"],
                    "guaranteed_invalid": case["guaranteed_invalid"],
                    "fixed_ret": fix_ret,
                    "float_ret": flt_ret,
                    "fixed_overflow": fix_overflow,
                    "float_overflow": flt_overflow,
                    "fixed_verdict": fix_verdict,
                    "float_verdict": flt_verdict,
                    "parameters": case["source_params"],
                }
            )
        elif case.get("guaranteed_invalid") and fix_ret >= 0 and flt_ret >= 0:
            examples.append(
                {
                    "kind": "malformed_accepted_by_both",
                    "case_id": case_id,
                    "mutation": case["mutation"],
                    "invalid_class": case["invalid_class"],
                    "fixed_ret": fix_ret,
                    "float_ret": flt_ret,
                    "fixed_overflow": fix_overflow,
                    "float_overflow": flt_overflow,
                    "parameters": case["source_params"],
                }
            )
    return {"counts": dict(counts), "examples": examples}


def write_counterexamples(path: Path, entries: Iterable[dict]) -> int:
    count = 0
    with path.open("w", encoding="utf-8") as f:
        for entry in entries:
            f.write(json.dumps(entry, ensure_ascii=False, sort_keys=True))
            f.write("\n")
            count += 1
    return count


def write_distribution_csv(path: Path, hist: collections.Counter) -> None:
    with path.open("w", encoding="ascii") as f:
        f.write("abs_error,count\n")
        for value in sorted(hist):
            f.write(f"{value},{hist[value]}\n")


def summarize_by(specs: Sequence[dict], comparisons: Dict[int, Comparison],
                 dimension: str) -> List[dict]:
    groups = collections.defaultdict(lambda: {"cases": 0, "failures": 0, "max_abs_error": 0,
                                               "error_samples": 0})
    for spec in specs:
        case_id = int(spec["id"])
        key = str(spec[dimension])
        group = groups[key]
        group["cases"] += 1
        comp = comparisons[case_id]
        if comp.sample_error_count:
            group["failures"] += 1
        group["max_abs_error"] = max(group["max_abs_error"], comp.max_abs_error)
        group["error_samples"] += comp.sample_error_count
    return [{"value": key, **value} for key, value in sorted(
        groups.items(), key=lambda item: (len(item[0]), item[0])
    )]


def make_markdown(report: dict, counter_paths: Dict[str, Path]) -> str:
    legal = report["legal"]
    malformed = report["malformed"]
    lines = [
        "# Speex fixed-point / floating-point differential report",
        "",
        f"Generated: `{report['metadata']['generated_utc']}`",
        f"Source revision: `{report['metadata']['git_sha']}`",
        f"Gate: per-sample `abs(fixed - float) <= {legal['quantization_level']} LSB`; "
        "silence must be exactly zero.",
        "",
        "## Legal corpus",
        "",
        f"- Cases: {legal['case_count']}",
        f"- Frames: {legal['frame_count']}",
        f"- Samples compared: {legal['sample_count']}",
        f"- Cases with any sample over tolerance: {legal['sample_error_cases']}",
        f"- Samples over tolerance: {legal['sample_error_samples']}",
        f"- Max absolute error: {legal['max_abs_error']} LSB",
        f"- p50 / p90 / p99 / p99.9: {legal['quantiles']['p50']} / "
        f"{legal['quantiles']['p90']} / {legal['quantiles']['p99']} / "
        f"{legal['quantiles']['p999']} LSB",
        f"- Pure-silence cases failing exact-zero: {legal['silence']['failing_cases']}",
        f"- Switch cases with detected clicks: {legal['clicks']['failing_cases']}",
        "",
        "### Error distribution",
        "",
        "| abs error (LSB) | samples | percent |",
        "|---:|---:|---:|",
    ]
    for item in legal["error_bins"]:
        hi = "+" if item["hi"] is None else str(item["hi"])
        label = str(item["lo"]) if item["lo"] == item["hi"] else f"{item['lo']}-{hi}"
        lines.append(f"| {label} | {item['count']} | {item['percent']:.4f}% |")
    lines.extend(
        [
            "",
            "### Failures by mode",
            "",
            "| mode | cases | failing | over-tolerance samples | max error |",
            "|---|---:|---:|---:|---:|",
        ]
    )
    for item in legal["by_mode"]:
        mode = PARAM_DISPLAY["mode"].get(int(item["value"]), item["value"])
        lines.append(
            f"| {mode} | {item['cases']} | {item['failures']} | "
            f"{item['error_samples']} | {item['max_abs_error']} |"
        )
    lines.extend(
        [
            "",
            "### Failures by signal",
            "",
            "| signal | cases | failing | over-tolerance samples | max error |",
            "|---|---:|---:|---:|---:|",
        ]
    )
    for item in legal["by_signal"]:
        signal = PARAM_DISPLAY["signal"].get(int(item["value"]), item["value"])
        lines.append(
            f"| {signal} | {item['cases']} | {item['failures']} | "
            f"{item['error_samples']} | {item['max_abs_error']} |"
        )
    lines.extend(
        [
            "",
            "### Minimal sample-error combinations",
            "",
        ]
    )
    combinations = legal.get("minimal_combinations", [])
    if combinations:
        for combination in combinations[:20]:
            compact = ", ".join(
                f"{key}={value}" for key, value in combination["parameters"].items()
                if key not in ("id",)
            )
            lines.append(f"- {combination['count']} cases: `{compact}`")
    else:
        lines.append("- (no failing cases)")
    lines.extend(
        [
            "",
            "## Malformed corpus",
            "",
            f"- Packets: {malformed['case_count']}",
        ]
    )
    for key in (
        "exact_match",
        "fixed_stricter",
        "float_stricter",
        "both_accept",
        "both_reject",
        "guaranteed_invalid",
        "guaranteed_invalid_rejected_both",
        "guaranteed_invalid_accepted_both",
        "fixed_accepts_float_rejects",
        "float_accepts_fixed_rejects",
        "different_rejection",
        "same_status_overflow_diff",
        "crash",
    ):
        lines.append(f"- `{key}`: {malformed['counts'].get(key, 0)}")
    lines.extend(
        [
            "",
            "## Counterexample files",
            "",
        ]
    )
    for name, path in counter_paths.items():
        lines.append(f"- `{name}`: `{path}`")
    lines.append("")
    return "\n".join(lines)


def git_sha(root: Path) -> str:
    try:
        return run_command(["git", "-C", str(root), "rev-parse", "HEAD"]).stdout.strip()
    except Exception:
        return "unknown"


def main(argv: Optional[Sequence[str]] = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[2])
    parser.add_argument("--build-fixed", type=Path, default=None)
    parser.add_argument("--build-float", type=Path, default=None)
    parser.add_argument("--out-dir", type=Path, default=None)
    parser.add_argument("--legal-count", type=int, default=3000)
    parser.add_argument("--malformed-count", type=int, default=1000)
    parser.add_argument("--seed", type=int, default=3697)
    parser.add_argument("--quantization-level", type=int, default=1)
    parser.add_argument("--skip-build", action="store_true")
    parser.add_argument("--no-shrink", action="store_true")
    parser.add_argument("--shrink-max-cases", type=int, default=0,
                        help="0 means shrink every failing case")
    args = parser.parse_args(argv)

    root = args.root.resolve()
    build_fixed = (args.build_fixed or root / "build-fixed").resolve()
    build_float = (args.build_float or root / "build-float").resolve()
    out_dir = (args.out_dir or root / "artifacts" / "differential").resolve()
    out_dir.mkdir(parents=True, exist_ok=True)
    work_dir = Path(tempfile.mkdtemp(prefix="speex-diff-", dir=str(out_dir)))

    fixed_tool, float_tool = ensure_tools(
        root, build_fixed, build_float, args.skip_build
    )
    log(f"fixed tool: {fixed_tool}")
    log(f"float tool: {float_tool}")

    legal_specs = make_legal_specs(args.legal_count, args.seed)
    log(f"generating and decoding {len(legal_specs)} legal streams")
    legal_fixed, legal_float = generate_and_decode(
        legal_specs, fixed_tool, float_tool, work_dir, "legal"
    )
    legal_corpus = []
    by_producer = collections.defaultdict(list)
    for spec in legal_specs:
        by_producer[int(spec["producer"])].append(spec)
    for producer in (0, 1):
        if by_producer[producer]:
            corpus_path = work_dir / f"legal.p{producer}.corpus.bin"
            legal_corpus.extend(parse_corpus(corpus_path))

    comparisons: Dict[int, Comparison] = {}
    total_hist: collections.Counter = collections.Counter()
    total_samples = 0
    total_frames = 0
    sample_error_cases = 0
    sample_error_samples = 0
    max_abs_error = 0
    decode_error_cases = 0
    for spec in legal_specs:
        case_id = int(spec["id"])
        comp = compare_decodes(legal_fixed[case_id], legal_float[case_id])
        comparisons[case_id] = comp
        if comp.sample_error_count:
            sample_error_cases += 1
            sample_error_samples += comp.sample_error_count
        max_abs_error = max(max_abs_error, comp.max_abs_error)
        if comp.decode_errors:
            decode_error_cases += 1
        total_hist.update(comp.abs_hist)
        total_samples += comp.sample_count
        total_frames += len(legal_fixed[case_id].frames)

    silence_failures = []
    click_failures = []
    for spec in legal_specs:
        case_id = int(spec["id"])
        fix_values = all_samples(legal_fixed[case_id])
        flt_values = all_samples(legal_float[case_id])
        if int(spec["signal"]) == 0:
            fix_nonzero = nonzero_summary(fix_values)
            flt_nonzero = nonzero_summary(flt_values)
            if fix_nonzero["nonzero"] or flt_nonzero["nonzero"]:
                silence_failures.append(
                    {
                        "kind": "silence_nonzero",
                        "case_id": case_id,
                        "parameters": case_params(spec),
                        "fixed": fix_nonzero,
                        "float": flt_nonzero,
                    }
                )
        if int(spec["switch_every"]) > 0:
            fixed_clicks = click_events(spec, fix_values)
            float_clicks = click_events(spec, flt_values)
            if fixed_clicks or float_clicks:
                click_failures.append(
                    {
                        "kind": "click_at_parameter_switch",
                        "case_id": case_id,
                        "parameters": case_params(spec),
                        "fixed": fixed_clicks,
                        "float": float_clicks,
                    }
                )

    failing_specs = [
        spec for spec in legal_specs
        if comparisons[int(spec["id"])].sample_error_count > 0
    ]
    minimal_by_id: Dict[int, dict] = {}
    if failing_specs and not args.no_shrink:
        shrink_specs = failing_specs
        if args.shrink_max_cases > 0:
            shrink_specs = failing_specs[:args.shrink_max_cases]
        log(f"minimising {len(shrink_specs)} sample-error counterexamples")
        minimal_by_id = shrink_cases(
            shrink_specs, fixed_tool, float_tool, work_dir,
        )

    minimal_comparisons: Dict[int, Comparison] = {}
    verify_specs = []
    for case_id, minimal in minimal_by_id.items():
        verify_specs.append({**minimal, "id": 900000 + case_id})
    if verify_specs:
        minimal_comparisons = evaluate_specs(
            verify_specs, fixed_tool, float_tool, work_dir, "verify-minimal"
        )
    minimal_counter = collections.Counter()
    for minimal in minimal_by_id.values():
        key = tuple(
            (name, case_params(minimal)[name])
            for name in PARAM_NAMES
            if name != "id"
        ) + (("producer", case_params(minimal)["producer"]),)
        minimal_counter[key] += 1

    counterexamples = []
    for spec in failing_specs:
        case_id = int(spec["id"])
        comp = comparisons[case_id]
        minimal = minimal_by_id.get(case_id)
        entry = {
            "kind": "sample_error",
            "case_id": case_id,
            "parameters": case_params(spec),
            "sample_error_count": comp.sample_error_count,
            "max_abs_error": comp.max_abs_error,
            "first_error": comp.first_error,
            "minimal_parameters": case_params(minimal) if minimal else None,
        }
        if minimal:
            min_comp = minimal_comparisons[900000 + case_id]
            entry["minimal_max_abs_error"] = min_comp.max_abs_error
            entry["minimal_sample_error_count"] = min_comp.sample_error_count
        counterexamples.append(entry)
    counterexamples.extend(silence_failures)
    counterexamples.extend(click_failures)

    malformed_specs = mutate_packets(
        legal_specs, legal_corpus, args.malformed_count, args.seed ^ 0x5A5A5A5A
    )
    log(f"decoding {len(malformed_specs)} malformed packets")
    malformed_decoded = decode_malformed(
        malformed_specs, fixed_tool, float_tool, work_dir
    )
    malformed_analysis = analyze_malformed(malformed_specs, malformed_decoded)
    counterexamples.extend(malformed_analysis["examples"])

    counter_path = out_dir / "counterexamples.jsonl"
    counter_count = write_counterexamples(counter_path, counterexamples)
    distribution_path = out_dir / "error_distribution.csv"
    write_distribution_csv(distribution_path, total_hist)

    bins = histogram_bins(total_hist, total_samples)
    quantiles = {
        "p50": quantile_from_hist(total_hist, total_samples, 0.50),
        "p90": quantile_from_hist(total_hist, total_samples, 0.90),
        "p99": quantile_from_hist(total_hist, total_samples, 0.99),
        "p999": quantile_from_hist(total_hist, total_samples, 0.999),
    }
    report = {
        "metadata": {
            "generated_utc": _dt.datetime.now(_dt.timezone.utc).isoformat(),
            "git_sha": git_sha(root),
            "legal_count": len(legal_specs),
            "malformed_count": len(malformed_specs),
            "seed": args.seed,
            "build_fixed": str(build_fixed),
            "build_float": str(build_float),
        },
        "legal": {
            "case_count": len(legal_specs),
            "frame_count": total_frames,
            "sample_count": total_samples,
            "quantization_level": args.quantization_level,
            "sample_error_cases": sample_error_cases,
            "sample_error_samples": sample_error_samples,
            "decode_error_cases": decode_error_cases,
            "max_abs_error": max_abs_error,
            "quantiles": quantiles,
            "error_bins": bins,
            "by_mode": summarize_by(legal_specs, comparisons, "mode"),
            "by_quality": summarize_by(legal_specs, comparisons, "quality"),
            "by_signal": summarize_by(legal_specs, comparisons, "signal"),
            "minimal_combinations": [
                {"count": count, "parameters": dict(key)}
                for key, count in minimal_counter.most_common()
            ],
            "silence": {
                "cases": sum(1 for spec in legal_specs if int(spec["signal"]) == 0),
                "failing_cases": len(silence_failures),
            },
            "clicks": {
                "cases": sum(
                    1 for spec in legal_specs if int(spec["switch_every"]) > 0
                ),
                "failing_cases": len(click_failures),
            },
        },
        "malformed": {
            "case_count": len(malformed_specs),
            "counts": malformed_analysis["counts"],
        },
        "counterexamples": {
            "count": counter_count,
            "path": str(counter_path),
            "distribution_path": str(distribution_path),
        },
    }
    report_path = out_dir / "report.json"
    report_path.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n",
                           encoding="utf-8")
    markdown_path = out_dir / "report.md"
    markdown_path.write_text(
        make_markdown(
            report,
            {
                "counterexamples": counter_path,
                "error distribution": distribution_path,
                "machine report": report_path,
            },
        ),
        encoding="utf-8",
    )

    log(f"report: {report_path}")
    log(f"counterexamples: {counter_path} ({counter_count} entries)")
    log(f"distribution: {distribution_path}")
    log(
        "legal: "
        f"{sample_error_cases}/{len(legal_specs)} cases over {args.quantization_level} LSB, "
        f"{sample_error_samples}/{total_samples} samples, max {max_abs_error} LSB"
    )
    log(
        "silence failures: "
        f"{len(silence_failures)}/{report['legal']['silence']['cases']}; "
        f"click failures: {len(click_failures)}/{report['legal']['clicks']['cases']}"
    )
    log(
        "malformed: "
        f"exact={malformed_analysis['counts'].get('exact_match', 0)}, "
        f"fixed_stricter={malformed_analysis['counts'].get('fixed_stricter', 0)}, "
        f"float_stricter={malformed_analysis['counts'].get('float_stricter', 0)}, "
        f"accept_mismatch="
        f"{malformed_analysis['counts'].get('fixed_accepts_float_rejects', 0) + malformed_analysis['counts'].get('float_accepts_fixed_rejects', 0)}"
    )
    shutil.rmtree(work_dir, ignore_errors=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
