"""Read-only incremental provider data, separate from provider control transports."""

from __future__ import annotations

import asyncio
import contextvars
import time
from pathlib import Path

from orb_agents.progress import JsonlTail, ProgressReducer
from orb_agents.usage import UsageCollector


class AgentData:
    def __init__(self, manager, data_dir: Path):
        self.manager = manager
        self.usage = UsageCollector(data_dir / "agents" / "claude-usage.json")
        self._tails: dict[Path, JsonlTail] = {}
        self._reducers: dict[str, ProgressReducer] = {}
        self._identities: dict[Path, str] = {}
        self._paths: dict[Path, str] = {}
        self._live_sessions: set[str] = set()
        self._next_scan = 0.0

    def _discover(self, sessions):
        paths = {}
        for source in self.manager.transcript_sources(sessions):
            paths[source.path] = source.provider
            # A parsed session_meta record is authoritative after replacement.
            if source.session_id is not None and source.path not in self._identities:
                self._identities[source.path] = source.session_id
                # The first read may have started beyond session_meta and
                # consumed this file for usage before its filename bound it.
                self._tails.pop(source.path, None)
        self._paths = paths
        for path in self._tails.keys() - paths.keys():
            self._tails.pop(path, None)
            self._identities.pop(path, None)
        live = {session.id for session in sessions}
        self._reducers = {key: value for key, value in self._reducers.items() if key in live}

    def _read(self, sessions):
        now = time.monotonic()
        if now >= self._next_scan:
            self._discover(sessions)
            self._next_scan = now + 2.0
        live = {s.id for s in sessions}
        newly_live = live - self._live_sessions
        for path, provider in self._paths.items():
            # Account usage is read even before discovery admits a session.
            # Hydrate its bounded recent progress when it becomes visible,
            # including a return after its reducer was pruned.
            if self._identities.get(path) in newly_live:
                self._tails.pop(path, None)
            tail = self._tails.setdefault(path, JsonlTail())
            try:
                records = tail.read(path)
            except OSError:
                # Providers rotate/remove files independently of the directory
                # scan. One disappearing session cannot stall every stream.
                continue
            for record in records:
                if provider == "codex":
                    self.usage.observe_codex(record)
                    payload = record.get("payload")
                    if record.get("type") == "session_meta" and isinstance(payload, dict):
                        native = payload.get("id")
                        if isinstance(native, str) and native:
                            self._identities[path] = f"codex:{native}"
                key = self._identities.get(path)
                if key in live:
                    reducer = self._reducers.setdefault(key, ProgressReducer(provider))
                    reducer.feed(record)
        self._live_sessions = live
        self.usage.refresh_claude()
        for observation in self.manager.usage_observations():
            self.usage.observe(observation)

    async def refresh(self, sessions):
        context = contextvars.copy_context()
        operation = asyncio.get_running_loop().run_in_executor(None, context.run, self._read, sessions)
        cancelled = False
        while not operation.done():
            try:
                await asyncio.shield(operation)
            except asyncio.CancelledError:
                # A native read still owns the tails/reducers after cancellation.
                # Join it even if shutdown cancels this waiter more than once.
                cancelled = True
        operation.result()
        if cancelled:
            raise asyncio.CancelledError

    def snapshot(self, session, wire_id, *, now=None):
        now = time.time() if now is None else now
        reducer = self._reducers.get(session.id)
        if reducer:
            value = reducer.snapshot(wire_id, now=now)
        else:
            value = {
                "session": wire_id,
                "seq": 0,
                "at": session.updated_at.timestamp(),
                "state": "idle",
                "stale": True,
                "activity": "Waiting for progress",
                "steps": [],
                "assistant": {"id": "", "text": "", "final": False},
            }
        # Hooks own waiting/exited state. Otherwise recent item progress wins
        # over lagging discovery (including equal timestamps on tool records).
        if (
            session.state in {"needs_approval", "waiting_input", "error", "done"}
            or reducer is None
            or not reducer.seq
            or session.updated_at.timestamp() > value["at"]
        ):
            value["state"] = {"working": "busy", "needs_approval": "waiting", "waiting_input": "waiting"}.get(
                session.state, session.state
            )
        return value
