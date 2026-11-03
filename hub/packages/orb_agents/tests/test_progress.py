"""All progress inputs are invented fixtures, never copied session transcripts."""

from __future__ import annotations

import json
from pathlib import Path

import pytest

from orb_agents.progress import (
    JsonlTail,
    ProgressReducer,
    display_text,
    last_sentence,
    tool_label,
)

FIXTURES = Path(__file__).parent / "fixtures"


def records(provider):
    return [
        json.loads(line)
        for line in (FIXTURES / f"{provider}_progress.jsonl").read_text().splitlines()
    ]


def test_claude_tool_correlation_human_labels_and_final_sentence():
    reducer = ProgressReducer("claude")
    values = records("claude")
    assert reducer.feed(values[0])
    initial = reducer.snapshot("claude:synth", now=1793102400)
    assert initial["activity"] == "Edited orb_nav.c"
    assert initial["assistant"]["text"] == "I am editing the navigation."
    assert "PRIVATE_THINKING" not in json.dumps(initial)
    for record in values[1:]:
        reducer.feed(record)
    result = reducer.snapshot("claude:synth", now=1793102407)
    assert result["steps"] == [
        {
            "id": "edit1",
            "kind": "tool",
            "label": "Edited orb_nav.c",
            "state": "done",
            "added": 0,
            "removed": 0,
        },
        {"id": "read1", "kind": "tool", "label": "Read 3 files", "state": "done"},
        {"id": "build1", "kind": "tool", "label": "Tests: 638 passed", "state": "done"},
    ]
    assert result["assistant"] == {
        "id": "msg4",
        "text": "The navigation is ready.",
        "final": True,
    }
    assert result["state"] == "idle"
    assert result["stale"] is False
    assert reducer.snapshot("claude:synth", now=result["at"] + 901)["stale"]
    seq = result["seq"]
    for record in values:
        assert reducer.feed(record) is False
    assert reducer.seq == seq


def test_codex_representations_tools_summary_and_deduplication():
    reducer = ProgressReducer("codex")
    values = records("codex")
    for record in values[:7]:
        reducer.feed(record)
    result = reducer.snapshot("codex:synth")
    assert result["steps"][0] == {
        "id": "reason1",
        "kind": "reasoning",
        "label": "Checking the navigation boundaries",
        "state": "done",
    }
    assert result["steps"][1]["label"] == "Tests: 638 passed"
    assert result["steps"][2]["label"] == "Edited orb_nav.c"
    assert result["assistant"] == {
        "id": "answer1",
        "text": "All tests passed.",
        "final": True,
    }
    assert "PRIVATE_" not in json.dumps(result)
    assert not reducer.feed(values[7])
    assert not reducer.feed(values[8])


def test_codex_no_raw_or_encrypted_reasoning_and_nonassistant_content():
    reducer = ProgressReducer("codex")
    for payload in [
        {
            "type": "reasoning",
            "content": [{"type": "text", "text": "hidden"}],
            "encrypted_content": "hidden",
        },
        {
            "type": "message",
            "role": "user",
            "content": [{"type": "output_text", "text": "hidden"}],
        },
    ]:
        assert not reducer.feed({"type": "response_item", "payload": payload})
    assert reducer.snapshot("codex:synth")["steps"] == []


def test_failed_results_and_completed_items():
    reducer = ProgressReducer("codex")
    reducer.feed(
        {
            "type": "response_item",
            "payload": {
                "type": "function_call",
                "name": "exec_command",
                "arguments": {"cmd": "pytest -q"},
                "call_id": "t1",
            },
        }
    )
    reducer.feed(
        {
            "type": "response_item",
            "payload": {
                "type": "function_call_output",
                "call_id": "t1",
                "output": {"exit_code": 1, "output": "details omitted"},
            },
        }
    )
    assert reducer.snapshot("codex:synth")["steps"][0]["state"] == "failed"
    reducer.feed(
        {
            "type": "event_msg",
            "payload": {
                "type": "item_completed",
                "item": {
                    "type": "commandExecution",
                    "id": "t2",
                    "command": "pytest -q",
                    "exitCode": 0,
                    "aggregatedOutput": "9 passed",
                },
            },
        }
    )
    result = reducer.snapshot("codex:synth")
    assert result["steps"][-1]["label"] == "Tests: 9 passed"
    assert result["steps"][-1]["state"] == "done"


