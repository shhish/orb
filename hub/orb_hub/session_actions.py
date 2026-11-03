"""Provider invocation with durable correlated first-answer results."""

import asyncio
import logging

from orb_hub import envelope, models
from orb_hub.envelope import LinkError
from orb_hub.lifecycle import finish_shutdown

log = logging.getLogger(__name__)


class SessionActions:
    def __init__(self, store, delivery, supervisor):
        self.supervisor = supervisor
        self.store = store
        self.delivery = delivery
        self.answers = {}
        self.message_handlers = {}
        self.message_checks = {}
        self._answer_locks = {}
        self.send_timeout = store.send_timeout
        self.clock = store.clock

    def device_connected(self, target, device_id):
        check = self.message_checks.get(target)
        if check is not None:
            check(device_id)
            return True
        return None

    async def answer(self, device_id: str, params: dict):
        return await self.supervisor.spawn(
            self._answer(device_id, params), name="orb-card-action", expected_errors=(LinkError,)
        )

    async def _answer(self, device_id: str, params: dict):
        models.fields(params, {"card", "option", "text"}, {"card"})
        key = models.text(params["card"], 64)
        if ("option" in params) == ("text" in params):
            raise LinkError(envelope.BAD_REQUEST, "answer needs either option or text")
        lock, users = self._answer_locks.get(key, (asyncio.Lock(), 0))
        self._answer_locks[key] = (lock, users + 1)
        try:
            async with lock:
                # Once the owner's answer is durable it must reach the provider:
                # stop() waits (bounded by send_timeout) instead of dropping it.
                return await finish_shutdown(self._decide_and_deliver(device_id, key, params), name="orb-card-answer")
        finally:
            row = self.store.card_record(key)
            if row is not None and row["status"] != "live":
                self.answers.pop(key, None)
                self.delivery.broadcast("card.cancel", {"card": key}, row["device"])
            _, users = self._answer_locks[key]
            if users == 1:
                self._answer_locks.pop(key)
            else:
                self._answer_locks[key] = (lock, users - 1)

    async def _decide_and_deliver(self, device_id: str, key: str, params: dict):
        row, answer, claimed = await self.store.decide(device_id, {**params, "card": key})
        if not claimed:
            return {"card": key, "answer": answer, "delivered": bool(row["delivered"])}
        callback = self.answers.pop(key, None)
        self.delivery.broadcast("card.cancel", {"card": key}, row["device"])
        delivered = False
        if callback is not None:
            try:
                async with asyncio.timeout(self.send_timeout):
                    result = await callback({"card": key, **answer, "device_id": device_id})
                delivered = result is not False
            except Exception:  # noqa: BLE001 - provider failures must not disclose payloads or secrets.
                # Provider payloads and decisions must never enter logs.
                log.warning("card answer delivery failed")
        await self.store.record_delivery(key, delivered)
        return {"card": key, "answer": answer, "delivered": delivered}

    def register_message_handler(self, target, handler, *, check=None):
        self.message_handlers[target] = handler
        if check is not None:
            self.message_checks[target] = check
        else:
            self.message_checks.pop(target, None)

    def unregister_message_handler(self, target):
        self.message_handlers.pop(target, None)
        self.message_checks.pop(target, None)

    def check_target(self, target, device_id=None):
        target = models.text(target, 48)
        if target != "dot" and target not in self.store.sessions:
            raise LinkError(envelope.NOT_FOUND, "unknown session")
        if device_id is not None:
            self.store.check_source(device_id, "dot" if target == "dot" else self.store.sessions[target]["agent"])
        if target not in self.message_handlers or (target != "dot" and not self.store.sessions[target]["can_message"]):
            raise LinkError("unsupported", "target cannot accept messages")
        if target in self.message_checks:
            self.message_checks[target](device_id)
        return target

    async def message(self, device_id, params):
        return await self.supervisor.spawn(
            self._message(device_id, params), name="orb-message-action", expected_errors=(LinkError,)
        )

    async def _message(self, device_id, params):
        models.fields(params, {"target", "text", "stream"}, {"target", "text"})
        target = self.check_target(params["target"], device_id)
        text = models.text(params["text"], 280)
        try:
            async with asyncio.timeout(self.send_timeout):
                result = await self.message_handlers[target](device_id, text, stream=params.get("stream"))
        except LinkError:
            raise
        except TimeoutError:
            raise LinkError(envelope.TIMEOUT, "message delivery timed out") from None
        except Exception:  # noqa: BLE001 - provider failures must not disclose payloads or secrets.
            raise LinkError(envelope.INTERNAL, "message delivery failed") from None
        if isinstance(result, dict) and result.get("accepted") is False:
            raise LinkError(result.get("reason", "unsupported"), "message was not accepted")
        status = result.get("status", "delivered") if isinstance(result, dict) else "delivered"
        if status not in {"queued", "delivered"}:
            raise LinkError(envelope.INTERNAL, "message delivery failed: invalid provider receipt")
        reason = (
            result.get("reason", "Provider transport accepted the message")
            if isinstance(result, dict)
            else "Provider transport accepted the message"
        )
        return {"accepted": True, "status": status, "reason": models.text(reason, 160)}

    def register_answer_handler(self, key, handler):
        self.answers[key] = handler

    def unregister_answer_handler(self, key):
        self.answers.pop(key, None)
