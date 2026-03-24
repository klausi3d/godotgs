#!/usr/bin/env python3
"""Run clang-tidy on changed C/C++ files and changed line ranges only."""

from __future__ import annotations

import argparse
import json
import os
import re
import subprocess
import sys
import shutil
from collections import defaultdict
from dataclasses import dataclass
from pathlib import Path
from typing import DefaultDict, Dict, Iterable, List, Optional, Sequence, Set, Tuple


EMPTY_TREE_SHA = "4b825dc642cb6eb9a060e54bf8d69288fbee4904"
SOURCE_EXTENSIONS = {".c", ".cc", ".cpp", ".cxx", ".m", ".mm"}
HEADER_EXTENSIONS = {".h", ".hh", ".hpp", ".hxx", ".inc"}
RELEVANT_EXTENSIONS = SOURCE_EXTENSIONS | HEADER_EXTENSIONS
ZERO_SHA = "0000000000000000000000000000000000000000"


@dataclass(frozen=True)
class LineRange:
    start: int
    end: int


@dataclass(frozen=True)
class Diagnostic:
    file: str
    line: int
    column: int
    kind: str
    check: str
    message: str


def run_command(
    command: Sequence[str],
    cwd: Path,
    *,
    check: bool = True,
    capture_output: bool = True,
) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        list(command),
        cwd=cwd,
        check=check,
        text=True,
        capture_output=capture_output,
    )


def git_output(repo_root: Path, args: Sequence[str]) -> str:
    result = run_command(["git", *args], cwd=repo_root)
    return result.stdout.strip()


def git_exists(repo_root: Path, ref: str) -> bool:
    result = subprocess.run(
        ["git", "rev-parse", "--verify", "--quiet", f"{ref}^{{commit}}"],
        cwd=repo_root,
        text=True,
        capture_output=True,
    )
    return result.returncode == 0


def resolve_repo_root() -> Path:
    result = subprocess.run(
        ["git", "rev-parse", "--show-toplevel"],
        text=True,
        capture_output=True,
        check=True,
    )
    return Path(result.stdout.strip()).resolve()


def resolve_path(repo_root: Path, value: str) -> Path:
    path = Path(value)
    if not path.is_absolute():
        path = repo_root / path
    return path.resolve()


def canonical_repo_path(repo_root: Path, value: str) -> str:
    path = Path(value.replace("\\", "/"))
    if not path.is_absolute():
        return path.as_posix()
    try:
        return path.resolve().relative_to(repo_root).as_posix()
    except ValueError:
        return path.resolve().as_posix()


def is_relevant_file(path: str) -> bool:
    return Path(path).suffix.lower() in RELEVANT_EXTENSIONS


def is_header(path: str) -> bool:
    return Path(path).suffix.lower() in HEADER_EXTENSIONS


def is_source(path: str) -> bool:
    return Path(path).suffix.lower() in SOURCE_EXTENSIONS


def resolve_base_ref(repo_root: Path, explicit: Optional[str]) -> Tuple[str, str]:
    if explicit:
        return explicit, "explicit --base-ref"

    env = os.environ
    candidates: List[Tuple[str, str]] = []

    if env.get("GITHUB_EVENT_NAME") == "pull_request":
        base_ref = env.get("GITHUB_BASE_REF", "").strip()
        if base_ref:
            candidates.append((f"origin/{base_ref}", "GITHUB_BASE_REF"))
            candidates.append((base_ref, "GITHUB_BASE_REF"))

    before_sha = env.get("GITHUB_EVENT_BEFORE", "").strip()
    if before_sha and before_sha != ZERO_SHA:
        candidates.append((before_sha, "GITHUB_EVENT_BEFORE"))

    gitlab_base = env.get("CI_MERGE_REQUEST_DIFF_BASE_SHA", "").strip()
    if gitlab_base:
        candidates.append((gitlab_base, "CI_MERGE_REQUEST_DIFF_BASE_SHA"))

    target_branch = env.get("CI_MERGE_REQUEST_TARGET_BRANCH_NAME", "").strip()
    if target_branch:
        candidates.append((f"origin/{target_branch}", "CI_MERGE_REQUEST_TARGET_BRANCH_NAME"))
        candidates.append((target_branch, "CI_MERGE_REQUEST_TARGET_BRANCH_NAME"))

    candidates.extend(
        [
            ("@{upstream}", "local upstream"),
            ("origin/HEAD", "origin default branch"),
            ("origin/main", "origin/main"),
            ("origin/master", "origin/master"),
            ("main", "main"),
            ("master", "master"),
            ("HEAD~1", "previous commit"),
        ]
    )

    seen: Set[str] = set()
    for candidate, reason in candidates:
        if candidate in seen:
            continue
        seen.add(candidate)
        if git_exists(repo_root, candidate):
            return candidate, reason

    return EMPTY_TREE_SHA, "empty tree fallback"


