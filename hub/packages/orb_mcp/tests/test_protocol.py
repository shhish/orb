"""Protocol wire contracts checked independently of storage and networking."""

import copy
import json

import pytest
from jsonschema import Draft202012Validator

from orb_mcp.protocol import (
    CAPABILITIES_KEY,
    INSTRUCTIONS,
    LEGACY_VERSIONS,
    MODERN_VERSION,
    SERVER_INFO_KEY,
    SUPPORTED_VERSIONS,
    TOOLS,
    VERSION_KEY,
    Protocol,
    http_metadata_error,
    request_headers,
)


def request(method, params=None, version=MODERN_VERSION, request_id=1):
    params = copy.deepcopy(params or {})
    if version == MODERN_VERSION:
        params["_meta"] = {VERSION_KEY: version, CAPABILITIES_KEY: {}}
    return {"jsonrpc": "2.0", "id": request_id, "method": method, "params": params}


class Service:
    def __init__(self):
        self.calls = []

    async def call_tool(self, name, arguments, principal):
        self.calls.append((name, arguments, principal))
        if arguments.get("card_id") == "conflict":
            raise ValueError("Card ID already has different content")
        if arguments.get("card_id") == "broken":
            raise RuntimeError("Private internal exception with a secret")
        return {"card_id": arguments.get("card_id"), "status": "showing"}


class Events:
    def __init__(self):
        self.calls = []

    async def dispatch(self, method, params, principal):
        self.calls.append((method, params, principal))
        return {"events": []} if method == "events/list" else {}


@pytest.fixture
def protocol():
    return Protocol(Service(), Events())


async def test_discover_is_stateless_and_has_required_modern_fields(protocol):
    response = await protocol.handle(request("server/discover"), "dot")
    assert response["jsonrpc"] == "2.0"
    assert response["id"] == 1
    result = response["result"]
    assert result["resultType"] == "complete"
    assert result["supportedVersions"] == list(SUPPORTED_VERSIONS)
    assert result["capabilities"] == {"tools": {}, "events": {}}
    assert result["_meta"][SERVER_INFO_KEY]["name"] == "orb-mcp"
    assert result["ttlMs"] > 0
    assert result["cacheScope"] == "private"
    assert len(result["instructions"]) <= 512
    for phrase in (
        "unique card_id",
        "Subscribe",
        "eventId",
        "not approval",
        "consequential",
    ):
        assert phrase in INSTRUCTIONS


@pytest.mark.parametrize("version", LEGACY_VERSIONS)
async def test_legacy_initialize_and_calls(protocol, version):
    params = {
        "protocolVersion": version,
        "capabilities": {},
        "clientInfo": {"name": "test", "version": "1"},
    }
    initialized = await protocol.handle(
        request("initialize", params, version), "claude", version
    )
    assert initialized["result"]["protocolVersion"] == version
    assert "events" not in initialized["result"]["capabilities"]
    assert "resultType" not in initialized["result"]
    tools = await protocol.handle(
        request("tools/list", version=version), "claude", version
    )
    assert len(tools["result"]["tools"]) == 7
    assert "resultType" not in tools["result"]


async def test_legacy_initialize_negotiates_unknown_version(protocol):
    params = {
        "protocolVersion": "1900-01-01",
        "capabilities": {},
        "clientInfo": {"name": "test", "version": "1"},
    }
    result = await protocol.handle(request("initialize", params, None), "codex")
    assert result["result"]["protocolVersion"] == LEGACY_VERSIONS[0]


@pytest.mark.parametrize(
    "params",
    [
        {},
        {"protocolVersion": 7, "capabilities": {}, "clientInfo": {}},
        {"protocolVersion": "2025-11-25", "capabilities": {}, "clientInfo": []},
    ],
)
async def test_malformed_initialize(protocol, params):
    response = await protocol.handle(request("initialize", params, None), "dot")
    assert response["error"]["code"] == -32602


async def test_modern_inline_tools_need_no_initialize(protocol):
    response = await protocol.handle(request("tools/list"), "dot")
    assert response["result"]["resultType"] == "complete"
    assert response["result"]["ttlMs"] > 0
    assert {tool["name"] for tool in response["result"]["tools"]} == {
        tool["name"] for tool in TOOLS
    }


