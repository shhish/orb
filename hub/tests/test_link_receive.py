"""An abrupt device drop is a disconnect, not a failed hub worker."""

from types import SimpleNamespace

from orb_hub.link import Link


class DroppingSocket:
    async def receive(self, **kwargs):
        assert kwargs["timeout"] == 1
        raise ConnectionResetError("peer reset")


async def test_connection_reset_during_receive_is_a_disconnect():
    link = SimpleNamespace(ws=DroppingSocket(), power=SimpleNamespace(idle_timeout=1))
    assert await Link._receive_frame(link) is None
