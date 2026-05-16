#!/usr/bin/env python3
"""
Supervisor for the Gaussian Splatting GPU test harness.

Drives the Godot test binary's --gs-gpu-test entrypoint in per-batch
subprocesses so a driver hang or GPU OOM during one batch can't corrupt
the next. Each batch is a doctest --test-case= filter set. Aggregates
per-batch results into a JSON report and returns max(batch_rc) so the
caller (baseline_qa.yml or a local developer) sees a non-zero exit
whenever any GPU test fails.

This is Phase 2B of the GPU harness rollout. The harness itself
(gs_gpu_test_runner.cpp) is Phase 2A; CI integration in baseline_qa.yml
is Phase 3.
"""

from __future__ import annotations

import argparse
import datetime
import json
import os
import re
import subprocess
import sys
from dataclasses import dataclass, field
from pathlib import Path
from typing import Optional

ROOT = Path(__file__).resolve().parent.parent.parent
DEFAULT_REPORT_PATH = ROOT / "tests" / "ci" / "gpu_harness_report.json"
DEFAULT_TIMEOUT_SECONDS = 60
REPORT_SCHEMA_VERSION = 1

# Each batch maps a name to one or more doctest --test-case patterns.
# The CompositorHazard batch is the only one that has an actually-running
# test today (test_output_compositor_composite_hazard.h). The rest are
# catalogued so future [RequiresGPU] migrations land in a known batch
# without re-shaping the supervisor; they currently report "0 tests
# matched" which the supervisor treats as success (advisory) per batch.
#
# Intentionally no catch-all "*[RequiresGPU]*" batch: the wildcard would
# match the 26 [SceneTree]+[RequiresGPU] tests deferred to v2 of the
# harness (Issue #329), which crash with an access violation because
# SceneTree isn't initialized. Tests that aren't in a named batch stay
# invisible until they're added explicitly.
BATCHES: tuple[tuple[str, tuple[str, ...]], ...] = (
    ("CompositorHazard", ("*HazardRepro*",)),
    ("OutputCompositor", ("*OutputCompositor*][RequiresGPU]*",)),
    ("ComputeInfrastructure", ("*ComputeInfra*][RequiresGPU]*",)),
    ("TileRenderer", ("*TileRenderer*][RequiresGPU]*",)),
    ("GpuSorting", ("*Sort*][RequiresGPU]*",)),
    ("MemoryStream", ("*MemoryStream*][RequiresGPU]*",)),
    ("Streaming", ("*Streaming*][RequiresGPU]*",)),
)

# Doctest summary lines we parse:
#   "[doctest] test cases:  N | M passed | K failed | L skipped"
#   "[doctest] assertions: N | M passed | K failed |"
#   "[doctest] Status: SUCCESS!" or "FAILURE!"
TEST_CASES_RE = re.compile(
    r"\[doctest\] test cases:\s*(\d+)\s*\|\s*(\d+)\s+passed\s*\|\s*(\d+)\s+failed(?:\s*\|\s*(\d+)\s+skipped)?"
)
ASSERTIONS_RE = re.compile(
    r"\[doctest\] assertions:\s*(\d+)\s*\|\s*(\d+)\s+passed\s*\|\s*(\d+)\s+failed"
)
STATUS_RE = re.compile(r"\[doctest\] Status:\s*(\w+)")

# Supervisor-level exit codes (orthogonal to harness exit codes 64-71).
SUPERVISOR_EXIT_OK = 0
SUPERVISOR_EXIT_HARNESS_FAILED = 1
SUPERVISOR_EXIT_TIMEOUT = 124
SUPERVISOR_EXIT_BAD_INVOCATION = 2
SUPERVISOR_EXIT_GODOT_MISSING = 3


@dataclass
class BatchResult:
    name: str
    filters: tuple[str, ...]
    rc: int = -1
    wall_seconds: float = 0.0
    timed_out: bool = False
    test_cases_total: int = 0
    test_cases_passed: int = 0
    test_cases_failed: int = 0
    test_cases_skipped: int = 0
    assertions_total: int = 0
    assertions_passed: int = 0
    assertions_failed: int = 0
    status: str = "UNKNOWN"
    stdout_tail: str = ""
    stderr_tail: str = ""

    def to_dict(self) -> dict:
        return {
            "name": self.name,
            "filters": list(self.filters),
            "rc": self.rc,
            "wall_seconds": round(self.wall_seconds, 3),
            "timed_out": self.timed_out,
            "test_cases": {
                "total": self.test_cases_total,
                "passed": self.test_cases_passed,
                "failed": self.test_cases_failed,
                "skipped": self.test_cases_skipped,
            },
            "assertions": {
                "total": self.assertions_total,
                "passed": self.assertions_passed,
                "failed": self.assertions_failed,
            },
            "status": self.status,
            "stdout_tail": self.stdout_tail,
            "stderr_tail": self.stderr_tail,
        }


