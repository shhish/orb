from orb_hub.tz import posix_tz_for_zone, posix_tz_from_bytes


def test_posix_tz_for_zone_america_new_york():
    posix = posix_tz_for_zone("America/New_York")
    assert posix == "EST5EDT,M3.2.0,M11.1.0"


def test_posix_tz_for_zone_unknown():
    assert posix_tz_for_zone("Mars/Curiosity") is None
    assert posix_tz_for_zone("Invalid/Not_A_Zone") is None


def test_posix_tz_for_zone_edge_cases():
    assert posix_tz_for_zone("") is None
    assert posix_tz_for_zone(None) is None
    assert posix_tz_for_zone("../../../etc/passwd") is None


def test_posix_tz_from_bytes_valid():
    # Synthetic TZif v2 file ending in \n<posix-tz>\n
    raw = b"TZif2dummydata\nEST5EDT,M3.2.0,M11.1.0\n"
    assert posix_tz_from_bytes(raw) == "EST5EDT,M3.2.0,M11.1.0"


def test_posix_tz_from_bytes_invalid_magic():
    raw = b"NOTZ2dummydata\nEST5EDT,M3.2.0,M11.1.0\n"
    assert posix_tz_from_bytes(raw) is None


def test_posix_tz_from_bytes_no_trailing_newline():
    raw = b"TZif2dummydata\nEST5EDT,M3.2.0,M11.1.0"
    assert posix_tz_from_bytes(raw) is None


def test_posix_tz_from_bytes_empty_footer():
    raw = b"TZif2dummydata\n\n"
    assert posix_tz_from_bytes(raw) is None


def test_posix_tz_from_bytes_non_printable():
    raw = b"TZif2dummydata\nEST5EDT\x01\x02\n"
    assert posix_tz_from_bytes(raw) is None


def test_posix_tz_from_bytes_too_long():
    long_tz = "A" * 64
    raw = b"TZif2dummydata\n" + long_tz.encode("ascii") + b"\n"
    assert posix_tz_from_bytes(raw) is None
