"""Fixture-only regressions for the hub QA sweep; never use owner services."""

import asyncio
import json

import pytest

from orb_hub import cli, speech_synth
from orb_hub.devtools import validate
from orb_hub.firmware import make_offer
from orb_hub.speech import SpeechSender


def manifest(version, *, assets=False):
    return {
        **({"kind": "assets"} if assets else {}),
        "channel": "dev",
        "version": version,
        "build": "abcdef0",
        "size": 1234,
        "sha256": "ab" * 32,
        "notes": "fixture",
    }


@pytest.mark.parametrize(
    "running,release",
    [
        ("0.5.3", "0.5.2"),
        ("0.5.2-bench", "0.5.2-feed"),
        ("0.5.2+bench", "0.5.2+feed"),
        ("0.5.2", "0.5.2-abcdef0"),
        ("bench-sound", "0.6.0"),
        ("0.5.2", "unknown-release"),
    ],
)
def test_feed_never_replaces_older_equal_or_unorderable_app_version(running, release):
    assert make_offer(manifest(release), running, None, "https://fixture") == {"available": False}


@pytest.mark.parametrize("running,release", [("0.5.2-bench", "0.5.3-feed"), ("0.9.9", "0.10.0")])
def test_newer_release_core_can_be_offered(running, release):
    assert make_offer(manifest(release), running, None, "https://fixture")["available"]


@pytest.mark.parametrize(
    "running,release,available", [("5", "4", False), ("5", "5", False), ("5", "6", True), ("", "1", True)]
)
def test_assets_offer_is_monotonic(running, release, available):
    assert make_offer(manifest(release, assets=True), running, None, "https://fixture")["available"] is available


def test_beads_benchmark_is_allowed_in_validator_and_cli(monkeypatch):
    validate("bench_game", ["beads", 5000])
    calls = []
    monkeypatch.setattr(
        cli.control, "call", lambda *args, **kwargs: calls.append((args, kwargs)) or {"text": "fixture\n"}
    )
    assert cli.main(["dev", "orb-fixture", "bench_game", "beads", "5000"]) == 0
    assert calls[0][0][2]["args"] == ["beads", 5000]


def test_speech_caption_fits_utf8_wire_budget():
    text = speech_synth.sanitize_speech_text("😀" * 400)
    assert text
    assert len(text.encode("utf-8")) <= 1024


async def test_speech_source_error_stops_active_device_playback():
    events = []

    async def notify(name, params):
        events.append((name, params))

    async def audio(stream, frame):
        pass

    async def failing_source():
        yield b"\0\0" * 160
        raise OSError("fixture provider failed")

    sender = SpeechSender(notify, audio)
    sender.configure({"formats": ["pcm_s16le"], "rates": [16000], "prebuffer_ms": 150}, voice=True)
    try:
        sender.start(failing_source(), rate=16000, text="Fixture")
        await asyncio.gather(sender._task, return_exceptions=True)
        assert events[-1] == ("voice.speak.stop", {"stream": 1})
        assert sender.last_reason == "bad_audio"
    finally:
        await sender.close()


async def test_speech_subprocess_is_reaped_on_cancel(monkeypatch):
    entered = asyncio.Event()

    class Process:
        returncode = None
        killed = False
        reaped = False

        async def communicate(self):
            entered.set()
            await asyncio.Event().wait()

        def kill(self):
            self.killed = True
            self.returncode = -9

        async def wait(self):
            self.reaped = True
            return self.returncode

    proc = Process()

    async def spawn(*args, **kwargs):
        return proc

    monkeypatch.setattr(speech_synth.asyncio, "create_subprocess_exec", spawn)
    task = asyncio.create_task(speech_synth._run_command(["fixture-only"]))
    await entered.wait()
    task.cancel()
    with pytest.raises(asyncio.CancelledError):
        await task
    assert proc.killed and proc.reaped


@pytest.mark.parametrize("action", ["show", "set"])
def test_prefs_json_flag_before_subcommand_is_preserved(monkeypatch, capsys, action):
    prefs = {"sources": {"claude": True, "codex": True, "dot": True}, "idle_minutes": 10}
    monkeypatch.setattr(cli, "_call", lambda *args, **kwargs: prefs)
    extra = ["--codex", "on"] if action == "set" else []
    assert cli.main(["prefs", "--json", action, "orb-fixture", *extra]) == 0
    assert json.loads(capsys.readouterr().out) == prefs


async def test_speech_subprocess_timeout_is_bounded_and_reaped(monkeypatch):
    class Process:
        returncode = None
        reaped = False

        async def communicate(self):
            await asyncio.Event().wait()

        def kill(self):
            self.returncode = -9

        async def wait(self):
            self.reaped = True
            return self.returncode

    proc = Process()

    async def spawn(*args, **kwargs):
        return proc

    monkeypatch.setattr(speech_synth.asyncio, "create_subprocess_exec", spawn)
    monkeypatch.setattr(speech_synth, "COMMAND_TIMEOUT_SECONDS", 0.01)
    async with asyncio.timeout(1):
        with pytest.raises(speech_synth.SpeechError, match="timed out"):
            await speech_synth._run_command(["fixture-only"])
    assert proc.reaped and proc.returncode == -9


@pytest.mark.parametrize(
    "app,available",
    [
        (None, False),
        ({"version": "0.5.2-feed"}, False),
        ({"version": "0.5.2-feed", "build": "7654321"}, False),
        ({"version": "0.5.2-feed", "build": "abcdef0"}, True),
    ],
    ids=["missing-app", "missing-build", "different-build", "exact-identity"],
)
def test_assets_offer_requires_known_exact_app_identity(short_tmp, app, available):
    from types import SimpleNamespace

    from orb_hub.config import Config
    from orb_hub.server import Hub

    hub = Hub(Config(data_dir=short_tmp / "data", log_dir=short_tmp / "logs"))
    pack = manifest("6", assets=True)
    hub.releases.current = lambda channel, assets=False: pack if assets else app
    device = SimpleNamespace(channel="dev", fw="0.5.2-feed", build="abcdef0", hello={"assets": "5"}, ota={})
    assert hub.assets_offer_for(device, "127.0.0.1")["available"] is available


@pytest.mark.parametrize("release", ["0.5.2-feed", "0.5.1"], ids=["same-core-bench", "downgrade"])
def test_release_warns_when_new_artifact_will_not_be_automatically_offered(signing_key, image_file, capsys, release):
    assert cli.main(["release", str(image_file("first.bin", version="0.5.2-bench")), "--channel", "dev"]) == 0
    capsys.readouterr()
    assert cli.main(["release", str(image_file("second.bin", version=release)), "--channel", "dev"]) == 0
    output = capsys.readouterr().out
    assert "Warning:" in output
    assert "higher numeric major.minor.patch" in output
    assert "explicit recovery" in output
