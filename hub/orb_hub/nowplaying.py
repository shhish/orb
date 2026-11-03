"""Now Playing: observe Mac playback and control media and output volume.

Provides the Mac mini side of the 'nowplaying' capability:
- Reads active playback: title, artist, album, duration, position, playing state, source app.
- Checks applications in priority order without installing anything:
  1. Music app via AppleScript (checks 'is running' first so it never launches the app)
  2. Spotify via AppleScript (checks 'is running' first so it never launches the app)
  3. 'nowplaying-cli' binary if present on PATH.
- Reads and sets macOS output volume with clamping [0, 100].
- Controls: play/pause, next, previous sent to the active source app.
- Subscriptions: polls every 2 seconds while subscribed, pushing changes to paired links.
- Uses injectable runners so tests never interact with real apps or change system volume.
"""

import asyncio
import logging
import math
import shutil
from collections.abc import Awaitable, Callable
from dataclasses import dataclass
from typing import TYPE_CHECKING, Any

from orb_hub import envelope
from orb_hub.envelope import LinkError
from orb_hub.lifecycle import LifecycleSupervisor

if TYPE_CHECKING:
    from orb_hub.link import Link

log = logging.getLogger(__name__)

POLL_INTERVAL = 2.0
SUPPORTED_ACTIONS = {"play", "pause", "playpause", "next", "previous", "volume"}

MUSIC_SCRIPT = """
if application "Music" is running then
    tell application "Music"
        set pState to player state as string
        try
            set pPos to player position
            set tName to name of current track
            set tArtist to artist of current track
            set tAlbum to album of current track
            set tDur to duration of current track
            return pState & "||" & pPos & "||" & tName & "||" & tArtist & "||" & tAlbum & "||" & tDur
        on error
            return pState & "||0||||||0"
        end try
    end tell
else
    return "NOT_RUNNING"
end if
""".strip()

SPOTIFY_SCRIPT = """
if application "Spotify" is running then
    tell application "Spotify"
        set pState to player state as string
        try
            set pPos to player position
            set tName to name of current track
            set tArtist to artist of current track
            set tAlbum to album of current track
            set tDur to (duration of current track) / 1000.0
            return pState & "||" & pPos & "||" & tName & "||" & tArtist & "||" & tAlbum & "||" & tDur
        on error
            return pState & "||0||||||0"
        end try
    end tell
else
    return "NOT_RUNNING"
end if
""".strip()


@dataclass(frozen=True)
class NowPlayingSnapshot:
    """Current Mac media playback and system volume state."""

    title: str = ""
    artist: str = ""
    album: str = ""
    duration: float = 0.0
    position: float = 0.0
    playing: bool = False
    state: str = "stopped"  # "playing", "paused", "stopped"
    app: str = ""
    volume: int = 0

    def to_dict(self) -> dict[str, Any]:
        return {
            "title": self.title,
            "artist": self.artist,
            "album": self.album,
            "duration": round(self.duration, 2),
            "position": round(self.position, 2),
            "playing": self.playing,
            "state": self.state,
            "app": self.app,
            "volume": self.volume,
        }


AsyncRunner = Callable[[list[str]], Awaitable[tuple[int, str, str]]]


async def default_command_runner(cmd: list[str]) -> tuple[int, str, str]:
    """Runs a command asynchronously via asyncio subprocess and returns (rc, stdout, stderr)."""
    proc = await asyncio.create_subprocess_exec(
        *cmd,
        stdout=asyncio.subprocess.PIPE,
        stderr=asyncio.subprocess.PIPE,
    )
    stdout, stderr = await proc.communicate()
    return (
        proc.returncode or 0,
        stdout.decode("utf-8", "replace").strip(),
        stderr.decode("utf-8", "replace").strip(),
    )


def clamp_volume(val: Any) -> int:
    """Clamps an input number into an integer [0, 100]."""
    if isinstance(val, bool) or not isinstance(val, (int, float)):
        raise LinkError(envelope.BAD_REQUEST, "volume value must be a number")
    if math.isnan(val) or math.isinf(val):
        raise LinkError(envelope.BAD_REQUEST, "volume must be a finite number")
    return max(0, min(100, round(val)))


