#!/usr/bin/env python3

import argparse
import csv
import os
import queue
import re
import shutil
import subprocess
import sys
import tempfile
import threading
import time
import xml.etree.ElementTree as ET
from dataclasses import dataclass
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
FIXTURE_DIR = Path(__file__).resolve().parent / "replays"
DEFAULT_REPLAYS = (
    "archives.ram",
    "dam.ram",
    "frigate_00.ram",
    "runway.ram",
    "runway_agent_grenade.ram",
)
REPLAY_HEADER_OFFSET = 0x600
REPLAY_MAGIC = b"GERP"
REPLAY_VERSION = 1
SRAM_SIZE = 128 * 1024
STATUS_TIMEOUT_SECONDS = 20
COMPLETE_PATTERN = re.compile(r"\bTEST_COMPLETE frames=(\d+)\b")
SEMVER_PATTERN = re.compile(
    r"(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)"
    r"(?:-[0-9A-Za-z-]+(?:\.[0-9A-Za-z-]+)*)?"
    r"(?:\+[0-9A-Za-z-]+(?:\.[0-9A-Za-z-]+)*)?"
)
DROPPED_FRAME_METRICS = (
    "dropped_frames_0",
    "dropped_frames_1",
    "dropped_frames_2",
    "dropped_frames_3",
    "dropped_frames_4",
    "dropped_frames_5_6",
    "dropped_frames_7_8",
    "dropped_frames_9_10",
    "dropped_frames_11_plus",
)
MEMORY_POOL_NAMES = ("mf", "pool2", "ml", "stage", "me", "permanent")
MEMORY_POOL_METRICS = tuple(
    f"memory_pool_{pool}_{field}_bytes"
    for pool in MEMORY_POOL_NAMES
    for field in ("peak", "capacity")
)
PROFILE_SUFFIXES = (
    "-summary.csv",
    "-functions.csv",
    "-tlb.csv",
    "-frames.csv",
    "-game-frames.csv",
    ".folded",
)
PROFILE_CSV_HEADERS = {
    "-summary.csv": ("metric", "value"),
    "-functions.csv": (
        "address",
        "size",
        "name",
        "calls",
        "self_cycles",
        "inclusive_cycles",
    ),
    "-tlb.csv": (
        "page",
        "accesses",
        "loads",
        "stores",
        "cache_hits",
        "cache_misses",
        "missing",
    ),
    "-frames.csv": ("frame", "start_cycle", "end_cycle", "delta_cycles"),
    "-game-frames.csv": (
        "frame",
        "tick_cycles",
        "tlb_loads",
        "start_cycle",
        "end_cycle",
    ),
}


@dataclass
class Result:
    name: str
    passed: bool
    detail: str
    duration: float
    output: str


def parse_args():
    parser = argparse.ArgumentParser(
        description="Run GoldenEye SRAM replays through the ares N64 profiler."
    )
    parser.add_argument(
        "--ares",
        type=Path,
        default=os.environ.get("ARES"),
        help="ares executable (default: $ARES or ares on PATH)",
    )
    parser.add_argument(
        "--rom",
        type=Path,
        default=os.environ.get("GOLDENEYE_ROM"),
        help="GoldenEye ROM (default: $GOLDENEYE_ROM)",
    )
    parser.add_argument(
        "--elf",
        type=Path,
        default=os.environ.get("GOLDENEYE_ELF"),
        help="ELF used to build the ROM (default: $GOLDENEYE_ELF)",
    )
    parser.add_argument(
        "--replay",
        action="append",
        type=Path,
        help=(
            "replay path or fixture name; may be repeated "
            "(default: all bundled US fixtures)"
        ),
    )
    parser.add_argument(
        "--artifacts",
        type=Path,
        help="keep replay logs and profiler output in this directory",
    )
    parser.add_argument(
        "--junit-xml",
        type=Path,
        help="write JUnit XML results",
    )
    return parser.parse_args()


def resolve_executable(configured):
    if configured:
        return configured.expanduser().resolve()
    found = shutil.which("ares")
    return Path(found).resolve() if found else None


def resolve_replays(configured):
    selected = configured or [Path(name) for name in DEFAULT_REPLAYS]
    result = []
    for replay in selected:
        candidate = replay.expanduser()
        if not candidate.is_file():
            candidate = FIXTURE_DIR / candidate
        if not candidate.is_file() and candidate.suffix != ".ram":
            candidate = candidate.with_suffix(".ram")
        result.append(candidate.resolve())
    return result


