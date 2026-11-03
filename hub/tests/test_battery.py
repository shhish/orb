"""Battery logging: storage, coalescing, retention, drain maths and the CLI."""

import json
import sqlite3

import pytest
from fake_device import paired_device

from orb_hub import cli
from orb_hub.battery import (
    MAX_GAP_MS,
    BatterySample,
    average_ma,
    discharge_segments,
    display_state_shares,
)
from orb_hub.config import Config
from orb_hub.store import BATTERY_COALESCE_MS, BATTERY_RETENTION_MS, Store, now_ms


def sample(
    minute: float,
    pct: int | None = 80,
    mv: int | None = 4000,
    *,
    charging: bool | None = False,
    usb: bool | None = False,
    state: str | None = "active",
    device: str = "orb-020000abcdef",
) -> BatterySample:
    return BatterySample(
        at=round(minute * 60_000),
        device_id=device,
        pct=pct,
        mv=mv,
        charging=charging,
        usb=usb,
        display_state=state,
    )


def record(store: Store, at: int, **overrides) -> bool:
    fields = {
        "pct": 80,
        "mv": 4000,
        "charging": False,
        "usb": False,
        "display_state": "active",
    }
    fields.update(overrides)
    return store.record_battery("orb-020000abcdef", at=at, **fields)


def test_battery_store_coalesces_unchanged_readings(short_tmp):
    store = Store(short_tmp / "devices.db")
    base = now_ms()
    assert record(store, base)
    # Same reading, under 5 minutes and under 10 mV: no new row.
    assert not record(store, base + 60_000, mv=4005)
    # Exactly 10 mV moved is a real change.
    assert record(store, base + 120_000, mv=4010)
    # Five minutes is the edge of the coalescing window.
    assert record(store, base + 120_000 + BATTERY_COALESCE_MS)
    # Any of the four identity fields changing writes a row.
    assert record(store, base + 421_000, pct=79)
    assert record(store, base + 422_000, pct=79, display_state="dimmed")
    assert record(store, base + 423_000, pct=79, display_state="dimmed", usb=True)
    assert record(store, base + 424_000, pct=79, display_state="dimmed", usb=True, charging=True)
    assert record(store, base + 425_000, pct=79, display_state="dimmed", usb=True, charging=True, mv=None)

    rows = store.battery_samples(device_id="orb-020000abcdef")
    assert len(rows) == 8
    assert [row.mv for row in rows][-1] is None
    assert rows[0].at == base and rows[-1].at == base + 425_000
    store.close()


def test_battery_store_never_coalesces_a_missing_voltage(short_tmp):
    store = Store(short_tmp / "devices.db")
    base = now_ms()
    assert record(store, base, mv=None)
    assert record(store, base + 60_000, mv=None)
    assert len(store.battery_samples()) == 2
    store.close()


def test_battery_store_keeps_at_most_thirty_days(short_tmp):
    store = Store(short_tmp / "devices.db")
    base = now_ms()
    expired = base - BATTERY_RETENTION_MS - 1
    kept = base - BATTERY_RETENTION_MS + 60_000
    assert record(store, expired, pct=99)
    assert record(store, kept, pct=90)
    assert record(store, base, pct=80)

    rows = store.battery_samples(device_id="orb-020000abcdef")
    assert [row.at for row in rows] == [kept, base]
    store.close()


def test_battery_store_filters_by_device_and_window(short_tmp):
    store = Store(short_tmp / "devices.db")
    base = now_ms()
    assert store.record_battery(
        "orb-000000000001", at=base - 60_000, pct=80, mv=4000, charging=False, usb=False, display_state="active"
    )
    assert store.record_battery(
        "orb-000000000002", at=base, pct=70, mv=3900, charging=False, usb=False, display_state="dimmed"
    )
    assert [row.device_id for row in store.battery_samples(device_id="orb-000000000002")] == ["orb-000000000002"]
    assert len(store.battery_samples(since_ms=base, until_ms=base)) == 1
    store.close()


