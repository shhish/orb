"""Bounded panic receipts retained after the next state sample replaces live state."""

import re

_ERROR = re.compile(r"[A-Z][A-Z0-9_]{0,63}\Z")


def crash_note(value: object) -> str | None:
    if not isinstance(value, dict):
        return None
    error = value.get("error")
    if isinstance(error, str) and _ERROR.fullmatch(error):
        return f"panic: crash summary unavailable: {error}"
    task, pc, backtrace = value.get("task"), value.get("pc"), value.get("backtrace")
    corrupted = value.get("corrupted")
    if not isinstance(task, str):
        return None
    try:
        task_bytes = len(task.encode("utf-8"))
    except UnicodeEncodeError:
        return None

    def address(item: object) -> bool:
        return type(item) is int and 0 <= item <= 0xFFFFFFFF

    if (
        not task
        or task_bytes > 16
        or any(ord(c) < 32 or ord(c) == 127 for c in task)
        or not address(pc)
        or not isinstance(backtrace, list)
        or not 1 <= len(backtrace) <= 16
        or not all(address(item) for item in backtrace)
        or not isinstance(corrupted, bool)
    ):
        return None
    trace = " ".join(f"0x{item:08x}" for item in backtrace)
    return f"panic: task={task} pc=0x{pc:08x} bt={trace} corrupted={int(corrupted)}"
