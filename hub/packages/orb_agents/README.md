# orb-agents

Standalone asyncio integration for Claude Code and Codex on Orb's Mac mini.
Python 3.13+, src layout, **no runtime dependencies**, and no `orb_hub` imports.
Importing or constructing an adapter performs no I/O. Nothing installs hooks,
changes launchd, configures notifications, or connects to the ESP32.

## Install and test

From the repository root:

```sh
/opt/homebrew/bin/python3.13 -m venv .venv
.venv/bin/python -m pip install -e 'hub/packages/orb_agents[test]'
.venv/bin/python -m pytest hub/packages/orb_agents/tests -m 'not live' -q
RUFF_CACHE_DIR="$PWD/.ruff_cache" .venv/bin/ruff check hub/packages/orb_agents
RUFF_CACHE_DIR="$PWD/.ruff_cache" .venv/bin/ruff format --check hub/packages/orb_agents
.venv/bin/python -m build hub/packages/orb_agents
```

The distribution is `orb-agents`; the Python module is `orb_agents`. The wheel
includes `orb_agents.testing.FakeAdapter` for downstream integration tests.
Build and direct test dependencies are pinned in `pyproject.toml`.

## Async API

```python
from orb_agents import AgentManager


async def use_agents():
    async with AgentManager() as agents:
        snapshot = await agents.sessions()
        # Starting a session is explicit; discovery never starts/resumes one.
        session = await agents.start(
            "claude", "/absolute/project", "Review this code", model="haiku"
        )
        await agents.send(session.id, "Focus on error handling")
```

Run event consumption in a separate task:

```python
async def forward_events(agents, publish):
    async for event in agents.events():
        await publish(event.to_dict())
```

| Method | Result / behavior |
| --- | --- |
| `await sessions()` | Refresh providers and return `list[Session]`, newest update first. Inspect `health` for discovery failures; last-known sessions remain visible. |
| `events()` | Async iterator of immutable `Event` objects. Each subscriber receives its own stream. Subscribe before starting work if its first changes matter. |
| `await approve(request_id, "allow" \| "deny", message=None)` | Answer an outstanding permission once. Raises `AgentError(code="stale_approval")` after another answer, cancellation, expiry, or disconnect. |
| `await send(session_id, text)` | Return `DeliveryResult(status,reason)` after owned transport delivery or opt-in Codex queue acceptance. Raises a typed error if unsupported, unavailable, or delivery is uncertain. Does not fork or resume observed sessions. |
| `can_message(session_id)` | Check the current transport/queue capability. Observation alone never enables sending. |
| `await start("claude" \| "codex", cwd, prompt, model=None)` | Launch an owned Claude process or create a thread on an owned Codex app-server. Returns `Session`; the turn continues asynchronously. |
| `pending_approvals()` | Current `list[ApprovalRequest]`; use after connecting or an event-stream gap. |
| `await close()` | Stop discovery, deny outstanding local waits, and close owned processes. Also done on context exit. |

Use one manager and store per event loop. `cwd` must exist. Prompts/messages
contain 1–8192 characters. IDs are namespaced (`claude:<native-id>`,
`codex:<native-id>`); approval IDs are opaque and scoped to this manager lifetime.
Starting again after shutdown requires a new manager. There is no automatic
retry for control operations whose delivery might already have happened.

`Session` fields: `id`, `kind`, `origin` (`desktop app`, `cli`, `hub-started`),
`title`, `cwd`, `state` (`working`, `waiting_input`, `needs_approval`, `idle`,
`done`, `error`), `waiting_for`, `last_assistant_message` (≤300 characters), and
UTC `updated_at`. `to_dict()` converts timestamps to ISO 8601 strings. Metadata
is immutable; snapshots already returned are not changed in place.

Titles prefer Claude's listing `name` (or older `title` alias) and Codex's
thread `title`. Without a name, adapters retain a redacted summary of the first
user prompt (≤120 characters); notifications, tool output, provider names and
folder names never supply a title. Unknown titles stay empty. Claude hooks can
initially know only the first observed prompt; bounded canonical transcript
metadata can replace that fallback with the actual first prompt. A real name
always wins and survives partial listings and later hooks. `title_source`
(`name`, `prompt`, `hook`, or empty) tracks this precedence in provider metadata;
it is not sent to devices. The hub separately projects the `cwd` folder into
the device's `project` subtitle and clips titles to 64 UTF-8 bytes.

Events: `session.changed`, `approval.requested`, `approval.resolved`,
`turn.completed`. An approval request contains `request_id`, `session_id`,
`tool`, and `summary`. Events contain monotonic `sequence` and UTC `updated_at`.
A bounded subscriber queue emits `stream.gap` if its consumer falls behind:
refresh both `sessions()` and `pending_approvals()`. There is no disk replay.
Approval resolutions carry a reason and a decision for local user answers or
local timeout denials; an externally cleared request has `decision=None`, since
the native app's choice may be unknown. A local answer is not proof that the
tool ran: follow subsequent provider events and session errors.

`AgentError.code` gives a stable category. `UnsupportedOperation` means this
origin has no supported control path; `SessionUnavailable` means an owned
session cannot currently accept input. `health` reports provider discovery
failures without raw provider diagnostics or credentials.

## Hook endpoint and generated settings

```sh
.venv/bin/python -m orb_agents.installer \
  --url http://127.0.0.1:8447/hooks/claude \
  --output /private/tmp/orb-claude-hooks.review.json
```

