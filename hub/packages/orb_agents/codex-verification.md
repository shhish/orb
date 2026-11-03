# Codex adapter verification — 2026-10-26

The installed binary is
`/Applications/ChatGPT.app/Contents/Resources/codex-cli/bin/codex`, reporting
`codex-cli 0.164.0-alpha.11.1`. This adapter has no Python runtime dependencies.

## Evidence

- `codex app-server --help` advertises stdio, Unix-socket and WebSocket transports;
  `codex app-server daemon --help` advertises daemon management. Orb uses only an
  owned stdio process and never changes daemon configuration or ownership.
- `codex app-server generate-json-schema --out /tmp/orb-codex-schema` succeeded.
  The installed schemas confirm `thread/start`, `thread/resume`, `thread/list`,
  `thread/read`, `turn/start`, `turn/steer` with `expectedTurnId`,
  `thread/status/changed`, and the three v2 approval request variants.
- `codex queue --help` advertises `--thread` and `--message`. The gated live
  test invoked it against its own throwaway thread in a fresh `CODEX_HOME`:
  exit 0, `Queued message <id> for thread <id>.` This proves queue acceptance.
  Consumption, model receipt and desktop delivery remain unverified.
- Read-only inspection of rollout **shapes** found `session_meta`,
  `event_msg/task_started`, `event_msg/turn_aborted`, `response_item/message`,
  `turn_context` and related item records. A desktop record had
  `source="vscode", originator="Codex Desktop"`; CLI records had
  `source="exec", originator="codex_exec"`. No owner transcript contents were
  printed and no owner sockets or sessions received messages.
- The [official app-server documentation](https://learn.chatgpt.com/docs/app-server)
  confirms initialization, JSONL framing, owned thread/turn operations,
  `accept`/`decline` responses, and turn-scoped permission grants. Installed
  generated schemas were used for the implementation rather than assuming
  online examples exactly match this alpha binary.

## Tests

```sh
.venv/bin/python -m pytest hub/packages/orb_agents/tests/test_codex.py hub/packages/orb_agents/tests/test_codex_live.py -q
```

Result: **48 passed, 1 skipped**. The deterministic subprocess fixture exercises
bidirectional JSON-RPC, independent response IDs, start/follow-up/steer,
allow/deny/expiry/external resolutions, permissions, user questions, malformed
traffic, disconnects, shutdown races, bounded process reaping, and rollout
rotation/partial lines/staleness/history hydration. The live test is skipped
unless explicitly enabled.

```sh
ORB_AGENTS_LIVE=1 .venv/bin/python -m pytest hub/packages/orb_agents/tests/test_codex_live.py -q
```

Attempted twice: native initialization, thread creation and turn submission
succeeded, with working status observed. The fresh home has no credentials;
`account/read` returned `account=null`, and the turn failed before requesting
approval. The second attempt also made the isolated queue probe above. The
full live proof therefore **failed**, rather than being reported as a pass.
All owned child processes were stopped and reaped.

The test defaults to the low-cost `gpt-6-luna` family; this is not a claim that
it is the globally cheapest model available to every account. Set
`ORB_AGENTS_CODEX_MODEL` to a cheaper available compatible model if needed.
For a complete authenticated rerun, the engineer may explicitly provide a
dedicated disposable provider key in `ORB_AGENTS_CODEX_TEST_API_KEY`. The test
references that environment variable in its temporary provider config;
it never reads or copies owner credentials and never writes the key to a file.
The test itself creates a new `CODEX_HOME` every time, forces file credential
storage, and excludes normal provider credentials from the child environment.

## Integration limits

- Only threads created by the current `CodexAdapter` instance are controllable.
  It does not resume someone else's desktop/CLI thread. Restart recovery is
  observation only; explicit hub ownership persistence/resume is future work.
- Existing desktop/CLI sessions are read-only JSONL observations. The default
  lookback is 24 hours; five minutes without rollout activity yields an
  explicit `live state unknown` message. Old or disappeared files are marked
  done with the same uncertainty. This is not authoritative process liveness.
- Native request IDs are scoped to the owned connection. Decisions are one
  invocation only: no persistent rule changes or session-wide grants.
  Codex's approval schema has no free-text denial-message field; `message`
  is not transmitted to Codex. Permission grants copy only the requested
  profile and set `scope="turn"`.
- One pending user question accepts plain `send()` text. Multiple questions
  require a JSON object mapping **every question ID** to a string answer.
  The waiting description includes those IDs. Structured MCP elicitation,
  dynamic-tool callbacks and legacy approval variants return explicit
  unsupported-method errors; no fallback auto-approval occurs.
- `codex queue` remains unavailable through `send()` because the live probe
  proved enqueueing only. No private desktop stdio transport is accessed.
- No `notify` integration is installed or needed. The owner's existing notify
  program and configuration remain untouched.