def collect_changed_files(repo_root: Path, base_ref: str) -> List[str]:
    if base_ref == EMPTY_TREE_SHA:
        output = git_output(
            repo_root,
            ["diff", "--root", "--name-only", "--diff-filter=ACMR", "HEAD", "--"],
        )
    else:
        output = git_output(
            repo_root,
            ["diff", "--name-only", "--diff-filter=ACMR", f"{base_ref}...HEAD", "--"],
        )

    files = [line.strip() for line in output.splitlines() if line.strip()]
    return [canonical_repo_path(repo_root, path) for path in files if is_relevant_file(path)]


def collect_line_ranges(repo_root: Path, base_ref: str, files: Sequence[str]) -> Dict[str, List[LineRange]]:
    if not files:
        return {}

    if base_ref == EMPTY_TREE_SHA:
        args = [
            "diff",
            "--root",
            "--unified=0",
            "--no-color",
            "--no-ext-diff",
            "HEAD",
            "--",
            *files,
        ]
    else:
        args = [
            "diff",
            "--unified=0",
            "--no-color",
            "--no-ext-diff",
            f"{base_ref}...HEAD",
            "--",
            *files,
        ]

    output = git_output(repo_root, args)

    ranges: DefaultDict[str, List[LineRange]] = defaultdict(list)
    current_file: Optional[str] = None
    hunk_re = re.compile(r"^@@ -\d+(?:,\d+)? \+(\d+)(?:,(\d+))? @@")

    for raw_line in output.splitlines():
        line = raw_line.rstrip("\n")
        if line.startswith("+++ "):
            if line == "+++ /dev/null":
                current_file = None
            else:
                current_file = canonical_repo_path(repo_root, line[6:])
            continue

        if line.startswith("@@ ") and current_file:
            match = hunk_re.match(line)
            if not match:
                continue
            start = int(match.group(1))
            length = int(match.group(2) or "1")
            if length <= 0:
                continue
            ranges[current_file].append(LineRange(start=start, end=start + length - 1))

    return {path: merge_line_ranges(items) for path, items in ranges.items() if items}


def merge_line_ranges(ranges: Sequence[LineRange]) -> List[LineRange]:
    if not ranges:
        return []

    ordered = sorted(ranges, key=lambda item: (item.start, item.end))
    merged = [ordered[0]]

    for current in ordered[1:]:
        previous = merged[-1]
        if current.start <= previous.end + 1:
            merged[-1] = LineRange(start=previous.start, end=max(previous.end, current.end))
        else:
            merged.append(current)

    return merged


def load_compile_database(repo_root: Path, compile_commands: Path) -> Tuple[Path, Set[str]]:
    compile_commands = resolve_path(repo_root, str(compile_commands))
    if not compile_commands.is_file():
        raise FileNotFoundError(f"compile_commands.json not found at {compile_commands}")

    with compile_commands.open("r", encoding="utf-8") as handle:
        entries = json.load(handle)

    source_files: Set[str] = set()
    for entry in entries:
        file_value = entry.get("file")
        directory_value = entry.get("directory")
        if not file_value or not directory_value:
            continue
        source = Path(file_value)
        if not source.is_absolute():
            source = Path(directory_value) / source
        source_files.add(canonical_repo_path(repo_root, str(source)))

    return compile_commands, source_files


