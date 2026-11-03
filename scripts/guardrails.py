#!/usr/bin/env python3
"""Ratchet Orb's handwritten firmware boundaries without changing production code.

This is a lexical C guard, not a compiler: comments/strings are masked, included
.inc bodies are expanded for translation-unit/function limits, and dependencies
are resolved against repository and locally installed ESP-IDF header owners.
The checked-in debt can only shrink, including during explicit regeneration.
"""

from __future__ import annotations

import argparse
import hashlib
import importlib.util
import json
import os
import re
import sys
from collections import Counter, defaultdict
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BASELINE = ROOT / "scripts" / "guardrails-baseline.json"
LIMITS = {"file_size": 784, "function_size": 100}
EXTENSIONS = {".c", ".h", ".inc"}
INCLUDE = re.compile(r'^\s*#\s*include\s*[<"]([^>"\n]+)[>"]', re.M)
CONTROL = re.compile(r"\blv_(?:button|slider|switch|arc)_create\s*\(")
# A function-pointer alias is still direct ownership of the global crown.
ROTARY_OWNER = re.compile(r"\borb_nav_rotary_(?:bind|unbind)\b")
APP_CALL = re.compile(r"\b(orb_app_[A-Za-z0-9_]+)\s*\(")
COLOUR = re.compile(
    r"\blv_color_hex(?:3)?\s*\([^;\n]*?\)|\b0x[0-9a-fA-F]{6}[uUlL]*\b|"
    r"\.(?:red|green|blue)\s*=\s*(?:0x[\da-fA-F]+|\d+)\b"
)
RGB_CALL = re.compile(r"\b(?:lv_color(?:32)?_make|LV_COLOR_MAKE)\s*\(")
PALETTE = re.compile(r"\blv_palette_(?:main|lighten|darken)\b")
TASK_CREATE_CAPS = re.compile(r"\bxTaskCreate\w*WithCaps\s*\(")
FLASH_CALL = re.compile(r"\b((?:nvs_|esp_partition_|esp_flash_|spi_flash_|esp_ota_)\w+)\s*\(")
SERVICES = {
    "orb_apps",
    "orb_fs",
    "orb_home_data",
    "orb_link",
    "orb_mem",
    "orb_memory",
    "orb_model",
    "orb_net",
    "orb_ota",
    "orb_power",
    "orb_settings",
    "orb_sound",
    "orb_time",
    "orb_voice",
}
TOKEN_FILES = {
    "orb_tokens/include/orb_color_tokens.h",
    "orb_ui/include/orb_ui.h",
    "orb_ui/include/orb_kit.h",
}
# Only artwork producers are exempt. Controls in these same files are checked.
ART_FUNCTIONS = {
    "orb_games/orb_game_play.c": {"disc", "arc", "star", "draw_miniature", "draw_peek"},
    "orb_games/orb_game_view.c": {"draw", "keepsake_sprite"},
    "orb_games/orb_game_sprite_draw.c": {"shape", "draw_gate", "draw_star", "game_sprite_draw"},
    "orb_faces/orb_faces.c": {"face_accent"},
}
ART_FILES = {"orb_games/orb_game_logic.c"}
GENERATED_FILES = {
    "orb_fonts/src/orb_fonts_table.c",
    "orb_fonts/src/orb_fonts_notices.c",
}
GENERATED_DIRS = {"orb_fonts/fonts", "orb_shell/fonts"}
BUILD_HEADERS = {"sdkconfig.h", "esp_idf_version.h"}
# C/POSIX runtime headers are supplied by the toolchain, not a chosen component.
RUNTIME_HEADERS = {
    "assert.h",
    "complex.h",
    "ctype.h",
    "errno.h",
    "fcntl.h",
    "fenv.h",
    "float.h",
    "inttypes.h",
    "limits.h",
    "locale.h",
    "math.h",
    "pthread.h",
    "sched.h",
    "setjmp.h",
    "signal.h",
    "stdalign.h",
    "stdarg.h",
    "stdatomic.h",
    "stdbool.h",
    "stddef.h",
    "stdint.h",
    "stdio.h",
    "stdlib.h",
    "stdnoreturn.h",
    "string.h",
    "strings.h",
    "threads.h",
    "time.h",
    "uchar.h",
    "unistd.h",
    "wchar.h",
    "wctype.h",
}


