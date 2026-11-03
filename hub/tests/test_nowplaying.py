"""Unit tests for Now Playing media observation, volume control, and polling."""

import asyncio

import pytest

from orb_hub.envelope import LinkError
from orb_hub.nowplaying import (
    MUSIC_SCRIPT,
    SPOTIFY_SCRIPT,
    NowPlayingService,
    clamp_volume,
)

# -- Safety and script inspections


def test_scripts_check_application_is_running_first():
    """Verify scripts guard with 'application is running' so macOS never launches apps."""
    assert 'if application "Music" is running then' in MUSIC_SCRIPT
    assert 'if application "Spotify" is running then' in SPOTIFY_SCRIPT


def test_clamp_volume():
    assert clamp_volume(0) == 0
    assert clamp_volume(50) == 50
    assert clamp_volume(100) == 100
    assert clamp_volume(-15) == 0
    assert clamp_volume(150) == 100
    assert clamp_volume(42.6) == 43

    for invalid in ("50", None, True, False, float("nan"), float("inf")):
        with pytest.raises(LinkError):
            clamp_volume(invalid)


# -- Fake Runners and Snapshot Detection Order


class FakeMediaRunner:
    """Deterministic fake osascript/command runner that never interacts with macOS system."""

    def __init__(self):
        self.volume = 65
        self.music_running = False
        self.music_track = ("playing", 42.0, "Song A", "Artist A", "Album A", 180.0)
        self.spotify_running = False
        self.spotify_track = ("paused", 10.0, "Spotify Song", "Spotify Artist", "Spotify Album", 200.0)
        self.nowplaying_cli_output = None
        self.commands = []

    async def __call__(self, cmd: list[str]) -> tuple[int, str, str]:
        self.commands.append(cmd)
        prog = cmd[0]
        if prog == "osascript":
            script = cmd[2]
            if "output volume of (get volume settings)" in script:
                return 0, str(self.volume), ""
            if "set volume output volume" in script:
                # Extract target volume
                val = script.split("output volume")[-1].strip()
                self.volume = int(val)
                return 0, "", ""
            if 'application "Music" is running' in script:
                if not self.music_running:
                    return 0, "NOT_RUNNING", ""
                state, pos, name, artist, album, dur = self.music_track
                return 0, f"{state}||{pos}||{name}||{artist}||{album}||{dur}", ""
            if 'application "Spotify" is running' in script:
                if not self.spotify_running:
                    return 0, "NOT_RUNNING", ""
                state, pos, name, artist, album, dur = self.spotify_track
                return 0, f"{state}||{pos}||{name}||{artist}||{album}||{dur}", ""
            # Control commands
            return 0, "", ""
        if prog == "nowplaying-cli":
            if self.nowplaying_cli_output:
                return 0, self.nowplaying_cli_output, ""
            return 1, "", "not running"
        return 0, "", ""


@pytest.mark.asyncio
async def test_priority_order_music_active():
    runner = FakeMediaRunner()
    runner.music_running = True
    runner.spotify_running = True

    service = NowPlayingService(runner=runner)
    snapshot = await service.read_snapshot()

    assert snapshot.app == "Music"
    assert snapshot.title == "Song A"
    assert snapshot.artist == "Artist A"
    assert snapshot.album == "Album A"
    assert snapshot.duration == 180.0
    assert snapshot.position == 42.0
    assert snapshot.playing is True
    assert snapshot.state == "playing"
    assert snapshot.volume == 65


@pytest.mark.asyncio
async def test_priority_order_spotify_active_when_music_stopped():
    runner = FakeMediaRunner()
    runner.music_running = False
    runner.spotify_running = True

    service = NowPlayingService(runner=runner)
    snapshot = await service.read_snapshot()

    assert snapshot.app == "Spotify"
    assert snapshot.title == "Spotify Song"
    assert snapshot.artist == "Spotify Artist"
    assert snapshot.album == "Spotify Album"
    assert snapshot.duration == 200.0
    assert snapshot.position == 10.0
    assert snapshot.playing is False
    assert snapshot.state == "paused"
    assert snapshot.volume == 65


@pytest.mark.asyncio
async def test_priority_order_nowplaying_cli_fallback(monkeypatch):
    runner = FakeMediaRunner()
    runner.music_running = False
    runner.spotify_running = False
    runner.nowplaying_cli_output = "CLI Title\nCLI Artist\nCLI Album\n240.0\n50.0\n1.0"

    monkeypatch.setattr(
        "shutil.which", lambda prog: "/usr/local/bin/nowplaying-cli" if prog == "nowplaying-cli" else None
    )

    service = NowPlayingService(runner=runner)
    snapshot = await service.read_snapshot()

    assert snapshot.app == "NowPlaying"
    assert snapshot.title == "CLI Title"
    assert snapshot.artist == "CLI Artist"
    assert snapshot.album == "CLI Album"
    assert snapshot.duration == 240.0
    assert snapshot.position == 50.0
    assert snapshot.playing is True


