"""Offline lead-script evidence validation; all device/serial entry points are fake."""

import base64
import importlib
import importlib.util
import struct
import subprocess
import sys
import wave
import zlib
from pathlib import Path
from types import SimpleNamespace

import orb_dev_remote as remote
import pytest
from orb_devtools.logd import validate_command

SCRIPT = Path(__file__).resolve().parents[1] / "board_validate.py"
SPEC = importlib.util.spec_from_file_location("orb_sound_board_validate", SCRIPT)
board = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(board)


@pytest.fixture
def hub_validator(monkeypatch):
    monkeypatch.syspath_prepend(str(SCRIPT.parents[2] / "hub"))
    module = importlib.import_module("orb_hub.devtools")
    return module.validate, module.ControlError


@pytest.fixture(scope="module")
def transfer():
    # Distinct signed values prove that stereo order and endianness survive.
    pcm = b"".join(struct.pack("<hh", i % 32768, -(i % 32768)) for i in range(80000))
    crc = zlib.crc32(pcm)
    lines = [f"@@ORBDEV MIC rate=16000 channels=2 frames=80000 bytes=320000 crc32={crc:08x}"]
    for sequence, offset in enumerate(range(0, len(pcm), 192)):
        encoded = base64.b64encode(pcm[offset : offset + 192]).decode()
        lines.append(f"@@ORBDEV MIC_CHUNK seq={sequence} data={encoded}")
    lines.append(f"@@ORBDEV MIC_END chunks={len(lines) - 1} bytes=320000 crc32={crc:08x}")
    return pcm, lines


def test_decode_full_stereo_and_wavs_are_exact(transfer, tmp_path):
    pcm, lines = transfer
    assert (
        board.decode_microphones("boot log\n" + "\r\n".join(lines) + "\n@@ORBDEV OK micrecord\n")
        == pcm
    )
    board.write_waves(tmp_path, pcm)
    expected = {
        "both-mics-5s.wav": (2, pcm),
        "mic-1-5s.wav": (1, b"".join(pcm[i : i + 2] for i in range(0, len(pcm), 4))),
        "mic-2-5s.wav": (1, b"".join(pcm[i : i + 2] for i in range(2, len(pcm), 4))),
    }
    for name, (channels, raw) in expected.items():
        with wave.open(str(tmp_path / name)) as wav:
            assert wav.getparams()[:4] == (channels, 2, 16000, 80000)
            assert wav.readframes(80001) == raw


@pytest.mark.parametrize(
    "corruption",
    [
        "no_header",
        "no_end",
        "missing",
        "duplicate",
        "reordered",
        "wrong_count",
        "trailer_crc",
        "payload_crc",
        "truncated",
        "bad_base64",
        "after_end",
        "duplicate_header",
        "duplicate_end",
    ],
)
def test_corrupted_microphone_transfer_fails_closed(transfer, corruption):
    _, original = transfer
    lines = original.copy()
    if corruption == "no_header":
        lines.pop(0)
    elif corruption == "no_end":
        lines.pop()
    elif corruption == "missing":
        lines.pop(10)
    elif corruption == "duplicate":
        lines.insert(10, lines[9])
    elif corruption == "reordered":
        lines[10], lines[11] = lines[11], lines[10]
    elif corruption == "wrong_count":
        lines[-1] = lines[-1].replace("chunks=1667", "chunks=1666")
    elif corruption == "trailer_crc":
        lines[-1] = lines[-1][:-8] + "00000000"
    elif corruption == "payload_crc":
        offset = lines[1].index("data=") + 5
        lines[1] = lines[1][:offset] + "AQ" + lines[1][offset + 2 :]
    elif corruption == "truncated":
        lines[-2] = lines[-2][:-4]
    elif corruption == "bad_base64":
        lines[1] = lines[1].replace("data=", "data=!")
    elif corruption == "after_end":
        lines.append(lines[1])
    elif corruption == "duplicate_header":
        lines.insert(1, lines[0])
    else:
        lines.append(lines[-1])
    with pytest.raises(ValueError):
        board.decode_microphones("\n".join(lines))


@pytest.mark.parametrize(
    "command",
    [
        "soundstatus",
        "soundstop",
        "senses",
        "soundplay 0",
        "soundplay 18",
        "soundvolume 0",
        "soundvolume 100",
        "soundsilent 0",
        "soundalwayson 1",
        "micrecord",
    ],
)
def test_sound_commands_use_bounded_transport_and_explicit_completion(command, hub_validator):
    validate_hub, _error = hub_validator
    name, *words = command.split()
    assert validate_hub(name, [int(value) for value in words]) is None
    assert validate_command(command) == command
    assert remote.command_timeout(command) == (90 if command == "micrecord" else 10)
    report = b"@@ORBDEV MIC_END chunks=1667 bytes=320000 crc32=12345678\n"
    assert not remote.complete(report, command)
    assert remote.complete(report + f"@@ORBDEV OK {command}\n".encode(), command)
    with pytest.raises(RuntimeError):
        remote.complete(
            report + f"@@ORBDEV ERROR {command}\n@@ORBDEV OK {command}\n".encode(), command
        )


