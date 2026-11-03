"""Deterministic subprocess peer for the Claude stream-json transport tests."""

import json
import os
import sys
import time


def emit(value):
    print(json.dumps(value), flush=True)


if "agents" in sys.argv:
    agents = os.environ.get("ORB_FAKE_AGENTS", "[]")
    if os.environ.get("ORB_FAKE_SPLIT"):
        print(agents[: len(agents) // 2], end="", flush=True)
        time.sleep(0.03)
        print(agents[len(agents) // 2 :], flush=True)
    elif os.environ.get("ORB_FAKE_OVERSIZE"):
        print("x" * (5 * 1024 * 1024), flush=True)
    else:
        print(agents)
    raise SystemExit

native_id = sys.argv[sys.argv.index("--session-id") + 1]
approval_number = 0
for line in sys.stdin:
    message = json.loads(line)
    if message["type"] == "control_request":
        if os.environ.get("ORB_FAKE_HANG"):
            continue
        emit(
            {
                "type": "control_response",
                "response": {
                    "subtype": "success",
                    "request_id": message["request_id"],
                    "response": {},
                },
            }
        )
    elif message["type"] == "user":
        prompt = message["message"]["content"]
        if prompt == "exit":
            raise SystemExit(3)
        if prompt == "corrupt":
            print('{"broken', flush=True)
            continue
        if prompt == "malformed-control":
            emit({"type": "control_response", "response": []})
            continue
        if prompt == "api-error":
            emit(
                {
                    "type": "assistant",
                    "error": "authentication_failed",
                    "is_api_error_message": True,
                    "message": {
                        "content": [{"type": "text", "text": "PRIVATE PROVIDER BODY"}]
                    },
                }
            )
            emit(
                {
                    "type": "result",
                    "subtype": "success",
                    "is_error": True,
                    "result": "Not logged in; PRIVATE PROVIDER BODY",
                }
            )
            continue
        if prompt in (
            "approve",
            "cancel",
            "unknown",
            "question",
            "multi-question",
            "question-cancel",
            "question-result",
            "bad-question",
        ):
            approval_number += 1
            request_id = f"request-{approval_number}"
            request = {
                "subtype": "can_use_tool",
                "tool_name": "Write",
                "input": {
                    "file_path": "/tmp/throwaway.txt",
                    "content": "PRIVATE FILE CONTENT",
                },
            }
            if prompt == "unknown":
                request["subtype"] = "unimplemented"
            if "question" in prompt:
                request["tool_name"] = "AskUserQuestion"
                request["input"] = {
                    "questions": [
                        {
                            "question": "Which color?",
                            "options": [
                                {
                                    "label": "Blue",
                                    "description": "PRIVATE CHOICE DETAILS",
                                }
                            ],
                        }
                    ]
                }
                if prompt == "multi-question":
                    request["input"]["questions"].append(
                        {"question": "Which size?", "options": []}
                    )
                if prompt == "bad-question":
                    request["input"] = {"questions": "invalid"}
            emit(
                {
                    "type": "control_request",
                    "request_id": request_id,
                    "request": request,
                }
            )
            if prompt in ("cancel", "question-cancel"):
                emit({"type": "control_cancel_request", "request_id": request_id})
                emit(
                    {
                        "type": "result",
                        "subtype": "success",
                        "result": "Cancelled",
                        "session_id": native_id,
                    }
                )
            if prompt == "question-result":
                emit(
                    {
                        "type": "result",
                        "subtype": "success",
                        "result": "Finished independently",
                        "session_id": native_id,
                    }
                )
        else:
            emit(
                {
                    "type": "assistant",
                    "session_id": native_id,
                    "message": {
                        "content": [{"type": "text", "text": "Reply to " + prompt}]
                    },
                }
            )
            emit(
                {
                    "type": "result",
                    "subtype": "success",
                    "result": "Reply to " + prompt,
                    "session_id": native_id,
                }
            )
    elif message["type"] == "control_response":
        response = message["response"]
        result = response.get("response", {})
        updated = result.get("updatedInput", {})
        if result.get("behavior") == "allow" and "questions" in updated:
            assert all(
                item["question"] in updated["answers"] for item in updated["questions"]
            )
            emit(
                {
                    "type": "result",
                    "subtype": "success",
                    "result": "Answered: " + ", ".join(updated["answers"].values()),
                    "session_id": native_id,
                }
            )
            continue
        if result.get("behavior") == "allow":
            assert result["updatedInput"]["content"] == "PRIVATE FILE CONTENT"
        emit(
            {
                "type": "result",
                "subtype": "success",
                "result": result.get("behavior", "unsupported"),
                "session_id": native_id,
            }
        )
