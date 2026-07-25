#!/usr/bin/env python3

import argparse
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
PROFILE_SUFFIXES = (
    "-summary.csv",
    "-functions.csv",
    "-tlb.csv",
    "-frames.csv",
    "-game-frames.csv",
    ".folded",
)


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


def verify_profiles(prefix):
    missing = []
    for suffix in PROFILE_SUFFIXES:
        path = prefix.parent / f"{prefix.name}-001{suffix}"
        if not path.is_file() or path.stat().st_size == 0:
            missing.append(path.name)
    return missing


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
    missing_profiles = verify_profiles(profile_prefix)
    if missing_profiles:
        return Result(
            name,
            False,
            f"missing profiler output: {', '.join(missing_profiles)}",
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