def replay_frame_count(path):
    data = path.read_bytes()
    header = data[REPLAY_HEADER_OFFSET : REPLAY_HEADER_OFFSET + 16]
    if len(data) != SRAM_SIZE:
        raise ValueError(f"expected {SRAM_SIZE} bytes, got {len(data)}")
    if header[:4] != REPLAY_MAGIC:
        raise ValueError("missing GERP replay header")
    version = int.from_bytes(header[4:6], "big")
    if version != REPLAY_VERSION:
        raise ValueError(f"unsupported replay version {version}")
    frame_count = int.from_bytes(header[12:16], "big")
    if frame_count == 0:
        raise ValueError("replay has no frames")
    return frame_count


def stream_output(process, output_queue):
    assert process.stdout is not None
    for line in process.stdout:
        output_queue.put(line)
    output_queue.put(None)


def stop_process(process):
    if process.poll() is not None:
        return
    process.terminate()
    try:
        process.wait(timeout=5)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait()


def profile_path(prefix, suffix):
    return prefix.parent / f"{prefix.name}-001{suffix}"


def read_profile_csv(prefix, suffix, errors):
    path = profile_path(prefix, suffix)
    try:
        with path.open(newline="", encoding="utf-8") as profile:
            reader = csv.DictReader(profile)
            expected = list(PROFILE_CSV_HEADERS[suffix])
            if reader.fieldnames != expected:
                errors.append(
                    f"{path.name} has header {reader.fieldnames}, expected {expected}"
                )
                return []
            return list(reader)
    except (OSError, csv.Error) as error:
        errors.append(f"could not read {path.name}: {error}")
        return []


def profile_integer(row, field, context, errors, base=10):
    try:
        return int(row[field], base)
    except (KeyError, TypeError, ValueError):
        errors.append(f"{context} has invalid {field}: {row.get(field)!r}")
        return None