@pytest.mark.parametrize("provider", ["claude", "codex"])
def test_failed_tool_does_not_claim_the_provider_turn_failed(provider):
    reducer = ProgressReducer(provider)
    if provider == "claude":
        records = [
            {
                "type": "assistant",
                "message": {
                    "content": [
                        {
                            "type": "tool_use",
                            "id": "t1",
                            "name": "Bash",
                            "input": {"command": "pytest -q"},
                        }
                    ]
                },
            },
            {
                "type": "user",
                "message": {
                    "content": [
                        {"type": "tool_result", "tool_use_id": "t1", "is_error": True}
                    ]
                },
            },
        ]
    else:
        records = [
            {
                "type": "response_item",
                "payload": {
                    "type": "function_call",
                    "call_id": "t1",
                    "name": "exec_command",
                    "arguments": {"cmd": "pytest -q"},
                },
            },
            {
                "type": "response_item",
                "payload": {
                    "type": "function_call_output",
                    "call_id": "t1",
                    "output": {"exit_code": 1},
                },
            },
        ]
    for record in records:
        reducer.feed(record)
    result = reducer.snapshot("s")
    assert result["steps"][0]["state"] == "failed"
    assert result["state"] == "busy"


def test_codex_user_interruption_is_stopped_instead_of_a_provider_error():
    reducer = ProgressReducer("codex")
    reducer.feed({"type": "event_msg", "payload": {"type": "task_started"}})
    reducer.feed({"type": "event_msg", "payload": {"type": "turn_aborted"}})
    result = reducer.snapshot("s")
    assert result["state"] == "idle"
    assert result["activity"] == "Turn stopped"


def test_claude_partial_messages_reconcile_complete_message():
    reducer = ProgressReducer("claude")
    reducer.feed(
        {
            "type": "stream_event",
            "event": {"type": "message_start", "message": {"id": "partial1"}},
        }
    )
    reducer.feed(
        {
            "type": "stream_event",
            "event": {
                "type": "content_block_delta",
                "delta": {"type": "text_delta", "text": "First sentence. "},
            },
        }
    )
    reducer.feed(
        {
            "type": "stream_event",
            "event": {
                "type": "content_block_delta",
                "delta": {"type": "text_delta", "text": "Second sentence."},
            },
        }
    )
    assert reducer.snapshot("claude:synth")["assistant"]["text"] == "Second sentence."
    seq = reducer.seq
    assert not reducer.feed(
        {
            "type": "assistant",
            "message": {
                "id": "partial1",
                "content": [
                    {"type": "text", "text": "First sentence. Second sentence."}
                ],
            },
        }
    )
    assert reducer.seq == seq
    reducer.feed({"type": "result", "is_error": False})
    assert reducer.snapshot("claude:synth")["assistant"]["final"]


@pytest.mark.parametrize("provider", ["claude", "codex"])
def test_utf8_byte_bounds_and_envelope(provider):
    reducer = ProgressReducer(provider)
    text = "Earlier sentence. " + "界" * 200
    if provider == "claude":
        record = {
            "type": "assistant",
            "message": {
                "id": "x" * 200,
                "content": [{"type": "text", "text": text}],
                "stop_reason": "end_turn",
            },
        }
    else:
        record = {
            "type": "response_item",
            "payload": {
                "type": "message",
                "id": "x" * 200,
                "role": "assistant",
                "content": [{"type": "output_text", "text": text}],
            },
        }
    reducer.feed(record)
    result = reducer.snapshot(f"{provider}:synth")
    assert 150 <= len(result["assistant"]["text"].encode()) <= 160
    assert len(result["assistant"]["id"].encode()) <= 64
    assert (
        len(json.dumps({"t": "evt", "m": "agent.stream", "p": result}).encode()) <= 4096
    )


