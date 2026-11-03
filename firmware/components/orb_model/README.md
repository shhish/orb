# Device model

`orb_model` is a C99 singleton store with no LVGL dependency. Initialize it once
before starting link/UI tasks. The application controls its lifetime; the model
never calls into the UI, opens a network connection, or starts a task.

On ESP-IDF the store and answer queue use PSRAM, guarded by a FreeRTOS mutex.
Host builds use pthreads. Both retain at most 24 sessions, 16 cards, four options
per card and 16 queued answers. Full stores reject a new ID without evicting an
unanswered card. Existing IDs can still be updated at capacity. Every input
string must fit its field including a trailing NUL and contain valid UTF-8.
IDs and option labels must be nonempty; option IDs must be unique within a
card. Approval cards require at least two options and a decline option (cancel
role or `deny` ID), so the UI never fabricates a hub answer.

Session IDs are bounded to 48 UTF-8 bytes, card IDs to 64 and option IDs to 32.
Approval `scope` is retained as operation/turn/unspecified; scope is rejected on
other card kinds. The hub-supplied labels and body remain the display contract.

The link task calls `orb_model_upsert_session`, `orb_model_upsert_card`,
`orb_model_remove_session` and `orb_model_cancel_card`. Session updates older
than the stored `updated` timestamp are rejected by the legacy upsert. The wire
mapper uses `orb_model_apply_session` instead, honoring ordered delivery even
when the hub clock moves backwards. Deleting a session does not
silently delete its independently managed cards. Expiry is explicit through
`orb_model_expire` using the synchronized Unix clock; explicit `expires:0` is
tracked separately from an absent expiry; there is no local timer
or timeout that automatically approves a card.

Under the LVGL lock, the UI calls `orb_model_snapshot_if_changed` with its last
revision and a caller-owned snapshot. A false result leaves the snapshot
untouched. Ordinary identical updates do not increment the revision. A reset
always does. Callers should allocate the snapshot in PSRAM on ESP, rather than
placing the full copy on a task stack.

`orb_model_sessions_chunk` stages reset/continuation chunks in PSRAM. No partial
snapshot becomes visible: the last chunk atomically replaces the session list
and clears old cards for replay. Invalid/duplicate/overflowing chunks discard
the stage and preserve the last complete view. `orb_model_sessions_discard`
withdraws a partial snapshot on disconnect. `online` and `synced` let the shell
distinguish an old cached view from current hub state.
`last_cancelled` identifies the latest explicit hub cancellation only; snapshot
replacement and local expiry do not manufacture a hub cancellation marker.

Focus snapshots retain the authoritative hub order. The v1.1 mapper limits
focus to eight sessions; ordered content updates preserve each row's place.
The legacy local/demo store remains compatible with its 24-session capacity.
`sessions.summary` is retained as `idle_count` and three `hidden_by_source`
counts, separately from focus membership. Removing a focus session never
answers or withdraws its independent cards.

Confirmed `prefs` default to all sources enabled and ten recent minutes.
`orb_model_prefs_request` queues one partial patch; `prefs_pending` remains true
until the hub returns complete preferences or an error. Omitted patch fields
are never sent. A timeout retains confirmed preferences and publishes an error;
it does not retry or optimistically toggle a switch. Disconnect also clears
pending edits with an explicit error.

Temperature preferences carry the resolved `temperature` plus
`temperature_owner` (`units.source`). An omitted additive units field retains
this device's last confirmed choice; a cold unknown choice displays Celsius.
`orb_model_temperature()` reads only that small field under the store lock.
`orb_units.h` is the shared display/control boundary: whole-degree readings,
50–90 °F in 1 °F detents or the existing 10–30 °C in 0.5 °C detents, and
Fahrenheit writes rounded to 0.1 °C. Hub persistence owns preferences across
reboots; no second device-side preference file is introduced.

Idle browsing uses `orb_model_idle_open` / `orb_model_idle_close`. Only an open
view owns a separate PSRAM allocation for at most eight sessions, an opaque
80-byte cursor, pending state and error. More replaces the page, so browsing
never accumulates all idle sessions. `cursor:null` is represented by an empty
cursor and disables More. Conflict discards the stale page/cursor and queues a
fresh first page. A generation prevents replies from a closed or previous view
from installing state after the user leaves or reopens. Reset frees the page,
and an allocation failure leaves the view closed without internal-RAM fallback.

`orb_model_answer` accepts exactly one offered option or a text reply only when
`reply` is enabled. It marks the card `answered` only after queuing succeeds.
This local flag means queued, not hub acceptance. The reliable link path uses
`orb_model_peek_answer` to claim, then retry, the same bounded queue record until
`orb_model_complete_answer` receives a terminal response. Claimed records survive
cancellation/expiry/reconnect. Unclaimed records are withdrawn on cancellation
or expiry and publish an unaccepted `cancelled` receipt so the shell does not wait
for a response to unsent work. Snapshot replacement preserves all pending answers,
and card replay
preserves their disabled state. `last_answer` stores the hub's recorded first
answer, acceptance, and transport-delivery status; another controller may have
answered differently. The compatibility `orb_model_take_answer` removes a
record immediately and must not be used for reliable link delivery.