def verify_profiles(prefix, expected_game_frames):
    errors = []
    for suffix in PROFILE_SUFFIXES:
        path = profile_path(prefix, suffix)
        if not path.is_file() or path.stat().st_size == 0:
            errors.append(f"missing or empty {path.name}")
    if errors:
        return errors

    summary_rows = read_profile_csv(prefix, "-summary.csv", errors)
    expected_metrics = {
        "stage",
        "start_cycle",
        "end_cycle",
        "total_cycles",
        "frames",
        "average_frame_delta",
        "tlb_cache_hits",
        "tlb_cache_misses",
        "tlb_missing",
        *DROPPED_FRAME_METRICS,
        *MEMORY_POOL_METRICS,
    }
    expected_version_metrics = {"ares_version", "profiler_version"}
    summary = {}
    for row in summary_rows:
        metric = row["metric"]
        if metric in summary:
            errors.append(f"summary has duplicate metric {metric!r}")
            continue
        if metric in expected_metrics:
            summary[metric] = profile_integer(
                row, "value", f"summary metric {metric!r}", errors
            )
        else:
            summary[metric] = row["value"]
    missing_metrics = sorted(
        (expected_metrics | expected_version_metrics) - summary.keys()
    )
    if missing_metrics:
        errors.append(f"summary is missing metrics: {', '.join(missing_metrics)}")
    if "ares_version" in summary and not summary["ares_version"]:
        errors.append("summary has an empty ares version")
    if (
        "profiler_version" in summary
        and not SEMVER_PATTERN.fullmatch(summary["profiler_version"])
    ):
        errors.append("summary has an invalid profiler semantic version")

    function_rows = read_profile_csv(prefix, "-functions.csv", errors)
    function_cycles = 0
    for index, row in enumerate(function_rows):
        context = f"function row {index}"
        address = profile_integer(row, "address", context, errors, 0)
        size = profile_integer(row, "size", context, errors)
        calls = profile_integer(row, "calls", context, errors)
        self_cycles = profile_integer(row, "self_cycles", context, errors)
        inclusive_cycles = profile_integer(
            row, "inclusive_cycles", context, errors
        )
        if not row["name"]:
            errors.append(f"{context} has an empty name")
        if address is not None and address < 0:
            errors.append(f"{context} has a negative address")
        for field, value in (
            ("size", size),
            ("calls", calls),
            ("self_cycles", self_cycles),
            ("inclusive_cycles", inclusive_cycles),
        ):
            if value is not None and value < 0:
                errors.append(f"{context} has negative {field}")
        if self_cycles is not None:
            function_cycles += self_cycles
    if not function_rows:
        errors.append("functions profile has no rows")
    elif function_cycles == 0:
        errors.append("functions profile has no attributed cycles")

    tlb_rows = read_profile_csv(prefix, "-tlb.csv", errors)
    tlb_totals = {"cache_hits": 0, "cache_misses": 0, "missing": 0}
    for index, row in enumerate(tlb_rows):
        context = f"TLB row {index}"
        profile_integer(row, "page", context, errors, 0)
        values = {
            field: profile_integer(row, field, context, errors)
            for field in (
                "accesses",
                "loads",
                "stores",
                "cache_hits",
                "cache_misses",
                "missing",
            )
        }
        if any(value is None for value in values.values()):
            continue
        if any(value < 0 for value in values.values()):
            errors.append(f"{context} contains a negative count")
            continue
        if values["loads"] + values["stores"] != values["accesses"]:
            errors.append(f"{context} loads and stores do not equal accesses")
        if values["cache_hits"] + values["cache_misses"] != values["accesses"]:
            errors.append(f"{context} cache results do not equal accesses")
        if values["missing"] > values["cache_misses"]:
            errors.append(f"{context} missing count exceeds cache misses")
        for field in tlb_totals:
            tlb_totals[field] += values[field]
    if not tlb_rows:
        errors.append("TLB profile has no rows")

    frame_rows = read_profile_csv(prefix, "-frames.csv", errors)
    frame_delta_total = 0
    for index, row in enumerate(frame_rows):
        context = f"frame row {index}"
        frame = profile_integer(row, "frame", context, errors)
        start = profile_integer(row, "start_cycle", context, errors)
        end = profile_integer(row, "end_cycle", context, errors)
        delta = profile_integer(row, "delta_cycles", context, errors)
        if None in (frame, start, end, delta):
            continue
        if frame != index:
            errors.append(f"{context} has non-sequential frame number {frame}")
        if end <= start or delta != end - start:
            errors.append(f"{context} has inconsistent cycle bounds")
        frame_delta_total += delta
    if not frame_rows:
        errors.append("frame profile has no rows")

    game_frame_rows = read_profile_csv(prefix, "-game-frames.csv", errors)
    for index, row in enumerate(game_frame_rows):
        context = f"game frame row {index}"
        frame = profile_integer(row, "frame", context, errors)
        ticks = profile_integer(row, "tick_cycles", context, errors)
        tlb_loads = profile_integer(row, "tlb_loads", context, errors)
        start = profile_integer(row, "start_cycle", context, errors)
        end = profile_integer(row, "end_cycle", context, errors)
        if None in (frame, ticks, tlb_loads, start, end):
            continue
        if frame != index:
            errors.append(f"{context} has non-sequential frame number {frame}")
        if end <= start or ticks != (end - start) // 2:
            errors.append(f"{context} has inconsistent cycle bounds")
        if tlb_loads < 0:
            errors.append(f"{context} has negative TLB loads")
    if len(game_frame_rows) != expected_game_frames:
        errors.append(
            "game frame profile has "
            f"{len(game_frame_rows)} rows, expected {expected_game_frames}"
        )

    folded_path = profile_path(prefix, ".folded")
    folded_cycles = 0
    try:
        for index, line in enumerate(
            folded_path.read_text(encoding="utf-8").splitlines()
        ):
            callstack, separator, count = line.rpartition(" ")
            try:
                cycles = int(count)
            except ValueError:
                cycles = 0
            if not separator or not callstack or cycles <= 0:
                errors.append(f"folded row {index} is invalid: {line!r}")
            else:
                folded_cycles += cycles
    except OSError as error:
        errors.append(f"could not read {folded_path.name}: {error}")
    if folded_cycles == 0:
        errors.append("folded profile has no attributed cycles")

    if expected_metrics <= summary.keys() and all(
        summary[metric] is not None for metric in expected_metrics
    ):
        if summary["end_cycle"] <= summary["start_cycle"]:
            errors.append("summary end cycle does not follow start cycle")
        if summary["total_cycles"] != (
            summary["end_cycle"] - summary["start_cycle"]
        ):
            errors.append("summary total cycles do not match its cycle bounds")
        if summary["frames"] != len(frame_rows):
            errors.append("summary frame count does not match frame profile")
        expected_average = (
            frame_delta_total // len(frame_rows) if frame_rows else 0
        )
        if summary["average_frame_delta"] != expected_average:
            errors.append("summary average frame delta does not match frame profile")
        for field, metric in (
            ("cache_hits", "tlb_cache_hits"),
            ("cache_misses", "tlb_cache_misses"),
            ("missing", "tlb_missing"),
        ):
            if summary[metric] != tlb_totals[field]:
                errors.append(f"summary {metric} does not match TLB profile")
        if sum(summary[metric] for metric in DROPPED_FRAME_METRICS) != len(
            game_frame_rows
        ):
            errors.append(
                "summary dropped-frame histogram does not match game frame profile"
            )
        for pool in MEMORY_POOL_NAMES:
            peak = summary[f"memory_pool_{pool}_peak_bytes"]
            capacity = summary[f"memory_pool_{pool}_capacity_bytes"]
            if peak < 0 or capacity < 0 or peak > capacity:
                errors.append(f"summary {pool} memory pool usage is invalid")
    return errors