@pytest.mark.parametrize(
    "name,value,expected",
    [
        (
            "Bash",
            {"command": "BUILD_TOKEN=throwaway idf.py -j4 build"},
            "Ran idf.py -j4 build",
        ),
        (
            "Bash",
            {"command": "curl --header 'Authorization: secret' https://example.test"},
            "Ran curl",
        ),
        ("Bash", {"command": "echo private; curl private"}, "Ran shell command"),
        ("Edit", {"path": "/tmp/API_KEY=throwaway"}, "Edited [redacted]"),
        (
            "Edit",
            {"path": "/tmp/0123456789abcdef0123456789abcdef"},
            "Edited [redacted]",
        ),
        ("Bash", {"command": "cat first.c second.c third.c"}, "Read 3 files"),
    ],
)
def test_safe_labels(name, value, expected):
    assert tool_label(name, value) == expected


def test_tail_partial_duplicate_truncation_and_inode_replacement(tmp_path):
    path = tmp_path / "synthetic.jsonl"
    tail = JsonlTail()
    first = {"type": "example", "id": "a"}
    second = {"type": "example", "id": "b"}
    path.write_text(json.dumps(first) + "\n" + '{"type":"example",')
    assert tail.read(path) == [first]
    with path.open("a") as stream:
        stream.write('"id":"b"}\n' + json.dumps(first) + "\n")
    assert tail.read(path) == [second]
    assert tail.read(path) == []
    path.write_text(json.dumps({"id": "c"}) + "\n")
    assert tail.read(path) == [{"id": "c"}]
    replacement = tmp_path / "replacement"
    replacement.write_text(json.dumps(first) + "\n" + json.dumps({"id": "d"}) + "\n")
    replacement.replace(path)
    assert tail.read(path) == [{"id": "d"}]


def test_tail_in_place_rewrite_larger_than_previous_offset(tmp_path):
    path = tmp_path / "synthetic.jsonl"
    tail = JsonlTail()
    path.write_text('{"id":"old"}\n')
    assert tail.read(path) == [{"id": "old"}]
    path.write_text('{"id":"new-with-longer-value"}\n')
    assert tail.read(path) == [{"id": "new-with-longer-value"}]


def test_tail_bounded_partial_and_invalid_lines(tmp_path):
    path = tmp_path / "synthetic.jsonl"
    tail = JsonlTail(max_read_bytes=10, max_line_bytes=20)
    path.write_text("\n" * 40)
    assert tail.read(path) == []
    with path.open("a") as stream:
        stream.write("x" * 40)
    for _ in range(5):
        tail.read(path)
        assert len(tail._partial) <= 20
    with path.open("a") as stream:
        stream.write('\nnot-json\n{"id":1}\n')
    output = []
    for _ in range(4):
        output.extend(tail.read(path))
    assert output == [{"id": 1}]


def test_consecutive_read_burst_keeps_stable_group_id():
    reducer = ProgressReducer("claude")
    for index in range(8):
        reducer.feed(
            {
                "type": "assistant",
                "message": {
                    "id": f"m{index}",
                    "content": [
                        {
                            "type": "tool_use",
                            "id": f"t{index}",
                            "name": "Read",
                            "input": {"path": f"file{index}"},
                        }
                    ],
                },
            }
        )
    assert reducer.snapshot("claude:synth")["steps"] == [
        {"id": "t0", "kind": "tool", "label": "Read 8 files", "state": "running"},
    ]


def test_quoted_environment_assignments_are_redacted():
    from orb_agents.progress import clip

    assert (
        clip('VALUE="two private words" KEY=secret done', 64)
        == "[redacted] [redacted] done"
    )
    assert clip('VALUE="two private words', 64) == "[redacted]"
    assert clip("VALUE='two private words", 64) == "[redacted]"


def test_duplicate_result_with_different_record_wrapper_does_not_restart_turn():
    reducer = ProgressReducer("codex")
    for record in records("codex"):
        reducer.feed(record)
    duplicate = records("codex")[3]
    duplicate["timestamp"] = "2026-10-27T12:00:10Z"
    assert not reducer.feed(duplicate)
    assert reducer.snapshot("codex:synth")["state"] == "idle"


