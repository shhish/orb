"""Optional embedding lifecycle port; standalone adapters retain their own close."""

import asyncio
import logging


def spawn(operation, *, name, owner=None):
    if owner is not None:
        return owner.spawn(operation, name=name)
    task = asyncio.create_task(operation, name=name)

    def finished(done):
        if not done.cancelled() and (error := done.exception()) is not None:
            logging.getLogger(__name__).error(
                "%s failed (%s)", name, type(error).__name__
            )

    task.add_done_callback(finished)
    return task