def run_replay(ares, rom, elf, replay, artifacts):
    name = replay.stem
    try:
        expected_frames = replay_frame_count(replay)
    except (OSError, ValueError) as error:
        return Result(name, False, f"invalid fixture: {error}", 0.0, "")

    test_dir = artifacts / name
    test_dir.mkdir(parents=True, exist_ok=True)
    runtime_dir = test_dir / "runtime"
    shutil.rmtree(runtime_dir, ignore_errors=True)
    runtime_dir.mkdir()
    staged_rom = runtime_dir / rom.name
    shutil.copyfile(rom, staged_rom)
    profile_prefix = test_dir / "profile"
    log_path = test_dir / "ares.log"

    environment = os.environ.copy()
    environment.update(
        {
            "ARES_N64_PROFILE_SYMBOLS": str(elf),
            "ARES_N64_PROFILE_OUTPUT": str(profile_prefix),
            "ARES_N64_REPLAY": str(replay),
            "ARES_N64_REPLAY_QUIT": "1",
            "XDG_CONFIG_HOME": str(runtime_dir / "config"),
            "XDG_DATA_HOME": str(runtime_dir / "data"),
        }
    )
    command = [
        str(ares),
        "--no-file-prompt",
        "--setting",
        "Audio/Driver=None",
        "--setting",
        "Input/Driver=None",
        str(staged_rom),
    ]

    started = time.monotonic()
    output = []
    complete_frames = None
    failure = None
    print(f"[{name}] running {expected_frames} frames", flush=True)
    try:
        process = subprocess.Popen(
            command,
            cwd=ROOT,
            env=environment,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            bufsize=1,
        )
    except OSError as error:
        shutil.rmtree(runtime_dir, ignore_errors=True)
        return Result(name, False, f"could not start ares: {error}", 0.0, "")

    output_queue = queue.Queue()
    output_thread = threading.Thread(
        target=stream_output, args=(process, output_queue), daemon=True
    )
    output_thread.start()
    last_status = started
    status_timed_out = False
    try:
        while True:
            remaining = STATUS_TIMEOUT_SECONDS - (time.monotonic() - last_status)
            if remaining <= 0:
                status_timed_out = True
                break
            try:
                line = output_queue.get(timeout=min(remaining, 0.25))
            except queue.Empty:
                continue
            if line is None:
                break
            output.append(line)
            print(f"[{name}] {line}", end="", flush=True)
            if "REPLAY_STARTED" in line or "REPLAY_STATUS" in line:
                last_status = time.monotonic()
            match = COMPLETE_PATTERN.search(line)
            if match:
                complete_frames = int(match.group(1))
                last_status = time.monotonic()
            if "TEST_FAILED" in line:
                failure = line.strip()
                last_status = time.monotonic()
    finally:
        stop_process(process)
        output_thread.join(timeout=1)

    duration = time.monotonic() - started
    combined_output = "".join(output)
    log_path.write_text(combined_output, encoding="utf-8")
    shutil.rmtree(runtime_dir, ignore_errors=True)
    if status_timed_out:
        return Result(
            name,
            False,
            (
                "no replay start or status update received for "
                f"{STATUS_TIMEOUT_SECONDS}s"
            ),
            duration,
            combined_output,
        )
    if failure:
        return Result(name, False, failure, duration, combined_output)
    if process.returncode:
        return Result(
            name,
            False,
            f"ares exited with status {process.returncode}",
            duration,
            combined_output,
        )
    if complete_frames is None:
        return Result(
            name, False, "ares exited without TEST_COMPLETE", duration, combined_output
        )
    if complete_frames != expected_frames:
        return Result(
            name,
            False,
            f"completed {complete_frames} of {expected_frames} frames",
            duration,
            combined_output,
        )
    profile_errors = verify_profiles(profile_prefix, expected_frames)
    if profile_errors:
        return Result(
            name,
            False,
            f"invalid profiler output: {'; '.join(profile_errors)}",
            duration,
            combined_output,
        )
    return Result(name, True, f"completed {complete_frames} frames", duration, combined_output)


