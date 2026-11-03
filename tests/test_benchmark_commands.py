"""Benchmark bounds and framing without opening serial ports or sockets."""

import orb_dev_remote as remote
import pytest
from orb_devtools.logd import validate_command


@pytest.mark.parametrize("duration", [1000, 1001, 12000, 59999, 60000])
def test_benchmark_bounds_agree_and_leave_cleanup_time(duration):
    command = f"bench {duration}"
    assert validate_command(command) == command
    assert remote.command_timeout(command) == duration / 1000 + 10
    assert remote.command_timeout(command) <= 120  # daemon request timeout ceiling


@pytest.mark.parametrize("surface", ["agents", "rooms", "settings", "notifications", "favourites"])
@pytest.mark.parametrize("duration", [1000, 60000])
def test_scroll_trace_receipt_has_bounded_deferred_output_time(surface, duration):
    command = f"bench_scroll {surface} {duration}"
    assert validate_command(command) == command
    assert remote.command_timeout(command) == duration / 1000 + 25
    assert remote.command_timeout(command) <= 120


@pytest.mark.parametrize(
    "command",
    [
        "bench",
        "bench 0",
        "bench -1000",
        "bench 999",
        "bench 60001",
        "bench 999999999999999999999999999999",
        "bench 1000 2000",
        "bench 1000.0",
        "bench nan",
        "bench  1000",
        " bench 1000",
        "bench 1000 ",
        "bench\t1000",
        "bench 1000\nheap",
        "bench 1000\r",
    ],
)
def test_invalid_benchmark_rejected_by_both_transports_before_io(monkeypatch, command):
    def forbidden(*_args, **_kwargs):
        pytest.fail("invalid benchmark reached a serial port or daemon socket")

    monkeypatch.setattr(remote, "open_port", forbidden)
    monkeypatch.setattr(remote, "daemon", forbidden)
    with pytest.raises(ValueError):
        validate_command(command)
    with pytest.raises(ValueError):
        remote.run_command("/not-a-device", command)


def test_benchmark_report_is_not_completion():
    command = "bench 1000"
    report = b"@@ORBDEV BENCH scroll seconds=1.00 frames=20 fps=20.0\n"
    assert not remote.complete(report, command)
    assert not remote.complete(report + b"@@ORBDEV OK bench 1000", command)
    assert not remote.complete(report + b"@@ORBDEV OK bench 10000\n", command)
    assert remote.complete(report + b"@@ORBDEV OK bench 1000\r\n", command)


def test_benchmark_failure_is_not_hidden_by_report_or_ok():
    with pytest.raises(RuntimeError, match="device rejected"):
        remote.complete(
            b"@@ORBDEV BENCH scroll seconds=0.5 frames=10\n"
            b"@@ORBDEV ERROR bench: scene must finish\n"
            b"@@ORBDEV OK bench 1000\n",
            "bench 1000",
        )


def test_power_is_bounded_read_only_command():
    assert validate_command("power") == "power"
    assert remote.command_timeout("power") == 10
    report = b"@@ORBDEV POWER BEGIN\nMode CPU_MAX 240MHz\n@@ORBDEV POWER END\n"
    assert not remote.complete(report, "power")
    assert remote.complete(report + b"@@ORBDEV OK power\n", "power")


@pytest.mark.parametrize("button", ["pwr", "boot"])
@pytest.mark.parametrize("gesture", ["short", "double", "hold"])
def test_button_commands_preserve_exact_synthetic_gesture_tokens(button, gesture):
    command = f"press {button} {gesture}"
    assert validate_command(command) == command
    assert remote.command_timeout(command) == 10
    assert remote.complete(f"@@ORBDEV OK {command}\n".encode(), command)


def test_button_history_is_bounded_read_only_command():
    assert validate_command("buttons") == "buttons"
    assert remote.command_timeout("buttons") == 10


@pytest.mark.parametrize(
    "command",
    [
        "buttons 1",
        "press",
        "press pwr",
        "press gpio3 short",
        "press pwr confirm",
        "press pwr hold 2000",
        "press pwr short\nheap",
        "press  pwr short",
        "press boot;reboot hold",
    ],
)
def test_button_commands_reject_raw_gpio_trust_escalation_and_command_injection(command):
    with pytest.raises(ValueError):
        validate_command(command)
    with pytest.raises(ValueError):
        remote.command_timeout(command)


@pytest.mark.parametrize("command", ["power 1", "power\nheap", "power ", " power"])
def test_power_rejects_arguments_and_command_injection(command):
    with pytest.raises(ValueError):
        validate_command(command)
    with pytest.raises(ValueError):
        remote.command_timeout(command)


@pytest.mark.parametrize("kind", ["bench_transition", "bench_quiet"])
@pytest.mark.parametrize("duration", [1000, 16000, 25000, 60000])
def test_fixture_commands_share_bounded_transport_contract(kind, duration):
    command = f"{kind} {duration}"
    assert validate_command(command) == command
    assert remote.command_timeout(command) == duration / 1000 + 10
    assert not remote.complete(b"@@ORBDEV OK bench 16000\n", command)
    assert remote.complete(f"@@ORBDEV OK {command}\n".encode(), command)