def _parse_summary(stdout: str, result: BatchResult) -> None:
    tc_match = TEST_CASES_RE.search(stdout)
    if tc_match:
        result.test_cases_total = int(tc_match.group(1))
        result.test_cases_passed = int(tc_match.group(2))
        result.test_cases_failed = int(tc_match.group(3))
        if tc_match.group(4):
            result.test_cases_skipped = int(tc_match.group(4))

    a_match = ASSERTIONS_RE.search(stdout)
    if a_match:
        result.assertions_total = int(a_match.group(1))
        result.assertions_passed = int(a_match.group(2))
        result.assertions_failed = int(a_match.group(3))

    s_match = STATUS_RE.search(stdout)
    if s_match:
        result.status = s_match.group(1)


def _run_batch(
    godot_binary: Path,
    name: str,
    filters: tuple[str, ...],
    timeout_sec: int,
    extra_args: list[str],
) -> BatchResult:
    # Note: doctest's banner-suppression flag is --no-intro / --dt-no-intro;
    # --no-header is silently ignored. Leaving the banner in stdout for now —
    # the supervisor's _parse_summary regex matches the summary lines regardless,
    # and the banner is useful when triaging a failing run.
    args: list[str] = [str(godot_binary), "--gs-gpu-test"]
    for f in filters:
        args.append(f"--test-case={f}")
    args.extend(extra_args)

    result = BatchResult(name=name, filters=filters)
    started = datetime.datetime.now(datetime.timezone.utc)
    print(f"[run_gpu_harness] >> batch={name} filters={list(filters)} timeout={timeout_sec}s", flush=True)
    try:
        cp = subprocess.run(
            args,
            timeout=timeout_sec,
            capture_output=True,
            text=True,
            check=False,
        )
        result.rc = cp.returncode
        # Tail at most ~4 KiB of each stream so the JSON report stays small.
        result.stdout_tail = cp.stdout[-4096:] if cp.stdout else ""
        result.stderr_tail = cp.stderr[-4096:] if cp.stderr else ""
        _parse_summary(cp.stdout, result)
    except subprocess.TimeoutExpired as exc:
        result.rc = SUPERVISOR_EXIT_TIMEOUT
        result.timed_out = True
        result.status = "TIMEOUT"
        partial_stdout = exc.stdout or ""
        partial_stderr = exc.stderr or ""
        if isinstance(partial_stdout, bytes):
            partial_stdout = partial_stdout.decode("utf-8", errors="replace")
        if isinstance(partial_stderr, bytes):
            partial_stderr = partial_stderr.decode("utf-8", errors="replace")
        result.stdout_tail = partial_stdout[-4096:]
        result.stderr_tail = partial_stderr[-4096:]
    elapsed = (datetime.datetime.now(datetime.timezone.utc) - started).total_seconds()
    result.wall_seconds = elapsed

    summary = (
        f"[run_gpu_harness] << batch={name} rc={result.rc} "
        f"cases={result.test_cases_passed}/{result.test_cases_total} "
        f"asserts={result.assertions_passed}/{result.assertions_total} "
        f"wall={elapsed:.1f}s status={result.status}"
    )
    print(summary, flush=True)
    return result


def _select_batches(filter_names: Optional[set[str]]) -> tuple[tuple[str, tuple[str, ...]], ...]:
    if not filter_names:
        return BATCHES
    return tuple((name, filters) for name, filters in BATCHES if name in filter_names)


def _build_argparser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--godot",
        required=False,
        help="Path to the Godot test binary (with tests=yes). "
        "Defaults to bin/godot.windows.editor.dev.x86_64.console.exe under the repo root.",
    )
    parser.add_argument(
        "--report",
        default=str(DEFAULT_REPORT_PATH),
        help=f"JSON report output path (default: {DEFAULT_REPORT_PATH.relative_to(ROOT)}).",
    )
    parser.add_argument(
        "--timeout",
        type=int,
        default=DEFAULT_TIMEOUT_SECONDS,
        help=f"Per-batch subprocess wall-clock timeout in seconds (default: {DEFAULT_TIMEOUT_SECONDS}).",
    )
    parser.add_argument(
        "--batch",
        action="append",
        default=None,
        help="Restrict to specific batch name(s). Repeatable. If omitted, all batches run.",
    )
    parser.add_argument(
        "--list-batches",
        action="store_true",
        help="Print the batch catalogue and exit.",
    )
    parser.add_argument(
        "--baseline-mode",
        choices=("compare", "update"),
        default=None,
        help="If set, exports GS_VISUAL_BASELINE_MODE for the harness. "
        "If omitted, the harness inherits whatever the calling environment provides.",
    )
    parser.add_argument(
        "--extra-arg",
        action="append",
        default=[],
        help="Extra argv to forward to the harness (e.g., --extra-arg=--success). Repeatable.",
    )
    return parser


