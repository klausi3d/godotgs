#!/usr/bin/env python3
"""Validate renderer evidence and public-alpha release gate contracts."""

from __future__ import annotations

import argparse
import datetime as _dt
import fnmatch
import hashlib
import importlib.util
import json
import re
import subprocess
import sys
from pathlib import Path
from typing import Any, Iterable


ROOT = Path(__file__).resolve().parents[2]
DEFAULT_MANIFEST = ROOT / "docs" / "reference" / "renderer_release_gate_manifest.json"
TEST_CASE_RE = re.compile(r'TEST_CASE\("([^"]*\[RequiresGPU\][^"]*)"')
TAG_RE = re.compile(r"\[([^\]]+)\]")


def _load_json(path: Path) -> Any:
    with path.open("r", encoding="utf-8") as fh:
        return json.load(fh)


def _repo_path(root: Path, value: str) -> Path:
    return root / value


def _extract_requires_gpu_tests(root: Path) -> list[dict[str, Any]]:
    tests_root = root / "modules" / "gaussian_splatting" / "tests"
    tests: list[dict[str, Any]] = []
    for path in sorted(tests_root.rglob("*")):
        if path.suffix not in {".h", ".cpp"}:
            continue
        try:
            text = path.read_text(encoding="utf-8", errors="ignore")
        except OSError:
            continue
        for match in TEST_CASE_RE.finditer(text):
            name = match.group(1)
            tests.append(
                {
                    "path": path.relative_to(root).as_posix(),
                    "line": text[: match.start()].count("\n") + 1,
                    "name": name,
                    "tags": TAG_RE.findall(name),
                }
            )
    return tests


def _requires_gpu_snapshot(tests: Iterable[dict[str, Any]]) -> tuple[int, str]:
    rows = [f"{item['path']}\0{item['name']}" for item in tests]
    payload = "\n".join(rows).encode("utf-8")
    return len(rows), hashlib.sha256(payload).hexdigest()


def _load_gpu_harness_batches(root: Path, script_rel: str) -> dict[str, tuple[str, ...]]:
    script = _repo_path(root, script_rel)
    spec = importlib.util.spec_from_file_location("godotgs_gpu_harness_contract", script)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"Unable to import {script_rel}")
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return {batch.name: tuple(batch.filters) for batch in module.BATCHES}


def _filter_matches(filter_pattern: str, test_name: str) -> bool:
    return fnmatch.fnmatchcase(test_name, filter_pattern)


def _is_broad_required_filter(filter_pattern: str) -> bool:
    compact = filter_pattern.replace("*", "").replace("?", "")
    return compact in {"[RequiresGPU]", "][RequiresGPU]", "RequiresGPU"}


def _tracked_actual_pngs(root: Path) -> list[str]:
    try:
        result = subprocess.run(
            ["git", "ls-files", "tests/visual_baselines/**/*.actual.png"],
            cwd=root,
            text=True,
            capture_output=True,
            check=False,
        )
        if result.returncode == 0:
            return [line for line in result.stdout.splitlines() if line.strip()]
    except OSError:
        pass
    return [p.relative_to(root).as_posix() for p in (root / "tests" / "visual_baselines").rglob("*.actual.png")]


