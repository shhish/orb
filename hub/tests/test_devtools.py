"""Developer tools with real loopback TLS and throwaway pairing/control state."""

import asyncio
import base64
import json
import struct
import zlib

import pytest
from fake_device import paired_device

from orb_hub import cli, control
from orb_hub.control import ControlError
from orb_hub.devtools import COMMANDS, MAX_EVENTS, MAX_OUTPUT, screenshot_png, validate


async def developer(hub, monkeypatch, **kwargs):
    device = await paired_device(hub, **kwargs)
    await device.disconnect()
    hub.store.set_config(device.device_id, channel="dev")
    await device.connect()
    hello = await device.hello(caps=["devtools"])
    assert hello["r"]["caps"] == ["devtools"]
    accept_commands(device, monkeypatch)
    return device


def accept_commands(device, monkeypatch):
    original = device.ws.send_str

    async def accepted(text):
        obj = json.loads(text)
        if obj.get("t") == "res" and obj.get("r") == {}:
            obj["r"] = {"accepted": True}
            text = json.dumps(obj)
        await original(text)

    monkeypatch.setattr(device.ws, "send_str", accepted)


def call(hub, device, command="heap", args=None):
    return asyncio.create_task(
        hub.handle_command(
            "dev",
            {
                "device_id": device.device_id,
                "command": command,
                "args": [] if args is None else args,
            },
        )
    )


async def requested(device):
    async with asyncio.timeout(2):
        return await device.hub_requests.get()


async def output(device, stream, text, seq=0):
    await device.send({"t": "evt", "m": "devtools.output", "p": {"stream": stream, "seq": seq, "text": text}})


async def done(device, stream, ok=True, code="internal"):
    params = {"stream": stream, "ok": ok}
    if not ok:
        params["code"] = code
    await device.send({"t": "evt", "m": "devtools.done", "p": params})


def shot_text(raw=None):
    raw = raw or b"\xe0\x07" * (466 * 466)
    header = f"@@ORBSHOT BEGIN 466 466 rgb565le 192 {len(raw)} {zlib.crc32(raw):08x}\n"
    rows = [
        f"@@{i:05x} {base64.b64encode(raw[off : off + 192]).decode()}\n"
        for i, off in enumerate(range(0, len(raw), 192))
    ]
    return header + "".join(rows) + "@@ORBSHOT END\n"


async def test_tls_developer_output_and_ping_remain_live(hub, monkeypatch):
    device = await developer(hub, monkeypatch)
    try:
        task = call(hub, device, "tap", [233, 400])
        request = await requested(device)
        assert request["m"] == "devtools.tap"
        assert request["p"] == {"stream": 1, "args": [233, 400]}
        assert await hub.online[device.device_id].request("device.ping") == {"uptime_s": 12}
        await output(device, 1, "@@ORBDEV OK tap 233 400\n")
        await done(device, 1)
        assert await task == {"text": "@@ORBDEV OK tap 233 400\n"}
        assert not hub.devtools.active
    finally:
        await device.aclose()


async def test_tls_screenshot_assembly_and_cli_atomic_save(hub, monkeypatch, short_tmp):
    device = await developer(hub, monkeypatch)
    try:
        task = call(hub, device, "shot")
        request = await requested(device)
        stream = request["p"]["stream"]
        text = shot_text()
        # Fragment boundaries intentionally split both headers and base64 lines.
        for seq, off in enumerate(range(0, len(text), 1733)):
            await output(device, stream, text[off : off + 1733], seq)
        await done(device, stream)
        result = await task
        png = base64.b64decode(result["png"])
        assert png.startswith(b"\x89PNG\r\n\x1a\n")
        assert struct.unpack("!II", png[16:24]) == (466, 466)
        monkeypatch.setattr(control, "call", lambda *args, **kwargs: result)
        path = short_tmp / "image.png"
        path.write_bytes(b"previous")
        assert cli.main(["dev", device.device_id, "shot", str(path)]) == 0
        assert path.read_bytes() == png
        assert not list(path.parent.glob(".image.png.*"))

        def failed(*args, **kwargs):
            raise ControlError("capture failed")

        monkeypatch.setattr(control, "call", failed)
        assert cli.main(["dev", device.device_id, "shot", str(path)]) == 1
        assert path.read_bytes() == png
    finally:
        await device.aclose()