def test_discharge_segment_maths():
    rows = [sample(minute, 80 - minute // 6, 4000 - minute // 6 * 10) for minute in range(0, 121, 10)]
    segments = discharge_segments(rows, capacity_mah=500)
    assert len(segments) == 1
    segment = segments[0]
    assert segment.start_pct == 80 and segment.end_pct == 60
    assert segment.start_mv == 4000 and segment.end_mv == 3800
    assert segment.duration_s == 7200
    assert segment.pct_per_hour == pytest.approx(10)
    assert segment.hours_per_full_charge == pytest.approx(10)
    assert segment.estimated_ma == pytest.approx(50)
    assert segment.reliable and segment.caveats == ()
    assert average_ma(segments, 500) == pytest.approx(50)


def test_discharge_segments_split_on_power_and_missing_data():
    rows = [
        sample(0, 80),
        sample(10, 70),
        sample(20, 60),
        sample(30, 60, usb=True),
        sample(40, 60, charging=True),
        sample(50, 50),
        # More than MAX_GAP_MS after the last row: a new segment.
        sample(50 + MAX_GAP_MS / 60_000 + 1, 50),
        sample(50 + MAX_GAP_MS / 60_000 + 11, 40),
    ]
    segments = discharge_segments(rows)
    assert [(segment.start_pct, segment.end_pct) for segment in segments] == [(80, 60), (50, 40)]
    assert [segment.duration_s for segment in segments] == [1200, 600]


def test_short_or_small_discharge_segments_are_marked_rough():
    short = discharge_segments([sample(0, 80), sample(10, 79)])
    assert len(short) == 1 and not short[0].reliable
    assert set(short[0].caveats) == {"under 20 minutes", "moved under 2 percent"}

    boundary = discharge_segments([sample(0, 80), sample(10, 79), sample(20, 78)])
    assert boundary[0].duration_s == 1200
    assert boundary[0].reliable and boundary[0].caveats == ()


def test_display_state_shares_are_time_weighted_and_ignore_gaps():
    rows = [
        sample(0, state="active"),
        sample(15, state="dimmed"),
        sample(30, state="always_on"),
        sample(45, state="sleep"),
        sample(60, state="sleep"),
        # Six hours without a report must not become six hours of "sleep".
        sample(600, state="sleep"),
    ]
    shares = display_state_shares(rows)
    assert shares == {
        "active": pytest.approx(0.25),
        "dimmed": pytest.approx(0.25),
        "always_on": pytest.approx(0.25),
        "sleep": pytest.approx(0.25),
    }
    assert sum(shares.values()) == pytest.approx(1)


def test_battery_cli_json(short_tmp, capsys):
    cfg = Config.load()
    cfg.data_dir.mkdir(parents=True, exist_ok=True)
    store = Store(cfg.db_path)
    base = now_ms() - 2 * 60 * 60 * 1000
    for minute in range(0, 61, 10):
        state = "active" if minute < 30 else "dimmed" if minute < 60 else "always_on"
        assert record(
            store,
            base + minute * 60_000,
            pct=80 - round(minute / 6),
            mv=4000 - round(minute / 6) * 10,
            display_state=state,
        )
    store.close()

    assert cli.main(["battery", "--hours", "4", "--capacity-mah", "500", "--json"]) == 0
    result = json.loads(capsys.readouterr().out)
    device = result["devices"][0]
    assert device["device_id"] == "orb-020000abcdef"
    segment = device["segments"][0]
    assert (segment["start_pct"], segment["end_pct"]) == (80, 70)
    assert segment["duration_s"] == 3600
    assert segment["pct_per_hour"] == pytest.approx(10)
    assert segment["hours_per_full_charge"] == pytest.approx(10)
    assert segment["estimated_ma"] == pytest.approx(50)
    assert segment["reliable"] is True
    assert device["display_state_shares"] == {"active": 0.5, "dimmed": 0.5}
    assert device["estimated_average_ma"] == pytest.approx(50)


def test_battery_cli_text_marks_rough_segments(short_tmp, capsys):
    cfg = Config.load()
    cfg.data_dir.mkdir(parents=True, exist_ok=True)
    store = Store(cfg.db_path)
    base = now_ms() - 30 * 60 * 1000
    assert record(store, base, pct=80)
    assert record(store, base + 10 * 60 * 1000, pct=79)
    store.close()

    assert cli.main(["battery", "--hours", "1"]) == 0
    out = capsys.readouterr().out
    assert "under 20 minutes" in out and "moved under 2 percent" in out
    assert "don't over-trust" in out


def test_battery_cli_with_no_samples(short_tmp, capsys):
    assert cli.main(["battery"]) == 0
    assert "No battery samples" in capsys.readouterr().out


async def test_state_report_records_a_battery_sample(hub):
    device = await paired_device(hub)
    try:
        state = {
            "power": {
                "battery_pct": 41,
                "battery_mv": 3900,
                "charging": True,
                "usb": True,
                "display_state": "dimmed",
            },
            "rssi": -60,
        }
        await device.send({"t": "evt", "m": "state.report", "p": state})
        await device.request("ota.check")
        rows = hub.store.battery_samples(device_id=device.device_id)
        assert len(rows) == 1
        assert (rows[0].pct, rows[0].mv, rows[0].charging, rows[0].usb, rows[0].display_state) == (
            41,
            3900,
            True,
            True,
            "dimmed",
        )
    finally:
        await device.aclose()


async def test_battery_log_failure_does_not_break_the_link(hub, monkeypatch):
    device = await paired_device(hub)
    monkeypatch.setattr(
        hub.store, "record_battery", lambda *args, **kwargs: (_ for _ in ()).throw(sqlite3.OperationalError("locked"))
    )
    try:
        await device.send({"t": "evt", "m": "state.report", "p": {"power": {"battery_pct": 40}}})
        assert (await device.request("ota.check"))["r"] == {"available": False}
    finally:
        await device.aclose()