async def test_unknown_version_reports_supported_and_requested(protocol):
    message = request("server/discover")
    message["params"]["_meta"][VERSION_KEY] = "2099-01-01"
    response = await protocol.handle(message, "dot")
    assert response["error"] == {
        "code": -32022,
        "message": "Unsupported protocol version",
        "data": {"supported": list(SUPPORTED_VERSIONS), "requested": "2099-01-01"},
    }


@pytest.mark.parametrize(
    "meta",
    [
        {VERSION_KEY: MODERN_VERSION},
        {VERSION_KEY: MODERN_VERSION, CAPABILITIES_KEY: []},
        {CAPABILITIES_KEY: {}},
        {VERSION_KEY: "2025-11-25", CAPABILITIES_KEY: {}},
    ],
)
async def test_modern_metadata_required(protocol, meta):
    message = request("tools/list")
    message["params"]["_meta"] = meta
    response = await protocol.handle(message, "dot", MODERN_VERSION)
    assert response["error"]["code"] == -32602


@pytest.mark.parametrize(
    "message",
    [
        None,
        [],
        [{"jsonrpc": "2.0", "id": 1, "method": "ping"}],
        {},
        {"jsonrpc": "1.0", "id": 1, "method": "ping"},
        {"jsonrpc": "2.0", "id": None, "method": "ping"},
        {"jsonrpc": "2.0", "id": True, "method": "ping"},
        {"jsonrpc": "2.0", "id": 1.5, "method": "ping"},
        {"jsonrpc": "2.0", "id": 1, "method": "ping", "result": {}},
        {"jsonrpc": "2.0", "id": 1, "method": ""},
    ],
)
async def test_jsonrpc_envelopes(protocol, message):
    response = await protocol.handle(message, "dot")
    assert response["error"]["code"] == -32600
    if (
        message is None
        or not isinstance(message, dict)
        or type(message.get("id")) not in (int, str)
    ):
        assert "id" not in response


@pytest.mark.parametrize("bad", [[], None, "params"])
async def test_params_must_be_object(protocol, bad):
    message = request("tools/list")
    message["params"] = bad
    response = await protocol.handle(message, "dot")
    assert response["error"]["code"] == -32602


@pytest.mark.parametrize(
    "method",
    [
        "notifications/initialized",
        "notifications/cancelled",
        "tools/call",
        "events/subscribe",
        "unknown",
    ],
)
async def test_notifications_have_no_response_and_no_side_effects(protocol, method):
    message = request(method, {"name": "orb_status"})
    del message["id"]
    assert await protocol.handle(message, "dot") is None
    assert protocol.service.calls == []
    assert protocol.events.calls == []


async def test_request_id_preserved(protocol):
    assert (await protocol.handle(request("ping", request_id="abc"), "dot"))[
        "id"
    ] == "abc"
    assert (await protocol.handle(request("ping", request_id=0), "dot"))["id"] == 0


async def test_unknown_methods_are_protocol_errors(protocol):
    assert (await protocol.handle(request("device.reboot"), "dot"))["error"][
        "code"
    ] == -32601
    assert (await protocol.handle(request("initialize"), "dot"))["error"][
        "code"
    ] == -32601
    assert (await protocol.handle(request("server/discover", version=None), "dot"))[
        "error"
    ]["code"] == -32601


def test_all_tool_object_schemas_closed_and_valid():
    def inspect(schema):
        if isinstance(schema, dict):
            if schema.get("type") == "object":
                assert schema["additionalProperties"] is False
            for value in schema.values():
                inspect(value)
        elif isinstance(schema, list):
            for value in schema:
                inspect(value)

    for tool in TOOLS:
        Draft202012Validator.check_schema(tool["inputSchema"])
        inspect(tool["inputSchema"])
        read_only = tool["name"] in {
            "orb_get_card",
            "orb_get_transcript",
            "orb_status",
            "orb_wait",
        }
        assert tool["annotations"]["readOnlyHint"] is read_only
        assert tool["annotations"]["destructiveHint"] is (tool["name"] == "orb_cancel")