def load_privacy():
    spec = importlib.util.spec_from_file_location(
        "orb_kit_privacy", ROOT / "sim" / "tests" / "test_kit_privacy.py"
    )
    module = importlib.util.module_from_spec(spec)
    assert spec.loader
    spec.loader.exec_module(module)
    # Include typed/name lookup and typed counts: all expose kit topology.
    module.CHILD = re.compile(
        r"\blv_obj_get_child(?:_by_type|_by_name|_count(?:_by_type)?)?"
        r"\s*\(\s*((?:\([^()\n]*\)|[^,\n)])+)(?:,|\))"
    )
    return module


PRIVACY = load_privacy()


def component(path: Path, root: Path) -> str | None:
    try:
        relative = path.resolve().relative_to(root.resolve() / "firmware" / "components")
        return relative.parts[0]
    except ValueError:
        return "main" if path.is_relative_to(root / "firmware" / "main") else None


def production_files(root: Path) -> list[Path]:
    result = []
    for directory in (root / "firmware" / "components", root / "firmware" / "main"):
        for path in directory.rglob("*"):
            if path.suffix not in EXTENSIONS or {"tests", "test_app"} & set(path.parts):
                continue
            relative = path.relative_to(directory).as_posix()
            if relative in GENERATED_FILES or any(
                relative.startswith(prefix + "/") for prefix in GENERATED_DIRS
            ):
                continue
            result.append(path)
    return sorted(result)


def masked(source: str, *, keep_strings: bool = False) -> str:
    if not keep_strings:
        return PRIVACY.code_only(source)
    # Includes need their quoted names, but comments must never create includes.
    pattern = re.compile(r'/\*.*?\*/|//[^\n]*|"(?:\\.|[^"\\])*"', re.S)
    return pattern.sub(
        lambda match: match[0] if match[0].startswith('"') else re.sub(r"[^\n]", " ", match[0]),
        source,
    )


def functions(source: str) -> list[tuple[str, int, int]]:
    """Find top-level C bodies while ignoring initializers and typedefs."""
    # Mask directives so body offsets retain their source locations.
    code = re.sub(
        r"(?m)^\s*#.*(?:\\\n.*)*",
        lambda match: re.sub(r"[^\n]", " ", match[0]),
        masked(source),
    )
    depth = 0
    previous = 0
    active = None
    result = []
    for match in re.finditer(r"[{};]", code):
        token = match[0]
        if token == "{" and depth == 0:
            signature = code[previous : match.start()]
            name = re.search(r"\b(\w+)\s*\([^;{}]*\)\s*$", signature)
            if name and name[1] not in {"if", "for", "while", "switch"}:
                active = (name[1], match.start())
        if token == "{":
            depth += 1
        elif token == "}":
            depth -= 1
            if depth == 0:
                if active:
                    result.append((*active, match.end()))
                    active = None
                previous = match.end()
        elif depth == 0:
            previous = match.end()
    return result


def logical_lines(body: str) -> int:
    """Count statements plus control/declaration lines, never blank/comment lines.

    Multiple statements on one physical line count separately, so compressing a
    function cannot evade the ceiling. Braces alone do not count as statements.
    """
    count = 0
    for line in masked(body).splitlines():
        text = line.strip().strip("{} ")
        if text and not text.startswith("#"):
            count += max(1, text.count(";"))
    return count


def expanded(path: Path, seen: frozenset[Path] = frozenset()) -> str:
    if path in seen:
        return ""
    source = path.read_text()
    for match in reversed(list(INCLUDE.finditer(masked(source, keep_strings=True)))):
        child = (path.parent / match[1]).resolve()
        if child.suffix == ".inc" and child.is_file():
            source = (
                source[: match.start()] + expanded(child, seen | {path}) + source[match.end() :]
            )
    return source


def add(
    violations: list[dict],
    rule: str,
    path: Path,
    root: Path,
    line: int,
    detail: str,
    value: int = 1,
    identity: str | None = None,
) -> None:
    relative = path.relative_to(root).as_posix()
    identity = identity or re.sub(r"\s+", " ", detail).strip()
    digest = hashlib.sha256(identity.encode()).hexdigest()[:16]
    # Repeated identical violations remain separate debt; additions cannot reuse
    # one existing allowance. Line numbers are diagnostics, never identity.
    prefix = f"{rule}:{relative}:{digest}"
    occurrence = sum(item["key"].startswith(prefix + ":") for item in violations)
    violations.append(
        {
            "rule": rule,
            "key": f"{prefix}:{occurrence}",
            "path": relative,
            "line": line,
            "detail": detail.strip(),
            "value": value,
        }
    )