def validate_contract(root: Path, manifest: dict[str, Any]) -> list[str]:
    failures: list[str] = []

    if manifest.get("schema_version") != 1:
        failures.append("manifest schema_version must be 1")

    for ref in manifest.get("references", []):
        if not _repo_path(root, ref).exists():
            failures.append(f"reference path missing: {ref}")

    known_limitations = manifest.get("known_limitations_page")
    if not known_limitations or not _repo_path(root, known_limitations).exists():
        failures.append("known limitations page is missing")

    predicate = manifest.get("public_alpha_predicate", {})
    for flag in (
        "disallow_path_filter_downgrade",
        "disallow_manual_downgrade",
        "disallow_missing_gpu_runner_downgrade",
        "disallow_open_world_advisory_only",
    ):
        if predicate.get(flag) is not True:
            failures.append(f"public_alpha_predicate.{flag} must be true")

    tests = _extract_requires_gpu_tests(root)
    count, digest = _requires_gpu_snapshot(tests)
    snapshot = manifest.get("requires_gpu_test_snapshot", {})
    if count != snapshot.get("test_count") or digest != snapshot.get("sha256"):
        failures.append(
            "RequiresGPU test snapshot drift: "
            f"current count={count} sha256={digest}, "
            f"manifest count={snapshot.get('test_count')} sha256={snapshot.get('sha256')}"
        )

    deferred_tags = set(snapshot.get("deferred_tags_any", []))
    deferred = [item for item in tests if deferred_tags.intersection(item["tags"])]
    if len(deferred) != snapshot.get("deferred_count"):
        failures.append(
            f"deferred RequiresGPU count drift: current={len(deferred)} "
            f"manifest={snapshot.get('deferred_count')}"
        )
    if deferred and snapshot.get("closure_policy", "").find("blocks") < 0:
        failures.append("deferred RequiresGPU tests must be declared closure blockers")

    for waiver in manifest.get("deferred_requires_gpu_waivers", []):
        for field in ("test_name", "issue_url", "owner", "expires_utc", "risk", "mitigation", "docs_path"):
            if not waiver.get(field):
                failures.append(f"deferred waiver missing {field}: {waiver!r}")
        docs_path = waiver.get("docs_path")
        if docs_path and not _repo_path(root, docs_path).exists():
            failures.append(f"deferred waiver docs_path missing: {docs_path}")

    gpu_policy = manifest.get("gpu_harness_policy", {})
    try:
        batches = _load_gpu_harness_batches(root, gpu_policy.get("script", ""))
    except Exception as exc:
        failures.append(f"unable to load GPU harness batches: {exc}")
        batches = {}
    test_names = [item["name"] for item in tests]
    for batch in gpu_policy.get("required_batches", []):
        name = batch.get("name")
        filters = batches.get(name)
        if not filters:
            failures.append(f"required GPU batch missing from harness: {name}")
            continue
        for pattern in filters:
            if _is_broad_required_filter(pattern):
                failures.append(f"required GPU batch {name} uses broad filter: {pattern}")
        matched = [case for case in test_names if any(_filter_matches(pattern, case) for pattern in filters)]
        if len(matched) < int(batch.get("minimum_test_cases", 1)):
            failures.append(f"required GPU batch {name} matches {len(matched)} tests")
        for ref in batch.get("reference_artifacts", []):
            if not _repo_path(root, ref).exists():
                failures.append(f"required GPU batch {name} reference missing: {ref}")

    visual = manifest.get("visual_acceptance", {})
    if visual.get("tracked_actual_png_allowed") is False:
        tracked_actual = _tracked_actual_pngs(root)
        if tracked_actual:
            failures.append(f"tracked .actual.png artifacts are forbidden: {tracked_actual}")
    for reference in visual.get("blocking_references", []):
        if reference.get("required") and not _repo_path(root, reference.get("path", "")).exists():
            failures.append(f"blocking visual reference missing: {reference.get('path')}")

    for workflow in manifest.get("workflow_policy", {}).get("required_workflows", []):
        workflow_file = workflow.get("file", "")
        text_path = _repo_path(root, workflow_file)
        if not text_path.exists():
            failures.append(f"required workflow missing: {workflow_file}")
            continue
        text = text_path.read_text(encoding="utf-8", errors="ignore")
        for marker in workflow.get("required_jobs", []):
            if marker not in text:
                failures.append(f"required workflow marker {marker!r} missing from {workflow_file}")
        if workflow.get("manual_input_may_disable") is not False:
            failures.append(f"{workflow_file} must not allow manual public-alpha downgrade")
        if workflow.get("path_filter_may_disable") is not False:
            failures.append(f"{workflow_file} must not allow path-filter public-alpha downgrade")

    return failures


def _find_lane(rows: Iterable[dict[str, Any]], lane_id: str) -> dict[str, Any] | None:
    for row in rows:
        if row.get("lane_id") == lane_id:
            return row
    return None


def _rows_from_report(report: Any) -> list[dict[str, Any]]:
    if isinstance(report, list):
        return [row for row in report if isinstance(row, dict)]
    if isinstance(report, dict):
        for key in ("lanes", "results", "rows"):
            value = report.get(key)
            if isinstance(value, list):
                return [row for row in value if isinstance(row, dict)]
    return []