def find_header_analysis_targets(
    repo_root: Path,
    changed_headers: Sequence[str],
    source_files_in_db: Sequence[str],
) -> Set[str]:
    if not changed_headers:
        return set()

    header_tokens: Set[str] = set()
    for header in changed_headers:
        header_path = Path(header)
        header_tokens.add(header_path.as_posix())
        header_tokens.add(header_path.name)

    candidates: Set[str] = set()
    for source in source_files_in_db:
        source_path = resolve_path(repo_root, source)
        if not source_path.is_file():
            continue
        try:
            content = source_path.read_text(encoding="utf-8", errors="ignore")
        except OSError:
            continue

        if "#include" not in content:
            continue

        if any(token in content for token in header_tokens):
            candidates.add(source)

    return candidates


def build_line_filter(repo_root: Path, line_ranges: Dict[str, List[LineRange]]) -> str:
    payload = []
    for path in sorted(line_ranges):
        ranges = [[item.start, item.end] for item in line_ranges[path]]
        absolute = str((repo_root / path).resolve())
        payload.append({"name": path, "lines": ranges})
        payload.append({"name": absolute, "lines": ranges})
    return json.dumps(payload, separators=(",", ":"))


def resolve_clang_tidy_binary(explicit: Optional[str]) -> str:
    candidates: List[str] = []
    if explicit:
        candidates.append(explicit)

    env_binary = os.environ.get("CLANG_TIDY", "").strip()
    if env_binary:
        candidates.append(env_binary)

    candidates.extend(["clang-tidy", "clang-tidy-20", "clang-tidy-19", "clang-tidy-18"])

    for candidate in candidates:
        resolved = shutil.which(candidate)
        if resolved:
            return resolved

    raise FileNotFoundError(
        "clang-tidy not found. Set CLANG_TIDY or install clang-tidy in PATH."
    )


def parse_diagnostics(output: str, repo_root: Path) -> List[Diagnostic]:
    diagnostics: List[Diagnostic] = []
    diag_re = re.compile(
        r"^(?P<file>[^:\n]+):(?P<line>\d+):(?P<column>\d+):\s+"
        r"(?P<kind>warning|error):\s+(?P<message>.*?)(?:\s+\[(?P<check>[^\]]+)\])?$"
    )

    for raw_line in output.splitlines():
        match = diag_re.match(raw_line)
        if not match:
            continue

        file_path = canonical_repo_path(repo_root, match.group("file"))
        diagnostics.append(
            Diagnostic(
                file=file_path,
                line=int(match.group("line")),
                column=int(match.group("column")),
                kind=match.group("kind"),
                check=match.group("check") or "",
                message=match.group("message").strip(),
            )
        )

    return diagnostics


def unique_diagnostics(diagnostics: Iterable[Diagnostic]) -> List[Diagnostic]:
    seen: Set[Tuple[str, int, int, str, str, str]] = set()
    ordered: List[Diagnostic] = []
    for item in diagnostics:
        key = (item.file, item.line, item.column, item.kind, item.check, item.message)
        if key in seen:
            continue
        seen.add(key)
        ordered.append(item)
    return ordered


def run_clang_tidy(
    repo_root: Path,
    clang_tidy: str,
    compile_commands: Path,
    target: str,
    line_filter: str,
) -> Tuple[int, str]:
    command = [
        clang_tidy,
        "-p",
        str(compile_commands.parent),
        f"-line-filter={line_filter}",
        "-warnings-as-errors=*",
        "-quiet",
        target,
    ]
    result = subprocess.run(command, cwd=repo_root, text=True, capture_output=True)
    return result.returncode, (result.stdout or "") + (result.stderr or "")