def _resolve_godot(arg_value: Optional[str]) -> Optional[Path]:
    if arg_value:
        candidate = Path(arg_value)
        if not candidate.is_absolute():
            candidate = (ROOT / candidate).resolve()
        return candidate if candidate.is_file() else None
    candidates = (
        ROOT / "bin" / "godot.windows.editor.dev.x86_64.console.exe",
        ROOT / "bin" / "godot.windows.editor.dev.x86_64.exe",
        ROOT / "bin" / "godot.windows.editor.x86_64.console.exe",
        ROOT / "bin" / "godot.windows.editor.x86_64.exe",
        ROOT / "bin" / "godot.linuxbsd.editor.dev.x86_64",
        ROOT / "bin" / "godot.linuxbsd.editor.x86_64",
    )
    for c in candidates:
        if c.is_file():
            return c
    return None


def main() -> int:
    parser = _build_argparser()
    args = parser.parse_args()

    if args.list_batches:
        print("Available batches:")
        for name, filters in BATCHES:
            print(f"  {name:24s} {list(filters)}")
        return SUPERVISOR_EXIT_OK

    godot = _resolve_godot(args.godot)
    if godot is None:
        sys.stderr.write(
            "[run_gpu_harness] FATAL: could not resolve Godot test binary. "
            "Pass --godot PATH explicitly.\n"
        )
        return SUPERVISOR_EXIT_GODOT_MISSING

    if args.baseline_mode:
        os.environ["GS_VISUAL_BASELINE_MODE"] = args.baseline_mode

    selected = _select_batches(set(args.batch) if args.batch else None)
    if not selected:
        sys.stderr.write(
            f"[run_gpu_harness] FATAL: no batches matched --batch filter {args.batch}\n"
        )
        return SUPERVISOR_EXIT_BAD_INVOCATION

    run_started = datetime.datetime.now(datetime.timezone.utc)
    print(f"[run_gpu_harness] godot={godot} timeout={args.timeout}s batches={len(selected)}", flush=True)
    if args.baseline_mode:
        print(f"[run_gpu_harness] GS_VISUAL_BASELINE_MODE={args.baseline_mode}", flush=True)

    results: list[BatchResult] = []
    max_rc = 0
    for name, filters in selected:
        r = _run_batch(godot, name, filters, args.timeout, args.extra_arg)
        results.append(r)
        if r.rc > max_rc:
            max_rc = r.rc

    run_ended = datetime.datetime.now(datetime.timezone.utc)

    totals = {
        "test_cases_total": sum(r.test_cases_total for r in results),
        "test_cases_passed": sum(r.test_cases_passed for r in results),
        "test_cases_failed": sum(r.test_cases_failed for r in results),
        "test_cases_skipped": sum(r.test_cases_skipped for r in results),
        "assertions_total": sum(r.assertions_total for r in results),
        "assertions_passed": sum(r.assertions_passed for r in results),
        "assertions_failed": sum(r.assertions_failed for r in results),
    }

    report = {
        "schema_version": REPORT_SCHEMA_VERSION,
        "timestamp_started_utc": run_started.isoformat(),
        "timestamp_finished_utc": run_ended.isoformat(),
        "wall_seconds": round((run_ended - run_started).total_seconds(), 3),
        "godot_binary": str(godot),
        "baseline_mode": args.baseline_mode or os.environ.get("GS_VISUAL_BASELINE_MODE", "compare"),
        "max_batch_rc": max_rc,
        "supervisor_exit": SUPERVISOR_EXIT_OK if max_rc == 0 else SUPERVISOR_EXIT_HARNESS_FAILED,
        "totals": totals,
        "batches": [r.to_dict() for r in results],
    }

    report_path = Path(args.report)
    if not report_path.is_absolute():
        report_path = (ROOT / report_path).resolve()
    report_path.parent.mkdir(parents=True, exist_ok=True)
    # Atomic write: serialize to a sibling temp file then os.replace into the
    # final path. Prevents downstream readers (e.g. the workflow's PowerShell
    # ConvertFrom-Json summary step) from blowing up on a half-written report
    # if the supervisor is killed mid-write or another invocation overlaps.
    tmp_path = report_path.with_suffix(report_path.suffix + ".tmp")
    tmp_path.write_text(json.dumps(report, indent=2), encoding="utf-8")
    os.replace(tmp_path, report_path)
    print(f"[run_gpu_harness] wrote {report_path}", flush=True)

    fail_count = totals["test_cases_failed"] + totals["assertions_failed"]
    print(
        f"[run_gpu_harness] DONE max_rc={max_rc} cases={totals['test_cases_passed']}/{totals['test_cases_total']} "
        f"asserts={totals['assertions_passed']}/{totals['assertions_total']} failures={fail_count}",
        flush=True,
    )

    return SUPERVISOR_EXIT_OK if max_rc == 0 else SUPERVISOR_EXIT_HARNESS_FAILED


if __name__ == "__main__":
    sys.exit(main())
