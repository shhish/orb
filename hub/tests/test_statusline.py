"""The real statusline command with synthetic stdin and isolated hub state."""

import io
import json

from orb_agents.usage import MAX_STATUSLINE_BYTES, claude_usage_path

from orb_hub import cli


def test_statusline_command_prints_model_context_and_caches_usage(short_tmp, monkeypatch, capsys):
    payload = {
        "model": {"display_name": "Opus"},
        "context_window": {"used_percentage": 25.4},
        "rate_limits": {"five_hour": {"used_percentage": 31, "resets_at": 2000000000}},
        "session_id": "SYNTHETIC-IGNORED",
    }
    monkeypatch.setattr("sys.stdin", io.StringIO(json.dumps(payload)))
    monkeypatch.setattr(
        cli, "_call", lambda *args, **kwargs: (_ for _ in ()).throw(AssertionError("No control socket"))
    )
    assert cli.main(["statusline"]) == 0
    output = capsys.readouterr()
    assert output.out == "Opus | context 25%\n"
    assert output.err == ""
    assert "SYNTHETIC-IGNORED" not in claude_usage_path(short_tmp / "data").read_text()
    assert not (short_tmp / ".claude").exists()
    assert not (short_tmp / ".codex").exists()


def test_statusline_custom_directory_and_absent_context(short_tmp, monkeypatch, capsys):
    monkeypatch.setattr("sys.stdin", io.StringIO('{"model":{"id":"synthetic-model"},"rate_limits":{}}'))
    directory = short_tmp / "custom"
    assert cli.main(["statusline", "--data-dir", str(directory)]) == 0
    assert capsys.readouterr().out == "synthetic-model | context ?\n"
    assert claude_usage_path(directory).is_file()
    assert not (short_tmp / "data").exists()


def test_statusline_invalid_input_is_generic_and_leaves_cache_untouched(short_tmp, monkeypatch, capsys):
    for raw in ("SYNTHETIC-PRIVATE-INPUT", "[]", "x" * (MAX_STATUSLINE_BYTES + 1)):
        monkeypatch.setattr("sys.stdin", io.StringIO(raw))
        assert cli.main(["statusline"]) == 1
        output = capsys.readouterr()
        assert output.out == "Claude | context ?\n"
        assert output.err == "orb-hub: invalid statusline input\n"
    assert not (short_tmp / "data").exists()


def test_statusline_missing_rates_never_invents_usage(short_tmp, monkeypatch, capsys):
    monkeypatch.setattr("sys.stdin", io.StringIO('{"model":null,"context_window":{"used_percentage":null}}'))
    assert cli.main(["statusline"]) == 0
    assert capsys.readouterr().out == "Claude | context ?\n"
    assert not claude_usage_path(short_tmp / "data").exists()


def test_codex_queue_flag_is_explicit():
    parser = cli.build_parser()
    assert parser.parse_args(["serve"]).codex_queue is False
    assert parser.parse_args(["serve", "--codex-queue"]).codex_queue is True