`orb_model_message` queues at most four text messages to a sendable session or
Dot. `orb_model_take_message` hands off once; `orb_model_complete_message`
records acceptance or the failure code in `last_message`, preserving user text
when delivery is uncertain. Stream-bearing confirmations carry their source
connection generation; the link rejects stale confirmations after reconnect.
A timeout never automatically queues another turn.

Voice uses an eight-entry command queue. UI mic buttons call
`orb_model_voice_request(ORB_VOICE_START, target, card)` and STOP on release.
`orb_model_voice_focus` sets the target for BOOT; the default is Dot. CANCEL
supersedes queued commands even at capacity. The audio worker publishes
`voice.phase`, `stream`, `target`, `card`, `text`, `error`, `error_code`, and
a 0–100 `level`. Stable error codes are preserved separately from display copy.
The shell should render a persistent microphone indicator during starting and
recording, recording level, transcription progress, and a transcript confirmation
surface in CONFIRMING with explicit Send and Cancel controls. Send queues
CONFIRM; text is never delivered merely because a result arrived. SENDING is
pending transport acceptance. ERROR displays the generic error and offers retry;
`last_answer.delivered=false` must not be shown as provider execution success.
These are data/control hooks only: this component creates no LVGL objects.

For session voice sends, the worker's authoritative SENDING → IDLE/ERROR
transition also records `last_message` before clearing the transient voice text.
This preserves a receipt even when UI polling coalesces the phase changes.
IDLE means transport acceptance; ERROR records `delivery_unconfirmed`, preserves
the confirmed text, and never queues another send. Receipt stream IDs have no
connection generation and must never be replayed. Card voice confirmations remain
owned by the reliable answer queue and report through `last_answer`.

`orb_model_load_demo` is explicit and replaces the store with synthetic data.
It is never called by initialization. `CONFIG_ORB_MODEL_DEMO` defaults off; the
application must additionally ensure the device is unpaired before loading it.
The simulator intentionally calls it without a live link.

Run host checks:

```sh
make -C firmware/components/orb_model/tests/host
make -C firmware/components/orb_model/tests/host BUILD=build/tsan SAN=-fsanitize=thread
```

The suite checks validation, capacity, revision coalescing, immutable snapshots,
stale session updates, atomic chunk commit/discard/overflow, answer retry ownership,
first-answer results, ID byte limits, bounded message/voice handoff, cancellation,
expiry and concurrent coherent snapshots. The first command runs ASan/UBSan;
the second runs ThreadSanitizer where supported.

## Live streams, usage and send receipts

Weather is a separate read-only capability. `orb_model_set_weather` atomically
publishes a bounded `orb_weather_snapshot_t` with its own revision; readers can
copy only that value through `orb_model_weather_snapshot`, or use `view.weather`
from the main snapshot. Its temperatures are already in `weather.unit` (F/C),
unlike Home's Celsius wire values. Check `available` before reading them;
`label` carries the unset/loading/unavailable copy. Disconnect keeps the last
good value and its observation timestamp, marks it stale, and revokes the
capability until the next hello. Repeated identical replacements do not bump
either revision, and weather uses the startup PSRAM store without allocations.

`orb_weather_icons.h` is the one condition-to-glyph map: sun/moon for clear,
cloud with sun/moon for partly cloudy, and cloud, fog, drizzle, rain, snow and
bolt glyphs for the rest (assets v6). Unknown returns "", so callers show the
label alone.

Each of the eight focus sessions has a latest `orb_agent_stream_t` replacement
and a fixed 24-row PSRAM ring. Step/assistant IDs update their existing row;
new IDs append and evict only the oldest display history. `orb_model_get_stream_log`
returns oldest-first rows with per-row revisions, so cached UI rows can update
independently. Accepted user messages append a user row. No complete transcript
archive is implied. Session removal prunes its ring; snapshots reset sequence
baselines and mark retained progress stale until replay. Disconnect also marks
usage stale. Identical replacements leave the revision unchanged.

`usage` retains at most ten real provider windows, finite percentages and reset/
observation times. `unknown[provider]` remains distinct from a known observation
with zero current windows. No observation is invented as zero use. Validation is
atomic: malformed windows never partly replace the last complete snapshot.

`last_message.status` preserves queued, delivered or failed and its bounded safe
reason. An older accepted-only response uses UNKNOWN delivery status. The voice
worker records the exact receipt before clearing its transient transcript; the
fallback phase transition cannot overwrite it. Neither a transport timeout nor
a failed send automatically resubmits the message. All live mappings, rings and
receipt updates allocate zero payloads after `orb_model_init`.