@pytest.mark.parametrize("damage", ["crc", "duplicate", "order", "truncated", "size", "base64", "header", "extra"])
def test_screenshot_rejects_corrupt_or_unbounded_transfer(damage):
    text = shot_text()
    if damage == "crc":
        text = text.replace(text.splitlines()[0][-8:], "00000000", 1)
    elif damage == "duplicate":
        text = text.replace("@@00001", "@@00000", 1)
    elif damage == "order":
        text = text.replace("@@00000", "@@00001", 1)
    elif damage == "truncated":
        text = text.replace("@@ORBSHOT END\n", "")
    elif damage == "size":
        text = text.replace("434312", "99999999999", 1)
    elif damage == "base64":
        text = text.replace("@@00000 ", "@@00000 !", 1)
    elif damage == "header":
        text = text.replace("466 466", "999 466", 1)
    else:
        text += "@@00000 AAAA\n"
    with pytest.raises(ControlError):
        screenshot_png(text)


@pytest.mark.parametrize(
    "command,args",
    [
        ("tap", [True, 1]),
        ("tap", [-1, 2]),
        ("hold", [1, 1, 5001]),
        ("swipe", [1, 2, 3, 4, 0]),
        ("arc", [0, 0, 90, 400]),
        ("arc", [233, 0, 90, 400]),
        ("arc", [216, -3601, 90, 400]),
        ("arc", [216, 0, 3601, 400]),
        ("arc", [216, 0, 90, 0]),
        ("arc", [216, 0, 90, 5001]),
        ("arc", [216, True, 90, 400]),
        ("arc", [216, 0.5, 90, 400]),
        ("arc", [216, 0, 90]),
        ("arc", [216, 0, 90, 400, 1]),
        ("tilt", [True, 0, 100]),
        ("tilt", ["0.1", 0, 100]),
        ("tilt", [float("nan"), 0, 100]),
        ("tilt", [0, float("inf"), 100]),
        ("tilt", [1.01, 0, 100]),
        ("tilt", [10**1000, 0, 100]),
        ("tilt", [0.8, 0.8, 100]),
        ("tilt", [0, 0, 0]),
        ("tilt", [0, 0, 5001]),
        ("tilt", [0, 0, 1.5]),
        ("tilt", [0, 0]),
        ("bench", [999]),
        ("bench", [60001]),
        ("heap", [1]),
        ("shot", ["file"]),
        ("profile", [1]),
        ("bench_open", ["invalid"]),
        ("bench_game", ["marble; reboot", 1000]),
        ("bench_scroll", ["agents", "1000"]),
        ("buttons", [1]),
        ("press", ["pwr; reboot", "short"]),
        ("press", ["pwr", "confirm"]),
        ("press", [0, "hold"]),
    ],
)
def test_rejects_invalid_developer_arguments(command, args):
    with pytest.raises(ControlError):
        validate(command, args)


async def test_tls_arc_preserves_unwrapped_clockwise_angles(hub, monkeypatch):
    device = await developer(hub, monkeypatch)
    try:
        task = call(hub, device, "arc", [216, 350, 370, 400])
        request = await requested(device)
        assert request["m"] == "devtools.arc"
        assert request["p"] == {"stream": 1, "args": [216, 350, 370, 400]}
        await output(device, 1, "@@ORBDEV OK arc 216 350 370 400\n")
        await done(device, 1)
        assert await task == {"text": "@@ORBDEV OK arc 216 350 370 400\n"}
    finally:
        await device.aclose()


async def test_tls_tilt_preserves_fractional_specific_force(hub, monkeypatch):
    device = await developer(hub, monkeypatch)
    try:
        task = call(hub, device, "tilt", [-0.25, 0.125, 1000])
        request = await requested(device)
        assert request["m"] == "devtools.tilt"
        assert request["p"] == {"stream": 1, "args": [-0.25, 0.125, 1000]}
        await output(device, 1, "@@ORBDEV OK tilt -0.25 0.125 1000\n")
        await done(device, 1)
        assert await task == {"text": "@@ORBDEV OK tilt -0.25 0.125 1000\n"}
    finally:
        await device.aclose()