class NowPlayingService:
    """Observes media playback, polls changes, and provides controls."""

    def __init__(
        self,
        sessions=None,
        runner: AsyncRunner | None = None,
        poll_interval: float = POLL_INTERVAL,
        available: bool | None = None,
    ):
        self.supervisor = LifecycleSupervisor("nowplaying")
        self.sessions = sessions
        self._runner = runner or default_command_runner
        self.poll_interval = poll_interval
        self._available = available
        self._subscribers: set[Link] = set()
        self._poll_task: asyncio.Task | None = None
        self._last_snapshot = NowPlayingSnapshot()
        self._lock = asyncio.Lock()

    @property
    def available(self) -> bool:
        """Now playing observation requires macOS osascript."""
        if self._available is not None:
            return self._available
        return shutil.which("osascript") is not None

    @available.setter
    def available(self, value: bool) -> None:
        self._available = value

    async def start(self) -> None:
        """Initialises the service."""

    async def stop(self) -> None:
        """Cancels background polling and clears subscribers."""
        if self._poll_task and not self._poll_task.done():
            self._poll_task.cancel()
            await asyncio.gather(self._poll_task, return_exceptions=True)
            self._poll_task = None
        await self.supervisor.stop()
        self._subscribers.clear()

    async def _run_applescript(self, script: str) -> str:
        """Executes an AppleScript snippet via osascript without launching apps."""
        code, stdout, stderr = await self._runner(["osascript", "-e", script])
        if code != 0:
            log.debug("AppleScript failed (%d): %s", code, stderr)
            return ""
        return stdout

    async def read_volume(self) -> int:
        """Reads current macOS master output volume (0-100)."""
        script = "output volume of (get volume settings)"
        out = await self._run_applescript(script)
        try:
            return clamp_volume(int(out))
        except (ValueError, TypeError, LinkError):
            return 0

    async def set_volume(self, value: float) -> int:
        """Sets macOS master output volume with clamping to [0, 100]."""
        target = clamp_volume(value)
        script = f"set volume output volume {target}"
        await self._run_applescript(script)
        return target

    def _parse_player_output(self, raw: str, app_name: str, volume: int) -> NowPlayingSnapshot | None:
        """Parses player output format: state||pos||name||artist||album||duration."""
        if not raw or raw == "NOT_RUNNING":
            return None

        parts = raw.split("||")
        if len(parts) < 6:
            return None

        p_state, s_pos, name, artist, album, s_dur = parts[:6]
        p_state = p_state.lower().strip()
        state = "playing" if p_state == "playing" else "paused" if p_state == "paused" else "stopped"
        playing = state == "playing"

        try:
            pos = float(s_pos)
        except (ValueError, TypeError):
            pos = 0.0

        try:
            dur = float(s_dur)
        except (ValueError, TypeError):
            dur = 0.0

        return NowPlayingSnapshot(
            title=name.strip(),
            artist=artist.strip(),
            album=album.strip(),
            duration=max(0.0, dur),
            position=max(0.0, pos),
            playing=playing,
            state=state,
            app=app_name,
            volume=volume,
        )

    async def _try_music(self, volume: int) -> NowPlayingSnapshot | None:
        """Tries to read Music app playback."""
        out = await self._run_applescript(MUSIC_SCRIPT)
        return self._parse_player_output(out, "Music", volume)

    async def _try_spotify(self, volume: int) -> NowPlayingSnapshot | None:
        """Tries to read Spotify playback."""
        out = await self._run_applescript(SPOTIFY_SCRIPT)
        return self._parse_player_output(out, "Spotify", volume)

    async def _try_nowplaying_cli(self, volume: int) -> NowPlayingSnapshot | None:
        """Tries to read playback via 'nowplaying-cli' binary if installed."""
        if not shutil.which("nowplaying-cli"):
            return None

        cmd = ["nowplaying-cli", "get", "title", "artist", "album", "duration", "elapsedTime", "playbackRate"]
        code, stdout, _ = await self._runner(cmd)
        if code != 0 or not stdout:
            return None

        lines = [line.strip() for line in stdout.splitlines()]
        if len(lines) < 6:
            return None

        title, artist, album, s_dur, s_pos, s_rate = lines[:6]
        if title == "null":
            title = ""
        if artist == "null":
            artist = ""
        if album == "null":
            album = ""

        try:
            rate = float(s_rate)
        except (ValueError, TypeError):
            rate = 0.0

        try:
            pos = float(s_pos)
        except (ValueError, TypeError):
            pos = 0.0

        try:
            dur = float(s_dur)
        except (ValueError, TypeError):
            dur = 0.0

        playing = rate > 0.0
        state = "playing" if playing else "paused" if (title or artist) else "stopped"

        return NowPlayingSnapshot(
            title=title,
            artist=artist,
            album=album,
            duration=max(0.0, dur),
            position=max(0.0, pos),
            playing=playing,
            state=state,
            app="NowPlaying",
            volume=volume,
        )

    async def read_snapshot(self) -> NowPlayingSnapshot:
        """Reads current media snapshot and volume in priority order."""
        volume = await self.read_volume()

        # 1. Try Music app
        music = await self._try_music(volume)
        if music and (music.playing or music.title):
            return music

        # 2. Try Spotify
        spotify = await self._try_spotify(volume)
        if spotify and (spotify.playing or spotify.title):
            return spotify

        # 3. Try nowplaying-cli if available
        np_cli = await self._try_nowplaying_cli(volume)
        if np_cli and (np_cli.playing or np_cli.title):
            return np_cli

        # 4. Fallback to paused/stopped state of whichever app was running, or empty
        if music:
            return music
        if spotify:
            return spotify

        return NowPlayingSnapshot(volume=volume)

    async def control(self, action: str, value: Any = None) -> dict[str, Any]:
        """Executes a playback or volume control action."""
        if action not in SUPPORTED_ACTIONS:
            raise LinkError(envelope.BAD_REQUEST, f"unsupported action: {action!r}")

        if action == "volume":
            new_vol = await self.set_volume(value)
            snapshot = await self.read_snapshot()
            return {"accepted": True, "action": "volume", "value": new_vol, "snapshot": snapshot.to_dict()}

        # For media controls, find target app from last or current snapshot
        current = await self.read_snapshot()
        app = current.app or self._last_snapshot.app

        if app == "Music":
            cmd_map = {
                "play": 'tell application "Music" to play',
                "pause": 'tell application "Music" to pause',
                "playpause": 'tell application "Music" to playpause',
                "next": 'tell application "Music" to next track',
                "previous": 'tell application "Music" to previous track',
            }
            await self._run_applescript(cmd_map[action])
        elif app == "Spotify":
            cmd_map = {
                "play": 'tell application "Spotify" to play',
                "pause": 'tell application "Spotify" to pause',
                "playpause": 'tell application "Spotify" to playpause',
                "next": 'tell application "Spotify" to next track',
                "previous": 'tell application "Spotify" to previous track',
            }
            await self._run_applescript(cmd_map[action])
        elif shutil.which("nowplaying-cli"):
            cli_map = {
                "play": "play",
                "pause": "pause",
                "playpause": "togglePlayPause",
                "next": "next",
                "previous": "previous",
            }
            await self._runner(["nowplaying-cli", cli_map[action]])
        else:
            raise LinkError("unsupported", "no active media player available to control")

        # Re-read state after command
        updated = await self.read_snapshot()
        self._last_snapshot = updated
        return {"accepted": True, "action": action, "snapshot": updated.to_dict()}

    async def request(self, device_id: str, method: str, params: dict) -> dict[str, Any]:
        """Handles link requests for 'nowplaying.snapshot' and 'nowplaying.control'."""
        if method == "nowplaying.snapshot":
            snapshot = await self.read_snapshot()
            return snapshot.to_dict()
        if method == "nowplaying.control":
            action = params.get("action")
            if not isinstance(action, str):
                raise LinkError(envelope.BAD_REQUEST, "action parameter must be a string")
            return await self.control(action, params.get("value"))
        raise LinkError(envelope.UNKNOWN_METHOD, f"unknown method: {method}")

    async def subscribe(self, link: "Link") -> None:
        """Subscribes a device link to 2-second playback polling."""
        async with self._lock:
            self._subscribers.add(link)
            if self._poll_task is None or self._poll_task.done():
                self._poll_task = self.supervisor.run(self._poll_loop, name="orb-nowplaying-poll")

        # Push immediate snapshot
        snapshot = await self.read_snapshot()
        self._last_snapshot = snapshot
        try:
            await link.notify("nowplaying.snapshot", snapshot.to_dict())
        except (ConnectionError, LinkError, OSError, TimeoutError) as exc:
            log.debug("could not push initial nowplaying snapshot (%s)", type(exc).__name__)

    def unsubscribe(self, link: "Link") -> None:
        """Unsubscribes a link and stops polling when none remain."""
        self._subscribers.discard(link)
        if not self._subscribers and self._poll_task and not self._poll_task.done():
            self._poll_task.cancel()
            self._poll_task = None

    async def _poll_loop(self) -> None:
        """Polls media state every 2 seconds while subscribers are active."""
        while self._subscribers:
            try:
                await asyncio.sleep(self.poll_interval)
                snapshot = await self.read_snapshot()
                if snapshot != self._last_snapshot:
                    self._last_snapshot = snapshot
                    payload = snapshot.to_dict()
                    for link in list(self._subscribers):
                        try:
                            await link.notify("nowplaying.snapshot", payload)
                        except (ConnectionError, LinkError, OSError, TimeoutError) as exc:
                            log.debug(
                                "failed to notify link %s of nowplaying update (%s)",
                                getattr(link, "label", ""),
                                type(exc).__name__,
                            )
            except asyncio.CancelledError:
                break
            except (ConnectionError, LinkError, OSError, TimeoutError) as exc:
                log.warning("error in nowplaying poll loop (%s)", type(exc).__name__)
