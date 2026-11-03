"""Per-device log files: what devices send in log.batch, plus the hub's notes about them (connects, OTA reports).

~/Library/Logs/orb-hub/devices/<device_id>.log, rotated at 5 MB with three old files kept.
"""

import logging
import logging.handlers
import re
from pathlib import Path

from orb_hub.config import ensure_private_dir

MAX_BYTES = 5 * 1024 * 1024
BACKUPS = 3
MAX_LINE = 1024

# ESP-IDF colours its log lines; the escapes are noise in a file.
_ANSI = re.compile(r"\x1b\[[0-9;]*[A-Za-z]")
_CONTROL = re.compile(r"[\x00-\x08\x0b-\x1f\x7f]")


def clean_line(line: str) -> str:
    line = _ANSI.sub("", line).rstrip("\r\n")
    return _CONTROL.sub("?", line.replace("\n", " "))[:MAX_LINE]


class DeviceLogs:
    def __init__(self, directory: Path):
        self.directory = directory
        self._loggers: dict[str, logging.Logger] = {}

    def lines(self, device_id: str, lines: list[str]) -> None:
        log = self._logger(device_id)
        for line in lines:
            log.info("%s", clean_line(line))

    def note(self, device_id: str, text: str) -> None:
        self._logger(device_id).info("[hub] %s", text)

    def path(self, device_id: str) -> Path:
        return self.directory / f"{device_id}.log"

    def close(self) -> None:
        for log in self._loggers.values():
            for handler in log.handlers[:]:
                handler.close()
                log.removeHandler(handler)
        self._loggers.clear()

    def _logger(self, device_id: str) -> logging.Logger:
        # device_id is validated (orb- + 12 hex digits) before it gets here, so it is safe in a file name.
        log = self._loggers.get(device_id)
        if log is None:
            ensure_private_dir(self.directory)
            handler = logging.handlers.RotatingFileHandler(
                self.path(device_id), maxBytes=MAX_BYTES, backupCount=BACKUPS, encoding="utf-8"
            )
            handler.setFormatter(logging.Formatter("%(asctime)s %(message)s"))
            # Not propagated, so device lines never reach the hub's own log.
            log = logging.getLogger(f"orb_hub.device.{device_id}")
            for old in log.handlers[:]:
                old.close()
                log.removeHandler(old)
            log.setLevel(logging.INFO)
            log.propagate = False
            log.addHandler(handler)
            self._loggers[device_id] = log
        return log
