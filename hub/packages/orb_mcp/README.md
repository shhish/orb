# orb-mcp

Standalone Python 3.13 MCP bridge for Orb's round touchscreen. The package owns
cards, subscriptions and delivery state in one SQLite database. The hub supplies
device I/O through `DeviceBackend`; this package does not depend on `orb_hub`.

```text
ChatGPT Dot -> Secure MCP Tunnel -> tunnel-client -> orb-mcp stdio
Claude Code / Codex ------------------------------> orb-mcp stdio
                                                         |
                                            loopback HTTP + client token
                                                         |
                                         127.0.0.1:8445/mcp
                                            |             |
                                      DeviceBackend   SQLite outbox
                                            |             |
                                          Orb       signed HTTPS callback
```

## Install and run locally

Run from the repository root. Dependencies and development tools are pinned in
this package's `pyproject.toml`; `requirements-dev.lock` also pins the tested
transitive dependencies.

```sh
/opt/homebrew/bin/python3.13 -m venv .venv
.venv/bin/python -m pip install -c hub/packages/orb_mcp/requirements-dev.lock -e 'hub/packages/orb_mcp[dev]'

# Keep generated credentials/database inside an isolated directory.
ORB_DEV_DIR=$(mktemp -d /tmp/orb-mcp-dev.XXXXXX)
.venv/bin/orb-mcp serve --fake-device --data-dir "$ORB_DEV_DIR" --port 8445
```

The foreground process binds only `127.0.0.1`; stop it with Ctrl-C when finished.
Choose another `--port` if 8445 is in use. `serve` creates distinct 32-byte tokens
at `tokens/dot.token`, `tokens/claude.token` and `tokens/codex.token`, all mode
0600. Existing tokens are retained and validated. It creates `orb-mcp.sqlite3`
with private permissions, WAL journaling and `synchronous=FULL`.

In another terminal, pass the same directory path and run the adapter:

```sh
.venv/bin/orb-mcp stdio \
  --url http://127.0.0.1:8445/mcp \
  --token-file /path/to/orb-mcp-dev/tokens/claude.token
```

The adapter consumes one JSON-RPC object per line and emits only JSON-RPC on
stdout. Diagnostics go to stderr. Each client uses its own adapter process and
token. Configure Claude Code, Codex or the tunnel to launch that command when
the owner chooses to register the server; installation never edits their real
configuration, running sessions, launchd jobs or Keychain.

The fake backend has authenticated development-only endpoints:

- `POST /__fake/answer`: `{"card_id":"demo-1","option_id":"yes"}`;
  use `"kind":"dismissed"` without `option_id` to dismiss.
- `POST /__fake/transcript`: `{"recording_id":"recording-1","text":"Hello",
  "duration_s":1.5,"device_id":"orb-fake","focus":"dot","confirmed":true}`.

These endpoints exist only when the in-memory backend is explicitly enabled.
There is no production callback-URL bypass. The authenticated `/health` endpoint
reports whether the fake backend is enabled.

## Tools and state

| Tool | Arguments | Effect |
| --- | --- | --- |
| `orb_show` | `card_id`, `text` ≤400; optional `title` ≤40, `device`, `priority` | Queue a short card |
| `orb_ask` | `card_id`, `text` ≤280, 1–3 `options`; optional `device`, `priority` | Queue a question |
| `orb_get_card` | `card_id` | Read a card and its persisted answer |
| `orb_get_transcript` | `recording_id` | Read a confirmed transcript |
| `orb_cancel` | `card_id` | Cancel a pending card |
| `orb_status` | `{}` | Read devices, caller's queue, subscriptions/expiry, outbox and speech readiness |
| `orb_wait` | `card_id`, optional `timeout_s` from 0 to 50 | Wait for a terminal card state |

Card IDs are nonempty strings up to 64 characters; option IDs are nonempty up to
32 characters and labels up to 24. Option IDs must be unique. Priority is
`normal` or `urgent`. All input objects, including nested options, reject unknown
properties. Read tools set `readOnlyHint:true`; cancellation sets
`destructiveHint:true`. Show/ask are writes, even though retries are idempotent.

Show/ask return `{card_id,status,device_online}`. The five statuses are `queued`,
`showing`, `answered`, `dismissed` and `cancelled`. A wait timeout returns the
current card, which may still be pending; it does not synthesize an answer.
Dismissal, cancellation, timeout and silence never mean approval.

One shared queue permits at most 20 pending cards across clients. Each card has
a `dot`, `claude` or `codex` source badge; clients can read/cancel only their own
cards and transcripts. `card_id` is globally unique: the same owner and canonical
content return the existing record, while different content or ownership fails.
Omitted defaults and their explicit equivalents have the same meaning. Queued
cards and pending cancellations survive restart and are resent idempotently.

