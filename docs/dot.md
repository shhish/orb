# Orb ↔ ChatGPT Dot (design and MCP implementation)

**Tags:** **V** = in the cited source, **I** = inferred or a design choice, **U** = unverified. [R] is rsms/esp32-dot, which has no license: it was read only, and nothing was copied.

## 1. Architecture

```
Dot (cloud) ─MCP─▶ OpenAI tunnel endpoint ◀─outbound long-poll─ tunnel-client (launchd)
   ▲                                                              │ stdio
   └── signed webhook, hub → callback URL ───────────────── orb-mcp stdio
Claude Code / Codex ─stdio─▶ orb-mcp stdio ─loopback HTTP+bearer─▶ orb-hub /mcp (aiohttp, SQLite)
                                                          │ pinned-TLS WSS
                                                         Orb
```

The tunnel carries inbound MCP only; webhooks leave the hub directly [E] (V). With tunnel-client down, events still flow but tool calls and refreshes fail (I).

## 2. User-only setup

1. **Plan with dots:** Pro 100, 200 or 500 (18+, outside the EEA, UK and Switzerland), Business Premium, or admin-enabled Enterprise. Rollout is gradual [D] (V).
2. **Custom MCP allowed:** the workspace must allow custom MCP servers [C] (V); events work in Work chats and dots [E] (V). Plan eligibility for custom MCP is undocumented (U).
3. **Platform permissions:** use your personal Platform org. Creating the tunnel needs Tunnels Read+Manage; the runtime key and selecting the tunnel need Read+Use. Propagation takes up to 30 minutes [T] (V).
4. **Create the tunnel** and associate it with the ChatGPT workspace [T] (V).
5. **Create a Read+Use runtime key** (never an admin key) and enter it at the `orb-mcp tunnel setup` prompt [TC] (V).
6. **Add the plugin:** at chatgpt.com/plugins choose **+ → Add custom MCP server → Tunnel → No authentication → Create as a plugin**, install it, then enable it for Dot [T][C][R] (V). **Refresh** after changing tools or events [P] (V).
7. **Ask Dot to subscribe.** A tool call doesn't subscribe it [R][DC] (V).

**Automated:**
- **tunnel-client:** a Go binary (Apache-2.0, v0.0.18 from 2026-10-23), asset `tunnel-client-<tag>-darwin-arm64.zip`, checked against `SHA256SUMS.txt` [TC].
- **Profile:** upstream recommends `init --sample sample_mcp_stdio_local --mcp-command …`, then `doctor --explain` [T]. Orb generates the pinned profile format offline and provides its own offline `tunnel doctor`; it does not invoke those upstream commands or send credentials (I).
- **Service:** a LaunchAgent runs `run --profile orb --health.listen-addr 127.0.0.1:8446`, because the default 8080 clashes with Orb's pairing port [TC] (V). It's used instead of `runtimes connect`, whose reboot behaviour is undocumented (I).
- **Key:** stored as a `file:` reference, mode 0600 [TC] (V).

## 3. MCP server

**Transport:** the hub serves Streamable HTTP at `127.0.0.1:8445/mcp` with a bearer token.
- **Adapter:** `orb-mcp stdio --url http://127.0.0.1:8445/mcp --token-file <client.token>` forwards JSON-RPC lines and reads the token from disk. It serves tunnel-client, Claude Code and Codex. [R] uses this pattern with Dot.
- **Alternative:** tunnel-client supports `--mcp-server-url` and extra headers [TC] (V), but the proposed `X-Orb-Auth` header is not implemented by Orb. This package requires `Authorization: Bearer …` and ships the stdio profile; a direct HTTP tunnel profile remains untested (U).
- **One client per tunnel ID** on stdio. It restarts a crashed child [TC] (V).

**Protocol:**
- **Version:** 2026-07-28 is required for events [E]. It brings `server/discover`, `_meta` versioning, `resultType` and `Mcp-Method`/`Mcp-Name` headers [M] (V). Keep legacy `initialize` for local clients (I).
- **Capabilities:** `{tools, events}`. Keep `instructions` self-contained within the first 512 characters [C] (V).

**Tools:** `additionalProperties:false`. `card_id` (up to 64 characters) is the caller's idempotency key. Limits suit 466 px (I).
- `orb_show {card_id, text≤400, title?≤40, device?, priority?:normal|urgent}` → `{card_id, status:queued|showing|answered|dismissed|cancelled, device_online}`
- `orb_ask {card_id, text≤280, options:[{id≤32,label≤24}]×1–3, device?, priority?}` → same
- `orb_get_card {card_id}` and `orb_get_transcript {recording_id}`: readOnly
- `orb_cancel {card_id}`: destructive
- `orb_status {}`: devices, queue, subscriptions with `refreshBefore`, outbox, whisper readiness (readOnly)

