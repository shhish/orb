"""Reject screen access to children owned by a kit factory.

This intentionally operates on the source (including .inc files), so a new
screen cannot hide an unsafe write behind a rarely exercised navigation path.
Generic traversal of a screen/list tree is permitted. Exact legacy exceptions
are limited to files assigned to other concurrent owner streams.
"""

import re
import sys
from pathlib import Path

COMPONENTS = Path(__file__).resolve().parents[2] / "firmware" / "components"
SCREENS = ("orb_shell", "orb_home", "orb_time_app", "orb_faces", "orb_games")
ASSIGN = re.compile(
    r"(?P<name>[A-Za-z_]\w*(?:->\w+)*(?:\[[^\]\n]+\])?)\s*=\s*"
    r"(?:[^;\n?]+\?\s*)?(?:orb_kit_\w+_create|kit_action)\s*\("
)
FACTORY = re.compile(r"(?:orb_kit_\w+_create|kit_action)\s*\(")
ASSIGNMENT = re.compile(
    r"(?<![\w>.])(?P<name>[A-Za-z_]\w*(?:(?:->|\.)\w+|\[[^\]\n]+\])*)\s*=(?!=)\s*"
    r"(?P<value>[^;]+);"
)
CHILD = re.compile(r"lv_obj_get_child\s*\(\s*([^,\n]+),")
C_LITERAL = re.compile(r'/\*.*?\*/|//[^\n]*|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'', re.S)


def code_only(source: str) -> str:
    # Preserve offsets/line numbers while ignoring commented examples and text.
    return C_LITERAL.sub(lambda match: re.sub(r"[^\n]", " ", match[0]), source)


def alias(expression: str) -> str:
    # A common cast must not hide a kit alias from the source guard.
    expression = (
        re.sub(r"^\(lv_obj_t\s*\*\)\s*", "", expression).strip().strip("()").replace(" ", "")
    )
    return re.sub(r"\[[^\]]*\]", "[]", expression)


# Keep the expression and complete source line: adding a new reach-through in
# an excluded file still fails, while the lead can migrate these exact sites.
LEGACY = {
    ("orb_games/orb_game_view.c", "lv_obj_t *label = lv_obj_get_child(*slot, 0);"),
}


def accesses(source: str, known: set[str]) -> list[tuple[int, str, str]]:
    result = []
    # Project C functions finish with an unindented closing brace. Keep local
    # names scoped to that function; "obj" in a generic recursive visitor is
    # unrelated to "obj" returned by a factory elsewhere in the source.
    offset = 0
    for function in re.split(r"(?m)(?<=\n)}", code_only(source)):
        local = {alias(name) for name in known}
        assignments = iter(ASSIGNMENT.finditer(function))
        assignment = next(assignments, None)
        for match in CHILD.finditer(function):
            while assignment and assignment.start() < match.start():
                name = alias(assignment["name"])
                value = assignment["value"].strip()
                if FACTORY.search(value) or alias(value) in local:
                    local.add(name)
                else:
                    local.discard(name)
                assignment = next(assignments, None)
            expression = match[1].strip().replace(" ", "")
            if alias(expression) not in local and not FACTORY.search(expression):
                continue
            line = source.count("\n", 0, offset + match.start()) + 1
            result.append((line, expression, source.splitlines()[line - 1].strip()))
        offset += len(function) + 1
    return result


def main() -> int:
    # Verify both the direct and cached-child regression shapes and leave
    # generic container traversal available for accessibility/debug scans.
    fixture = """
        lv_obj_t *chip = orb_kit_button_secondary_create(page, "9%");
        lv_label_set_text(lv_obj_get_child(chip, 0), "100%");
        value = lv_obj_get_child(chip, 0);
        generic = lv_obj_get_child(page, 0);
    """
    assert len(accesses(fixture, set())) == 2
    aliases = """
        lv_obj_t *chip = orb_kit_button_secondary_create(page, "9%");
        lv_obj_t *copy = chip;
        lv_obj_t *cast = (lv_obj_t *) copy;
        lv_label_set_text(lv_obj_get_child(cast, 0), "100%");
        lv_obj_get_child(orb_kit_button_secondary_create(page, "9%"), 0);
        copy = page;
        generic = lv_obj_get_child(copy, 0);
        /* lv_obj_get_child(chip, 0); is documentation, not code. */
    """
    assert len(accesses(aliases, set())) == 2
    assert len(accesses("child = lv_obj_get_child(w->chip, 0);", {"w->chip"})) == 1
    assert len(accesses("child = lv_obj_get_child(w->chips[selected], 0);", {"w->chips[i]"})) == 1
    scopes = """
void first(void) {
    lv_obj_t *obj = orb_kit_button_secondary_create(page, "9%");
}
void second(void) {
    generic = lv_obj_get_child(obj, 0);
}
"""
    assert not accesses(scopes, set())
    files = sorted(
        path
        for component in SCREENS
        for path in (COMPONENTS / component).glob("**/*")
        if path.suffix in {".c", ".inc"}
    )
    # Factories and consumers often live in separate .inc files of a screen.
    sources = {path: path.read_text() for path in files}
    members = {
        directory: {
            match["name"].replace(" ", "")
            for path, source in sources.items()
            if path.parent == directory
            for match in ASSIGN.finditer(code_only(source))
            if "->" in match["name"]
        }
        for directory in {path.parent for path in files}
    }
    violations = []
    legacy_seen = set()
    for path, source in sources.items():
        known = set(members[path.parent])
        known.add("*slot")  # Games' kit primary is assigned through an output slot.
        relative = path.relative_to(COMPONENTS).as_posix()
        for line, expression, text in accesses(source, known):
            legacy = (relative, text)
            if legacy in LEGACY and legacy not in legacy_seen:
                legacy_seen.add(legacy)
                print(f"lead-owned exception: {relative}:{line}: {text}")
            else:
                violations.append(f"{relative}:{line}: kit child {expression}: {text}")
    if violations:
        print("Screen reaches into kit-owned internals:\n" + "\n".join(violations), file=sys.stderr)
        return 1
    print(f"kit privacy: {len(files)} screen source files, no kit child access")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
