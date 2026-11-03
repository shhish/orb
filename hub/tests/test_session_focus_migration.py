"""Migration and review regressions for focused session delivery."""

import asyncio
import sqlite3

import pytest

from orb_hub import envelope
from orb_hub.envelope import LinkError
from orb_hub.sessions import COALESCE_SECONDS, SessionHub, encoded


class CaptureLink:
    device_id = "orb-001122334455"

    def __init__(self):
        self.events = []

    async def notify(self, method, params):
        self.events.append((method, params))


@pytest.mark.parametrize("source", ["claude", "codex", "dot"])
async def test_legacy_source_unknown_until_exact_provider_rebind(short_tmp, source):
    card = {
        "id": "mcp:legacy-card",
        "kind": "choice",
        "title": "Legacy question",
        "body": "Choose in the original provider",
        "options": [{"id": "continue", "label": "Continue", "role": "default"}],
        "reply": False,
        "priority": "normal",
    }
    # The previous schema persisted neither source nor provider identity in
    # the wire Card, so migration cannot safely infer Dot from missing data.
    with sqlite3.connect(short_tmp / "sessions.db") as old:
        old.execute(
            "CREATE TABLE session_cards (id TEXT PRIMARY KEY,content TEXT NOT NULL,device TEXT,"
            "status TEXT NOT NULL,answer TEXT,delivered INTEGER NOT NULL DEFAULT 0)"
        )
        old.execute(
            "INSERT INTO session_cards(id,content,device,status) VALUES (?,?,?,'live')",
            (card["id"], encoded(card), CaptureLink.device_id),
        )
    coordinator = SessionHub(short_tmp)
    link = CaptureLink()
    delivered = []

    async def answer(value):
        delivered.append(value)
        return True

    async def drain():
        await asyncio.wait_for(coordinator._subscribers[link].queue.join(), 1)

    try:
        await coordinator.subscribe(link)
        await drain()
        assert all(method != "card.show" for method, _ in link.events)
        assert card["id"] in coordinator.cards
        with pytest.raises(LinkError, match="unsupported"):
            await coordinator.answer(link.device_id, {"card": card["id"], "option": "continue"})
        assert delivered == []

        await coordinator.set_prefs(link.device_id, {"sources": {source: False}})
        assert await coordinator.show_card(card, device_id=link.device_id, answer_handler=answer, source=source)
        await drain()
        assert all(method != "card.show" for method, _ in link.events)
        assert coordinator.db.execute("SELECT source FROM session_cards").fetchone()[0] == source
        with pytest.raises(LinkError, match="conflict"):
            await coordinator.show_card(
                {**card, "body": "Replacement content"}, device_id=link.device_id, source=source
            )

        await coordinator.set_prefs(link.device_id, {"sources": {source: True}})
        await drain()
        assert [(method, value["id"]) for method, value in link.events if method == "card.show"] == [
            ("card.show", card["id"])
        ]
        result = await coordinator.answer(link.device_id, {"card": card["id"], "option": "continue"})
        assert result["delivered"] is True
        assert len(delivered) == 1
    finally:
        await coordinator.close()


def session(key, *, state="idle", summary="Ready"):
    return {
        "id": key,
        "agent": "codex",
        "title": "Orb",
        "project": "hub",
        "state": state,
        "reason": "",
        "summary": summary,
        "updated": 1,
        "can_message": False,
    }


async def test_provider_removals_preserve_coalescing_cadence_and_latest_content(short_tmp):
    class TimedLink(CaptureLink):
        def __init__(self):
            super().__init__()
            self.updates = []
            self.latest = asyncio.Event()

        async def notify(self, method, params):
            if method == "session.update":
                self.updates.append((asyncio.get_running_loop().time(), params["session"]))
                if params["session"]["summary"] == "step 39":
                    self.latest.set()
            await super().notify(method, params)

    coordinator = SessionHub(short_tmp)
    link = TimedLink()
    try:
        await coordinator.upsert_session(session("visible", state="busy"))
        for index in range(20):
            await coordinator.upsert_session(session(f"idle-{index}"))
        await coordinator.subscribe(link)
        await coordinator._subscribers[link].queue.join()
        await coordinator.start()
        # Unrelated provider removals used to force refreshes between regular
        # ticks, publishing every burst instead of its latest visible state.
        for index in range(40):
            await coordinator.upsert_session(session("visible", state="busy", summary=f"step {index}"))
            if index % 2 == 0:
                await coordinator.remove_session(f"idle-{index // 2}")
            await asyncio.sleep(0.02)
        await asyncio.wait_for(link.latest.wait(), 2)
        assert len(link.updates) >= 3
        assert all(
            later[0] - earlier[0] >= COALESCE_SECONDS - 0.01 for earlier, later in zip(link.updates, link.updates[1:])
        )
        assert link.updates[-1][1]["summary"] == "step 39"
    finally:
        await coordinator.close()


async def test_large_valid_request_id_cannot_make_nonempty_page_look_exhausted(short_tmp):
    coordinator = SessionHub(short_tmp)
    request_id = int("9" * 4000)
    params = {"include": "idle"}
    try:
        await coordinator.upsert_session(session("collapsed"))
        assert len(encoded(envelope.request(request_id, "sessions.list", params)).encode()) <= 4096
        with pytest.raises(LinkError, match="too_large"):
            coordinator.list_sessions(CaptureLink.device_id, params, request_id=request_id)
    finally:
        await coordinator.close()