ChatGPT confirms any tool that lacks `readOnlyHint` [C] (V).

**Events:** webhook delivery only. Payloads are data, never instructions [E] (V).
- `card.answered`: arguments `{card_id?, device?}`, payload `{card_id, kind, option_id?, option_label?, device_id, answered_at}`
- `voice.transcribed`: arguments `{device?}`, payload `{recording_id, text≤8192, duration_s, device_id}`

**Subscriptions** [E][S] (V):
- **Secret:** `whsec_` + base64 of 24–64 bytes.
- **Callback:** https only. Resolve once **per connection attempt**, reject non-global IPs, and connect to a validated IP using the original SNI. Repeat the check for verification and every delivery/retry; do not cache an address for the subscription lifetime. No redirects or proxies.
- **Verification:** POST a signed `{type:"verification",challenge}` and check the echo in constant time. On failure, return `-32015`.
- **ID:** `sub_` + sha256(canonical `[principal,url,name,args]`), upserted in SQLite.
- **TTL:** 24 h by default, clamped to 5 min–7 days. `ttlMs:null` still gets a finite grant (I). `refreshBefore` is the granted expiry; ChatGPT refreshes before it [E].
- **Rotation:** when the secret changes, sign with both for 5 minutes.
- **Cursor:** `null`, so there is no replay.

**Delivery** [E][S] (V):
- **Outbox:** written before each attempt. One event per POST, at most 256 KiB.
- **Headers:** `webhook-id`=eventId, `webhook-timestamp`, `webhook-signature: v1,base64(HMAC-SHA256(b64decode(secret),"id.ts.body"))`, `X-MCP-Subscription-Id`.
- **Retries:** re-sign each retry, keep the same eventId, and back off for about 15 minutes. Never retry 410 or 413.
- **Signing only:** the hub signs webhooks; it never verifies incoming ones.

## 4. Production hub wiring (protocol v1.1)

`orb_hub.dot.DotRuntime` now embeds the MCP service on a **separate loopback
listener**, enabled explicitly with `orb-hub serve --dot` (default MCP port
8445). Install `hub/packages/orb_mcp` alongside the hub first. Without that
optional package/flag the existing pairing, OTA and v1 device path still works.
The running owner's hub is not automatically restarted or reconfigured.

- **Private state:** `<hub-data>/mcp/orb-mcp.sqlite3` and
  `<hub-data>/mcp/tokens/{dot,claude,codex}.token`. Runtime initialization creates
  independent per-client credentials with mode 0600 under private
  directories; it never prints their contents or writes owner agent settings.
- **Device backend:** paired devices are listed from the actual hub registry;
  `online` for this surface requires an authenticated live link announcing the
  `cards` capability. Legacy v1 devices stay offline for MCP cards. A requested
  device remains the sole recipient; another paired device cannot answer it.
- **Wire model:** `card.show` events contain the v1.1 Card from
  [protocol.md](protocol.md), and `card.cancel` contains `{card}`. The display
  card ID is a namespaced full SHA-256 digest, encoded to fit 64 bytes. Original
  MCP IDs, option IDs/labels and full text remain in the durable MCP database.
  Display strings are safely clipped on UTF-8 boundaries to v1.1 limits, and
  local option aliases map answers back to the original IDs. Plain messages and
  questions both offer an explicit Dismiss option; navigation never dismisses.
  MCP source ownership is persisted separately from the wire Card, including
  Claude/Codex cards without a session reference. Per-device `prefs.sources`
  filters both new delivery and replay; disabling a source cancels only that
  device's presentation, preserving the provider request. Old database cards
  without recorded source ownership stay hidden until the provider rebinds
  the exact existing card identity; no source is guessed during migration.
- **Answers:** device `card.answer {card,option}` passes through SessionHub's
  first-answer-wins record and awaits the MCP callback, which atomically commits
  the source answer and webhook outbox before successful delivery is reported.
  A process failure between the root record and callback can leave
  `delivered:false`; approvals are never automatically replayed after restart.
- **Bounds:** the production MCP queue shares the hub's 16 live-card budget,
  including offline and queued work across providers. SessionHub handles replay,
  per-device delivery bounds and capability negotiation. Existing package-only
  fake demonstrations retain their documented independent 20-card limit.
- **Dot session:** the synthetic `dot` Session truthfully shows “Dot not
  connected” without an active matching `voice.transcribed` subscription. It
  stays idle, so a missing plugin subscription cannot outrank a real permission
  prompt. Token removal also revokes event authorization. Its messaging
  capability refreshes per paired device as subscriptions change; each actual
  send additionally checks device matching. The last Dot answer is replayed
  only to the paired device addressed by that exchange, never in shared session
  metadata. A previous answer remains readable while Dot is disconnected.
  Outside focus, Orb discovers Dot through the existing idle-session pager;
  the device-specific Session summary carries its last answer sentence. Queue
  acceptance is idle, while an actual answer is done and receives the normal
  recent-session grace period.