async def test_tls_requires_dev_channel_and_negotiated_build_capability(hub, monkeypatch):
    device = await paired_device(hub)
    try:
        with pytest.raises(ControlError, match="dev-channel"):
            await call(hub, device)
        await device.disconnect()
        hub.store.set_config(device.device_id, channel="stable")
        await device.connect()
        result = await device.hello(caps=["devtools"])
        assert result["r"]["caps"] == []
        with pytest.raises(ControlError):
            await call(hub, device)
        assert device.hub_requests.empty()
    finally:
        await device.aclose()


@pytest.mark.parametrize("caps", [["devtools"], []])
async def test_boot_support_negotiates_on_one_socket_and_legacy_stays_online(hub, caps, caplog):
    device = await paired_device(hub)
    try:
        await device.disconnect()
        hub.store.set_config(device.device_id, channel="dev")
        caplog.clear()
        with caplog.at_level("INFO"):
            await device.connect()
            result = await device.hello(caps=caps)
            link = hub.online[device.device_id]
            assert result["r"]["caps"] == caps
            assert result["r"]["device"]["channel"] == "dev"
            assert await link.request("device.ping") == {"uptime_s": 12}
            assert (await device.request("ota.check"))["ok"]
            assert hub.online[device.device_id] is link
        assert sum(f"{device.device_id} online from" in record.message for record in caplog.records) == 1
        assert not any(f"{device.device_id} offline" in record.message for record in caplog.records)
    finally:
        await device.aclose()


@pytest.mark.parametrize("failure", ["sequence", "fragment", "events", "bytes", "done", "device", "unicode", "badcode"])
async def test_tls_rejects_output_and_completion_failures(hub, monkeypatch, failure):
    device = await developer(hub, monkeypatch)
    try:
        task = call(hub, device)
        request = await requested(device)
        transfer = hub.devtools.active[hub.online[device.device_id]]
        stream = request["p"]["stream"]
        if failure == "sequence":
            await output(device, stream, "data", 1)
        elif failure == "fragment":
            await output(device, stream, "x" * 2049)
        elif failure == "events":
            transfer.sequence = MAX_EVENTS
            await output(device, stream, "data", MAX_EVENTS)
        elif failure == "bytes":
            transfer.size = MAX_OUTPUT
            await output(device, stream, "data")
        elif failure == "unicode":
            # The envelope parser rejects invalid Unicode before routing; also
            # exercise the service boundary directly against an invalid caller.
            hub.devtools.event(
                hub.online[device.device_id], "devtools.output", {"stream": stream, "seq": 0, "text": "\ud800"}
            )
        elif failure == "badcode":
            await device.send({"t": "evt", "m": "devtools.done", "p": {"stream": stream, "ok": False, "code": []}})
        elif failure == "done":
            await device.send({"t": "evt", "m": "devtools.done", "p": {"stream": stream, "ok": 1}})
        else:
            await done(device, stream, False, "busy")
        with pytest.raises(ControlError):
            await task
        assert not hub.devtools.active
    finally:
        await device.aclose()


async def test_tls_busy_disconnect_reconnect_and_late_output(hub, monkeypatch):
    device = await developer(hub, monkeypatch)
    try:
        task = call(hub, device)
        await requested(device)
        with pytest.raises(ControlError, match="busy"):
            await call(hub, device)
        old = hub.online[device.device_id]
        await device.disconnect()
        with pytest.raises(ControlError, match="closed"):
            await task
        await device.connect()
        assert (await device.hello(caps=["devtools"]))["ok"]
        accept_commands(device, monkeypatch)
        new = hub.online[device.device_id]
        assert old is not new
        # Old link output cannot satisfy a new connection even with identical IDs.
        next_task = call(hub, device)
        await requested(device)
        hub.devtools.event(old, "devtools.done", {"stream": 1, "ok": True})
        await output(device, 2, "stale")
        assert not next_task.done()
        await output(device, 1, "new")
        await done(device, 1)
        assert await next_task == {"text": "new"}
    finally:
        await device.aclose()


async def test_tls_channel_change_revokes_pending_access(hub, monkeypatch):
    device = await developer(hub, monkeypatch)
    try:
        task = call(hub, device)
        await requested(device)
        await hub.handle_command("config", {"device_id": device.device_id, "channel": "stable"})
        with pytest.raises(ControlError, match="revoked"):
            await task
        assert "devtools" not in hub.online[device.device_id].caps
        await hub.handle_command("config", {"device_id": device.device_id, "channel": "dev"})
        with pytest.raises(ControlError, match="dev-channel"):
            await call(hub, device)
    finally:
        await device.aclose()


