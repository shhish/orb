# Coding-agent sessions for Orb

`hub/packages/orb_agents` is an installable, framework-independent Python 3.13
package. It supplies discovery, status events, permission arbitration, and
message delivery to `orb_hub`, without importing or changing that package.
The public asyncio API and examples are in
[the package README](../hub/packages/orb_agents/README.md).

## Capability by origin

| Provider / origin | Observe | Approve | Send |
| --- | --- | --- | --- |
| Claude, hub-started | Persistent stream-json stdout; assistant summaries and turn results | `can_use_tool` control requests over stdio, or explicitly selected HTTP hooks | Stream-json user input and hosted questions; persistent process supports subsequent turns; blocked while permission is pending |
| Claude, CLI | `claude agents --json`, bounded transcript tail, optional hooks | PermissionRequest HTTP hook after owner installs generated snippet | Unsupported; private inbox protocol has not been validated |
| Claude, desktop app | Same listing/hooks; desktop inferred from process ancestry | Same hook route, subject to the app's first-answer-wins behavior and installation scope | Unsupported; `experimental_inbox=True` still fails explicitly until wire format is verified |
| Codex, hub-started | Owned stdio app-server notifications | Command/file approval and requested permission subsets; allow is scoped to the current request/turn | `turn/start` when idle, `turn/steer` for an active turn, response to pending user-input questions |
| Codex, desktop app or CLI | Read-only incremental `rollout-*.jsonl` observation | Unsupported; native desktop app-server is private | Opt-in `--codex-queue`; reports queued after CLI acceptance, never desktop delivery. Disabled by default; live routing remains unverified |

A discovered session is never resumed, forked, or taken over as a side effect of
`send`. Its original ID remains the identity. Codex private app-servers and
Claude inbox sockets are not opened. Hub-owned sessions are controllable only
while this manager owns their subprocess/transport; no ownership is persisted
or automatically reclaimed after hub restart.

## Version evidence and truthful state

Verified on 2026-10-26:

- `claude --version`: **2.1.299**. `claude agents --json` ran successfully and
  returned an empty list at inspection. Its installed implementation describes
  `pid`, `cwd`, `kind`, `startedAt`, `sessionId`, `name`, `status`, `waitingFor`
  and background task state. It does **not** expose a desktop-origin field.
  Parsing is fixture-tested against that shape; real desktop classification
  remains unproven because no desktop session was available for inspection.
- Claude help exposes persistent stream-json, host permission prompts, explicit
  settings selection and the model flag. The adapter follows the Agent SDK's
  control protocol without adding the SDK as a runtime dependency.
- `/Applications/ChatGPT.app/Contents/Resources/codex-cli/bin/codex --version`:
  **0.164.0-alpha.11.1**. Version-specific JSON schemas were generated and checked
  for initialization, thread/start, turn/start/steer and approval messages.
- Codex help exposes `app-server daemon`, Unix socket endpoints and
  `queue --thread ... --message ...`. Their existence is not evidence that
  they can control a desktop session on another private stdio transport.
  Orb launches its own stdio app-server, without starting/managing a daemon.
  An isolated queue probe against the test-owned thread returned exit 0 and
  acknowledged enqueueing; authentication prevented proof of consumption.
- Read-only rollout metadata inspection found desktop records with
  `originator="Codex Desktop"`, `source="vscode"`, and CLI records with
  `originator="codex_exec"`, `source="exec"`. No transcript contents were printed.

Claude polling reports native busy/waiting/idle and maps permission waits to
`needs_approval`; disappearance is marked done. HTTP hooks add immediate
SessionStart/End, UserPromptSubmit, Notification, Stop/StopFailure and permission
state. A notification about a permission is not itself an answerable request.
A network prompt may therefore require the native UI even with hooks installed.

Codex rollouts are a last-observed view, not a liveness oracle. The observer
reads recent files (24-hour lookback), handles rotation, malformed/partial JSONL
and bounded per-file work, and never tails reasoning/tool output into the API.
Active state with no activity for five minutes becomes idle with an explicit
“live state unknown” explanation. It cannot reliably infer a permission request
that is not logged. The native app remains the authority. Initial hydration
must not announce old completions as new events.

State is in memory. Discovery failures retain previous snapshots and appear in
`AgentManager.health`. `updated_at` means last observed change/activity, not a
promise that the process is alive. Done/error records can remain visible for
this manager's lifetime. Events are bounded and not durable: on `stream.gap`
refresh sessions and outstanding approvals. The hub should persist its own
user-facing card delivery if it needs replay across restart.