- **Confirmed voice/text:** `message.send {target:"dot",text,stream?}` invokes
  the transcript callback only after device confirmation. Missing or expired
  matching subscriptions yield `not_listening`. The confirmed text and webhook
  event commit atomically; transcription results alone never trigger a send.
  Confirmed voice preserves its duration and uses a link-scoped recording ID
  so retrying one recording cannot enqueue another event.
  Direct text messages use a fresh recording ID and a zero-second duration.
  Acceptance returns **queued** for the durable webhook outbox; it does not
  claim ChatGPT has received or acted on the message. A failed send retains the
  transcript on Orb and is never retried automatically.

`orb-hub dot status` reads the running hub's queue, subscriptions with
`refreshBefore`, outbox and device availability. Tunnel setup remains an owner
operation through `orb-mcp tunnel …`; generated profiles and plists remain
unloaded. Registration uses `orb-mcp stdio --url http://127.0.0.1:8445/mcp
--token-file <hub-data>/mcp/tokens/<client>.token`; merge generated settings
manually. No cloud enrollment or physical device validation is claimed here.

## 5. Security

- **Data path:** tool arguments and transcripts pass through OpenAI [TC] (V).
- **No authentication:** the plugin relies on tunnel workspace scoping plus the Use permission [T]. Keep the plugin personal (U).
- **Hub `/mcp`:** loopback only. Each client (dot, claude, codex) gets its own 32-byte token in a 0600 file, compared in constant time. Reject any `Origin` header or non-loopback `Host` (I).
- **Not exposed:** reboot, OTA, pairing, config and audio stay out of MCP.
- **Transcripts:** untrusted. The skill makes Dot confirm consequential actions. The production hub shares a 16-card budget across providers; the independent fake service permits 20.

## 6. Failure modes

- **Expiry:** refreshing needs both the tunnel and the hub, and a missed refresh stops delivery without warning. `orb_status` shows `refreshBefore`; the device shows "Dot not listening" (I).
- **Delay:** a 2xx means received, not acted on; processing is asynchronous and may be batched [E] (V).
- **Tunnel limits:** a 120 s deadline that includes queue time, 600 requests/min, 10 MiB, 1,000 pending [T] (V).
- **Dedup:**
  - A repeated `card_id` with identical content returns the existing record; different content is an error.
  - Answers are persisted before the device gets an ack.
  - eventIds are stable and events can arrive out of order, so Dot dedups by eventId [E][R].
- **Sleep:** the queue persists, but the Mac mini must not sleep.

## 7. Coexistence

- **Register:** configure each local client to launch the installed `orb-mcp stdio --url http://127.0.0.1:8445/mcp --token-file <its-own-0600-token-file>`. Registration remains an owner action; the package does not modify Claude or Codex configuration.
- **No local events:** callbacks must be public HTTPS (I), so add `orb_wait {card_id, timeout_s≤50}` for local clients (I).
- **One shared queue,** with a source badge on each card.
- **Skill text:** goes in `instructions` [C]. [R] also serves a skill through `skills/list`; OpenAI's docs don't cover this (U).

## 8. Without the tunnel

- **Cloud plugins:** need either the tunnel or a public HTTPS endpoint, which means a proxy plus OAuth or OpenAI-managed mTLS [P][MS] (V). Events have no local-only path.
- **Partial workaround (I/U):** connect the Mac to Dot with the ChatGPT app open [DC]. Local Codex tasks Dot starts could use the `~/.codex/config.toml` stdio server: tools only, no transcripts.

## Implementation

`hub/packages/orb_mcp` is an independently installable Python 3.13 package with
its own `pyproject.toml`, `src/orb_mcp` and pytest suite. The
[package README](../hub/packages/orb_mcp/README.md) documents installation, all
CLI commands, backend integration, protocol details and an isolated local demo.
It has no import dependency on the separately developed `orb_hub`.

