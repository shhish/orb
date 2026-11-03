"""Deterministic bidirectional app-server peer; no network or owner state."""

import json
import os
import signal
import sys
import time


def emit(message):
    print(json.dumps(message), flush=True)


def result(rid, value):
    emit({"id": rid, "result": value})


def notify(method, **params):
    emit({"method": method, "params": {"threadId": "test-thread", **params}})


def completed(text, status="completed"):
    notify("item/completed", item={"id": "reply", "type": "agentMessage", "text": text})
    notify("turn/completed", turn={"id": "test-turn", "status": status})


waiting = None
for line in sys.stdin:
    message = json.loads(line)
    rid = message.get("id")
    method = message.get("method")
    params = message.get("params", {})
    if method == "initialize":
        if os.environ.get("ORB_FAKE_INIT") == "malformed-ignore-term":
            signal.signal(signal.SIGTERM, signal.SIG_IGN)
            print("not-json", flush=True)
            time.sleep(60)
            continue
        if os.environ.get("ORB_FAKE_INIT") == "hang":
            continue
        if os.environ.get("ORB_FAKE_INIT") == "malformed":
            print("not-json", flush=True)
            continue
        if os.environ.get("ORB_FAKE_INIT") == "error":
            emit(
                {"id": rid, "error": {"code": -32000, "message": "secret=never-print"}}
            )
            continue
        result(rid, {"userAgent": "orb-test"})
    elif method == "thread/start":
        result(rid, {"thread": {"id": "test-thread", "cwd": params["cwd"]}})
    elif method == "turn/start":
        text = params["input"][0]["text"]
        if text == "crash":
            sys.exit(1)
        if text == "stderr":
            sys.stderr.write("x" * 200_000)
            sys.stderr.flush()
        notify("turn/started", turn={"id": "test-turn", "status": "inProgress"})
        if text == "complete-before-response":
            completed("completed before response")
        result(rid, {"turn": {"id": "test-turn", "status": "inProgress"}})
        if text == "malformed-notification":
            notify("turn/completed", turn=[])
            continue
        if text == "duplicate-request":
            for _ in range(2):
                emit(
                    {
                        "id": 999,
                        "method": "item/commandExecution/requestApproval",
                        "params": {"threadId": "test-thread"},
                    }
                )
            continue
        if text in {"approval", "permissions", "filechange", "external"}:
            waiting = text
            approval_kind = {
                "permissions": "permissions",
                "filechange": "fileChange",
            }.get(text, "commandExecution")
            emit(
                {
                    "id": 900,
                    "method": f"item/{approval_kind}/requestApproval",
                    "params": {
                        "threadId": "test-thread",
                        "turnId": "test-turn",
                        "itemId": "tool-one",
                        "startedAtMs": 1,
                        "command": "python -c 'file contents and secret=not-for-device'",
                        "cwd": params.get("cwd", "/tmp"),
                        "permissions": {"network": {"enabled": True}},
                    },
                }
            )
            if text == "external":
                notify("serverRequest/resolved", requestId=900)
                completed("resolved elsewhere")
        elif text in {"question", "multiquestion"}:
            waiting = text
            questions = [{"id": "q1", "question": "What should I do?"}]
            if text == "multiquestion":
                questions.append({"id": "q2", "question": "Which directory?"})
            emit(
                {
                    "id": 901,
                    "method": "item/tool/requestUserInput",
                    "params": {"threadId": "test-thread", "questions": questions},
                }
            )
        elif text == "fail":
            completed("failed safely", "failed")
        elif text == "usage-week":
            emit(
                {
                    "method": "account/rateLimits/updated",
                    "params": {
                        "rateLimits": {
                            "limitId": "codex",
                            "primary": None,
                            "secondary": {
                                "usedPercent": 37,
                                "windowDurationMins": 10080,
                                "resetsAt": time.time() + 86400,
                            },
                        },
                    },
                }
            )
            completed(text)
        elif text == "status-approval":
            notify(
                "thread/status/changed",
                status={"type": "active", "activeFlags": ["waitingOnApproval"]},
            )
        elif text not in {"hold", "complete-before-response"}:
            completed(text)
    elif method == "turn/steer":
        assert params["expectedTurnId"] == "test-turn"
        result(rid, {"turnId": "test-turn"})
        completed("steered: " + params["input"][0]["text"])
    elif method is None and waiting:
        if rid == 900:
            completed(json.dumps(message.get("result")))
        elif rid == 901:
            completed(json.dumps(message.get("result")))
        waiting = None