def _parse_time(value: str) -> _dt.datetime | None:
    try:
        return _dt.datetime.fromisoformat(value.replace("Z", "+00:00"))
    except (AttributeError, ValueError):
        return None


def validate_candidate(
    root: Path,
    manifest: dict[str, Any],
    evidence: dict[str, Any],
    issues: list[dict[str, Any]] | None = None,
) -> list[str]:
    failures = validate_contract(root, manifest)

    known_limitations = manifest.get("known_limitations_page")
    if known_limitations and not _repo_path(root, known_limitations).exists():
        failures.append("candidate known limitations page is missing")

    tests = _extract_requires_gpu_tests(root)
    deferred_tags = set(manifest.get("requires_gpu_test_snapshot", {}).get("deferred_tags_any", []))
    deferred = [item for item in tests if deferred_tags.intersection(item["tags"])]
    waivers = manifest.get("deferred_requires_gpu_waivers", [])
    if len(waivers) < len(deferred):
        failures.append(
            f"candidate has {len(deferred)} deferred RequiresGPU tests but only {len(waivers)} explicit waivers"
        )

    artifacts = evidence.get("artifacts", {})
    required_groups = manifest.get("artifact_requirements", {}).get("required_groups", [])
    required_fields = manifest.get("artifact_requirements", {}).get("each_artifact_requires", [])
    commit = evidence.get("commit")
    commit_time = _parse_time(evidence.get("commit_time_utc", ""))
    for group in required_groups:
        artifact = artifacts.get(group)
        if not isinstance(artifact, dict):
            failures.append(f"candidate artifact group missing: {group}")
            continue
        for field in required_fields:
            if not artifact.get(field):
                failures.append(f"candidate artifact {group} missing {field}")
        if commit and artifact.get("godot_binary_commit") != commit:
            failures.append(f"candidate artifact {group} built from stale commit {artifact.get('godot_binary_commit')}")
        mtime = _parse_time(artifact.get("godot_binary_mtime_utc", ""))
        if commit_time and mtime and mtime < commit_time:
            failures.append(f"candidate artifact {group} binary predates commit")

    gpu_report_path = evidence.get("gpu_harness_report")
    if gpu_report_path:
        report = _load_json(_repo_path(root, gpu_report_path))
        batches = {batch.get("name"): batch for batch in report.get("batches", [])}
        for required in manifest.get("gpu_harness_policy", {}).get("required_batches", []):
            name = required.get("name")
            batch = batches.get(name)
            if not batch:
                failures.append(f"candidate GPU report missing required batch: {name}")
                continue
            cases = batch.get("test_cases", {})
            assertions = batch.get("assertions", {})
            if cases.get("total", 0) < int(required.get("minimum_test_cases", 1)):
                failures.append(f"candidate GPU batch {name} matched zero/too few tests")
            if batch.get("timed_out"):
                failures.append(f"candidate GPU batch {name} timed out")
            if batch.get("summary_parse_ok") is not True:
                failures.append(f"candidate GPU batch {name} summary did not parse")
            if cases.get("failed", 0) or assertions.get("failed", 0) or batch.get("rc", 0) != 0:
                failures.append(f"candidate GPU batch {name} failed")
            if not required.get("allow_skips", False) and cases.get("skipped", 0):
                failures.append(f"candidate GPU batch {name} skipped tests")
            if not required.get("allow_rid_leaks", False) and batch.get("rid_leak_bytes", 0):
                failures.append(f"candidate GPU batch {name} reported RID leaks")
    else:
        failures.append("candidate evidence missing gpu_harness_report")

    benchmark_report_path = evidence.get("benchmark_report")
    if benchmark_report_path:
        rows = _rows_from_report(_load_json(_repo_path(root, benchmark_report_path)))
        required_non_null = manifest.get("benchmark_acceptance", {}).get("required_fields_non_null", [])
        visual_rules = manifest.get("visual_acceptance", {}).get("candidate_rules", {})
        for lane_id in manifest.get("benchmark_acceptance", {}).get("candidate_required_lanes", []):
            row = _find_lane(rows, lane_id)
            if row is None:
                failures.append(f"candidate benchmark lane missing: {lane_id}")
                continue
            if row.get("timed_out"):
                failures.append(f"candidate benchmark lane timed out: {lane_id}")
            for field in required_non_null:
                if row.get(field) is None:
                    failures.append(f"candidate benchmark lane {lane_id} has null/missing {field}")
            if row.get("capture_count", 0) < visual_rules.get("capture_count_min", 1):
                failures.append(f"candidate benchmark lane {lane_id} has no visual captures")
            if row.get("capture_reference_match_count") != row.get("capture_count"):
                failures.append(f"candidate benchmark lane {lane_id} did not match all references")
            if row.get("capture_ssim_min") is None:
                failures.append(f"candidate benchmark lane {lane_id} has null capture_ssim_min")
            if row.get("capture_psnr_min") is None:
                failures.append(f"candidate benchmark lane {lane_id} has null capture_psnr_min")
            gpu_time = row.get("gpu_time_frame_ms")
            gpu_available = row.get("gpu_timing_available")
            if gpu_available is True and (gpu_time is None or gpu_time <= 0):
                failures.append(f"candidate benchmark lane {lane_id} has invalid GPU time")
            if gpu_available is not True and row.get("gpu_timing_source") != "unavailable":
                failures.append(f"candidate benchmark lane {lane_id} lacks explicit GPU timing unavailability")
            if row.get("cpu_fallback_route") and not row.get("fallback_route_allowed"):
                failures.append(f"candidate benchmark lane {lane_id} used unallowed CPU/fallback route")
    else:
        failures.append("candidate evidence missing benchmark_report")

    if issues is not None:
        policy = manifest.get("public_alpha_predicate", {}).get("required_issue_query", {})
        blocking_any = set(policy.get("blocking_labels_any", []))
        alpha_p1_all = set(policy.get("alpha_relevant_p1_labels_all", []))
        classifications = evidence.get("issue_classifications", {})
        for issue in issues:
            if issue.get("state", "OPEN").upper() not in {"OPEN", "open".upper()}:
                continue
            labels = {
                label.get("name", label) if isinstance(label, dict) else label
                for label in issue.get("labels", [])
            }
            relevant = bool(blocking_any.intersection(labels)) or alpha_p1_all.issubset(labels)
            if not relevant:
                continue
            number = str(issue.get("number"))
            classification = classifications.get(number)
            if not classification:
                failures.append(f"candidate issue #{number} missing alpha classification")
                continue
            status = classification.get("status")
            if status == "blocking":
                failures.append(f"candidate issue #{number} is still blocking")
            elif status == "accepted_alpha_limitation":
                docs_path = classification.get("docs_path")
                if not docs_path or not _repo_path(root, docs_path).exists():
                    failures.append(f"candidate issue #{number} accepted limitation missing docs_path")
            elif status == "post_alpha":
                if not classification.get("rationale"):
                    failures.append(f"candidate issue #{number} post_alpha classification missing rationale")
            else:
                failures.append(f"candidate issue #{number} has invalid classification {status!r}")

    return failures


def _build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--mode", choices=("contract", "candidate"), default="contract")
    parser.add_argument("--manifest", default=str(DEFAULT_MANIFEST))
    parser.add_argument("--candidate-evidence", default=None)
    parser.add_argument("--issues-json", default=None)
    return parser


def main(argv: list[str] | None = None) -> int:
    args = _build_parser().parse_args(argv)
    manifest_path = Path(args.manifest).resolve()
    root = manifest_path.parents[2]
    manifest = _load_json(manifest_path)

    if args.mode == "contract":
        failures = validate_contract(root, manifest)
    else:
        if not args.candidate_evidence:
            print("--candidate-evidence is required in candidate mode", file=sys.stderr)
            return 2
        evidence = _load_json(Path(args.candidate_evidence))
        issues = _load_json(Path(args.issues_json)) if args.issues_json else None
        failures = validate_candidate(root, manifest, evidence, issues)

    if failures:
        print("Renderer release gate check failed:")
        for failure in failures:
            print(f" - {failure}")
        return 1

    print(f"Renderer release gate {args.mode} check passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