@pytest.mark.parametrize(
    ("name", "arguments"),
    [
        ("orb_show", {"card_id": "c", "text": "hello", "unexpected": True}),
        ("orb_show", {"card_id": "x" * 65, "text": "hello"}),
        ("orb_show", {"card_id": "", "text": "hello"}),
        ("orb_show", {"card_id": "c", "text": "x" * 401}),
        ("orb_show", {"card_id": "c", "text": "hello", "title": "x" * 41}),
        ("orb_show", {"card_id": "c", "text": "hello", "priority": "maximum"}),
        (
            "orb_ask",
            {"card_id": "c", "text": "x" * 281, "options": [{"id": "a", "label": "A"}]},
        ),
        ("orb_ask", {"card_id": "c", "text": "?", "options": []}),
        (
            "orb_ask",
            {"card_id": "c", "text": "?", "options": [{"id": "a", "label": "A"}] * 4},
        ),
        (
            "orb_ask",
            {
                "card_id": "c",
                "text": "?",
                "options": [{"id": "a", "label": "A", "url": "bad"}],
            },
        ),
        (
            "orb_ask",
            {"card_id": "c", "text": "?", "options": [{"id": "a" * 33, "label": "A"}]},
        ),
        (
            "orb_ask",
            {"card_id": "c", "text": "?", "options": [{"id": "a", "label": "A" * 25}]},
        ),
        (
            "orb_ask",
            {
                "card_id": "c",
                "text": "?",
                "options": [{"id": "a", "label": "A"}, {"id": "a", "label": "B"}],
            },
        ),
        ("orb_status", {"extra": True}),
        ("orb_wait", {"card_id": "c", "timeout_s": 51}),
        ("orb_wait", {"card_id": "c", "timeout_s": -1}),
        ("orb_wait", {"card_id": "c", "timeout_s": True}),
        ("orb_get_transcript", {"recording_id": "x" * 65}),
    ],
)
async def test_tool_input_constraints_are_actionable_errors(protocol, name, arguments):
    response = await protocol.handle(
        request("tools/call", {"name": name, "arguments": arguments}), "dot"
    )
    assert response["result"]["isError"] is True
    assert response["result"]["resultType"] == "complete"
    assert protocol.service.calls == []


@pytest.mark.parametrize(
    ("name", "arguments"),
    [
        ("orb_show", {"card_id": "c", "text": "x" * 400, "title": "x" * 40}),
        (
            "orb_ask",
            {
                "card_id": "c",
                "text": "x" * 280,
                "options": [{"id": str(i), "label": "x" * 24} for i in range(3)],
            },
        ),
        ("orb_get_card", {"card_id": "c"}),
        ("orb_get_transcript", {"recording_id": "r"}),
        ("orb_cancel", {"card_id": "c"}),
        ("orb_status", {}),
        ("orb_wait", {"card_id": "c", "timeout_s": 50}),
    ],
)
async def test_tool_calls_forward_principal_and_return_content(
    protocol, name, arguments
):
    response = await protocol.handle(
        request("tools/call", {"name": name, "arguments": arguments}), "codex"
    )
    assert response["result"]["isError"] is False
    assert (
        json.loads(response["result"]["content"][0]["text"])
        == response["result"]["structuredContent"]
    )
    assert protocol.service.calls == [(name, arguments, "codex")]


@pytest.mark.parametrize(
    "params",
    [
        {},
        {"name": "unknown"},
        {"name": "orb_status", "arguments": []},
        {"name": "orb_status", "extra": 1},
    ],
)
async def test_malformed_tool_call_is_protocol_error(protocol, params):
    assert (await protocol.handle(request("tools/call", params), "dot"))["error"][
        "code"
    ] == -32602


async def test_semantic_tool_error_and_private_internal_error(protocol):
    for card_id in ("conflict", "broken"):
        response = await protocol.handle(
            request(
                "tools/call",
                {"name": "orb_get_card", "arguments": {"card_id": card_id}},
            ),
            "dot",
        )
        if card_id == "conflict":
            assert response["result"]["isError"] is True
        else:
            assert response["error"]["code"] == -32603
            assert "secret" not in json.dumps(response)