@pytest.mark.parametrize("provider", ["claude", "codex"])
def test_schema_variations_ignore_nontext_or_malformed_optional_fields(provider):
    reducer = ProgressReducer(provider)
    malformed = [
        {"type": "stream_event", "event": ["unexpected"]},
        {
            "type": "stream_event",
            "event": {"type": "content_block_delta", "delta": [1]},
        },
        {"type": "stream_event", "event": {"type": "message_start", "message": []}},
        {
            "type": "response_item",
            "payload": {"type": "agent_message", "author": {"name": "other"}},
        },
        {
            "type": "response_item",
            "payload": {
                "type": "message",
                "role": "assistant",
                "content": [{"type": "output_text", "text": {"private": "body"}}],
            },
        },
        {"type": "response_item", "payload": {"type": []}},
        {"type": "event_msg", "payload": {"type": []}},
        {"type": "response_item", "payload": ["unexpected"]},
    ]
    for record in malformed:
        assert not reducer.feed(record)
    assert reducer.snapshot(f"{provider}:synth")["assistant"]["text"] == ""


def test_excessively_nested_json_line_does_not_block_later_records(tmp_path):
    path = tmp_path / "synthetic.jsonl"
    path.write_text(
        '{"nested":' + "[" * 20000 + "0" + "]" * 20000 + '}\n{"id":"valid"}\n'
    )
    assert JsonlTail().read(path) == [{"id": "valid"}]


def test_excessively_nested_tool_json_is_safely_omitted():
    nested = "[" * 20000 + "0" + "]" * 20000
    assert tool_label("exec_command", nested) == "Ran shell command"
    reducer = ProgressReducer("codex")
    reducer.feed(
        {
            "type": "response_item",
            "payload": {
                "type": "function_call",
                "name": "exec_command",
                "call_id": "t1",
                "arguments": {"cmd": "pytest -q"},
            },
        }
    )
    assert reducer.feed(
        {
            "type": "response_item",
            "payload": {
                "type": "function_call_output",
                "call_id": "t1",
                "output": nested,
            },
        }
    )
    assert reducer.snapshot("codex:synth")["steps"][0]["state"] == "done"


@pytest.mark.parametrize(
    "command,expected",
    [
        ("make sim-test -j4", "Ran make sim-test -j4"),
        ("cd /private/synthetic && nice -n 10 make hub-test", "Ran make hub-test"),
        (
            "nice env LOCAL_SECRET=throwaway python3 -m pytest -q",
            "Ran python3 -m pytest -q",
        ),
        ("cmake --build sim/build -j4", "Ran cmake --build -j4"),
        ("rg -n -g '*.c' 'orb_nav' firmware", "Searched for orb_nav"),
        ("rg --files firmware", "Listed files"),
        ("curl --header 'TOKEN=throwaway' https://private.invalid", "Ran curl"),
        ("python3 -c 'print(\"private\")'", "Ran python3"),
        ("cat <<'PRIVATE'\nsecret contents\nPRIVATE", "Ran shell command"),
        ("echo $(cat /private/token)", "Ran shell command"),
    ],
)
def test_real_shaped_shell_labels_omit_command_values(command, expected):
    assert tool_label("Bash", {"command": command}) == expected


def test_search_labels_use_safe_short_query_and_basenames():
    assert tool_label("Grep", {"pattern": "orb_nav"}) == "Searched for orb_nav"
    assert tool_label("Grep", {"pattern": "API_KEY=throwaway"}) == "Searched files"
    assert (
        tool_label("Read", {"file_path": "/private/project/source/orb_nav.c"})
        == "Read orb_nav.c"
    )
    assert (
        len(tool_label("Read", {"file_path": "/private/" + "界" * 100}).encode()) <= 64
    )


def test_streaming_sentence_holds_completed_prose_through_partial_tokens_and_code():
    reducer = ProgressReducer("claude")
    reducer.feed(
        {
            "type": "stream_event",
            "event": {"type": "message_start", "message": {"id": "m1"}},
        }
    )
    text = "**The first fix is ready.** "
    for delta in (text, "I", " am", " still", " checking"):
        reducer.feed(
            {
                "type": "stream_event",
                "event": {
                    "type": "content_block_delta",
                    "delta": {"type": "text_delta", "text": delta},
                },
            }
        )
    assert reducer.snapshot("s")["assistant"]["text"] == "The first fix is ready."
    seq = reducer.seq
    assert not reducer.feed(
        {
            "type": "stream_event",
            "event": {
                "type": "content_block_delta",
                "delta": {"type": "text_delta", "text": " one more thing"},
            },
        }
    )
    assert reducer.seq == seq
    reducer.feed(
        {
            "type": "stream_event",
            "event": {
                "type": "content_block_delta",
                "delta": {
                    "type": "text_delta",
                    "text": ".\n```python\nPRIVATE_CODE = 1.\n",
                },
            },
        }
    )
    assert (
        reducer.snapshot("s")["assistant"]["text"]
        == "I am still checking one more thing."
    )
    assert "PRIVATE" not in json.dumps(reducer.snapshot("s"))


