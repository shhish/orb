"""Explicit lifetime ownership for asynchronous hub work.

Only operation labels and exception types enter diagnostics; exception messages
can contain provider text, device tokens or owner paths.
"""

import asyncio
import logging
from collections.abc import Callable, Coroutine
from typing import Any

log = logging.getLogger(__name__)


class LifecycleSupervisor:
    def __init__(self, owner: str):
        self.owner = owner
        self._tasks: set[asyncio.Task] = set()
        self._failures: dict[str, dict] = {}
        self._closed = False

    @property
    def tasks(self) -> frozenset[asyncio.Task]:
        return frozenset(self._tasks)

    def health(self) -> dict:
        return {
            "healthy": not self._failures,
            "active": len(self._tasks),
            "stopped": self._closed,
            "failures": {name: dict(value) for name, value in self._failures.items()},
        }

    def _failed(self, name: str, error: BaseException) -> None:
        previous = self._failures.get(name, {})
        self._failures[name] = {"type": type(error).__name__, "count": previous.get("count", 0) + 1}
        log.error("%s worker %s failed (%s)", self.owner, name, type(error).__name__)

    def spawn(
        self, operation: Coroutine[Any, Any, Any], *, name: str, expected_errors: tuple[type[Exception], ...] = ()
    ) -> asyncio.Task:
        if self._closed:
            operation.close()
            raise RuntimeError("Service supervisor is stopped")
        task = asyncio.create_task(operation, name=name)
        self._tasks.add(task)

        def finished(done: asyncio.Task) -> None:
            self._tasks.discard(done)
            if not done.cancelled():
                error = done.exception()
                if error is not None and not isinstance(error, expected_errors):
                    self._failed(name, error)

        task.add_done_callback(finished)
        return task

    def run(self, factory: Callable, *, name: str, restarts: int = 2, delay: float = 0.05) -> asyncio.Task:
        """Restart read-only/reconciling workers only; commands never use this."""
        if restarts < 0 or delay < 0:
            raise ValueError("Invalid recovery budget")

        async def worker():
            for attempt in range(restarts + 1):
                try:
                    return await factory()
                except asyncio.CancelledError:
                    raise
                except Exception as error:  # noqa: BLE001 - lifecycle boundary records sanitized failure types.
                    self._failed(name, error)
                    if attempt == restarts:
                        return None
                    await asyncio.sleep(delay * (attempt + 1))

        return self.spawn(worker(), name=name)

    async def stop(self) -> None:
        self._closed = True
        tasks = tuple(self._tasks)
        for task in tasks:
            task.cancel()
        joined = asyncio.gather(*tasks, return_exceptions=True)
        cancelled = False
        while True:
            try:
                await asyncio.shield(joined)
                break
            except asyncio.CancelledError:
                cancelled = True
        self._tasks.clear()
        if cancelled:
            raise asyncio.CancelledError


async def finish_shutdown(operation: Coroutine[Any, Any, Any], *, name: str):
    """Defer caller cancellation until the complete resource teardown returns.

    Shutdown owns a separate task so cancellation of its caller cannot skip
    later client, socket or database cleanup after workers have been joined.
    """
    owner = LifecycleSupervisor("shutdown")
    task = owner.spawn(operation, name=name)
    cancelled = False
    try:
        while not task.done():
            try:
                await asyncio.shield(task)
            except asyncio.CancelledError:
                cancelled = True
        result = task.result()
    finally:
        try:
            await owner.stop()
        except asyncio.CancelledError:
            cancelled = True
    if cancelled:
        raise asyncio.CancelledError
    return result
