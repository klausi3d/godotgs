#!/usr/bin/env python3
"""Derive the agentic risk class (R0-R3) for a set of changed paths.

Classification rules live in ``.agentic/policy.json``. For each changed path the
highest matching class is taken; the overall result is the maximum class across
all paths. Paths that match no rule fall back to ``default_unclassified`` (R3) so
that unrecognized, potentially sensitive paths fail closed.

Examples
--------
    python scripts/agentic/classify_change.py --paths modules/gaussian_splatting/renderer/foo.cpp
    python scripts/agentic/classify_change.py --base-ref master --format json
"""

from __future__ import annotations

import argparse
import fnmatch
import json
import os
import subprocess
import sys
from pathlib import Path
from typing import Any

ROOT = Path(__file__).resolve().parents[2]
DEFAULT_POLICY = ROOT / ".agentic" / "policy.json"


def load_policy(policy_path: Path) -> dict[str, Any]:
    with open(policy_path, encoding="utf-8") as handle:
        return json.load(handle)


def _norm(path: str) -> str:
    text = path.replace("\\", "/")
    if text.startswith("./"):
        text = text[2:]
    return text


def _matches(path: str, glob: str) -> bool:
    # Case-sensitive matching; fnmatch's ``*`` spans ``/`` which is the behavior
    # we want for ``**``-style prefixes used in the policy globs.
    return fnmatch.fnmatchcase(path, glob)


def classify_paths(paths: list[str], policy: dict[str, Any]) -> tuple[str, list[dict[str, str]]]:
    """Return (overall_class, per_path_detail)."""
    classification = policy["classification"]
    ordering = classification["ordering"]
    rank = {cls: index for index, cls in enumerate(ordering)}
    default = classification["default_unclassified"]
    rules = classification["rules"]

    per_path: list[dict[str, str]] = []
    overall: str | None = None

    for raw in paths:
        path = _norm(raw)
        best: str | None = None
        best_reason = ""
        for rule in rules:
            cls = rule["class"]
            for glob in rule["path_globs"]:
                if _matches(path, _norm(glob)):
                    if best is None or rank[cls] > rank[best]:
                        best = cls
                        best_reason = rule["reason"]
                    break
        if best is None:
            best = default
            best_reason = "unclassified path (fail-closed)"
        per_path.append({"path": path, "class": best, "reason": best_reason})
        if overall is None or rank[best] > rank[overall]:
            overall = best

    if overall is None:
        # No changed paths: nothing risky to classify.
        overall = ordering[0]
    return overall, per_path


def git_changed_paths(base_ref: str) -> list[str]:
    result = subprocess.run(
        ["git", "-C", str(ROOT), "diff", "--name-only", f"{base_ref}...HEAD"],
        capture_output=True,
        text=True,
    )
    if result.returncode != 0:
        # Fall back to a two-dot diff if the merge-base form is unavailable.
        result = subprocess.run(
            ["git", "-C", str(ROOT), "diff", "--name-only", base_ref],
            capture_output=True,
            text=True,
        )
    if result.returncode != 0:
        raise SystemExit(f"git diff failed for base-ref {base_ref!r}: {result.stderr.strip()}")
    return [line.strip() for line in result.stdout.splitlines() if line.strip()]


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--paths", nargs="*", help="Explicit changed paths to classify.")
    parser.add_argument("--base-ref", help="Git ref to diff HEAD against to discover changed paths.")
    parser.add_argument("--policy", type=Path, default=DEFAULT_POLICY, help="Path to policy.json.")
    parser.add_argument("--format", choices=["text", "json"], default="text", help="Output format.")
    parser.add_argument(
        "--github-output",
        action="store_true",
        help="Also write risk_class to the file named by $GITHUB_OUTPUT.",
    )
    return parser


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    policy = load_policy(args.policy)

    if args.paths is not None:
        paths = args.paths
    elif args.base_ref:
        paths = git_changed_paths(args.base_ref)
    else:
        print("error: provide --paths or --base-ref", file=sys.stderr)
        return 2

    risk_class, per_path = classify_paths(paths, policy)

    if args.format == "json":
        print(json.dumps({"risk_class": risk_class, "paths": per_path}, indent=2))
    else:
        print(f"risk_class: {risk_class}")
        for detail in per_path:
            print(f"  {detail['class']}  {detail['path']}  ({detail['reason']})")

    if args.github_output:
        output_file = os.environ.get("GITHUB_OUTPUT")
        if output_file:
            with open(output_file, "a", encoding="utf-8") as handle:
                handle.write(f"risk_class={risk_class}\n")

    return 0


if __name__ == "__main__":
    sys.exit(main())