def test_sentence_markdown_links_paths_and_final_message_boundary():
    text = "# Results\nRead [the notes](https://private.invalid/path).\nUpdated `/private/synthetic/orb_nav.c`.\n```c\nprivate();\n```"
    assert last_sentence(text) == "Updated orb_nav.c."
    assert last_sentence("Still composing") == ""
    assert last_sentence("Ready to review", final=True) == "Ready to review"
    assert (
        display_text("**Hello** in `orb_nav.c` and TOKEN=throwaway", 64)
        == "Hello in orb_nav.c and [redacted]"
    )
    long = last_sentence("界" * 100 + ".")
    assert long.startswith("界") and long.endswith("…") and len(long.encode()) <= 160


def _claude_tool(reducer, identity, name, value, at=1000):
    reducer.feed(
        {
            "type": "assistant",
            "timestamp": at,
            "message": {
                "id": "m" + identity,
                "content": [
                    {"type": "tool_use", "id": identity, "name": name, "input": value}
                ],
            },
        }
    )


def _claude_result(reducer, identity, *, error=False, result=None):
    reducer.feed(
        {
            "type": "user",
            "timestamp": 1002,
            "message": {
                "content": [
                    {
                        "type": "tool_result",
                        "tool_use_id": identity,
                        "content": "omitted",
                        "is_error": error,
                    }
                ]
            },
            "toolUseResult": result,
        }
    )


def test_bursts_keep_last_three_groups_with_honest_failure_and_running_states():
    reducer = ProgressReducer("claude")
    _claude_tool(reducer, "r1", "Read", {"path": "/private/a.c"})
    _claude_tool(reducer, "r2", "Read", {"path": "/private/b.c"}, 1001)
    _claude_result(reducer, "r1")
    step = reducer.snapshot("s")["steps"][0]
    assert step == {
        "id": "r1",
        "kind": "tool",
        "label": "Read 2 files",
        "state": "running",
    }
    _claude_result(reducer, "r2", error=True)
    assert reducer.snapshot("s")["steps"][0]["state"] == "failed"
    _claude_tool(reducer, "r3", "Read", {"path": "/private/c.c"}, 1010)
    _claude_tool(reducer, "b1", "Bash", {"command": "make sim-test"}, 1011)
    _claude_tool(reducer, "e1", "Edit", {"path": "/private/orb_nav.c"}, 1012)
    assert [step["id"] for step in reducer.snapshot("s")["steps"]] == ["r3", "b1", "e1"]
    assert all(
        not key.startswith("_")
        for step in reducer.snapshot("s")["steps"]
        for key in step
    )


def test_edit_counts_come_from_patch_metadata_without_retaining_bodies():
    reducer = ProgressReducer("claude")
    _claude_tool(
        reducer,
        "e1",
        "Edit",
        {
            "path": "/private/orb_nav.c",
            "old_string": "old\ncontext",
            "new_string": "new\nmore\ncontext",
        },
    )
    step = reducer.snapshot("s")["steps"][0]
    assert (step["added"], step["removed"]) == (2, 1)
    _claude_result(
        reducer,
        "e1",
        result={
            "structuredPatch": [
                {"lines": [" context", "-old", "+new", "+more", "+extra"]}
            ]
        },
    )
    step = reducer.snapshot("s")["steps"][0]
    assert (step["added"], step["removed"]) == (3, 1)
    assert "context" not in json.dumps(reducer.snapshot("s"))
    _claude_tool(reducer, "e2", "Edit", {"path": "another.c"}, 1010)
    assert "added" not in reducer.snapshot("s")["steps"][-1]