async def test_tls_deadline_and_cancellation_cleanup(hub, monkeypatch):
    from orb_hub import devtools

    monkeypatch.setattr(devtools, "DEADLINE", 0.03)
    device = await developer(hub, monkeypatch)
    try:
        task = call(hub, device)
        await requested(device)
        with pytest.raises(ControlError, match="deadline"):
            await task
        assert not hub.devtools.active
        monkeypatch.setattr(devtools, "DEADLINE", 90)
        task = call(hub, device)
        await requested(device)
        task.cancel()
        with pytest.raises(asyncio.CancelledError):
            await task
        assert not hub.devtools.active
    finally:
        await device.aclose()


@pytest.mark.parametrize("command", COMMANDS)
def test_cli_developer_arguments_reach_only_private_control(command, monkeypatch):
    specification = COMMANDS[command]
    values = [permitted[0] for permitted in specification]
    if command == "tilt":
        values = [-0.25, 0.125, 1000]
    observed = []

    def fake_call(path, cmd, args, timeout):
        observed.append((cmd, args, timeout))
        return {"text": "receipt\n", "png": base64.b64encode(b"unused").decode()}

    monkeypatch.setattr(control, "call", fake_call)
    if command == "shot":
        return  # File safety is covered by the TLS screenshot test.
    assert cli.main(["dev", "orb-020000abcdef", command, *map(str, values)]) == 0
    assert observed == [("dev", {"device_id": "orb-020000abcdef", "command": command, "args": values}, 95)]


async def test_tls_screenshot_revocation_during_encoding(hub, monkeypatch):
    from orb_hub import devtools

    device = await developer(hub, monkeypatch)
    entered, release = asyncio.Event(), asyncio.Event()

    async def paused_encoding(function, text):
        entered.set()
        await release.wait()
        return b"unused"

    monkeypatch.setattr(devtools.asyncio, "to_thread", paused_encoding)
    try:
        task = call(hub, device, "shot")
        await requested(device)
        await done(device, 1)
        async with asyncio.timeout(1):
            await entered.wait()
        hub.devtools.revoke(hub.online[device.device_id])
        release.set()
        with pytest.raises(ControlError, match="revoked"):
            await task
    finally:
        release.set()
        await device.aclose()


async def test_global_developer_admission_cap(hub, monkeypatch):
    device = await developer(hub, monkeypatch)
    try:
        # Four reservations held by independent links fill the shared budget.
        for index in range(4):
            hub.devtools.active[object()] = object()
        with pytest.raises(ControlError, match="busy"):
            await call(hub, device)
        assert device.hub_requests.empty()
    finally:
        hub.devtools.active.clear()
        await device.aclose()


async def test_tls_failed_benchmark_preserves_bounded_handler_reason(hub, monkeypatch):
    device = await developer(hub, monkeypatch)
    try:
        task = call(hub, device, "bench_open", ["apps"])
        await requested(device)
        receipt = "@@ORBDEV ERROR bench_open surface=apps reason=source_timeout"
        await output(device, 1, "unrelated private log\n" + receipt + "\n@@ORBDEV ERROR /private/provider/path\n")
        await done(device, 1, False, "internal")
        with pytest.raises(ControlError) as error:
            await task
        assert str(error.value) == "developer command failed: internal\n" + receipt
    finally:
        await device.aclose()


@pytest.mark.parametrize("enabled", ["on", "off"])
async def test_lightsleep_usb_uses_private_wifi_command(hub, monkeypatch, enabled):
    device = await developer(hub, monkeypatch)
    try:
        task = call(hub, device, "lightsleep", ["usb", enabled])
        request = await requested(device)
        assert request["m"] == "devtools.lightsleep"
        assert request["p"]["args"] == ["usb", enabled]
        stream = request["p"]["stream"]
        await output(device, stream, f"@@ORBDEV OK lightsleep usb {enabled}\n")
        await done(device, stream)
        assert (await task)["text"].endswith(f"usb {enabled}\n")
    finally:
        await device.aclose()


@pytest.mark.parametrize("args", [["battery", "on"], ["usb", True], ["usb", "on", 1]])
def test_lightsleep_rejects_invalid_arguments(args):
    with pytest.raises(ControlError):
        validate("lightsleep", args)