def write_junit(results, path):
    suite = ET.Element(
        "testsuite",
        {
            "name": "n64-replay",
            "tests": str(len(results)),
            "failures": str(sum(not result.passed for result in results)),
            "errors": "0",
            "time": f"{sum(result.duration for result in results):.3f}",
        },
    )
    for result in results:
        case = ET.SubElement(
            suite,
            "testcase",
            {
                "classname": "n64-replay",
                "name": result.name,
                "time": f"{result.duration:.3f}",
            },
        )
        if not result.passed:
            failure = ET.SubElement(
                case,
                "failure",
                {"message": result.detail, "type": "ReplayFailure"},
            )
            failure.text = result.output
        elif result.output:
            ET.SubElement(case, "system-out").text = result.output
    ET.indent(suite, space="  ")
    path.parent.mkdir(parents=True, exist_ok=True)
    ET.ElementTree(suite).write(path, encoding="utf-8", xml_declaration=True)


def write_github_summary(results):
    summary_path = os.environ.get("GITHUB_STEP_SUMMARY")
    if not summary_path:
        return
    passed = sum(result.passed for result in results)
    lines = [
        "## GoldenEye replay tests",
        "",
        f"**{passed} passed, {len(results) - passed} failed**",
        "",
        "| Replay | Result | Duration | Detail |",
        "| --- | --- | ---: | --- |",
    ]
    for result in results:
        status = "✅" if result.passed else "❌"
        lines.append(
            f"| `{result.name}` | {status} | {result.duration:.2f}s | {result.detail} |"
        )
    with open(summary_path, "a", encoding="utf-8") as summary:
        summary.write("\n".join(lines) + "\n")


def main():
    args = parse_args()
    ares = resolve_executable(args.ares)
    rom = args.rom.expanduser().resolve() if args.rom else None
    elf = args.elf.expanduser().resolve() if args.elf else None
    inputs = (("ares executable", ares), ("ROM", rom), ("ELF", elf))
    for label, path in inputs:
        if path is None or not path.is_file():
            print(f"error: {label} does not exist: {path}", file=sys.stderr)
            return 2

    replays = resolve_replays(args.replay)
    missing = [str(path) for path in replays if not path.is_file()]
    if missing:
        print(f"error: replay does not exist: {', '.join(missing)}", file=sys.stderr)
        return 2

    temporary = None
    if args.artifacts:
        artifacts = args.artifacts.expanduser().resolve()
        artifacts.mkdir(parents=True, exist_ok=True)
    else:
        temporary = tempfile.TemporaryDirectory(prefix="ares-replay-tests-")
        artifacts = Path(temporary.name)

    results = [
        run_replay(ares, rom, elf, replay, artifacts)
        for replay in replays
    ]
    if args.junit_xml:
        write_junit(results, args.junit_xml.expanduser().resolve())
    write_github_summary(results)

    print("\nReplay test summary")
    print("-------------------")
    for result in results:
        status = "PASS" if result.passed else "FAIL"
        print(f"{status:4}  {result.name}: {result.detail} ({result.duration:.2f}s)")
        if not result.passed and os.environ.get("GITHUB_ACTIONS"):
            print(f"::error title=Replay failed::{result.name}: {result.detail}")
    passed = sum(result.passed for result in results)
    print(f"\n{passed} passed, {len(results) - passed} failed")

    if temporary:
        temporary.cleanup()
    return 0 if passed == len(results) else 1


if __name__ == "__main__":
    sys.exit(main())