## Permissions, races and privacy

The installer **generates** a new mode-0600 settings fragment with a random
secret. It never reads or patches `~/.claude/settings.json`,
`~/.codex/config.toml`, launchd or Keychain. Owner review/installation is a
separate step. Generated hooks go only to
`http://127.0.0.1:<port>/hooks/claude` with `X-Orb-Secret`.

A PermissionRequest hook blocks until the user answers or its local deadline
expires. Claude's prompt and other hooks can win first. Disconnection,
PostToolUse correlation, Stop/End and expiry clear stale requests; duplicate
approvals are rejected. PermissionRequest lacks `tool_use_id`, so identical
parallel tool inputs cannot always be correlated. Such ambiguous requests wait
for cancellation, session/turn completion, or expiry. Resolutions cleared by an
external action carry no claimed native decision. No rule grants a persistent
allowlist or auto-approves requests. Expiry denies and emits a resolution.

Codex command and file responses map allow/deny to accept/decline. Permission
requests grant only the requested scopes, for this turn, and deny returns no
scopes. Optional denial text is supported by Claude; Codex's approval protocol
has no equivalent free-text denial field, so it is not forwarded there.
Unsupported server requests receive an explicit RPC error. A single user-input
question accepts plain text via `send`; multiple questions require a JSON object
mapping all displayed question IDs to string answers.
`ApprovalRequest.scope` records the actual grant lifetime (`operation` or `turn`).
`approve` awaits a delivery receipt covering the provider write/drain, including
HTTP hook responses; transport failure raises `delivery_failed`. Resolving the
internal decision future alone does not establish delivery.
Card callers supply `expected_request` to atomically reject a reused provider
request ID that now points to a different approval instance or scope.

Owned Claude's default stdio mode also hosts `AskUserQuestion`: `send` accepts
plain text for one question, or a JSON object keyed by displayed `q1`, `q2`, etc.
for multiple questions. The adapter maps these to Claude's original question
text keys internally. Its optional hooks-only mode does not host questions;
use the default stdio mode for full conversation control. Observed HTTP question
hooks fall back to Claude's native UI, with `waiting_input` shown in Orb.
Owned-session Stop hooks update summaries only; stream results determine turn
completion, preventing a delayed result from overwriting a newly sent turn.

Events never contain raw tool input, file bodies, shell scripts, patch contents,
reasoning, or transcript records. Tool summaries show paths, the executable
without arguments, and bounded permission-scope counts. Unknown input is
omitted. Assistant summaries and titles are bounded, remove fenced code and
redact common credential forms; arbitrary prose cannot be guaranteed free of
secrets. `cwd`, titles and summaries are private metadata. Authenticate hub
callers, and redact them in Always On/short-look surfaces as required by
[hig.md](hig.md). Voice transcripts remain untrusted user input; confirm before
sending. The package does not grant Dot or devices unrestricted local control.

No Codex notify integration is needed. The owner's existing notify slot remains
untouched; no wrapper or replacement is installed. If a future feature needs
notify, it must chain the existing command explicitly after owner review.

## Hub integration (v1.1)

`hub/orb_hub/agents.py` embeds one `AgentManager` in `AgentsBridge` and maps
provider metadata into the byte-bounded Session/Card shapes in
[protocol.md](protocol.md). Install the local package alongside the hub:

```sh
python -m pip install -e hub/packages/orb_agents
orb-hub serve --agents --hooks-port 8447
orb-hub agents status
orb-hub agents hooks-snippet
```

Integration is explicitly enabled with `--agents`; the base hub remains usable
without the provider package or an additional listener. The bridge starts
read-only Claude discovery (`claude agents --json`), Codex rollout observation,
one bounded provider-event consumer, and the authenticated hook listener at
`127.0.0.1:8447/hooks/claude`. The listener can use port 0 in isolated tests.
No provider session starts during discovery. Owner authentication and existing
sessions are not changed. The status command reports discovery degradation,
published-session and pending-approval counts, the hook URL and the token-file
path; it never reports the token itself.

`agents hooks-snippet` prints JSON with a literal
`REPLACE_WITH_PRIVATE_ORB_HOOK_TOKEN` header placeholder. The bridge creates a
private `agents/hook.token` below the hub data directory on its first enabled
start. The owner substitutes that file's contents **privately** when merging
the snippet into Claude settings. The placeholder is not a usable credential.
The CLI does not print a real secret or read/modify Claude's settings. Existing
hooks must be preserved by the owner. Restart Claude as its own configuration
rules require after applying the fragment; Orb does not restart it.

