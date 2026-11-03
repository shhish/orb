"""The hub owns device I/O; callbacks return only after durable acknowledgement."""

from collections.abc import Awaitable, Callable
from copy import deepcopy
from dataclasses import dataclass
from typing import Protocol


@dataclass(frozen=True)
class Answer:
    card_id: str
    device_id: str
    kind: str = "option"
    option_id: str | None = None


@dataclass(frozen=True)
class Transcript:
    recording_id: str
    text: str
    duration_s: float
    device_id: str
    focus: str = "dot"
    confirmed: bool = False


AnswerHandler = Callable[[Answer], Awaitable[dict]]
TranscriptHandler = Callable[[Transcript], Awaitable[dict]]


class DeviceBackend(Protocol):
    """Called on one asyncio loop. show/cancel must be idempotent by card_id.

    show_card returns True only after the device accepts the card, False if
    offline. Device dictionaries contain device_id, name, online. Hub adapters
    authenticate device_id before invoking callbacks and ACK only on success.
    On reconnect call service.sync_device(device_id) to replay pending cards.
    """

    def set_handlers(
        self, answer: AnswerHandler, transcript: TranscriptHandler
    ) -> None: ...
    async def show_card(self, card: dict) -> bool: ...
    async def cancel_card(self, card_id: str, device_id: str | None) -> bool: ...
    async def list_devices(self) -> list[dict]: ...


class FakeDeviceBackend:
    """In-memory device, never a connection to physical hardware."""

    def __init__(self, *, online: bool = True):
        self.devices = [{"device_id": "orb-fake", "name": "Fake Orb", "online": online}]
        self.cards: dict[str, dict] = {}
        self.shown: list[str] = []
        self.cancelled: list[str] = []
        self._answer: AnswerHandler | None = None
        self._transcript: TranscriptHandler | None = None

    def set_handlers(
        self, answer: AnswerHandler, transcript: TranscriptHandler
    ) -> None:
        self._answer, self._transcript = answer, transcript

    async def list_devices(self) -> list[dict]:
        return deepcopy(self.devices)

    async def show_card(self, card: dict) -> bool:
        online = any(
            d["device_id"] == card["device_id"] and d["online"] for d in self.devices
        )
        if online:
            self.cards[card["card_id"]] = deepcopy(card)
            self.shown.append(card["card_id"])
        return online

    async def cancel_card(self, card_id: str, device_id: str | None) -> bool:
        online = any(d["device_id"] == device_id and d["online"] for d in self.devices)
        if online:
            self.cards.pop(card_id, None)
            self.cancelled.append(card_id)
        return online

    async def answer(
        self,
        card_id: str,
        option_id: str | None = None,
        *,
        kind: str = "option",
        device_id: str = "orb-fake",
    ) -> dict:
        if self._answer is None:
            raise RuntimeError("Backend is not attached")
        return await self._answer(Answer(card_id, device_id, kind, option_id))

    async def transcribe(self, **kwargs) -> dict:
        if self._transcript is None:
            raise RuntimeError("Backend is not attached")
        return await self._transcript(Transcript(**kwargs))