@pytest.mark.asyncio
async def test_idle_state_when_no_media_playing():
    runner = FakeMediaRunner()
    runner.music_running = False
    runner.spotify_running = False
    runner.nowplaying_cli_output = None

    service = NowPlayingService(runner=runner)
    snapshot = await service.read_snapshot()

    assert snapshot.playing is False
    assert snapshot.state == "stopped"
    assert snapshot.title == ""
    assert snapshot.artist == ""
    assert snapshot.volume == 65


# -- Volume controls


@pytest.mark.asyncio
async def test_volume_get_and_set():
    runner = FakeMediaRunner()
    service = NowPlayingService(runner=runner)

    assert await service.read_volume() == 65

    new_vol = await service.set_volume(80)
    assert new_vol == 80
    assert runner.volume == 80

    # Test clamping high
    clamped_high = await service.set_volume(120)
    assert clamped_high == 100
    assert runner.volume == 100

    # Test clamping low
    clamped_low = await service.set_volume(-10)
    assert clamped_low == 0
    assert runner.volume == 0


# -- Playback controls


@pytest.mark.asyncio
async def test_control_music_app():
    runner = FakeMediaRunner()
    runner.music_running = True

    service = NowPlayingService(runner=runner)
    res = await service.control("pause")
    assert res["accepted"] is True
    assert res["action"] == "pause"
    assert any('tell application "Music" to pause' in cmd[2] for cmd in runner.commands if cmd[0] == "osascript")

    res = await service.control("next")
    assert res["accepted"] is True
    assert any('tell application "Music" to next track' in cmd[2] for cmd in runner.commands if cmd[0] == "osascript")


@pytest.mark.asyncio
async def test_control_volume_action():
    runner = FakeMediaRunner()
    service = NowPlayingService(runner=runner)

    res = await service.control("volume", value=77)
    assert res["accepted"] is True
    assert res["action"] == "volume"
    assert res["value"] == 77
    assert runner.volume == 77
    assert res["snapshot"]["volume"] == 77


@pytest.mark.asyncio
async def test_control_invalid_action():
    runner = FakeMediaRunner()
    service = NowPlayingService(runner=runner)

    with pytest.raises(LinkError, match="unsupported action"):
        await service.control("invalid_cmd")


# -- Request handling


@pytest.mark.asyncio
async def test_request_snapshot_and_control():
    runner = FakeMediaRunner()
    runner.music_running = True
    service = NowPlayingService(runner=runner)

    # 1. nowplaying.snapshot
    snap = await service.request("orb-123", "nowplaying.snapshot", {})
    assert snap["title"] == "Song A"
    assert snap["volume"] == 65

    # 2. nowplaying.control
    ctrl = await service.request("orb-123", "nowplaying.control", {"action": "volume", "value": 50})
    assert ctrl["accepted"] is True
    assert ctrl["value"] == 50


# -- Subscriptions and Polling


class FakeLink:
    def __init__(self, label="test-orb"):
        self.label = label
        self.notifications = []
        self.notified = asyncio.Event()

    async def notify(self, name: str, params: dict):
        self.notifications.append((name, params))
        self.notified.set()


@pytest.mark.asyncio
async def test_subscription_polling_and_updates():
    runner = FakeMediaRunner()
    runner.music_running = True
    # Fast poll interval for unit test
    service = NowPlayingService(runner=runner, poll_interval=0.05)

    link = FakeLink()
    await service.subscribe(link)

    # Must receive immediate snapshot upon subscribing
    assert len(link.notifications) == 1
    assert link.notifications[0][0] == "nowplaying.snapshot"
    assert link.notifications[0][1]["title"] == "Song A"

    # Background poll task should be active
    assert service._poll_task is not None
    assert not service._poll_task.done()

    # Simulate a song change
    runner.music_track = ("playing", 0.0, "Song B", "Artist B", "Album B", 210.0)
    link.notified.clear()

    # Wait for poller to detect change and notify
    await asyncio.wait_for(link.notified.wait(), timeout=1.0)
    assert len(link.notifications) >= 2
    assert link.notifications[-1][1]["title"] == "Song B"

    # Unsubscribe link
    service.unsubscribe(link)
    assert len(service._subscribers) == 0
    assert service._poll_task is None

    await service.stop()