To explicitly create a controllable session on a hub-owned transport:

```sh
orb-hub agents start codex --cwd /absolute/project --prompt 'Review this code'
orb-hub agents start claude --cwd /absolute/project --prompt 'Review this code' --model haiku
```

The wire `can_message` is true while an available hub-owned transport or an
explicitly enabled Codex queue path can accept the session's next message.
Observed Claude sessions remain read-only. A session waiting on approval cannot
accept a new message. Queue success reports `queued`, owned transport success
reports `delivered`, and failures include a safe reason and `failed` status.
`message.send` routes confirmed text to `AgentManager.send`. Question cards use
`AgentManager.answer_question(session_id, question_id, text)`, capturing the
immutable `Session.question_id` when published. Adapters atomically validate and
claim that pending question before yielding; stale cards cannot answer a newer
question or start another turn. Metadata refresh preserves the card identity.
Per-instance tokens distinguish reused provider request IDs; queued replies are
re-checked under the transport write lock before sending.
One hosted question accepts plain text; multiple
questions need the adapter's JSON object keyed by displayed question IDs.
The public provider metadata contains question text rather than a stable option
schema, so these cards offer a text reply. Questions without a stable provider
identity, observed questions and permission
notifications without an actual PermissionRequest show an attention card
directing the owner to the native application, with an explicit Dismiss action
and no pretend delivery path.

Permission cards carry their actual scope. Operation grants offer **Allow once**;
turn grants offer **Allow for turn** and explicitly cover subsequent operations
in the current turn. Both keep **Deny**, with no tinted/default grant action
(HIG §7). Scopes the device cannot represent show a read-only attention card
with Dismiss and direct the owner to the provider application. A grant maps to
one outstanding `AgentManager.approve` call. The SessionHub records the first
answer before acknowledgment, awaits the provider transport write before setting
`delivered:true`, and returns that durable result on retries, including failure.
Provider expiry,
disconnection, external resolution or turn/session completion cancels the
card. Card admission failure never answers a permission: the bridge retries
publication while the provider request remains live, and the provider's own
expiry remains fail-closed. Shutdown closes the listener and owned transports,
cancels their live cards, and removes session message handlers. After a crash,
SessionHub cancels stale provider approval/question records rather than
reclaiming provider ownership or replaying a decision.

Bridge publication selects at most 24 sessions, retaining already-published
pending approvals, owned transports, then pending/active/recent sessions.
Starts serialize and reserve shared capacity before provider startup, releasing
that reservation on failure or cancellation so no owned transport starts hidden.
SessionHub
shares that capacity and the 16-live-card budget with MCP/Dot, enforces UTF-8
byte limits, coalesces updates, and bounds device delivery queues. A provider
`stream.gap` or periodic reconciliation refreshes the public projection from
current immutable metadata and live approvals. Normal UUID-based IDs are kept;
long provider IDs receive a stable bounded hash alias. Full provider IDs remain
internal to the bridge for routing.