def test_async_exec_receipt_keeps_running_glyph():
    reducer = ProgressReducer("codex")
    reducer.feed(
        {
            "type": "response_item",
            "payload": {
                "type": "function_call",
                "name": "exec_command",
                "call_id": "t1",
                "arguments": {"cmd": "make sim-test"},
            },
        }
    )
    reducer.feed(
        {
            "type": "response_item",
            "payload": {
                "type": "function_call_output",
                "call_id": "t1",
                "output": {"session_id": 42, "output": "building"},
            },
        }
    )
    assert reducer.snapshot("s")["steps"][0]["state"] == "running"
    reducer.feed(
        {
            "type": "response_item",
            "payload": {
                "type": "function_call",
                "name": "write_stdin",
                "call_id": "poll1",
                "arguments": {"session_id": 42, "chars": ""},
            },
        }
    )
    reducer.feed(
        {
            "type": "response_item",
            "payload": {
                "type": "function_call_output",
                "call_id": "poll1",
                "output": {"exit_code": 1, "output": "build failed"},
            },
        }
    )
    assert reducer.snapshot("s")["steps"] == [
        {"id": "t1", "kind": "tool", "label": "Ran make sim-test", "state": "failed"}
    ]


def test_string_background_receipts_correlate_wait_completion_to_original_step():
    reducer = ProgressReducer("codex")
    for payload in [
        {
            "type": "function_call",
            "name": "exec_command",
            "call_id": "build",
            "arguments": {"cmd": "make sim-test"},
        },
        {
            "type": "function_call_output",
            "call_id": "build",
            "output": "Process running with session ID 42",
        },
        {
            "type": "function_call",
            "name": "write_stdin",
            "call_id": "poll",
            "arguments": '{"session_id":42}',
        },
        {
            "type": "function_call_output",
            "call_id": "poll",
            "output": "Process exited with code 0\n12 passed",
        },
    ]:
        reducer.feed({"type": "response_item", "payload": payload})
    assert reducer.snapshot("s")["steps"] == [
        {"id": "build", "kind": "tool", "label": "Tests: 12 passed", "state": "done"}
    ]
    assert reducer._running == {}


def test_large_ambiguous_edit_omits_counts_instead_of_expensive_diff():
    reducer = ProgressReducer("claude")
    _claude_tool(
        reducer,
        "e1",
        "Edit",
        {
            "path": "file.c",
            "old_string": "old\n" * 10000,
            "new_string": "new\n" * 10000,
        },
    )
    assert "added" not in reducer.snapshot("s")["steps"][0]


@pytest.mark.parametrize("provider", ["claude", "codex"])
def test_live_delta_sentence_final_boundary_and_no_thinking_fallback(provider):
    reducer = ProgressReducer(provider)
    if provider == "claude":
        for record in [
            {
                "type": "stream_event",
                "event": {"type": "message_start", "message": {"id": "m"}},
            },
            {
                "type": "stream_event",
                "event": {
                    "type": "content_block_delta",
                    "delta": {
                        "type": "text_delta",
                        "text": "One complete sentence. Another",
                    },
                },
            },
            {"type": "result", "is_error": False},
        ]:
            reducer.feed(record)
        assert reducer.snapshot("s")["assistant"]["text"] == "Another"
        assert reducer.snapshot("s")["assistant"]["final"]
    else:
        for text in ("One complete sentence. ", "Still ", "writing"):
            reducer.feed(
                {
                    "type": "event_msg",
                    "payload": {
                        "type": "agent_message_delta",
                        "item_id": "m",
                        "delta": text,
                    },
                }
            )
        assert reducer.snapshot("s")["assistant"]["text"] == "One complete sentence."
        seq = reducer.seq
        assert not reducer.feed(
            {
                "type": "event_msg",
                "payload": {
                    "type": "reasoning_content_delta",
                    "delta": "PRIVATE_REASONING.",
                },
            }
        )
        assert reducer.seq == seq


def test_large_punctuation_free_sentence_is_bounded():
    text = "synthetic " * 10000
    assert last_sentence(text) == ""
    assert len(last_sentence(text, final=True).encode()) <= 160