## MCP wire behavior

Modern requests use `2026-07-28`; no initialization handshake is necessary. For
example, the stdio adapter accepts this single line:

```json
{"jsonrpc":"2.0","id":1,"method":"server/discover","params":{"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28","io.modelcontextprotocol/clientCapabilities":{}}}}
```

Discovery advertises tools and events. Successful modern results have
`resultType:"complete"`; discovery and catalogs include `ttlMs` and `cacheScope`.
Server identity is in result `_meta.io.modelcontextprotocol/serverInfo`. The
self-contained server instructions are under 512 characters and explain unique
IDs, subscribing first, event deduplication, dismissal and consequential actions.

HTTP requests must accept both `application/json` and `text/event-stream` and use
`Content-Type: application/json`. Modern requests mirror body metadata in
`MCP-Protocol-Version` and `Mcp-Method`; `tools/call` also needs `Mcp-Name`.
The adapter supplies those headers, including the spec's Base64 sentinel encoding
when necessary. Mismatches return `-32020`/HTTP 400; unsupported versions return
`-32022`/HTTP 400 with supported versions; unknown methods return `-32601`/HTTP 404.
Responses use the permitted JSON response form. The modern transport has no GET
event stream or protocol session. [MCP transport specification](https://modelcontextprotocol.io/specification/2026-07-28/basic/transports/streamable-http)

Legacy `initialize` supports `2025-11-25`, `2025-06-18`, `2025-03-26` and
`2024-11-05` for local clients. The adapter retains the negotiated legacy version
per process. Legacy clients receive tools and use `orb_wait`; draft event methods
require modern metadata. There is no principal-wide version state that can be
changed by another client. Malformed requests and unknown tools are JSON-RPC
errors; valid tool calls with invalid arguments or domain failures return
`isError:true`. Notifications produce no reply and never create cards.

### Why a small dispatcher instead of the Python MCP SDK?

The official Python SDK **v2.3.0**, released 2026-10-02, supports the modern core,
including discovery and 2026-07-28 types. Its release has no built-in draft
`events/list`, `events/subscribe` or `events/unsubscribe` types/handlers. Custom
handlers are possible through `add_request_handler`; using the SDK would still
require the event, subscription and delivery implementation. For this limited
surface, `protocol.py` explicitly implements both eras with `jsonschema`
validation and conformance tests, and `aiohttp` supplies transport. This avoids
coupling the package to the SDK's model/transport stack while the events
extension is still a draft. It is not a claim that the SDK lacks modern MCP.
Sources: [v2.3.0 wire types](https://github.com/modelcontextprotocol/python-sdk/blob/v2.3.0/src/mcp-types/mcp_types/_v2026_07_28/__init__.py),
[v2.3.0 low-level server](https://github.com/modelcontextprotocol/python-sdk/blob/v2.3.0/src/mcp/server/lowlevel/server.py).

## Events and delivery

`events/list` describes `card.answered` (optional `card_id` and `device` filters)
and `voice.transcribed` (optional `device`). Implemented methods are
`events/list`, `events/subscribe` and `events/unsubscribe`. Subscribe/refresh uses
`name`, `arguments`, `delivery:{mode:"webhook",url,secret}`, optional `ttlMs`, and
`cursor:null`. Unsubscribe identifies the original `name`, `arguments` and
`delivery:{mode:"webhook",url}`; it does not take a subscription ID.

Subscription IDs are `sub_` plus SHA256 of canonical JSON
`[principal,url,name,arguments]`. Refresh upserts the same identity. The default
grant is 24 hours, clamped to 5 minutes–7 days; `ttlMs:null` receives a finite
24-hour grant. `refreshBefore` is the granted expiry, not an earlier reminder.
Expired subscriptions stop delivery. These event types have no replay:
`cursor` remains null and expired grants do not revive undelivered events.

Signing secrets must start with `whsec_` and decode from Base64 to 24–64 bytes.
A callback receives a signed, short-lived, random verification challenge before
any application data. Activation requires a 2xx response and a constant-time
challenge comparison. Successful verification is cached for five minutes by
principal, URL and secret, so replacing a secret is verified again. Failure
returns `-32015` with a categorized `data.reason`.

Production callbacks require HTTPS. Every connection resolves the hostname once,
rejects any non-global result, and connects only to a validated address with the
original TLS hostname. Proxies and redirects are disabled. Validation applies
to both verification and delivery, including retries.

Application events carry `eventId`, `name`, occurrence `timestamp`, `data` and
`cursor:null`. The entire serialized body is at most 256 KiB. Standard Webhooks
signing covers the exact `eventId.timestamp.body` bytes with HMAC-SHA256; headers
are `webhook-id`, `webhook-timestamp`, `webhook-signature` and
`X-MCP-Subscription-Id`. Rotation adds old-key signatures for five minutes.
Independent `standardwebhooks` verification is used in tests.

Answers/transcripts and their outbox entries commit in one SQLite transaction
before the device callback returns. Delivery claims persist before network I/O;
a crash leaves a lease that can be retried. Retries preserve `eventId` and body,
re-sign at the attempt time and back off exponentially, bounded by 16 attempts
and 15 minutes. Transient network failures, 408/425/429 and 5xx are retryable;
410 and 413 are terminal. A 410 deactivates the subscription. A 2xx acknowledges
receipt, not the model's execution. Consumers must deduplicate `eventId` and
tolerate out-of-order events. [OpenAI MCP Events](https://developers.openai.com/plugins/build/mcp-events)

## Hub integration

Implement `DeviceBackend` in the hub, using its authenticated device registry and
link. All calls and callbacks run on the same asyncio loop:

```python
def set_handlers(self, answer_handler, transcript_handler) -> None: ...
async def show_card(self, card: dict) -> bool: ...
async def cancel_card(self, card_id: str, device_id: str | None) -> bool: ...
async def list_devices(self) -> list[dict]: ...
```

Device dictionaries contain `device_id`, `name`, `online`. `show_card` returns true
only after device acceptance, false when offline. Show/cancel must be idempotent
by card ID. On reconnect call `await service.sync_device(device_id)` to replay
pending work. Do not expose the hub's reboot, OTA, pairing, config or raw audio
operations through MCP.

Store the callback handlers and call them with `Answer` / `Transcript` from
`orb_mcp.backend`. Authenticate `device_id` first. Send a device acknowledgement
only after the awaited handler succeeds; duplicates return the persisted answer
and conflicting answers fail. Answer kinds are `option` and `dismissed`.

Transcripts must be confirmed on the device, have a stable recording ID, contain
at most 8192 characters and name their `dot`/`claude`/`codex` focus. Only Dot focus
with an active matching subscription emits `voice.transcribed`. A Dot transcript
without a listener returns `{"accepted":false,"reason":"not_listening"}` so the
hub can show “Dot not listening” or apply its explicit fallback policy. Claude
and Codex focus persists the transcript for that principal; dispatch into agent
sessions remains the hub's responsibility.

Construct the package with one shared database connection:

```python
from pathlib import Path
from aiohttp import web
from orb_mcp.auth import Tokens, provision_tokens
from orb_mcp.events import Events
from orb_mcp.server import create_app
from orb_mcp.service import OrbService
from orb_mcp.storage import connect


async def start_mcp(backend, data_dir: Path):
    data_dir.mkdir(parents=True, exist_ok=True, mode=0o700)
    tokens = Tokens(provision_tokens(data_dir / "tokens"))
    connection = connect(data_dir / "orb-mcp.sqlite3")
    events = Events(
        connection,
        authorize=lambda principal, name, arguments: tokens.active(principal),
    )
    service = OrbService(connection, backend, events, whisper_ready=False)
    app = create_app(service, tokens)
    runner = web.AppRunner(app, handler_cancellation=True)
    await runner.setup()
    await web.TCPSite(runner, "127.0.0.1", 8445).start()
    return runner, service, connection


# During hub shutdown: await runner.cleanup(), then connection.close().
```

The hub should extend `authorize` with its own event/device access rules and set
`service.whisper_ready` from actual transcription readiness. The default standalone
configuration rechecks token-file validity before delivery, so removing a token
revokes that principal. HTTP bearer comparison uses constant time; token files
are reread, must be owned regular 0600 files, and cannot be symlinks. Any `Origin`
header, non-loopback literal `Host`, non-loopback peer, or duplicate security
header is rejected. The data directory must be owned and mode 0700, protecting
SQLite sidecar files as well as the database. MCP requests are limited per
principal to 32 in flight and 600 per rolling minute; excess requests receive
HTTP 429 with `Retry-After`.

Keep MCP on a **separate loopback listener at 8445**. Port 8443 belongs to the
device TLS link and port 8080 to pairing; attaching these MCP routes to the LAN
listener would violate the boundary. `create_app` manages the card/outbox worker
for its lifetime. The hub must own orderly listener shutdown and database closure.

## Tunnel preparation

The owner creates and associates the Secure MCP Tunnel through OpenAI and obtains
a Read+Use runtime key. This package never creates a tunnel or sends a key to
OpenAI. Its install command downloads only public GitHub release assets.

```sh
# Use a NEW private directory chosen by the owner; no default config is changed.
ORB_TUNNEL_DIR=$(mktemp -d /tmp/orb-mcp-tunnel.XXXXXX)
.venv/bin/orb-mcp tunnel install --directory "$ORB_TUNNEL_DIR"
.venv/bin/orb-mcp tunnel setup \
  --directory "$ORB_TUNNEL_DIR" \
  --tunnel-id '<existing-tunnel-id>' \
  --orb-command "$PWD/.venv/bin/orb-mcp" \
  --token-file /path/to/orb-mcp-data/tokens/dot.token
.venv/bin/orb-mcp tunnel doctor --directory "$ORB_TUNNEL_DIR"
.venv/bin/orb-mcp tunnel status
```

Installation pins `openai/tunnel-client` **v0.0.18**, the darwin-arm64 ZIP, its
SHA256 and `SHA256SUMS.txt` digest. Both digest pins and the matching manifest
entry must agree. Extraction rejects unsafe paths, duplicate names and symlinks.
Installation never executes the downloaded binary.

Setup prompts with `getpass` and refuses an echoing fallback or admin-key prefix.
It writes a private runtime-key file and a `file:` reference in
`profiles/orb.yaml` (JSON, a YAML subset), plus an **unloaded**
`com.orb.mcp-tunnel.plist` in the requested directory. The profile launches the
stdio adapter; the generated LaunchAgent would run `tunnel-client run --profile
orb --profile-dir … --health.listen-addr 127.0.0.1:8446`. Neither setup nor doctor
invokes the tunnel binary. Doctor checks generated files and installation hashes
offline; status checks only local `/healthz` and `/readyz` at 127.0.0.1:8446.
Review and deployment of the plist is a separate owner action.

This intentionally generates the profile offline instead of executing upstream
`init` or `doctor`, which may have additional network behavior. Profile keys,
stdio command handling and file references were checked against the pinned
[tunnel-client configuration](https://github.com/openai/tunnel-client/blob/v0.0.18/docs/configuration.md)
and [Secure MCP Tunnel guide](https://developers.openai.com/api/docs/guides/secure-mcp-tunnels).

## Validation

```sh
.venv/bin/ruff format --check --no-cache hub/packages/orb_mcp
.venv/bin/ruff check --no-cache hub/packages/orb_mcp
.venv/bin/pytest -q hub/packages/orb_mcp/tests
.venv/bin/python hub/packages/orb_mcp/scripts/local_session.py
.venv/bin/python -m build hub/packages/orb_mcp
```

The suite covers both protocol eras, strict schemas/annotations, tokens and HTTP
guards, stdio subprocesses, persistence and deduplication, subscription lifecycle,
independent webhook verification, retry/expiry/rotation, DNS pinning and SSRF,
and tunnel artifacts/download validation with fakes. The scripted local session
starts the fake service and stdio client, subscribes before asking, answers from
the fake device, and checks a signed `card.answered` delivery. Its receiver binds
127.0.0.1 on an ephemeral port through an injected **test-only transport**; this
does not relax the production HTTPS/public-IP rules. All script listeners and
subprocesses are stopped and throwaway state is removed on exit.

## Remaining integration work

- The concurrently developed `orb_hub` must supply its backend, registry/access
  policy, speech readiness and transcript routing. This package does not import it.
- `docs/protocol.md` v1 does not yet define the proposed `card.show`, `card.cancel`
  and `card.answer` link methods. Hub and firmware engineers must agree the v1.1
  contract, including dismissals, before connecting hardware.
- No physical device, real Claude/Codex session, OpenAI runtime key, live tunnel or
  actual Dot subscription was exercised. The package proves the isolated local
  path; end-to-end cloud enrollment remains an owner integration test.
- SQLite state has no retention/compaction policy yet. Back up the private
  database and protect it as credential-bearing data; it contains callback secrets
  and transcripts. Event replay is not implemented.

## Sources and provenance

Verified 2026-10-26 against the primary sources linked above and the
[2026-07-28 changelog](https://modelcontextprotocol.io/specification/2026-07-28/changelog),
[schema](https://modelcontextprotocol.io/specification/2026-07-28/schema),
[versioning](https://modelcontextprotocol.io/specification/2026-07-28/basic/versioning),
[tools error handling](https://modelcontextprotocol.io/specification/2026-07-28/server/tools),
[draft Events proposal](https://github.com/modelcontextprotocol/experimental-ext-triggers-events/blob/main/docs/design-sketch-proposal.md),
and [tunnel-client release](https://github.com/openai/tunnel-client/releases/tag/v0.0.18).
The unlicensed [rsms/esp32-dot](https://github.com/rsms/esp32-dot) was read for
architecture context only; no source, assets or documentation were copied.