def main(argv: Optional[Sequence[str]] = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--base-ref", help="Base ref or commit to diff against")
    parser.add_argument(
        "--compile-commands",
        default="compile_commands.json",
        help="Path to compile_commands.json",
    )
    parser.add_argument("--clang-tidy", help="Path to the clang-tidy binary")
    args = parser.parse_args(argv)

    repo_root = resolve_repo_root()
    base_ref, base_reason = resolve_base_ref(repo_root, args.base_ref)

    try:
        compile_commands, source_files_in_db = load_compile_database(
            repo_root, resolve_path(repo_root, args.compile_commands)
        )
    except FileNotFoundError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2

    try:
        changed_files = collect_changed_files(repo_root, base_ref)
    except subprocess.CalledProcessError as exc:
        print("error: failed to resolve changed files with git diff", file=sys.stderr)
        if exc.stderr:
            print(exc.stderr.strip(), file=sys.stderr)
        elif exc.stdout:
            print(exc.stdout.strip(), file=sys.stderr)
        return 2

    if not changed_files:
        print(f"clang-tidy diff: no relevant C/C++ files changed against {base_ref} ({base_reason}).")
        return 0

    changed_sources = [path for path in changed_files if is_source(path)]
    changed_headers = [path for path in changed_files if is_header(path)]

    missing_sources = [path for path in changed_sources if path not in source_files_in_db]
    if missing_sources:
        print("error: changed source files are missing from compile_commands.json:", file=sys.stderr)
        for path in missing_sources:
            print(f"  {path}", file=sys.stderr)
        return 2

    try:
        line_ranges = collect_line_ranges(repo_root, base_ref, changed_files)
    except subprocess.CalledProcessError as exc:
        print("error: failed to compute changed line ranges with git diff", file=sys.stderr)
        if exc.stderr:
            print(exc.stderr.strip(), file=sys.stderr)
        elif exc.stdout:
            print(exc.stdout.strip(), file=sys.stderr)
        return 2

    if not line_ranges:
        print(
            f"clang-tidy diff: relevant files changed against {base_ref} ({base_reason}), "
            "but no added or modified lines were found."
        )
        return 0

    targets: Set[str] = set(changed_sources)
    if changed_headers:
        header_targets = find_header_analysis_targets(repo_root, changed_headers, sorted(source_files_in_db))
        if header_targets:
            targets.update(header_targets)
        elif changed_sources:
            print(
                "clang-tidy diff: no translation-unit targets found for changed headers; "
                "falling back to changed source files only."
            )
        else:
            print(
                "clang-tidy diff: changed headers detected but no translation-unit targets "
                "were found in compile_commands.json."
            )
            return 0

    if not targets:
        print(
            f"clang-tidy diff: relevant files changed against {base_ref} ({base_reason}), "
            "but no analysis targets were resolved."
        )
        return 0

    try:
        clang_tidy = resolve_clang_tidy_binary(args.clang_tidy)
    except FileNotFoundError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2

    line_filter = build_line_filter(repo_root, line_ranges)
    all_diagnostics: List[Diagnostic] = []
    tool_failures: List[Tuple[str, int, str]] = []

    print(f"clang-tidy diff base: {base_ref} ({base_reason})")
    print(f"Relevant files: {len(changed_files)}")
    print(f"Analysis targets: {len(targets)}")

    for target in sorted(targets):
        returncode, output = run_clang_tidy(
            repo_root,
            clang_tidy,
            compile_commands,
            target,
            line_filter,
        )
        diagnostics = parse_diagnostics(output, repo_root)
        if diagnostics:
            all_diagnostics.extend(diagnostics)
        if returncode != 0 and not diagnostics:
            tool_failures.append((target, returncode, output.strip()))

    all_diagnostics = unique_diagnostics(all_diagnostics)

    if not all_diagnostics and not tool_failures:
        print("clang-tidy diff: clean on changed lines.")
        return 0

    if all_diagnostics:
        print(f"clang-tidy diagnostics on changed lines: {len(all_diagnostics)}")
        for diagnostic in all_diagnostics[:50]:
            check = f" [{diagnostic.check}]" if diagnostic.check else ""
            print(
                f"  {diagnostic.file}:{diagnostic.line}:{diagnostic.column}: "
                f"{diagnostic.kind}:{check} {diagnostic.message}"
            )
        if len(all_diagnostics) > 50:
            print(f"  ... {len(all_diagnostics) - 50} more diagnostics omitted")

    if tool_failures:
        print("clang-tidy execution failures:", file=sys.stderr)
        for target, code, output in tool_failures:
            print(f"  {target} (exit {code})", file=sys.stderr)
            if output:
                snippet = output.splitlines()[0]
                print(f"    {snippet}", file=sys.stderr)

    return 1


if __name__ == "__main__":
    raise SystemExit(main())