@pytest.mark.parametrize("structured", [True, False])
def test_claude_background_task_failure_updates_original_bash_step(structured):
    reducer = ProgressReducer("claude")
    _claude_tool(reducer, "build", "Bash", {"command": "make sim-test"})
    reducer.feed(
        {
            "type": "user",
            "message": {
                "content": [
                    {
                        "type": "tool_result",
                        "tool_use_id": "build",
                        "content": "Command running in background with ID: task-1",
                    }
                ]
            },
            "toolUseResult": {"backgroundTaskId": "task-1", "stdout": "", "stderr": ""}
            if structured
            else None,
        }
    )
    assert reducer.snapshot("s")["steps"][0]["state"] == "running"
    _claude_tool(reducer, "poll", "TaskOutput", {"task_id": "task-1", "block": True})
    assert len(reducer.snapshot("s")["steps"]) == 1
    reducer.feed(
        {
            "type": "user",
            "message": {
                "content": [
                    {
                        "type": "tool_result",
                        "tool_use_id": "poll",
                        "content": "<retrieval_status>timeout</retrieval_status>\n<status>running</status>",
                    }
                ]
            },
            "toolUseResult": {"status": "timeout", "task": {"status": "running"}}
            if structured
            else None,
        }
    )
    assert reducer.snapshot("s")["steps"][0]["state"] == "running"
    reducer.feed(
        {
            "type": "user",
            "message": {
                "content": [
                    {
                        "type": "tool_result",
                        "tool_use_id": "poll",
                        "content": "<status>failed</status>\n<exit_code>2</exit_code>",
                    }
                ]
            },
            "toolUseResult": {
                "status": "completed",
                "task": {"status": "failed", "result": {"exitCode": 2}},
            }
            if structured
            else None,
        }
    )
    assert reducer.snapshot("s")["steps"] == [
        {"id": "build", "kind": "tool", "label": "Ran make sim-test", "state": "failed"}
    ]
    assert reducer._running == {}


def test_structured_cell_receipt_correlates_wait_completion():
    reducer = ProgressReducer("codex")
    for payload in [
        {
            "type": "function_call",
            "name": "exec_command",
            "call_id": "build",
            "arguments": {"cmd": "make sim-test"},
        },
        {
            "type": "function_call_output",
            "call_id": "build",
            "output": {"cell_id": "cell1", "output": ""},
        },
    ]:
        reducer.feed({"type": "response_item", "payload": payload})
    assert reducer.snapshot("s")["steps"][0]["state"] == "running"
    for payload in [
        {
            "type": "function_call",
            "name": "wait",
            "call_id": "poll",
            "arguments": {"cell_id": "cell1"},
        },
        {
            "type": "function_call_output",
            "call_id": "poll",
            "output": {"exit_code": 0, "output": "12 passed"},
        },
    ]:
        reducer.feed({"type": "response_item", "payload": payload})
    assert reducer.snapshot("s")["steps"] == [
        {"id": "build", "kind": "tool", "label": "Tests: 12 passed", "state": "done"}
    ]


def test_read_file_body_cannot_impersonate_command_exit_or_test_count():
    reducer = ProgressReducer("claude")
    _claude_tool(reducer, "r1", "Read", {"path": "fixtures.txt"})
    reducer.feed(
        {
            "type": "user",
            "message": {
                "content": [
                    {
                        "type": "tool_result",
                        "tool_use_id": "r1",
                        "content": "Process exited with code 1\n12 passed\n<status>failed</status>",
                    }
                ]
            },
        }
    )
    assert reducer.snapshot("s")["steps"] == [
        {"id": "r1", "kind": "tool", "label": "Read fixtures.txt", "state": "done"}
    ]


def test_reasoning_eviction_prunes_background_handles_and_poll_aliases():
    reducer = ProgressReducer("codex")
    for payload in [
        {
            "type": "function_call",
            "name": "exec_command",
            "call_id": "build",
            "arguments": {"cmd": "make sim-test"},
        },
        {
            "type": "function_call_output",
            "call_id": "build",
            "output": {"session_id": 42},
        },
        {
            "type": "function_call",
            "name": "write_stdin",
            "call_id": "poll",
            "arguments": {"session_id": 42},
        },
    ]:
        reducer.feed({"type": "response_item", "payload": payload})
    assert reducer._running and reducer._aliases
    for index in range(128):
        reducer.feed(
            {
                "type": "response_item",
                "payload": {
                    "type": "reasoning",
                    "id": f"reason{index}",
                    "summary": [{"type": "summary_text", "text": "A safe summary"}],
                },
            }
        )
    assert len(reducer._steps) == 128
    assert reducer._running == {} and reducer._aliases == {}