@pytest.mark.parametrize(
    "method", ["events/list", "events/subscribe", "events/unsubscribe"]
)
async def test_events_modern_only_and_metadata_not_forwarded(protocol, method):
    response = await protocol.handle(request(method, {"name": "card.answered"}), "dot")
    assert response["result"]["resultType"] == "complete"
    assert protocol.events.calls == [(method, {"name": "card.answered"}, "dot")]
    response = await protocol.handle(request(method, version=None), "dot")
    assert response["error"]["code"] == -32601


async def test_event_errors_keep_categorized_reason(protocol):
    class CallbackError(Exception):
        code = -32015
        data = {"reason": "challenge_failed"}

    async def failed(*args):
        raise CallbackError("Callback verification failed")

    protocol.events.dispatch = failed
    response = await protocol.handle(request("events/subscribe"), "dot")
    assert response["error"]["code"] == -32015
    assert response["error"]["data"] == {"reason": "challenge_failed"}


def test_modern_headers_round_trip_and_case_insensitive_names():
    message = request("tools/call", {"name": "orb_status", "arguments": {}})
    headers = request_headers(message)
    assert headers == {
        "MCP-Protocol-Version": MODERN_VERSION,
        "Mcp-Method": "tools/call",
        "Mcp-Name": "orb_status",
    }
    assert (
        http_metadata_error(
            message, {key.upper(): value for key, value in headers.items()}
        )
        is None
    )


@pytest.mark.parametrize(
    "name", ["Hello, 世界", " padded ", "line1\nline2", "=?base64?literal?="]
)
def test_mcp_name_sentinel_encoding(name):
    message = request("tools/call", {"name": name})
    headers = request_headers(message)
    assert headers["Mcp-Name"].startswith("=?base64?")
    assert http_metadata_error(message, headers) is None


@pytest.mark.parametrize("header", ["MCP-Protocol-Version", "Mcp-Method", "Mcp-Name"])
def test_missing_or_mismatched_modern_headers(header):
    message = request("tools/call", {"name": "orb_status"})
    headers = request_headers(message)
    del headers[header]
    assert http_metadata_error(message, headers)["error"]["code"] == -32020
    headers[header] = "wrong"
    assert http_metadata_error(message, headers)["error"]["code"] == -32020


@pytest.mark.parametrize(
    "value",
    ["=?base64?!!!?=", "=?base64?/w==?=", "orb_status\r\nX-Evil: yes", " orb_status"],
)
def test_invalid_encoded_or_unsafe_headers(value):
    message = request("tools/call", {"name": "orb_status"})
    headers = request_headers(message)
    headers["Mcp-Name"] = value
    assert http_metadata_error(message, headers)["error"]["code"] == -32020


def test_legacy_headers_do_not_implicitly_upgrade():
    message = request("tools/list", version=None)
    assert request_headers(message)["MCP-Protocol-Version"] == "2025-03-26"
    assert http_metadata_error(message, {}) is None
    assert (
        http_metadata_error(message, {"MCP-Protocol-Version": MODERN_VERSION})["error"][
            "code"
        ]
        == -32020
    )


def test_header_internal_tab_is_valid_ascii():
    message = request("tools/call", {"name": "with\ttab"})
    assert http_metadata_error(message, request_headers(message)) is None


async def test_nonfinite_timeout_never_reaches_backend(protocol):
    response = await protocol.handle(
        request(
            "tools/call",
            {
                "name": "orb_wait",
                "arguments": {"card_id": "c", "timeout_s": float("nan")},
            },
        ),
        "dot",
    )
    assert response["result"]["isError"] is True
    assert protocol.service.calls == []


async def test_tool_catalog_cannot_be_mutated_by_caller(protocol):
    first = await protocol.handle(request("tools/list"), "dot")
    first["result"]["tools"][0]["inputSchema"]["additionalProperties"] = True
    second = await protocol.handle(request("tools/list"), "dot")
    assert second["result"]["tools"][0]["inputSchema"]["additionalProperties"] is False
