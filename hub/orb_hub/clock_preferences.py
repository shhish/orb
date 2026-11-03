"""Read the owner's hour cycle once, without spawning a tool or changing locale."""

import ctypes
import locale
import sys
from functools import lru_cache


def hour24_from_pattern(pattern: str) -> bool | None:
    """ICU hour fields outside quoted literal text determine the hour cycle."""
    quoted = False
    index = 0
    while index < len(pattern):
        character = pattern[index]
        if character == "'":
            if index + 1 < len(pattern) and pattern[index + 1] == "'":
                index += 2
                continue
            quoted = not quoted
        elif not quoted and character in "Hk":
            return True
        elif not quoted and character in "hK":
            return False
        index += 1
    return None


def _mac_time_pattern() -> str:
    cf = ctypes.CDLL("/System/Library/Frameworks/CoreFoundation.framework/CoreFoundation")
    cf.CFLocaleCopyCurrent.argtypes = []
    cf.CFLocaleCopyCurrent.restype = ctypes.c_void_p
    cf.CFDateFormatterCreate.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_long, ctypes.c_long]
    cf.CFDateFormatterCreate.restype = ctypes.c_void_p
    cf.CFDateFormatterGetFormat.argtypes = [ctypes.c_void_p]
    cf.CFDateFormatterGetFormat.restype = ctypes.c_void_p
    cf.CFStringGetCString.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_long, ctypes.c_uint32]
    cf.CFStringGetCString.restype = ctypes.c_bool
    cf.CFRelease.argtypes = [ctypes.c_void_p]
    cf.CFRelease.restype = None
    current = cf.CFLocaleCopyCurrent()
    formatter = None
    try:
        # Current locale includes the owner's forced 12/24-hour preference.
        # No date plus short time keeps literal weekday/month text out of this
        # narrow query; no locale or preference is changed by these calls.
        formatter = cf.CFDateFormatterCreate(None, current, 0, 1)
        if not formatter:
            return ""
        pattern = cf.CFDateFormatterGetFormat(formatter)
        buffer = ctypes.create_string_buffer(512)
        if pattern and cf.CFStringGetCString(pattern, buffer, len(buffer), 0x08000100):
            return buffer.value.decode("utf-8")
        return ""
    finally:
        if formatter:
            cf.CFRelease(formatter)
        if current:
            cf.CFRelease(current)


@lru_cache(maxsize=1)
def local_hour24() -> bool:
    if sys.platform == "darwin":
        try:
            value = hour24_from_pattern(_mac_time_pattern())
            if value is not None:
                return value
        except (OSError, AttributeError, UnicodeError):
            pass
    # strftime directives belong to libc's current locale, not ICU. Avoid
    # setlocale: the hub has concurrent providers which share process locale.
    pattern = locale.nl_langinfo(locale.T_FMT)
    return not any(field in pattern for field in ("%I", "%l", "%r"))