def art_exception(path: str, offset: int, source: str, bodies: list[tuple[str, int, int]]) -> bool:
    if path in ART_FILES:
        return True
    if any(
        start <= offset < end and name in ART_FUNCTIONS.get(path, set())
        for name, start, end in bodies
    ):
        return True
    if path == "orb_faces/orb_faces.c":
        palette = re.search(r"\bs_colours\[\]\s*=\s*\{[^}]*}", masked(source), re.S)
        return bool(palette and palette.start() <= offset < palette.end())
    return False


def rgb_literals(code: str) -> list[int]:
    """Numeric RGB channels are literals; arithmetic pixel conversion is not."""
    result = []
    for call in RGB_CALL.finditer(code):
        depth = 1
        argument = ""
        for char in code[call.end() :]:
            if char == "(":
                depth += 1
            elif char == ")":
                depth -= 1
            if (char == "," and depth == 1) or depth == 0:
                argument = re.sub(
                    r"^\s*\(\s*(?:u?int(?:8|16|32)_t|unsigned char|int)\s*\)", "", argument
                )
                if re.fullmatch(r"\s*(?:0x[\da-fA-F]+|\d+)[uUlL]*\s*", argument):
                    result.append(call.start())
                    break
                argument = ""
            else:
                argument += char
            if depth == 0:
                break
    return result


def call_arguments(code: str, start: int) -> list[str]:
    """Split a masked C call after its opening parenthesis, retaining expressions."""
    depth = 1
    previous = start
    result = []
    for offset in range(start, len(code)):
        char = code[offset]
        if char == "(":
            depth += 1
        elif char == ")":
            depth -= 1
        if (char == "," and depth == 1) or depth == 0:
            result.append(code[previous:offset])
            previous = offset + 1
        if depth == 0:
            break
    return result


def psram_stack(code: str) -> bool:
    # Follow local constant/macro aliases, without treating unrelated PSRAM
    # buffers in an internal-stack task's translation unit as external stacks.
    code = re.sub(r"\\\n", " ", code)
    external = {"MALLOC_CAP_SPIRAM"}
    internal = {"MALLOC_CAP_INTERNAL"}
    assignments = re.findall(r"\b(\w+)\s*=\s*([^;]+);|#\s*define\s+(\w+)\s+([^\n]+)", code)
    while True:
        before = len(external) + len(internal)
        for name, value, macro, expansion in assignments:
            tokens = set(re.findall(r"\b\w+\b", value or expansion))
            for capabilities in (external, internal):
                if tokens & capabilities:
                    capabilities.add(name or macro)
        if len(external) + len(internal) == before:
            break
    for match in TASK_CREATE_CAPS.finditer(code):
        args = call_arguments(code, match.end())
        # Generic 8BIT/default or unknown caps can also select PSRAM. Require
        # an explicit internal-heap constraint before calling that stack safe.
        caps = set(re.findall(r"\b\w+\b", args[-1])) if args else set()
        if caps & external or not caps & internal:
            return True
    # Explicit pthread capabilities and PSRAM-backed static task stacks are
    # external too. These APIs do not inherit xTaskCreateWithCaps' last argument.
    for match in re.finditer(r"\.stack_alloc_caps\s*=\s*([^;,}]+)", code):
        if set(re.findall(r"\b\w+\b", match[1])) & external:
            return True
    stacks = set(re.findall(r"\bEXT_RAM_(?:BSS|NOINIT)_ATTR\s+(?:\w+\s+)*(\w+)\s*\[", code))
    for match in re.finditer(r"\bxTaskCreate\w*Static\w*\s*\(", code):
        args = call_arguments(code, match.end())
        if any(set(re.findall(r"\b\w+\b", arg)) & (stacks | external) for arg in args):
            return True
    return False


def psram_flash_rules(path: Path, unit: str, root: Path) -> list[dict]:
    code = masked(unit)
    if not psram_stack(code):
        return []
    result = []
    bodies = functions(unit)
    for match in FLASH_CALL.finditer(code):
        owner = next((name for name, start, end in bodies if start <= match.start() < end), "TU")
        add(
            result,
            "psram_flash",
            path,
            root,
            code.count("\n", 0, match.start()) + 1,
            f"PSRAM-stack translation unit: {owner} calls {match[1]}",
            identity=f"{owner}:{match[1]}",
        )
    return result