This creates a new **0600** JSON snippet with a random per-install
`X-Orb-Secret`. Existing output files are refused. It prints no secret and
never applies settings. The owner must review and merge the `hooks` lists into
Claude's settings later, preserving existing entries. Keep this file outside
Git. Read its header value privately into the receiver; do not log it.

```python
import json
from orb_agents import AgentManager
from orb_agents.hooks import HookServer


async def serve(snippet_path):
    snippet = json.loads(snippet_path.read_text())
    secret = snippet["hooks"]["SessionStart"][0]["hooks"][0]["headers"]["X-Orb-Secret"]
    async with AgentManager() as agents:
        async with HookServer(agents.store, secret, port=8447):
            await run_hub(agents)  # your hub's lifetime coroutine
```

Only `127.0.0.1` is bound. The receiver requires the secret, an exact loopback
Host, JSON with Content-Length, and no Origin header. It limits body size to
1 MiB, headers to 16 KiB, and simultaneous connections to 64. Requests that
exceed these limits fall back to Claude's native permission handling; they do
not auto-approve. The default permission wait is 120 seconds; the generated
HTTP hook timeout is 130 seconds. An expired request is denied and emitted as
resolved, never silently approved. The UI should show the expiry and allow the
owner to retry in Claude, rather than implying that a pending approval vanished.

Alternatively mount `HookServer.handle(payload)` in the hub's own server;
that route must implement equivalent authentication, loopback binding, body
limits, Origin/Host rejection and cancellation on client disconnect. The built-in
receiver exposes no HTTP control methods. Authentication and confirmation of
Orb/voice requests belong to `orb_hub`.

By default, owned Claude sessions use their stdio permission channel and disable
inherited hooks and MCP settings. To explicitly use the HTTP hook path instead,
construct `ClaudeAdapter(store, settings=snippet_path,
permission_transport="hooks")`. Do not enable two approval routes for the same
owned session: Claude races them and the native prompt. Hook-origin sessions
have provisional `cli` origin until discovery can classify their process.
The default stdio mode supports Claude's structured questions through `send`:
plain text for one question, or JSON keyed by displayed `q1`, `q2`, etc. for
multiple questions. Hooks-only mode does not host questions; native sessions
show “Answer this question in Claude” and retain their native interaction.

## Inject adapters and fakes

```python
from orb_agents import AgentManager, StateStore
from orb_agents.claude import ClaudeAdapter
from orb_agents.codex import CodexAdapter
from orb_agents.testing import FakeAdapter

store = StateStore()
manager = AgentManager(
    store=store,
    adapters={
        "claude": ClaudeAdapter(store, executable="claude"),
        "codex": CodexAdapter(store),
    },
)

# Deterministic fake; no model, filesystem, sockets, or credentials.
fake_store = StateStore()
fake = FakeAdapter(fake_store, "codex")
fake_manager = AgentManager(store=fake_store, adapters={"codex": fake})
# fake.complete(session_id, "Done") drives completion.
# await fake.ask_permission(session_id) blocks until fake_manager.approve(...).
```

Codex defaults to the binary bundled with ChatGPT.app. Both adapters accept an
explicit executable and child environment. `ClaudeAdapter.config_dir` and
`CodexAdapter.sessions_root` select observation roots. `CodexAdapter(observe=False)`
disables rollout discovery. See [the integration notes](../../../docs/agents.md)
for verified capabilities, origin limits, isolated smoke tests and open issues.

## Agents 2.0 passive data and confirmed talk

`orb_agents.progress.JsonlTail` incrementally reads bounded complete JSONL records,
handling partial lines, truncation, inode replacement and duplicates.
`ProgressReducer("claude"|"codex")` reduces those records to bounded live progress:
the last three tool/summary steps and the last assistant sentence. Thinking and
encrypted reasoning are ignored; labels redact assignments and credential forms.
All progress fixtures in the tests are invented, never copied transcripts.

`orb_agents.usage.UsageCollector` accepts Codex rollout quota observations and
optional replies from an already owned app-server. Context/token totals never
become quota percentages. Claude usage has two sources: the hub's `orb-hub
statusline` private cache, and a primary `claude -p /usage` poll. The poll runs
at agents startup and then no more often than every five minutes, with a
30-second timeout and failure backoff; it resolves `claude` from `PATH` or
`~/.local/bin/claude`, uses no shell and no model call, and retains only parsed
numbers. The freshest usable source wins, with `observed_at` and `source`
recorded per window; readings are stale after 15 minutes. Follow the
[lead-applied statusline snippet](../../../docs/agent-briefs/wave2/claude-statusline-snippet.md)
to enable the cache source without replacing existing hooks/settings.

The embedding hub publishes `agent.stream` for at most eight focus sessions at
250 ms intervals, with snapshot repair on subscribe. Independent `usage` links
receive `usage.snapshot` on change no more than every 30 seconds. These share the
4096-byte envelope and bounded delivery queue in [the protocol](../../../docs/protocol.md).

`AgentManager(enable_codex_queue=True)` enables the documented `codex queue`
path for already observed Codex sessions; the hub exposes this as
`orb-hub serve --agents --codex-queue`. It defaults off. The adapter uses exact
argv, no shell, bounded timeouts and discarded provider output. Success means
**queued**, not delivered into the provider UI or executed. Owned Claude stdin
and Codex turn/start or turn/steer acknowledgements mean **delivered** to the
transport. Failures return a safe reason. Claude's experimental private inbox
remains disabled. Fake-binary tests do not validate real desktop routing.
