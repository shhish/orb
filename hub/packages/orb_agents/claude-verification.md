# Claude adapter verification (2026-10-26)

The local `claude --version` reports **2.1.299**. `claude --help` exposes
`-p`, stream-json input/output, `--permission-prompts host|none`, manual
permission mode, `--settings`, `--setting-sources`, and `--session-id`.
`claude agents --help` documents `--json` as active interactive and background
sessions; `--all` additionally includes completed background sessions.

The read-only `claude agents --json` probe returned an empty array. Inspection
of the installed executable's serializer established the emitted field names:
`pid`, `cwd`, `kind`, `startedAt`, `sessionId`, `name`, `status`, `waitingFor`,
and, for background jobs, `id` and `state`. The serializer omits origin and
last-assistant text. Desktop origin is therefore inferred from read-only
process ancestry through `Claude.app`; without that evidence, origin is `cli`.
The latest assistant text is extracted from at most 128 KiB of the matching
UUID transcript's tail and reduced to the package's bounded display summary.
No owner transcript contents were printed during verification.

The subprocess adapter implements only the stream-json/control fields required
for initialization, user messages, can-use-tool permission responses,
cancellation, assistant messages, and results. Field shapes were checked
against Anthropic's maintained Python SDK source; this package has no SDK
runtime dependency. The installed CLI accepted the initialization exchange
in an isolated real subprocess.

## Evidence and limits

```sh
.venv/bin/python -m pytest hub/packages/orb_agents/tests/test_claude.py -q
# 39 passed

ORB_AGENTS_LIVE=1 .venv/bin/python -m pytest \
  hub/packages/orb_agents/tests/test_claude_live.py -q
# 1 failed: Claude authentication is unavailable in this configuration
```

The live test uses a fresh `CLAUDE_CONFIG_DIR`, a temporary explicit settings
file with generated authenticated HTTP hooks, a temporary workspace, model
`haiku`, and a $0.10 budget. The real CLI initializes, writes only its isolated
session files, and emits HTTP lifecycle hooks. It then reports the machine
readable `authentication_failed` error and a failed result before requesting
tool permission. The test deliberately fails when enabled; it does not claim
approval or second-message success when authentication is unavailable.
The sandbox did not prevent temporary session-file writes in this attempt.

An engineer can rerun the command with a dedicated test API key intentionally
supplied as `ORB_AGENTS_CLAUDE_TEST_API_KEY`. Only that variable is mapped to
the subprocess's `ANTHROPIC_API_KEY`; normal auth variables are not inherited.
`ORB_AGENTS_CLAUDE_MODEL` can explicitly select a different inexpensive model
if `haiku` is unavailable. Do not copy the owner's OAuth files or
change the real Claude settings to make the test pass. With authentication,
the test requires a real Write approval via the HTTP hook, approves the
throwaway file, checks its content, sends a second stream-json user message,
and requires its completion. Every child and loopback listener is closed in
`finally`/context-manager cleanup.

The fake subprocess suite proves persistent send, allow and deny, permission
timeout, cancellation, process exit, malformed frames, bounded discovery,
state transitions, privacy, shutdown during startup, completion during a send,
and cleanup without credentials or model calls. It also verifies
`AskUserQuestion` in the default stdio mode: one question accepts plain text
through `send`; multiple questions require a JSON object mapping `q1`, `q2`,
and so on to answer strings. These IDs are shown in `waiting_for`. The adapter
maps them to Claude's original question text keys internally and returns the
answers in `updatedInput`. Options and raw request contents stay internal.
The hooks-only transport used by the live hook smoke leaves questions to
Claude's native handling; it does not host the SDK input callback.

Existing desktop/CLI inbox delivery remains unavailable, even when
`experimental_inbox=True`: the undocumented format has not been validated
against an authenticated throwaway session. No owner inbox was opened.

## Primary references

- [Claude hook reference](https://code.claude.com/docs/en/hooks): HTTP
  PermissionRequest decisions, lifecycle hooks, and competition with the host.
- [Streaming input](https://code.claude.com/docs/en/agent-sdk/streaming-vs-single-mode):
  persistent input and queued user messages.
- [Approvals and user input](https://code.claude.com/docs/en/agent-sdk/user-input):
  the distinct AskUserQuestion answer shape.
- [Python SDK control implementation](https://github.com/anthropics/claude-agent-sdk-python/blob/main/src/claude_agent_sdk/_internal/query.py)
  and [subprocess transport](https://github.com/anthropics/claude-agent-sdk-python/blob/main/src/claude_agent_sdk/_internal/transport/subprocess_cli.py):
  interoperability field names, checked alongside the installed CLI.