def source_rules(path: Path, source: str, root: Path, known: set[str]) -> list[dict]:
    violations = []
    owner = component(path, root)
    relative = (
        path.relative_to(root / "firmware" / "components").as_posix()
        if owner != "main"
        else "main/" + path.name
    )
    code = masked(source)
    if owner and owner.startswith("orb_app_"):
        # Resolve the longest registered prefix: a future orb_app_home_extra
        # owns its own calls rather than looking like a foreign Home factory.
        apps = sorted(
            (
                directory.name
                for directory in (root / "firmware" / "components").glob("orb_app_*")
                if directory.is_dir()
            ),
            key=len,
            reverse=True,
        )
        for match in APP_CALL.finditer(code):
            target = next((app for app in apps if match[1].startswith(app + "_")), None)
            if target and target != owner:
                line = code.count("\n", 0, match.start()) + 1
                add(
                    violations,
                    "app_boundary",
                    path,
                    root,
                    line,
                    f"app references another app function: {match[1]}",
                )
        for match in ROTARY_OWNER.finditer(code):
            line = code.count("\n", 0, match.start()) + 1
            add(violations, "rim_owner", path, root, line, source.splitlines()[line - 1])
    if owner != "orb_ui":
        for line, expression, text in PRIVACY.accesses(source, known):
            add(violations, "kit_privacy", path, root, line, f"kit child {expression}: {text}")
        for match in CONTROL.finditer(code):
            add(
                violations,
                "raw_control",
                path,
                root,
                code.count("\n", 0, match.start()) + 1,
                source.splitlines()[code.count("\n", 0, match.start())].strip(),
            )
    if relative not in TOKEN_FILES and not relative.startswith("orb_ui/tokens/"):
        bodies = functions(source)
        offsets = [
            match.start()
            for match in COLOUR.finditer(code)
            if match[0].lower() != "0x10ffff"
            and not re.search(r"[&>]\s*$", code[max(0, match.start() - 8) : match.start()])
            and not re.search(
                r"orb_game_init\([^;\n]*,\s*$", code[max(0, match.start() - 100) : match.start()]
            )
        ]
        offsets.extend(match.start() for match in PALETTE.finditer(code))
        for offset in offsets + rgb_literals(code):
            if not art_exception(relative, offset, source, bodies):
                line = code.count("\n", 0, offset) + 1
                add(violations, "raw_colour", path, root, line, source.splitlines()[line - 1])
    if path.suffix in {".c", ".h"}:
        unit = expanded(path) if path.suffix == ".c" else source
        if path.suffix == ".c":
            violations.extend(psram_flash_rules(path, unit, root))
        count = len(unit.splitlines())
        if path.suffix == ".c" and count > LIMITS["file_size"]:
            add(
                violations, "file_size", path, root, 1, f"{count} handwritten TU lines", count, "TU"
            )
        for name, start, end in functions(unit):
            count = logical_lines(unit[start:end])
            if count > LIMITS["function_size"]:
                add(
                    violations,
                    "function_size",
                    path,
                    root,
                    unit.count("\n", 0, start) + 1,
                    f"{name}: {count} logical lines (expanded TU)",
                    count,
                    name,
                )
    return violations


def header_index(root: Path, idf: Path | None) -> dict[str, set[str]]:
    result = defaultdict(set)
    exported = defaultdict(set)
    directories = [root / "firmware" / "components", root / "firmware" / "managed_components"]
    if idf and idf.is_dir():
        directories.append(idf / "components")
    for directory in directories:
        for header in directory.rglob("*.h"):
            relative = header.relative_to(directory)
            if len(relative.parts) < 2 or {"tests", "test_app", "test", "examples"} & set(
                relative.parts
            ):
                continue
            owner = relative.parts[0].split("__")[-1]
            result[header.name].add(owner)
            result[Path(*relative.parts[1:]).as_posix()].add(owner)
            for i, part in enumerate(relative.parts):
                if part in {"include", "private_include"}:
                    exported[Path(*relative.parts[i + 1 :]).as_posix()].add(owner)
    # The checked-in LVGL fork is a component alias, not an IDF-managed path.
    for header in (root / "firmware" / "third_party" / "lvgl__lvgl").rglob("*.h"):
        result[header.name].add("lvgl")
        relative = header.relative_to(root / "firmware" / "third_party" / "lvgl__lvgl").as_posix()
        result[relative].add("lvgl")
        result["lvgl/" + relative].add("lvgl")
    # A public include path wins over unrelated private headers with the same
    # basename. E.g. lwip/apps/mdns.h cannot satisfy a bare mdns.h dependency.
    result.update(exported)
    return dict(result)


