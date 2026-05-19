#!/usr/bin/env python3
"""Validate the Gaussian ProjectSettings manifest against source use.

The manifest is a contract inventory, not a behavior generator. This check
keeps it in sync with production C++ source paths that mention
rendering/gaussian_splatting/* settings, including common static String path
constants built from section prefixes.
"""

from __future__ import annotations

import json
import re
import sys
from pathlib import Path
from typing import Iterable

REPO_ROOT = Path(__file__).resolve().parents[3]
MODULE_ROOT = REPO_ROOT / "modules" / "gaussian_splatting"
MANIFEST_PATH = MODULE_ROOT / "config" / "project_settings_manifest.json"
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


def main() -> int:
    try:
        manifest = _load_manifest()
        failures = _validate_schema(manifest)
        manifest_keys = {entry["key"] for entry in manifest.get("settings", []) if isinstance(entry, dict) and "key" in entry}
        source_keys = collect_source_setting_paths()

        missing_from_manifest = sorted(source_keys - manifest_keys)
        stale_manifest_entries = sorted(manifest_keys - source_keys)

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