@pytest.mark.parametrize(
    "command",
    [
        "soundplay 19",
        "soundplay -1",
        "soundvolume 101",
        "soundsilent 2",
        "soundalwayson -1",
        "micrecord 5",
        "senses extra",
        "soundplay 0\nheap",
    ],
)
def test_invalid_sound_commands_cannot_reach_io(command, monkeypatch, hub_validator):
    validate_hub, hub_error = hub_validator
    name, *words = command.split()
    values = [int(word) if word.lstrip("-").isdigit() else word for word in words]
    with pytest.raises(hub_error):
        validate_hub(name, values)

    def forbidden(*_args, **_kwargs):
        pytest.fail("invalid command reached serial or daemon")

    monkeypatch.setattr(remote, "daemon", forbidden)
    monkeypatch.setattr(remote, "open_port", forbidden)
    with pytest.raises(ValueError):
        validate_command(command)
    with pytest.raises(ValueError):
        remote.run_command("/not-a-device", command)


@pytest.mark.parametrize("command", ["soundstatus", "senses", "micrecord"])
def test_usb_cli_keeps_machine_readable_devtools_prefix(command, monkeypatch, capsys, transfer):
    if command == "micrecord":
        reply = ("\n".join(transfer[1]) + "\n@@ORBDEV OK micrecord\n").encode()
    else:
        reply = f"@@ORBDEV SOUND ready=1 volume=37 silent=1 always_on=1 face_down=0\n@@ORBDEV OK {command}\n".encode()
    calls = []

    def fake_run(port, name):
        calls.append((port, name))
        return reply

    monkeypatch.setattr(remote, "run_command", fake_run)
    monkeypatch.setattr(sys, "argv", ["orb_dev_remote.py", "/not-a-device", command])
    assert remote.main() == 0
    output = capsys.readouterr().out
    assert calls == [("/not-a-device", command)]
    assert output == reply.decode()
    if command == "micrecord":
        assert board.decode_microphones(output) == transfer[0]
    else:
        assert board.sound_status(output)["volume"] == 37


def test_plan_and_existing_evidence_never_contact_a_device(monkeypatch, tmp_path, capsys):
    def forbidden(*_args, **_kwargs):
        pytest.fail("offline plan or existing evidence contacted a device")

    monkeypatch.setattr(board.subprocess, "run", forbidden)
    monkeypatch.setattr(sys, "argv", [str(SCRIPT), "--plan", "--out", str(tmp_path / "fresh")])
    board.main()
    assert "offline" in board.__doc__ and "5 s stereo" in capsys.readouterr().out
    monkeypatch.setattr(sys, "argv", [str(SCRIPT), "--out", str(tmp_path)])
    with pytest.raises(SystemExit, match="output already exists"):
        board.main()


def fake_board(monkeypatch, transfer, *, fail=None, device="orb-020000abcdef"):
    calls = []
    state = {"ready": 1, "volume": 37, "silent": 1, "always_on": 1, "face_down": 0}

    def run(argv, **kwargs):
        assert argv[0] == str(board.ROOT / "scripts/orb-dev.sh")
        assert kwargs == {"text": True, "capture_output": True, "timeout": 100, "check": False}
        command = " ".join(argv[1:])
        assert validate_command(command) == command
        assert remote.command_timeout(command) <= 90
        if command == "wake":  # keeps the display awake; not a validation step
            return SimpleNamespace(returncode=0, stdout="@@ORBDEV OK wake\n", stderr="")
        calls.append(command)
        if fail is not None and fail(command, calls):
            return SimpleNamespace(returncode=1, stdout="@@ORBDEV ERROR injected\n", stderr="")
        kind, *args = command.split()
        if kind == "soundstatus":
            text = (
                "@@ORBDEV SOUND "
                + " ".join(f"{key}={value}" for key, value in state.items())
                + "\n"
            )
        elif kind == "micrecord":
            text = "\n".join(transfer[1]) + "\n"
        elif kind in ("soundvolume", "soundsilent", "soundalwayson"):
            state[
                {"soundvolume": "volume", "soundsilent": "silent", "soundalwayson": "always_on"}[
                    kind
                ]
            ] = int(args[0])
            text = ""
        elif kind == "soundplay" and (
            not state["volume"]
            # Silent and Always On drop ordinary cues; timer and alarm ring.
            or (state["always_on"] or state["silent"])
            and int(args[0]) not in (12, 13)
        ):
            return SimpleNamespace(
                returncode=1,
                stdout="orb transport: device rejected command: @@ORBDEV ERROR soundplay reason=unavailable_or_suppressed\n",
                stderr="",
            )
        else:
            text = f"@@ORBDEV SENSES device={device} simulated=1\n"
        return SimpleNamespace(returncode=0, stdout=text + f"@@ORBDEV OK {command}\n", stderr="")

    monkeypatch.setattr(board.subprocess, "run", run)
    monkeypatch.setattr(board.time, "sleep", lambda _seconds: None)
    return calls, state