def declared_dependencies(cmake: str) -> set[str]:
    text = re.sub(r"#.*", "", cmake)
    result = set()
    active = False
    for token in re.findall(r'"[^"]*"|[^\s()"]+|[()]', text):
        token = token.strip('"')
        if token in {"REQUIRES", "PRIV_REQUIRES"}:
            active = True
        elif token in {"(", ")"} or re.fullmatch(r"[A-Z_]+", token):
            active = False
        elif active:
            result.add(token.split("__")[-1])
    return result


def declaration_rules(path: Path, root: Path, owner: str, declared: set[str]) -> list[dict]:
    """A forbidden edge is still a dependency when no header is included yet."""
    violations = []
    presentations = {
        "lvgl",
        "orb_ui",
        "orb_shell",
        "orb_screen",
        "orb_apps",
        "orb_home",
        "orb_time_app",
        "orb_faces",
        "orb_games",
        "orb_demo",
    }
    for target in sorted(declared):
        app = target.startswith("orb_app_")
        reason = None
        if owner in SERVICES and (target in presentations or app):
            reason = "service declares presentation"
        elif owner == "orb_ui" and (
            target in presentations - {"lvgl", "orb_ui"} or target == "orb_link" or app
        ):
            reason = "UI declares application/shell/framework/link"
        elif owner.startswith("orb_app_") and app and target != owner:
            reason = "app declares another app"
        if reason:
            add(violations, "include_layering", path, root, 1, f"{reason}: {target}")
    return violations


def dependency_rules(
    path: Path, source: str, root: Path, index: dict[str, set[str]], declared: set[str]
) -> list[dict]:
    violations = []
    owner = component(path, root)
    for match in INCLUDE.finditer(masked(source, keep_strings=True)):
        include = match[1]
        line = source.count("\n", 0, match.start()) + 1
        local = (path.parent / include).resolve()
        target = component(local, root) if local.is_file() else None
        candidates = {target} if target else index.get(include, set())
        if include in BUILD_HEADERS or include in RUNTIME_HEADERS or include.startswith("sys/"):
            continue
        if candidates and candidates <= {owner}:
            continue
        forbidden = []
        app_header = include not in {"orb_home.h", "orb_time.h", "orb_time_voice.h"} and any(
            candidate in {"orb_home", "orb_time_app", "orb_faces", "orb_games", "orb_demo"}
            for candidate in candidates
        )
        if owner in SERVICES and (
            app_header
            or any(
                candidate == "lvgl"
                or candidate in {"orb_shell", "orb_screen", "orb_apps", "orb_ui"}
                or candidate.startswith("orb_app_")
                for candidate in candidates
            )
        ):
            forbidden.append("service includes presentation")
        if owner == "orb_ui" and any(
            candidate
            in {
                "orb_shell",
                "orb_screen",
                "orb_link",
                "orb_apps",
                "orb_home",
                "orb_time_app",
                "orb_faces",
                "orb_games",
                "orb_demo",
            }
            or candidate.startswith("orb_app_")
            for candidate in candidates
        ):
            forbidden.append("UI includes application/shell/link")
        if (
            owner
            and owner.startswith("orb_app_")
            and any(
                candidate.startswith("orb_app_") and candidate != owner for candidate in candidates
            )
        ):
            forbidden.append("app includes another app")
        if ".." in Path(include).parts and target and target != owner:
            forbidden.append("relative cross-component include")
        for reason in forbidden:
            add(violations, "include_layering", path, root, line, f"{reason}: {include}")
        dependencies = candidates - {owner}
        if dependencies and not dependencies.intersection(declared):
            add(
                violations,
                "undeclared_dependency",
                path,
                root,
                line,
                f"{include}: requires one of {', '.join(sorted(dependencies))}",
            )
        elif not candidates and (include.startswith("orb_") or include.startswith("../")):
            add(
                violations,
                "undeclared_dependency",
                path,
                root,
                line,
                f"{include}: unresolved project header",
            )
    return violations


