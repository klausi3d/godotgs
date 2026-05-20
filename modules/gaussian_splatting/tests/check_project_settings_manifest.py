#!/usr/bin/env python3
"""Validate the Gaussian ProjectSettings manifest against source use.

The manifest is a contract inventory, not a behavior generator. This check
keeps it in sync with production C++ source paths that mention
rendering/gaussian_splatting/* settings, including common static String path
constants built from section prefixes.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import subprocess
import sys
from pathlib import Path
from typing import Iterable

REPO_ROOT = Path(__file__).resolve().parents[3]
MODULE_ROOT = REPO_ROOT / "modules" / "gaussian_splatting"
MANIFEST_PATH = MODULE_ROOT / "config" / "project_settings_manifest.json"
PUBLIC_API_BASELINE_PATH = MODULE_ROOT / "config" / "project_settings_public_api_baseline.json"
PUBLIC_API_BASELINE_REPO_PATH = PUBLIC_API_BASELINE_PATH.relative_to(REPO_ROOT).as_posix()
SETTING_PREFIX = "rendering/gaussian_splatting/"

SOURCE_SUFFIXES = {".cpp", ".h"}
SOURCE_EXCLUDE_PARTS = {"tests", "doc_classes"}

STRING_LITERAL_RE = re.compile(r'"([^"\\]*(?:\\.[^"\\]*)*)"')
PATH_ASSIGNMENT_RE = re.compile(
    r"(?:const\s+String|static\s+const\s+String|constexpr\s+const\s+char\s*\*|"
    r"constexpr\s+char|static\s+constexpr\s+char|const\s+char\s*\*)\s+"
    r"((?:[A-Za-z_][A-Za-z0-9_]*::)?[A-Za-z_][A-Za-z0-9_]*)\s*(?:\[\])?\s*=\s*([^;]+);"
)
DEFINE_RE = re.compile(r"^\s*#define\s+([A-Za-z_][A-Za-z0-9_]*)\s+(.+)$")
TOKEN_RE = re.compile(
    r'"([^"\\]*(?:\\.[^"\\]*)*)"|((?:[A-Za-z_][A-Za-z0-9_]*::)?[A-Za-z_][A-Za-z0-9_]*)'
)

IGNORED_TOKENS = {
    "String",
    "StringName",
    "PropertyInfo",
    "Variant",
    "INT",
    "BOOL",
    "FLOAT",
    "PROPERTY_HINT_RANGE",
    "PROPERTY_HINT_ENUM",
    "PROPERTY_HINT_NONE",
    "PROPERTY_USAGE_NO_EDITOR",
    "PROPERTY_USAGE_STORAGE",
    "GPUSortingConstants",
}

REQUIRED_FIELDS = {
    "owner",
    "scope",
    "reload_semantics",
    "effective_state",
    "visibility",
    "publicness",
    "test_coverage",
}

ENUMS = {
    "scope": {
        "runtime",
        "diagnostic",
        "debug",
        "editor",
        "import",
        "migration_alias",
        "internal",
        "compatibility",
    },
    "reload_semantics": {
        "startup",
        "settings_changed",
        "per_frame",
        "on_demand",
        "save_only",
        "test_only",
        "unknown",
    },
    "visibility": {"editor_visible", "hidden_no_editor", "runtime_only", "dynamic_optional"},
    "publicness": {"public", "internal", "debug_only", "deprecated_alias", "cleanup_candidate"},
    "test_coverage": {
        "covered",
        "partial",
        "inventory_only",
        "documented_gap",
        "needs_behavior_test",
        "test_only",
    },
}

PUBLIC_API_PUBLICNESS = {"public", "cleanup_candidate", "deprecated_alias"}
SOURCE_PARITY_EXEMPT_PUBLICNESS = {"deprecated_alias"}
SOURCE_PARITY_EXEMPT_SCOPES = {"migration_alias"}
RETIREMENT_STATUS_VALUES = {"deprecated", "removed", "migration_alias"}
RETIRED_SETTING_REQUIRED_FIELDS = {"key", "status", "removed_in", "reason", "migration"}
BASE_REF_ENV = "GS_PROJECT_SETTINGS_MANIFEST_BASE_REF"
BASE_REF_CANDIDATES = ("origin/main", "main", "origin/master", "master")


class ContractError(RuntimeError):
    pass


def _read_text(path: Path) -> str:
    try:
        return path.read_text(encoding="utf-8")
    except OSError as exc:
        raise ContractError(f"Failed reading '{path}': {exc}") from exc


def _decode_literal(value: str) -> str:
    return bytes(value, "utf-8").decode("unicode_escape", errors="ignore")


def _production_sources() -> Iterable[Path]:
    for path in MODULE_ROOT.rglob("*"):
        if path.suffix not in SOURCE_SUFFIXES:
            continue
        relative_parts = set(path.relative_to(MODULE_ROOT).parts)
        if relative_parts & SOURCE_EXCLUDE_PARTS:
            continue
        yield path


def _is_full_setting_path(value: str) -> bool:
    return (
        value.startswith(SETTING_PREFIX)
        and not value.endswith("/")
        and " " not in value
        and "`" not in value
        and "'" not in value
    )


def _collect_path_expressions() -> tuple[set[str], dict[str, str]]:
    literal_paths: set[str] = set()
    expressions: dict[str, str] = {}

    for path in _production_sources():
        text = _read_text(path)
        for match in STRING_LITERAL_RE.finditer(text):
            literal = _decode_literal(match.group(1))
            if _is_full_setting_path(literal):
                literal_paths.add(literal)

        for line in text.splitlines():
            assignment = PATH_ASSIGNMENT_RE.search(line)
            if assignment:
                expressions[assignment.group(1)] = assignment.group(2).strip()
            define = DEFINE_RE.match(line)
            if define:
                expressions[define.group(1)] = define.group(2).strip()

    return literal_paths, expressions


def _evaluate_expression(
    expression: str,
    expressions: dict[str, str],
    context: str,
    cache: dict[tuple[str, str], str | None],
    depth: int = 0,
) -> str | None:
    cache_key = (context, expression)
    if cache_key in cache:
        return cache[cache_key]
    if depth > 20:
        cache[cache_key] = None
        return None

    expression = re.sub(r"\bStringName\(([^()]*)\)", r"\1", expression.strip())
    expression = re.sub(r"\bString\(([^()]*)\)", r"\1", expression)

    value = ""
    had_token = False
    for match in TOKEN_RE.finditer(expression):
        string_value, name = match.groups()
        if string_value is not None:
            value += _decode_literal(string_value)
            had_token = True
            continue

        if name in IGNORED_TOKENS:
            continue
        candidates = [name] if "::" in name else ([f"{context}::{name}"] if context else []) + [name]
        resolved = None
        for candidate in candidates:
            if candidate not in expressions:
                continue
            next_context = candidate.split("::", 1)[0] if "::" in candidate else context
            resolved = _evaluate_expression(
                expressions[candidate],
                expressions,
                next_context,
                cache,
                depth + 1,
            )
            break
        if resolved is None:
            cache[cache_key] = None
            return None
        value += resolved
        had_token = True

    cache[cache_key] = value if had_token else None
    return cache[cache_key]


def collect_source_setting_paths() -> set[str]:
    literal_paths, expressions = _collect_path_expressions()
    resolved_paths = set(literal_paths)
    cache: dict[tuple[str, str], str | None] = {}

    for name, expression in expressions.items():
        context = name.split("::", 1)[0] if "::" in name else ""
        value = _evaluate_expression(expression, expressions, context, cache)
        if value and _is_full_setting_path(value):
            resolved_paths.add(value)

    return resolved_paths


def _load_manifest() -> dict:
    try:
        data = json.loads(_read_text(MANIFEST_PATH))
    except json.JSONDecodeError as exc:
        raise ContractError(f"{MANIFEST_PATH}: invalid JSON: {exc}") from exc
    if not isinstance(data, dict):
        raise ContractError(f"{MANIFEST_PATH}: root must be a JSON object")
    return data


def _load_public_api_baseline() -> dict:
    try:
        data = json.loads(_read_text(PUBLIC_API_BASELINE_PATH))
    except json.JSONDecodeError as exc:
        raise ContractError(f"{PUBLIC_API_BASELINE_PATH}: invalid JSON: {exc}") from exc
    if not isinstance(data, dict):
        raise ContractError(f"{PUBLIC_API_BASELINE_PATH}: root must be a JSON object")
    return data


def _run_git(args: list[str]) -> tuple[int, str, str]:
    try:
        result = subprocess.run(
            ["git", *args],
            cwd=REPO_ROOT,
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="replace",
            check=False,
        )
    except OSError as exc:
        return 1, "", str(exc)
    return result.returncode, result.stdout or "", result.stderr or ""


def _resolve_base_ref() -> str | None:
    explicit_ref = os.environ.get(BASE_REF_ENV)
    if explicit_ref:
        return explicit_ref

    for candidate in BASE_REF_CANDIDATES:
        code, out, _ = _run_git(["merge-base", "HEAD", candidate])
        if code == 0 and out.strip():
            return out.strip()
    return None


def _baseline_path_missing(stderr: str) -> bool:
    normalized_stderr = stderr.lower()
    baseline_path = PUBLIC_API_BASELINE_REPO_PATH.lower()
    if baseline_path not in normalized_stderr:
        return False
    return "exists on disk, but not in" in normalized_stderr or "does not exist in" in normalized_stderr


def _parse_base_public_api_baseline(text: str, base_ref: str) -> tuple[dict | None, list[str]]:
    try:
        data = json.loads(text)
    except json.JSONDecodeError as exc:
        return None, [f"{PUBLIC_API_BASELINE_REPO_PATH}: base copy at {base_ref} is invalid JSON: {exc}"]
    if not isinstance(data, dict):
        return None, [f"{PUBLIC_API_BASELINE_REPO_PATH}: base copy at {base_ref} root must be a JSON object"]
    return data, []


def _load_base_public_api_baseline() -> tuple[dict | None, list[str]]:
    base_ref = _resolve_base_ref()
    if not base_ref:
        return None, []

    code, out, err = _run_git(["show", f"{base_ref}:{PUBLIC_API_BASELINE_REPO_PATH}"])
    if code == 0:
        return _parse_base_public_api_baseline(out, base_ref)

    # The first PR that introduces the baseline has no base copy to compare.
    if _baseline_path_missing(err):
        return None, []
    return None, [f"{PUBLIC_API_BASELINE_REPO_PATH}: failed reading base copy at {base_ref}: {err.strip()}"]


def _family_defaults(manifest: dict, key: str) -> dict:
    matches = []
    for family in manifest.get("families", []):
        prefix = family.get("prefix")
        if isinstance(prefix, str) and key.startswith(prefix):
            matches.append(family)
    if not matches:
        return {}
    matches.sort(key=lambda item: len(item["prefix"]))
    result = dict(matches[-1])
    result.pop("prefix", None)
    return result


def _resolved_entry(manifest: dict, entry: dict) -> dict:
    key = entry["key"]
    resolved = _family_defaults(manifest, key)
    resolved.update(entry)
    return resolved


def _resolved_entries_by_key(manifest: dict) -> dict[str, dict]:
    entries: dict[str, dict] = {}
    for entry in manifest.get("settings", []):
        if not isinstance(entry, dict):
            continue
        key = entry.get("key")
        if isinstance(key, str) and _is_full_setting_path(key):
            entries[key] = _resolved_entry(manifest, entry)
    return entries


def _is_source_parity_exempt(entry: dict) -> bool:
    return (
        entry.get("publicness") in SOURCE_PARITY_EXEMPT_PUBLICNESS
        or entry.get("scope") in SOURCE_PARITY_EXEMPT_SCOPES
    )


def _validate_schema(manifest: dict) -> list[str]:
    failures: list[str] = []
    if manifest.get("root_prefix") != SETTING_PREFIX:
        failures.append(f"{MANIFEST_PATH}: root_prefix must be '{SETTING_PREFIX}'")

    entries = manifest.get("settings")
    if not isinstance(entries, list):
        failures.append(f"{MANIFEST_PATH}: settings must be a list")
        return failures

    seen: set[str] = set()
    ordered_keys: list[str] = []
    for index, entry in enumerate(entries):
        if not isinstance(entry, dict):
            failures.append(f"{MANIFEST_PATH}: settings[{index}] must be an object")
            continue
        key = entry.get("key")
        if not isinstance(key, str) or not _is_full_setting_path(key):
            failures.append(f"{MANIFEST_PATH}: settings[{index}] has invalid key '{key}'")
            continue
        if key in seen:
            failures.append(f"{MANIFEST_PATH}: duplicate setting key '{key}'")
        seen.add(key)
        ordered_keys.append(key)

        resolved = _resolved_entry(manifest, entry)
        missing = sorted(field for field in REQUIRED_FIELDS if not resolved.get(field))
        if missing:
            failures.append(f"{key}: missing classification fields: {', '.join(missing)}")
        for field, allowed in ENUMS.items():
            value = resolved.get(field)
            if value and value not in allowed:
                failures.append(f"{key}: invalid {field} '{value}'")

        if resolved.get("publicness") == "cleanup_candidate" and not resolved.get("notes"):
            failures.append(f"{key}: cleanup_candidate entries must explain the candidate in notes")
        if "quality/tier_" in key and "policy" not in resolved.get("notes", "").lower():
            failures.append(f"{key}: quality tier entries must document policy override semantics in notes")
        if resolved.get("test_coverage") in {"documented_gap", "needs_behavior_test"} and not resolved.get("notes"):
            failures.append(f"{key}: documented gaps must include notes")

    if ordered_keys != sorted(ordered_keys):
        failures.append(f"{MANIFEST_PATH}: settings must be sorted by key")

    return failures


def _baseline_public_keys(baseline: dict) -> set[str]:
    return {
        key
        for key in baseline.get("public_settings", [])
        if isinstance(key, str) and _is_full_setting_path(key)
    }


def _coerce_baseline_list(
    baseline: dict,
    field_name: str,
    *,
    default_to_empty: bool = False,
) -> tuple[list, list[str]]:
    value = baseline.get(field_name, [] if default_to_empty else None)
    if isinstance(value, list):
        return value, []
    return [], [f"{PUBLIC_API_BASELINE_PATH}: {field_name} must be a list"]


def _validate_public_settings(public_settings: list) -> tuple[set[str], list[str]]:
    failures: list[str] = []
    baseline_keys: set[str] = set()
    ordered_public_keys: list[str] = []

    for index, key in enumerate(public_settings):
        if not isinstance(key, str) or not _is_full_setting_path(key):
            failures.append(f"{PUBLIC_API_BASELINE_PATH}: public_settings[{index}] has invalid key '{key}'")
            continue
        if key in baseline_keys:
            failures.append(f"{PUBLIC_API_BASELINE_PATH}: duplicate public API baseline key '{key}'")
        baseline_keys.add(key)
        ordered_public_keys.append(key)

    if ordered_public_keys != sorted(ordered_public_keys):
        failures.append(f"{PUBLIC_API_BASELINE_PATH}: public_settings must be sorted by key")

    return baseline_keys, failures


def _validate_retired_setting_fields(key: str, record: dict) -> list[str]:
    failures: list[str] = []
    missing = sorted(field for field in RETIRED_SETTING_REQUIRED_FIELDS if not record.get(field))
    if missing:
        failures.append(f"{key}: retired settings must include {', '.join(missing)}")
    status = record.get("status")
    if status not in RETIREMENT_STATUS_VALUES:
        failures.append(f"{key}: invalid retired setting status '{status}'")
    replacement = record.get("replacement")
    if replacement is not None and (not isinstance(replacement, str) or not _is_full_setting_path(replacement)):
        failures.append(f"{key}: replacement must be null or a full Gaussian setting key")
    return failures


def _validate_retired_settings(retired_settings: list) -> tuple[set[str], list[str]]:
    failures: list[str] = []
    retired_keys: set[str] = set()
    ordered_retired_keys: list[str] = []

    for index, record in enumerate(retired_settings):
        if not isinstance(record, dict):
            failures.append(f"{PUBLIC_API_BASELINE_PATH}: retired_settings[{index}] must be an object")
            continue
        key = record.get("key")
        if not isinstance(key, str) or not _is_full_setting_path(key):
            failures.append(f"{PUBLIC_API_BASELINE_PATH}: retired_settings[{index}] has invalid key '{key}'")
            continue
        if key in retired_keys:
            failures.append(f"{PUBLIC_API_BASELINE_PATH}: duplicate retired setting key '{key}'")
        retired_keys.add(key)
        ordered_retired_keys.append(key)
        failures.extend(_validate_retired_setting_fields(key, record))

    if ordered_retired_keys != sorted(ordered_retired_keys):
        failures.append(f"{PUBLIC_API_BASELINE_PATH}: retired_settings must be sorted by key")

    return retired_keys, failures


def _validate_base_baseline_deletions(
    baseline_keys: set[str],
    base_baseline: dict | None,
) -> list[str]:
    if base_baseline is None:
        return []

    failures: list[str] = []
    removed_baseline_keys = sorted(_baseline_public_keys(base_baseline) - baseline_keys)
    for key in removed_baseline_keys:
        failures.append(
            f"{key}: public API baseline entries must not be deleted; "
            "add a retired_settings record instead"
        )
    return failures


def _validate_public_api_manifest_alignment(
    manifest: dict,
    baseline_keys: set[str],
    retired_keys: set[str],
) -> list[str]:
    failures: list[str] = []
    resolved_entries = _resolved_entries_by_key(manifest)
    manifest_keys = set(resolved_entries)
    live_public_keys = {
        key
        for key, entry in resolved_entries.items()
        if entry.get("publicness") in PUBLIC_API_PUBLICNESS
    }

    for key in sorted(live_public_keys - baseline_keys):
        failures.append(f"{key}: live public setting is missing from public API baseline")
    for key in sorted(retired_keys - baseline_keys):
        failures.append(f"{key}: retired setting must remain listed in public_settings baseline")
    for key in sorted(retired_keys & manifest_keys):
        failures.append(f"{key}: retired setting records must not also be live manifest settings")
    for key in sorted(baseline_keys - live_public_keys - retired_keys):
        failures.append(f"{key}: public API baseline key is neither live nor retired")
    return failures


def _validate_public_api_baseline(manifest: dict, baseline: dict, base_baseline: dict | None) -> list[str]:
    failures: list[str] = []

    if baseline.get("schema_version") != 1:
        failures.append(f"{PUBLIC_API_BASELINE_PATH}: schema_version must be 1")

    public_settings, public_settings_failures = _coerce_baseline_list(baseline, "public_settings")
    failures.extend(public_settings_failures)
    retired_settings, retired_settings_failures = _coerce_baseline_list(
        baseline,
        "retired_settings",
        default_to_empty=True,
    )
    failures.extend(retired_settings_failures)

    baseline_keys, public_key_failures = _validate_public_settings(public_settings)
    failures.extend(public_key_failures)
    retired_keys, retired_key_failures = _validate_retired_settings(retired_settings)
    failures.extend(retired_key_failures)
    failures.extend(_validate_base_baseline_deletions(baseline_keys, base_baseline))
    failures.extend(_validate_public_api_manifest_alignment(manifest, baseline_keys, retired_keys))

    return failures


def _self_test_public_api_guard() -> list[str]:
    key = f"{SETTING_PREFIX}example/public_setting"
    manifest = {
        "root_prefix": SETTING_PREFIX,
        "settings": [
            {
                "effective_state": "ExampleConfig.enabled",
                "key": key,
                "owner": "self-test",
                "publicness": "public",
                "reload_semantics": "settings_changed",
                "scope": "runtime",
                "test_coverage": "covered",
                "visibility": "editor_visible",
            }
        ],
    }
    retired_record = {
        "key": key,
        "migration": "No automatic migration; setting was removed intentionally.",
        "reason": "Self-test retirement record.",
        "removed_in": "self-test",
        "replacement": None,
        "status": "removed",
    }

    cases = [
        (
            "requires baseline entry for live public setting",
            manifest,
            {"schema_version": 1, "public_settings": [], "retired_settings": []},
            None,
            "missing from public API baseline",
        ),
        (
            "rejects unrecorded baseline orphan",
            {"root_prefix": SETTING_PREFIX, "settings": []},
            {"schema_version": 1, "public_settings": [key], "retired_settings": []},
            None,
            "neither live nor retired",
        ),
        (
            "accepts explicit retirement record",
            {"root_prefix": SETTING_PREFIX, "settings": []},
            {"schema_version": 1, "public_settings": [key], "retired_settings": [retired_record]},
            None,
            None,
        ),
        (
            "rejects baseline deletion against base",
            {"root_prefix": SETTING_PREFIX, "settings": []},
            {"schema_version": 1, "public_settings": [], "retired_settings": []},
            {"schema_version": 1, "public_settings": [key], "retired_settings": []},
            "must not be deleted",
        ),
    ]

    failures: list[str] = []
    if BASE_REF_CANDIDATES.index("origin/main") > BASE_REF_CANDIDATES.index("origin/master"):
        failures.append("base ref resolution must prefer origin/main before origin/master")
    if BASE_REF_CANDIDATES.index("main") > BASE_REF_CANDIDATES.index("master"):
        failures.append("base ref resolution must prefer main before master")
    baseline_missing_samples = (
        f"fatal: path '{PUBLIC_API_BASELINE_REPO_PATH}' exists on disk, but not in 'base'",
        f"fatal: Path '{PUBLIC_API_BASELINE_REPO_PATH}' does not exist in 'base'",
    )
    for sample in baseline_missing_samples:
        if not _baseline_path_missing(sample):
            failures.append(f"baseline missing matcher rejected valid git stderr: {sample}")
    unrelated_git_error = "fatal: ambiguous argument 'bad:path': unknown revision or path not in the working tree."
    if _baseline_path_missing(unrelated_git_error):
        failures.append("baseline missing matcher accepted unrelated git path error")

    for name, test_manifest, baseline, base_baseline, expected_fragment in cases:
        case_failures = _validate_public_api_baseline(test_manifest, baseline, base_baseline)
        if expected_fragment is None:
            if case_failures:
                failures.append(f"{name}: expected pass, got {case_failures}")
        elif not any(expected_fragment in failure for failure in case_failures):
            failures.append(f"{name}: expected failure containing '{expected_fragment}', got {case_failures}")
    return failures


def _parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--self-test", action="store_true", help="run internal public API guard self-tests")
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = _parse_args(sys.argv[1:] if argv is None else argv)
    if args.self_test:
        failures = _self_test_public_api_guard()
        if failures:
            print("Gaussian ProjectSettings manifest guard self-test failed:", file=sys.stderr)
            for failure in failures:
                print(f"  - {failure}", file=sys.stderr)
            return 1
        print("Gaussian ProjectSettings manifest guard self-test OK.")
        return 0

    try:
        manifest = _load_manifest()
        baseline = _load_public_api_baseline()
        base_baseline, base_baseline_failures = _load_base_public_api_baseline()
        failures = _validate_schema(manifest)
        failures.extend(base_baseline_failures)
        failures.extend(_validate_public_api_baseline(manifest, baseline, base_baseline))
        manifest_keys = {
            entry["key"]
            for entry in manifest.get("settings", [])
            if isinstance(entry, dict) and "key" in entry
        }
        resolved_entries = _resolved_entries_by_key(manifest)
        source_keys = collect_source_setting_paths()

        missing_from_manifest = sorted(source_keys - manifest_keys)
        source_parity_exempt_keys = {
            key for key, entry in resolved_entries.items() if _is_source_parity_exempt(entry)
        }
        stale_manifest_entries = sorted(manifest_keys - source_keys - source_parity_exempt_keys)

        for key in missing_from_manifest:
            failures.append(f"{key}: referenced by Gaussian source but missing from project_settings_manifest.json")
        for key in stale_manifest_entries:
            failures.append(f"{key}: listed in manifest but no longer referenced by Gaussian production source")
    except ContractError as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 1

    if failures:
        print("Gaussian ProjectSettings manifest check failed:", file=sys.stderr)
        for failure in failures:
            print(f"  - {failure}", file=sys.stderr)
        return 1

    print(f"Gaussian ProjectSettings manifest OK ({len(manifest_keys)} settings).")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