def test_full_offline_lead_script_plays_palette_records_stereo_and_restores(
    monkeypatch, tmp_path, transfer
):
    calls, state = fake_board(monkeypatch, transfer)
    output = tmp_path / "new-run"
    monkeypatch.setattr(sys, "argv", [str(SCRIPT), "--out", str(output)])
    board.main()
    for cue in range(19):
        assert f"soundplay {cue}" in calls
    assert all(f"soundvolume {value}" in calls for value in (0, 20, 40, 60, 80, 100))
    assert "micrecord" in calls and calls.count("senses") >= 2
    first_mutation = next(
        i for i, command in enumerate(calls) if command.startswith("soundsilent ")
    )
    assert calls.index("senses") < first_mutation
    assert {key: state[key] for key in ("volume", "silent", "always_on")} == {
        "volume": 37,
        "silent": 1,
        "always_on": 1,
    }
    assert (output / "COMPLETE").exists()
    with wave.open(str(output / "both-mics-5s.wav")) as wav:
        assert wav.readframes(80000) == transfer[0]


def test_command_failure_still_restores_every_preference(monkeypatch, tmp_path, transfer):
    calls, state = fake_board(monkeypatch, transfer, fail=lambda cmd, _calls: cmd == "soundplay 5")
    output = tmp_path / "failed-run"
    monkeypatch.setattr(sys, "argv", [str(SCRIPT), "--out", str(output)])
    with pytest.raises(RuntimeError, match="device command failed"):
        board.main()
    assert "soundvolume 37" in calls and "soundsilent 1" in calls and "soundalwayson 1" in calls
    assert state["volume"] == 37 and state["silent"] == state["always_on"] == 1
    assert not (output / "COMPLETE").exists()


def test_failed_stop_during_restore_attempts_remaining_settings_and_never_marks_complete(
    monkeypatch, tmp_path, transfer
):
    calls, _state = fake_board(
        monkeypatch, transfer, fail=lambda cmd, calls: cmd == "soundstop" and "micrecord" in calls
    )
    output = tmp_path / "restore-failed-run"
    monkeypatch.setattr(sys, "argv", [str(SCRIPT), "--out", str(output)])
    with pytest.raises(RuntimeError):
        board.main()
    assert calls[-5:] == [
        "soundstop",
        "soundvolume 37",
        "soundsilent 1",
        "soundalwayson 1",
        "soundstatus",
    ]
    assert not (output / "COMPLETE").exists()


def test_timeout_preserves_failed_run_and_attempts_restore(monkeypatch, tmp_path, transfer):
    calls, _state = fake_board(monkeypatch, transfer)
    original = board.subprocess.run

    def timed_out(argv, **kwargs):
        if argv[1:] == ["micrecord"]:
            raise subprocess.TimeoutExpired(argv, 100, output="partial mic transfer")
        return original(argv, **kwargs)

    monkeypatch.setattr(board.subprocess, "run", timed_out)
    output = tmp_path / "timed-out-run"
    monkeypatch.setattr(sys, "argv", [str(SCRIPT), "--out", str(output)])
    with pytest.raises(subprocess.TimeoutExpired):
        board.main()
    assert "soundvolume 37" in calls and "soundsilent 1" in calls and "soundalwayson 1" in calls
    assert not (output / "COMPLETE").exists() and not (output / "both-mics-5s.wav").exists()


def test_wrong_device_is_rejected_before_any_mutating_command(monkeypatch, tmp_path, transfer):
    calls, state = fake_board(monkeypatch, transfer, device="orb-000000000002")
    output = tmp_path / "wrong-device-run"
    monkeypatch.setattr(sys, "argv", [str(SCRIPT), "--out", str(output)])
    with pytest.raises(SystemExit):
        board.main()
    assert "senses" in calls
    assert set(calls) <= {"senses", "soundstatus"}
    assert state["volume"] == 37 and state["silent"] == state["always_on"] == 1
    assert not (output / "COMPLETE").exists()


@pytest.mark.parametrize(
    "name,maximum",
    [("soundplay", 18), ("soundvolume", 100), ("soundsilent", 1), ("soundalwayson", 1)],
)
@pytest.mark.parametrize("invalid", [True, False, None, 0.0, "0", [], {}])
def test_hub_sound_arguments_reject_coercions(name, maximum, invalid, hub_validator):
    validate_hub, hub_error = hub_validator
    assert validate_hub(name, [0]) is None
    assert validate_hub(name, [maximum]) is None
    with pytest.raises(hub_error):
        validate_hub(name, [invalid])