def scan(root: Path = ROOT, idf: Path | None = None) -> list[dict]:
    files = production_files(root)
    sources = {path: path.read_text() for path in files}
    members = defaultdict(set)
    for path, source in sources.items():
        for match in PRIVACY.ASSIGN.finditer(masked(source)):
            if "->" in match["name"]:
                members[path.parent].add(match["name"])
    index = header_index(root, idf)
    dependencies = {}
    result = []
    for path, source in sources.items():
        owner = component(path, root)
        if owner not in dependencies:
            base = root / "firmware" / ("main" if owner == "main" else f"components/{owner}")
            cmake = base / "CMakeLists.txt"
            dependencies[owner] = (
                declared_dependencies(cmake.read_text()) if cmake.is_file() else set()
            )
            result.extend(declaration_rules(cmake, root, owner, dependencies[owner]))
        known = members[path.parent] | {"*slot"}
        result.extend(source_rules(path, source, root, known))
        result.extend(dependency_rules(path, source, root, index, dependencies[owner]))
    return sorted(result, key=lambda item: (item["rule"], item["key"]))


def regressions(current: list[dict], baseline: dict) -> list[dict]:
    previous = {item["key"]: item for item in baseline.get("violations", [])}
    previous.update(
        {item["key"]: item for item in baseline.get("exceptions", []) if item.get("reason")}
    )
    return [
        item
        for item in current
        if item["key"] not in previous
        or item["rule"] != previous[item["key"]]["rule"]
        or item["value"] > previous[item["key"]]["value"]
    ]


def baseline_document(current: list[dict], previous: dict | None = None) -> dict:
    # Exceptions are explicit, per-call baseline entries, never source-file
    # allowlists. Regeneration can remove them but cannot approve new ones.
    approved = {
        item["key"]: item for item in (previous or {}).get("exceptions", []) if item.get("reason")
    }
    exceptions = [
        dict(item, reason=approved[item["key"]]["reason"])
        for item in current
        if item["key"] in approved
    ]
    debt = [item for item in current if item["key"] not in approved]
    return {
        "schema": 1,
        "policy": "Shrink only. --regenerate-baseline refuses new sites or increased metrics.",
        "scope": "Handwritten production firmware/components and firmware/main C/header/inc files; tests excluded.",
        "limits": LIMITS,
        "generated_dirs": sorted(GENERATED_DIRS),
        "generated_files": sorted(GENERATED_FILES),
        "token_files": sorted(TOKEN_FILES),
        "content_renderer_files": sorted(ART_FILES),
        "content_renderer_functions": {key: sorted(value) for key, value in ART_FUNCTIONS.items()},
        "counts": dict(sorted(Counter(item["rule"] for item in debt).items())),
        "exceptions": exceptions,
        "violations": debt,
    }


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=ROOT)
    parser.add_argument("--baseline", type=Path, default=BASELINE)
    parser.add_argument(
        "--idf-path",
        type=Path,
        default=Path(os.environ.get("IDF_PATH", Path.home() / "esp/esp-idf")),
    )
    parser.add_argument(
        "--regenerate-baseline",
        action="store_true",
        help="initialize missing baseline or ratchet existing debt downward only",
    )
    parser.add_argument("--json", action="store_true", help="print the complete current inventory")
    args = parser.parse_args(argv)
    current = scan(args.root.resolve(), args.idf_path)
    baseline = json.loads(args.baseline.read_text()) if args.baseline.is_file() else None
    if args.json:
        print(json.dumps(baseline_document(current, baseline), indent=2))
    if baseline is None and not args.regenerate_baseline:
        print(
            "guardrails: missing baseline; explicit --regenerate-baseline is required",
            file=sys.stderr,
        )
        return 1
    new = regressions(current, baseline) if baseline is not None else []
    if new:
        for item in new:
            print(
                f"{item['path']}:{item['line']}: {item['rule']}: {item['detail']}", file=sys.stderr
            )
        print(
            f"guardrails: {len(new)} new/increased violations; baseline cannot grow",
            file=sys.stderr,
        )
        return 1
    if args.regenerate_baseline:
        args.baseline.write_text(json.dumps(baseline_document(current, baseline), indent=2) + "\n")
    document = baseline_document(current, baseline)
    counts = document["counts"]
    print(
        "guardrails: no regressions; existing debt "
        + ", ".join(f"{key}={value}" for key, value in sorted(counts.items()))
        + f"; documented exceptions={len(document['exceptions'])}",
        file=sys.stderr if args.json else sys.stdout,
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
