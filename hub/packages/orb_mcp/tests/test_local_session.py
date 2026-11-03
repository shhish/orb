import importlib.util
from pathlib import Path


async def test_scripted_serve_stdio_webhook_session():
    path = Path(__file__).parents[1] / "scripts" / "local_session.py"
    spec = importlib.util.spec_from_file_location("orb_local_session", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    result = await module.run_session()
    assert result == {
        "discover": True,
        "cards_shown": 2,
        "callback_verified": True,
        "signed_events_received": 1,
        "answer_persisted": True,
    }