- **Server:** `orb-mcp serve --fake-device --data-dir <isolated-directory>` runs
  the in-memory device at `127.0.0.1:8445/mcp`, creates separate private tokens for
  Dot/Claude/Codex, and persists cards, transcripts, subscriptions and outbox state
  in SQLite. The hub embeds `create_app` with its own `DeviceBackend` on a separate
  loopback listener. Device TLS and pairing retain their configured hub ports (the
  owner's existing service uses TLS 8444).
- **Backend:** `show_card`, `cancel_card`, `list_devices` and awaited answer/voice
  callbacks form the boundary. A callback returns only after its source record
  and matching outbox rows commit atomically. Backend show/cancel must be
  idempotent; `sync_device` replays pending work after reconnection. The queue
  limit is shared, while read/cancel operations are scoped to the authenticated
  client. Dot voice requires confirmation, Dot focus and an active matching
  subscription; `not_listening` is returned to the hub otherwise.
- **Protocol:** all seven tools, modern discovery, legacy initialization and the
  three event methods are implemented. The dispatcher enforces input schemas,
  annotations, idempotency and per-client isolation. Instructions fit in 512
  characters. Outbound callbacks use DNS pinning, TLS hostname verification,
  bounded verification caching, five-minute secret rotation and a durable
  retry outbox. Token removal revokes access and the hub can supply a stricter
  event-authorization callback.
- **Tunnel artifacts:** `orb-mcp tunnel install|setup|doctor|status` pins
  tunnel-client v0.0.18 darwin-arm64, verifies the archive and manifest SHA256s,
  writes a private key reference and stdio profile, and generates an unloaded
  LaunchAgent with health at `127.0.0.1:8446`. Setup never starts tunnel-client,
  creates a tunnel, loads launchd or sends a key to OpenAI.
- **Validation:** unit and integration tests use throwaway state and fake
  devices/transports. `scripts/local_session.py` under the package demonstrates
  a fake-service/stdio-client/question/answer/webhook round trip. Its injected
  loopback webhook transport is test-only; production callbacks still require
  global HTTPS. Actual Dot enrollment and physical device validation remain integration work.
  The production adapter is `hub/orb_hub/dot.py`, with isolated integration tests
  in `hub/tests/test_dot_bridge.py`.

### Clarifications found against primary sources

1. **Modern metadata is per request.** The version and client capabilities belong
   in `params._meta` under `io.modelcontextprotocol/protocolVersion` and
   `io.modelcontextprotocol/clientCapabilities`. Modern results require
   `resultType:"complete"`; discovery includes `supportedVersions`, and
   discovery/catalog results include `ttlMs` and `cacheScope`. Server identity
   moves to result `_meta.io.modelcontextprotocol/serverInfo`. Some abbreviated
   event examples omit these core fields; Orb includes them. [M][M2]
2. **HTTP mirrors the body.** Modern requests require `MCP-Protocol-Version`,
   `Mcp-Method`, and `Mcp-Name` for tool calls. Header mismatches use `-32020` with
   HTTP 400, unsupported versions use `-32022` with HTTP 400, and unknown methods
   use `-32601` with HTTP 404. Modern Streamable HTTP has no GET stream or
   protocol-level session; a JSON response to POST is sufficient here. [M3]
3. **Subscription lifecycle uses identity, not ID.** Unsubscribe takes the
   original event name, arguments and callback delivery URL. `refreshBefore` is
   the expiry granted by the server. Callback verification failure includes a
   categorized `data.reason`; successful verification can be cached briefly,
   with re-verification when the secret changes. [E]
4. **SDK support needs a precise distinction.** Python SDK v2.3.0 supports the
   modern core and custom request handlers but has no built-in draft Events
   methods. Orb uses a small dispatcher for this limited surface; the reason is
   draft-event integration and dependency scope, not absence of MCP 2.0 support.
   [PY]
5. **Device wire adoption is explicit.** `protocol.md` v1.1 defines the
   production wire methods and shared display model. Section 4 describes the
   hub adapter; devices must announce the `cards` capability before receiving
   this traffic. Legacy devices retain their existing path.

Research and implementation were checked on 2026-10-26. No code, assets or prose
were copied from the unlicensed [R].

## Sources

- [T] developers.openai.com/api/docs/guides/secure-mcp-tunnels
- [E] developers.openai.com/plugins/build/mcp-events
- [C] developers.openai.com/api/docs/guides/custom-mcp-server
- [P] developers.openai.com/plugins/deploy/connect-chatgpt
- [MS] developers.openai.com/plugins/build/mcp-server
- [D] learn.chatgpt.com/docs/dots
- [DC] learn.chatgpt.com/docs/dots/computers-and-apps
- [TC] github.com/openai/tunnel-client (README; docs/configuration.md, architecture.md, onboarding.md)
- [S] github.com/modelcontextprotocol/experimental-ext-triggers-events/blob/main/docs/design-sketch-proposal.md
- [M] modelcontextprotocol.io/specification/2026-07-28/changelog
- [M2] https://modelcontextprotocol.io/specification/2026-07-28/schema
- [M3] https://modelcontextprotocol.io/specification/2026-07-28/basic/transports/streamable-http
- [PY] https://github.com/modelcontextprotocol/python-sdk/blob/v2.3.0/src/mcp/server/lowlevel/server.py and https://github.com/modelcontextprotocol/python-sdk/blob/v2.3.0/src/mcp-types/mcp_types/_v2026_07_28/__init__.py
- [R] github.com/rsms/esp32-dot