@pytest.mark.parametrize("kind", ["bench_transition", "bench_quiet"])
@pytest.mark.parametrize("duration", ["999", "60001", "0", "nan", "16000\nheap"])
def test_fixture_commands_reject_invalid_duration_and_injection(kind, duration):
    with pytest.raises(ValueError):
        validate_command(f"{kind} {duration}")
    with pytest.raises(ValueError):
        remote.command_timeout(f"{kind} {duration}")


@pytest.mark.parametrize("surface", ["play", "dot", "agents", "apps", "back"])
def test_open_commands_are_bounded_and_framed(surface):
    command = f"bench_open {surface}"
    assert validate_command(command) == command
    assert remote.command_timeout(command) == 20
    assert not remote.complete(b"@@ORBDEV OPEN surface=play cold=1\n", command)
    assert remote.complete(f"@@ORBDEV OK {command}\n".encode(), command)


@pytest.mark.parametrize(
    "command",
    [
        "bench_open",
        "bench_open 1000",
        "bench_open unknown",
        "bench_open PLAY",
        "bench_open play warm",
        "bench_open  play",
        "bench_open play\nheap",
        "bench_open play ",
        "bench_open\tplay",
    ],
)
def test_open_rejects_invalid_surfaces_and_injection_before_io(command):
    with pytest.raises(ValueError):
        validate_command(command)
    with pytest.raises(ValueError):
        remote.command_timeout(command)


@pytest.mark.parametrize("game", ["marble", "orbit", "ring-snake", "pop"])
@pytest.mark.parametrize("duration", [1000, 16000, 60000])
def test_game_benchmark_commands_are_bounded_and_framed(game, duration):
    command = f"bench_game {game} {duration}"
    assert validate_command(command) == command
    assert remote.command_timeout(command) == duration / 1000 + 10
    assert not remote.complete(b"@@ORBDEV GAME game=marble fps=30.0\n", command)
    assert not remote.complete(b"@@ORBDEV OK bench 16000\n", command)
    assert remote.complete(f"@@ORBDEV OK {command}\n".encode(), command)


@pytest.mark.parametrize(
    "command",
    [
        "bench_game",
        "bench_game marble",
        "bench_game unknown 16000",
        "bench_game MARBLE 16000",
        "bench_game marble 999",
        "bench_game marble 60001",
        "bench_game marble nan",
        "bench_game marble 1000.0",
        "bench_game marble  1000",
        "bench_game marble 1000 extra",
        "bench_game marble 1000\nheap",
        "bench_game marble 1000 ",
        "bench_game\tmarble 1000",
    ],
)
def test_game_benchmark_rejects_invalid_commands_before_io(monkeypatch, command):
    def forbidden(*_args, **_kwargs):
        pytest.fail("invalid game benchmark reached serial or daemon")

    monkeypatch.setattr(remote, "open_port", forbidden)
    monkeypatch.setattr(remote, "daemon", forbidden)
    with pytest.raises(ValueError):
        validate_command(command)
    with pytest.raises(ValueError):
        remote.run_command("/not-a-device", command)


@pytest.mark.parametrize(
    "gesture",
    ["slide-left", "slide-right", "stack-up", "shade-down", "back", "iris", "rotary"],
)
@pytest.mark.parametrize("duration", [1000, 16000, 60000])
def test_navigation_benchmark_commands_are_bounded_and_framed(gesture, duration):
    command = f"bench_nav {gesture} {duration}"
    assert validate_command(command) == command
    assert remote.command_timeout(command) == duration / 1000 + 10
    report = f"@@ORBDEV NAV gesture={gesture} fps=45.0\n".encode()
    assert not remote.complete(report, command)
    assert not remote.complete(report + b"@@ORBDEV OK bench 16000\n", command)
    assert remote.complete(report + f"@@ORBDEV OK {command}\r\n".encode(), command)


@pytest.mark.parametrize(
    "command",
    [
        "bench_nav",
        "bench_nav slide-left",
        "bench_nav unknown 16000",
        "bench_nav SLIDE-LEFT 16000",
        "bench_nav slide-left 999",
        "bench_nav slide-left 60001",
        "bench_nav slide-left nan",
        "bench_nav slide-left 1000.0",
        "bench_nav slide-left  1000",
        "bench_nav slide-left 1000 extra",
        "bench_nav slide-left 1000\nheap",
        "bench_nav slide-left 1000 ",
        "bench_nav\tslide-left 1000",
    ],
)
def test_navigation_benchmark_rejects_invalid_commands_before_io(monkeypatch, command):
    def forbidden(*_args, **_kwargs):
        pytest.fail("invalid navigation benchmark reached serial or daemon")

    monkeypatch.setattr(remote, "open_port", forbidden)
    monkeypatch.setattr(remote, "daemon", forbidden)
    with pytest.raises(ValueError):
        validate_command(command)
    with pytest.raises(ValueError):
        remote.run_command("/not-a-device", command)