Each device receives a separate focus projection of at most eight sessions:
waiting first, then busy, then recent state changes (ten minutes by default).
Collapsed sessions are counted and paged on demand; observation still retains
the shared 24-session admission bound. `orb-hub prefs show <device>` and
`orb-hub prefs set <device> --codex off --idle-minutes 10` inspect/change the
persisted per-device projection. Disabling a source cancels its presented cards
on that device without answering the provider or affecting another device.
Re-enabling replays still-pending cards. See [protocol.md](protocol.md#per-device-focus-and-preferences)
for ordering, counts, paging, and preference validation.

The bridge's fake-provider tests include an actual authenticated loopback HTTP
PermissionRequest → SessionHub card → answer → hook response round trip,
first-answer-wins replay, card cancellation, owned text delivery, truthful
observed capabilities, byte limits and card-admission retry. These are local
integration proofs; authenticated native-provider and physical-panel validation
remain separate, as recorded below.

## Tests and isolated live checks

The default suite uses in-memory fakes, real stdio subprocess fixtures and real
loopback HTTP. It tests approvals, cancellation/races, concurrent requests,
partial/rotated rollouts, status, two-turn message delivery, EOF/crash cleanup,
authentication, HTTP framing/limits, secret handling and disabled desktop writes.
Every server binds `127.0.0.1` and is stopped by fixture/context cleanup.

Run all normal tests from the repository root:

```sh
.venv/bin/python -m pytest hub/packages/orb_agents/tests -m 'not live' -q
```

Live tests are opt-in and use throwaway settings, working directories and
provider homes. They never reuse an owner session, copy credentials, change
real config, or install hooks. The Claude test supplies `--settings` with the
generated HTTP hooks; Codex creates a fresh app-server thread under a temporary
`CODEX_HOME`. The tests require status, an actual provider permission request,
an explicit decision, and subsequent user-message completion to pass. A blocked
startup or missing authentication is a failure when opted in, not a fake pass.

```sh
ORB_AGENTS_LIVE=1 .venv/bin/python -m pytest \
  hub/packages/orb_agents/tests/test_claude_live.py \
  hub/packages/orb_agents/tests/test_codex_live.py -v -s
```

Cheap-model defaults are Claude `haiku` and Codex `gpt-6-luna`; availability and
account pricing can differ. Override with `ORB_AGENTS_CLAUDE_MODEL` and
`ORB_AGENTS_CODEX_MODEL`. Dedicated test authentication must be supplied
explicitly to a test, never copied from owner config. See the live test modules
for the allowlisted environment variables. The sandbox can block the CLI's own
native startup even with isolated homes; do not bypass it to make a test pass.

Live execution results and remaining validation gaps are recorded below.

## Sources

- [Claude hooks reference](https://code.claude.com/docs/en/hooks): HTTP fields,
  PermissionRequest decisions, first-answer-wins semantics, notifications and
  hook failures.
- [Claude Python Agent SDK](https://github.com/anthropics/claude-agent-sdk-python):
  stream-json initialization, control requests/responses and user messages.
- [Codex app-server](https://learn.chatgpt.com/docs/app-server): stdio transport,
  thread/turn control, status and server-initiated requests.
- [Codex app-server approvals](https://learn.chatgpt.com/docs/app-server#approvals):
  command/file decisions and turn-scoped permission subsets.

Installed CLI help and generated schemas take precedence over inferred
capabilities. Neither private inbox delivery nor desktop queue delivery is
claimed as working.

## Local live-test result (2026-10-26)

Both gated smoke tests were attempted in this worktree. Neither is recorded as
an end-to-end success:

- **Claude:** real persistent stream initialization and HTTP lifecycle hooks
  succeeded with temporary `--settings` and `CLAUDE_CONFIG_DIR`. The turn then
  reported `authentication_failed` / “Not logged in”, before a permission request.
- **Codex:** a real stdio app-server created a thread and accepted turn/start;
  working status was observed. The isolated account lookup returned no account,
  and the turn failed before a permission request.

The immediate blocker was missing authentication in isolated homes, not a
claimed sandbox write failure. No credentials were copied, no owner config or
session was changed, and the test-owned processes/listeners were stopped.
Run the command above with deliberately provided **test-only** API credentials:
`ORB_AGENTS_CLAUDE_TEST_API_KEY` and `ORB_AGENTS_CODEX_TEST_API_KEY` (supply values
through the engineer's secure environment, never command history or this repo).
Those variables are not printed or written into provider settings. The Claude
test passes its dedicated value as `ANTHROPIC_API_KEY`; the Codex temporary
provider config references the variable by name. If the engineer's sandbox or
account blocks the model path, the gated test must remain a reported failure.

Still open: authenticated live approval/send proof for both providers; real
Claude desktop origin/hooks validation; private Claude inbox format and Codex
queue delivery into desktop sessions. Claude desktop sending remains unavailable;
Codex queue is opt-in and never reports unverified desktop consumption as delivered.

Final local verification:

```text
.venv/bin/python -m pytest hub/packages/orb_agents/tests -q
181 passed, 2 skipped (the opt-in live tests)

RUFF_CACHE_DIR="$PWD/.ruff_cache" .venv/bin/ruff check hub/packages/orb_agents
All checks passed

RUFF_CACHE_DIR="$PWD/.ruff_cache" .venv/bin/ruff format --check hub/packages/orb_agents
19 files already formatted

.venv/bin/python -m build hub/packages/orb_agents
Built wheel and source distribution
```

The wheel was also installed without dependencies into a temporary target and
passed an import/start/send fake-session roundtrip. Both subprocess fixtures
were verified present in the source distribution. The two opted-in real smoke
attempts failed at isolated authentication, as reported above.
