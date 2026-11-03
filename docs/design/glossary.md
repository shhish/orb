# Orb Copy Glossary and State Vocabulary

> Drafted by Antigravity (Gemini) from the four polish reviews and the firmware strings at trunk b92f394; line citations drift as code changes. Fix lanes apply it with judgment: where a canonical word would lose information (e.g. "Connecting to Mac" vs a bare "Loading…"), keep the specific phrase in sentence case.

This document defines the canonical vocabulary, copy rules, state terminology, action naming, and brand conventions for Orb (firmware UI on a 466×466 round AMOLED display following Apple HIG and watchOS design principles).

Every firmware screen, complication, notification, card, and setting must use exactly one word per meaning.

---

## 1. Rules

### 1.1 Sentence Case
Every label, row title, subtitle, button, capsule, and status message uses **sentence case** (watchOS style).
- **Rule:** Only capitalize the first word and proper nouns.
- **Examples:**
  - `Try again` (not `Try Again` or `TRY AGAIN`)
  - `End round` (not `End Round`)
  - `Play again` (not `Play Again`)
  - `Turn off` (not `Turn Off`)
  - `Back to play` (not `Back to Play`)
  - `Not saved` (not `Not Saved`)
  - `Quick timers` (not `Quick Timers`)
  - `Custom days` (not `Custom Days`)
- **Proper Nouns Retaining Title Case:**
  - System surfaces and core features: `Control Center`, `Always On`, `Smart Stack`, `Apple Home`
  - Assistant providers and system components: `Claude`, `Codex`, `Dot`, `Home`, `Mac`, `Wi-Fi`
  - Hardware buttons: `BOOT`, `PWR`
  - Days of week and months: `Mon`, `Tue`, `Jan`, `Feb`, etc.

### 1.2 US English
Orb uses **US English** across all user-facing text, matching system designations like `Control Center`.
- Use `Color` (never `Colour`).
- Use `Licenses` (never `Licences`).
- Use `Canceled` (never `Cancelled`).
- Use `Center` (never `Centre`).

### 1.3 Typographic Ellipsis (U+2026)
Always use the single Unicode character `…` (`\xE2\x80\xA6` or U+2026) for truncation, pending actions, and progress indicators.
- **Never** use three ASCII periods (`...`) or four dots (`....`).
- When truncating dynamic text at line ends, truncate on whole glyph boundaries and append `…`.
- Trim any whitespace immediately preceding the ellipsis (e.g. `Living Room…`, not `Living Room …`).

### 1.4 Curly Quotes and Apostrophes
Always use typographic curly quotes and apostrophes in copy strings:
- Apostrophe / Right single quote: `’` (U+2019, `\xE2\x80\x99`), never straight ASCII `'`.
- Left single quote: `‘` (U+2018, `\xE2\x80\x98`).
- Left double quote: `“` (U+201C, `\xE2\x80\x9C`), never straight ASCII `"`.
- Right double quote: `”` (U+201D, `\xE2\x80\x9D`), never straight ASCII `"`.
- Examples: `Couldn’t load`, `Didn’t catch that`, `“Reading file…”`.

### 1.5 Punctuation and Trailing Periods
- **No final period** on labels, button titles, status tags, row subtitles, or one-line captions.
  - Correct: `Start a session on your Mac`
  - Incorrect: `Start a session on your Mac.`
  - Correct: `New updates appear here`
  - Incorrect: `New updates will appear here.`
- Trailing periods are reserved exclusively for multi-sentence descriptions or extended prose blocks in approval review and notification detail sheets.

### 1.6 Digits with Units
Digits and their accompanying units must always be separated by a space (or non-breaking space `\xC2\xA0`), using standard watchOS compact unit symbols:
- Minutes: `13 min` (never `13m`, `13 min.`, or spelled-out `13 minutes` in tabular or glanceable slots).
- Seconds: `10 s` (never `10s` or `10 sec`).
- Hours: `5 hr` (never `5h` or spelled-out `5 hours` in tabular rows and status lines).
- Days: `2 d` (never `2d` or `2 days`).
- Relative timestamps: `now`, `10 s ago`, `5 min ago`, `2 hr ago` (never mixed forms like `1 s` vs `1 s ago`).

### 1.7 Temperature Formatter
Temperature values must follow a single consistent format across all faces, complications, climate controls, and settings:
- **Whole degrees only:** Always round to the nearest whole integer (`22°`, `72°`). Never show decimals (`23.0°C`).
- **Degree symbol without space:** `22°`.
- **Unit shown once or omitted when implicit:**
  - In compact complications and glance cards: `72°` or `22°` (implicit from the user’s Temperature setting).
  - On detail screens, headers, or where disambiguation is required: `72°F` or `22°C` (unit immediately follows degree symbol).
  - Range format: `68°–76°F` (en-dash, unit once at end).

---

## 2. State Vocabulary

Orb uses one canonical string per system and hardware condition. The table below lists every canonical state, its domain, replaced variants, and exact citations where variants were found in firmware sources.

| Canonical String | Domain | Replaces (Variants Found) | Citations (`file:line`) | Usage & Semantics |
|---|---|---|---|---|
| **`Mac offline`** | System / Link | `"Offline"`<br>`"Mac offline. Connect your Mac to refresh sessions."`<br>`"Waiting for your Mac."`<br>`"—"` (on Activity/Meridian) | [orb_workspace.c:675](../../firmware/components/orb_shell/orb_workspace.c#L675)<br>[orb_workspace.c:1488](../../firmware/components/orb_shell/orb_workspace.c#L1488)<br>[orb_agents_secondary.inc:318](../../firmware/components/orb_shell/orb_agents_secondary.inc#L318)<br>[orb_agents_secondary.inc:349](../../firmware/components/orb_shell/orb_agents_secondary.inc#L349)<br>[orb_faces.c:651](../../firmware/components/orb_faces/orb_faces.c#L651) | Hub Mac is unreachable over WebSocket link. Controls that depend on Mac are disabled and dimmed. |
| **`Wi-Fi offline`** | System / Net | `"Wi-Fi failed"`<br>`"Off"`<br>`"Network name unav..."` | [orb_system_copy.c:20](../../firmware/components/orb_shell/orb_system_copy.c#L20)<br>[orb_workspace.c:674](../../firmware/components/orb_shell/orb_workspace.c#L674) | Wi-Fi interface is disconnected or local network is unreachable. |
| **`No response`** | Home / Accessory | `"No Response"`<br>`"Accessory unavailable"`<br>`"State unavailable"` | [orb_home_view.c:514](../../firmware/components/orb_home/orb_home_view.c#L514)<br>[orb_home_view.c:555](../../firmware/components/orb_home/orb_home_view.c#L555)<br>[orb_home_view.c:1026](../../firmware/components/orb_home/orb_home_view.c#L1026)<br>[orb_home_view.c:1039](../../firmware/components/orb_home/orb_home_view.c#L1039)<br>[orb_home_detail.c:604](../../firmware/components/orb_home/orb_home_detail.c#L604)<br>[orb_home_detail.c:773](../../firmware/components/orb_home/orb_home_detail.c#L773)<br>[orb_workspace.c:2765](../../firmware/components/orb_shell/orb_workspace.c#L2765) | A HomeKit accessory does not reply to HomeRelay/Hub queries. Sentence case: lowercase "response". |
| **`Home unavailable`** | Home / Service | `"Home is offline"`<br>`"Waiting for Home"`<br>`"Awaiting Home"`<br>`"Offline"`<br>`"Unavailable"` | [orb_home_detail.c:311](../../firmware/components/orb_home/orb_home_detail.c#L311)<br>[orb_home_detail.c:770](../../firmware/components/orb_home/orb_home_detail.c#L770)<br>[orb_home_detail.c:771](../../firmware/components/orb_home/orb_home_detail.c#L771)<br>[orb_home_detail.c:772](../../firmware/components/orb_home/orb_home_detail.c#L772)<br>[orb_home_detail.c:825](../../firmware/components/orb_home/orb_home_detail.c#L825)<br>[orb_home_view.c:359](../../firmware/components/orb_home/orb_home_view.c#L359)<br>[orb_home_view.c:645](../../firmware/components/orb_home/orb_home_view.c#L645)<br>[orb_home_view.c:870](../../firmware/components/orb_home/orb_home_view.c#L870)<br>[orb_workspace.c:2749](../../firmware/components/orb_shell/orb_workspace.c#L2749) | Home relay daemon or Apple Home daemon is unreachable or unconfigured. Replaces all fragmented variants. |
| **`Loading…`** | General / Async | `"Connecting to Mac"`<br>`"Loading page"`<br>`"Loading idle sessions"`<br>`"Checking Dot"`<br>`"Connecting"`<br>`"Searching"`<br>`"Syncing with your Mac…"` | [orb_agents_secondary.inc:349](../../firmware/components/orb_shell/orb_agents_secondary.inc#L349)<br>[orb_workspace.c:1487](../../firmware/components/orb_shell/orb_workspace.c#L1487)<br>[orb_agents_primary.inc:416](../../firmware/components/orb_shell/orb_agents_primary.inc#L416)<br>[orb_agents_secondary.inc:320](../../firmware/components/orb_shell/orb_agents_secondary.inc#L320) | Active background data fetch or network query in progress. Always uses typographic ellipsis. |
| **`Updating…`** | Accessories / Data | `"Changing…"`<br>`"Turning off…"`<br>`"Locking…"`<br>`"Unlocking…"` | [orb_home_view.c:515](../../firmware/components/orb_home/orb_home_view.c#L515)<br>[orb_home_view.c:556](../../firmware/components/orb_home/orb_home_view.c#L556)<br>[orb_home_detail.c:776](../../firmware/components/orb_home/orb_home_detail.c#L776)<br>[orb_home_detail.c:821](../../firmware/components/orb_home/orb_home_detail.c#L821)<br>[orb_workspace.c:2765](../../firmware/components/orb_shell/orb_workspace.c#L2765) | Hardware accessory state change pending hub confirmation. |
| **`Saving…`** | Settings / Records | `"Saving"`<br>`"Saving preferences…"` | [orb_system_screens.inc:73](../../firmware/components/orb_shell/orb_system_screens.inc#L73)<br>[orb_app_settings/sound_screen.c:284](../../firmware/components/orb_app_settings/sound_screen.c#L284)<br>[orb_app_settings/agent_settings_screen.c:146](../../firmware/components/orb_app_settings/agent_settings_screen.c#L146)<br>[orb_app_settings/units_screen.c:120](../../firmware/components/orb_app_settings/units_screen.c#L120) | Asynchronous persistence write to flash or hub preference storage. |
| **`Saved`** | General / Confirmation | `"Reminder added"`<br>`"Saved."`<br>`"Done"` (as status) | [orb_agents_secondary.inc:234](../../firmware/components/orb_shell/orb_agents_secondary.inc#L234) | Asynchronous save successfully confirmed by local storage or hub. |
| **`Not saved`** | General / Persistence | `"Score not saved"`<br>`"Save unavailable"`<br>`"Could not save"`<br>`"On · Not saved"` | [orb_system_screens.inc:72](../../firmware/components/orb_shell/orb_system_screens.inc#L72)<br>[orb_game_view.c:831](../../firmware/components/orb_games/orb_game_view.c#L831)<br>[orb_game_view.c:1390](../../firmware/components/orb_games/orb_game_view.c#L1390)<br>[orb_app_settings/sound_screen.c:358](../../firmware/components/orb_app_settings/sound_screen.c#L358)<br>[orb_app_settings/battery_screen.c:113](../../firmware/components/orb_app_settings/battery_screen.c#L113)<br>[orb_faces.c:988](../../firmware/components/orb_faces/orb_faces.c#L988)<br>[orb_workspace.c:2170](../../firmware/components/orb_shell/orb_workspace.c#L2170) | Persistence write failed (NVS or hub I/O). Canonical status label across games, settings, and faces. |
| **`Last known · <time>`** | Cache / Outage | `"Last known"`<br>`"Last known: "`<br>`"Last known · stale"`<br>`"stale"`<br>`"Weather stale"`<br>`"Updated 2h ago"` | [orb_home_view.c:513](../../firmware/components/orb_home/orb_home_view.c#L513)<br>[orb_home_detail.c:826](../../firmware/components/orb_home/orb_home_detail.c#L826)<br>[orb_agents_primary.inc:234](../../firmware/components/orb_shell/orb_agents_primary.inc#L234)<br>[orb_agents_primary.inc:452](../../firmware/components/orb_shell/orb_agents_primary.inc#L452)<br>[orb_agents_secondary.inc:379](../../firmware/components/orb_shell/orb_agents_secondary.inc#L379)<br>[orb_faces.c:464](../../firmware/components/orb_faces/orb_faces.c#L464)<br>[orb_usage_copy.c:56](../../firmware/components/orb_shell/orb_usage_copy.c#L56) | Value is retained from cache during offline or sync outage. Format: `"Last known · 2:14 PM"` if timestamp is known; `"Last known"` if timestamp is unknown. Stale value is dimmed to secondary color. |
| **`—`** (em-dash) | Missing Data | `"—"` (hyphen)<br>`"Unknown"`<br>`"0 working"`<br>`"resets —"`<br>`"State unavailable"` | [orb_faces.c:187](../../firmware/components/orb_faces/orb_faces.c#L187)<br>[orb_faces.c:205](../../firmware/components/orb_faces/orb_faces.c#L205)<br>[orb_faces.c:214](../../firmware/components/orb_faces/orb_faces.c#L214)<br>[orb_usage_copy.c:13](../../firmware/components/orb_shell/orb_usage_copy.c#L13)<br>[orb_usage_copy.c:62](../../firmware/components/orb_shell/orb_usage_copy.c#L62)<br>[orb_app_settings/battery_screen.c:97](../../firmware/components/orb_app_settings/battery_screen.c#L97) | Numerical or telemetric value not yet received or unconfigured. Never use a bare zero or hyphen. |
| **`Time not set`** | Clock / System | `"Clock offline"`<br>`"Waiting for time"`<br>`"Time not set"` | [orb_time_view.c:352](../../firmware/components/orb_time_app/orb_time_view.c#L352)<br>[orb_time_view.c:743](../../firmware/components/orb_time_app/orb_time_view.c#L743)<br>[orb_game_view.c:141](../../firmware/components/orb_games/orb_game_view.c#L141)<br>[orb_faces.c:307](../../firmware/components/orb_faces/orb_faces.c#L307) | Real-time clock has not synchronized with NTP or Mac hub. Replaces `"Clock offline"` and `"Waiting for time"`. |
| **`Motion unavailable`** | Sensors / IMU | `"Touch controls enabled"` | [orb_game_view.c:140](../../firmware/components/orb_games/orb_game_view.c#L140) | IMU hardware missing or failing calibration. Used in all games (Beads, Marble, Snake, Orbit, Pop). |
| **`Touch controls`** | Input Mode | `"Touch"` (with green play icon) | [orb_game_view.c:852](../../firmware/components/orb_games/orb_game_view.c#L852) | Row toggle in game options to steer by touch instead of tilt. Uses hand icon and game accent. |
| **`Read-only`** | Permissions | `"Read only"` (unhyphenated) | [orb_kit.h:108](../../firmware/components/orb_ui/include/orb_kit.h#L108)<br>[orb_agents_copy.c:17](../../firmware/components/orb_shell/orb_agents_copy.c#L17) | Session or card does not accept inbound voice or text answers. Hyphenated compound adjective. |

### 2.1 Delivery Results Model
Every outgoing message, reminder addition, approval response, and question answer maps to exactly one of four delivery states:

1. **`Sent`**: Transmission dispatched over network link to the Mac hub.
2. **`Delivered`**: Delivery positively acknowledged by the destination session or agent process. Replaces `"Recorded"`.
3. **`Not sent`**: Message failed to dispatch or was rejected by the hub. Replaces `"Failed"` in receipt headers.
4. **`Unknown`**: Transmission dispatched, but confirmation timed out without error. Replaces `"Delivery unknown"`.

*Citations:* [orb_workspace.c:1525](../../firmware/components/orb_shell/orb_workspace.c#L1525), [orb_agents_secondary.inc:235-238](../../firmware/components/orb_shell/orb_agents_secondary.inc#L235-L238), [orb_agents_secondary.inc:515](../../firmware/components/orb_shell/orb_agents_secondary.inc#L515).

### 2.2 Error Pattern: `Couldn't <verb> <object>`
All user-facing errors must describe the failure plainly in the active voice using the contraction **`Couldn’t`** (with typographic apostrophe), followed by a clear action button labeled **`Try again`**:
- `Couldn’t save reminder` + `[Try again]` (replaces `Retry save` / `Not saved`)
- `Couldn’t snooze` (replaces `Clock offline` on reminder snooze failure)
- `Couldn’t load sessions` + `[Try again]` (replaces `Could not load` / `Load failed`)
- `Couldn’t connect to Mac` + `[Try again]`
- `Couldn’t reach accessory` + `[Try again]` (replaces `Change failed`)
- `Couldn’t run scene` + `[Try again]` (replaces `Scene failed`)

*Citations:* [orb_time_view.c:352](../../firmware/components/orb_time_app/orb_time_view.c#L352), [orb_time_view.c:978](../../firmware/components/orb_time_app/orb_time_view.c#L978), [orb_workspace.c:1487](../../firmware/components/orb_shell/orb_workspace.c#L1487), [orb_home_detail.c:775](../../firmware/components/orb_home/orb_home_detail.c#L775).

### 2.3 Empty States Pattern: Title + One-Line Hint
Empty states consist of:
1. **Title:** Centered, bold/headline, naming what is absent (e.g. `No sessions`).
2. **One-Line Hint:** Centered secondary caption below, explaining how to populate the view. No trailing period.

Canonical empty pairs:
- Sessions: `No sessions` / `Start a session on your Mac`
- Notifications: `No notifications` / `New updates appear here`
- Smart Stack: `All caught up` / `Useful updates appear here`
- Reminders: `No reminders` / `Hold BOOT to talk`
- Scenes: `No scenes` / `Add scenes in Apple Home`
- Accessories: `No accessories` / `Add accessories in Apple Home`

---

## 3. Actions

Every user action has a single canonical verb and role.

### 3.1 Leaving a Screen: `Done` vs `Exit`
Orb distinguishes between **completing/acknowledging** a finished activity and **bailing out** of an active or paused state:
- **`Done`** is used on:
  - Game over / round results screens (e.g. Pop, Orbit, Snake, Marble scores).
  - Receipt and confirmation sheets (e.g. reminder added, approval completed).
  - Modal sheets where settings or values were applied.
  - *Rationale:* `Done` signals to the user that the task or round has reached completion and dismissing the screen retains the recorded result.
- **`Exit`** is used on:
  - Game pause sheets (e.g. Beads, Marble, Orbit, Pop, Snake pause menu).
  - System power/pause sheet (`Exit` replaces `Exit app` in [orb_shell.c:305](../../firmware/components/orb_shell/orb_shell.c#L305)).
  - Intermediate wizard or setup cancellation.
  - *Rationale:* `Exit` explicitly signals that an in-progress activity is being interrupted or abandoned prematurely.

### 3.2 End Round
- **`End round`** (sentence case, replaces `End Round` in [orb_game_view.c:841](../../firmware/components/orb_games/orb_game_view.c#L841)).
- Ends current gameplay and advances to the results screen without immediately closing the app.

### 3.3 Try Again
- **`Try again`** (sentence case, replaces `Retry`, `Retry save`, `Try Again` in [orb_time_view.c:978](../../firmware/components/orb_time_app/orb_time_view.c#L978), [orb_time_view.c:1116](../../firmware/components/orb_time_app/orb_time_view.c#L1116), [orb_workspace.c:1487](../../firmware/components/orb_shell/orb_workspace.c#L1487)).

### 3.4 Dismiss vs Deny
- **`Deny`** is strictly reserved for security and permission approval cards (e.g. bash commands, MCP tool authorizations, write operations). It signals an active negative decision to the requesting process.
- **`Dismiss`** is used everywhere else: timers, alarms, informational cards, incoming doorbell alerts, and error toasts.

### 3.5 Voice Phrasing
Voice input is triggered by holding the hardware BOOT button. All cues and instructions use one phrasing:
- **`Hold BOOT to talk`** is the canonical instruction across the shell, voice screens, reminders, and session views.
  - Replaces `Hold to speak` ([orb_agents_secondary.inc:260](../../firmware/components/orb_shell/orb_agents_secondary.inc#L260)).
  - Replaces `or hold BOOT` ([orb_agents_secondary.inc:265](../../firmware/components/orb_shell/orb_agents_secondary.inc#L265)).
  - Replaces `Hold BOOT to reply` ([orb_agents_secondary.inc:587](../../firmware/components/orb_shell/orb_agents_secondary.inc#L587)).
  - Replaces `Hold BOOT to add` ([orb_time_view.c:959](../../firmware/components/orb_time_app/orb_time_view.c#L959)).
  - Replaces `Talk` in onboarding row subtitles ([orb_system_screens.inc:60](../../firmware/components/orb_shell/orb_system_screens.inc#L60)).
- **`Release BOOT to review`** is the release instruction during hardware recording ([orb_agents_secondary.inc:199](../../firmware/components/orb_shell/orb_agents_secondary.inc#L199)).
- **`Reply by voice`** is used for full-chord row actions in notifications ([orb_workspace.c:1855](../../firmware/components/orb_shell/orb_workspace.c#L1855)); compact button capsules use **`Reply`** ([orb_workspace.c:1662](../../firmware/components/orb_shell/orb_workspace.c#L1662)).

---

## 4. Names

### 4.1 Sessions vs Agents
- **Decision:** Use **`Sessions`** as the user-facing destination, screen title, list header, and Smart Stack entry.
- **Justification:** Following Apple HIG and watchOS conventions, apps and list destinations are named with plural nouns that describe the user’s items (`Sessions`, `Alarms`, `Reminders`). "Agents" refers exclusively to the backend runtime engine category. Users manage and interact with *Sessions*.
- Replaces `"Agents"` as a navigation title ([orb_agents_primary.inc:296](../../firmware/components/orb_shell/orb_agents_primary.inc#L296)).

### 4.2 Assistant Providers
- **`Claude`**: Always "Claude". Never "Claude Code" in user-facing UI ([orb_app_settings/agent_settings_screen.c:153](../../firmware/components/orb_app_settings/agent_settings_screen.c#L153)).
- **`Codex`**: Always "Codex".
- **`Dot`**: Always "Dot".

### 4.3 Core Surfaces and Apps
- **`Control Center`**: The top-level swipe-down utility panel.
- **`Smart Stack`**: The scrollable widget carousel accessed from the bottom edge.
- **`Always On`**: The low-power standby face mode (not "Always-on" or "AOD").
- **`Home`**: The smart home application (not "Demo Home" or "HOME").
- **`Sound`**: The audio settings page (replaces "Sounds" in [settings_screen.c:34](../../firmware/components/orb_app_settings/settings_screen.c#L34) to match [sound_screen.c:278](../../firmware/components/orb_app_settings/sound_screen.c#L278)).
- **`Licenses`**: Legal notices in Settings > About (replaces "Licences" in [about_screen.c:36](../../firmware/components/orb_app_settings/about_screen.c#L36)).
- **`Mac`**: Connection destination and hub reference (replaces "Hub preference" in [units_screen.c:122](../../firmware/components/orb_app_settings/units_screen.c#L122)).

---

## 5. Replacement List

This exhaustive list specifies the exact replacements for fix agents, referencing real file lines verified in firmware C sources:

### `firmware/components/orb_app_settings/`
- [about_screen.c:36](../../firmware/components/orb_app_settings/about_screen.c#L36) — `"Licences"` → `"Licenses"`
- [about_screen.c:97](../../firmware/components/orb_app_settings/about_screen.c#L97) — `"Licences"` → `"Licenses"`
- [settings_screen.c:34](../../firmware/components/orb_app_settings/settings_screen.c#L34) — `"Sounds"` → `"Sound"`
- [agent_settings_screen.c:153](../../firmware/components/orb_app_settings/agent_settings_screen.c#L153) — `"Claude Code"` → `"Claude"`
- [agent_settings_screen.c:207](../../firmware/components/orb_app_settings/agent_settings_screen.c#L207) — `"%u minutes"` → `"%u min"`
- [units_screen.c:122](../../firmware/components/orb_app_settings/units_screen.c#L122) — `"Hub preference"` → `"Mac"`
- [battery_screen.c:114](../../firmware/components/orb_app_settings/battery_screen.c#L114) — `"80% maximum"` → `"80% limit"`

### `firmware/components/orb_home/`
- [orb_home_detail.c:603](../../firmware/components/orb_home/orb_home_detail.c#L603) — `"HOME"` → `"Home"`
- [orb_home_detail.c:620](../../firmware/components/orb_home/orb_home_detail.c#L620) — `"Colour"` → `"Color"`
- [orb_home_detail.c:639](../../firmware/components/orb_home/orb_home_detail.c#L639) — `"THERMOSTAT MODE"` → `"Thermostat Mode"`
- [orb_home_detail.c:658](../../firmware/components/orb_home/orb_home_detail.c#L658) — `"SCENES"` → `"Scenes"`
- [orb_home_detail.c:757](../../firmware/components/orb_home/orb_home_detail.c#L757) — `"Colour"` → `"Color"`
- [orb_home_detail.c:770](../../firmware/components/orb_home/orb_home_detail.c#L770) — `"Home is offline"` → `"Home unavailable"`
- [orb_home_detail.c:771](../../firmware/components/orb_home/orb_home_detail.c#L771) — `"Waiting for Home"` → `"Loading…"`
- [orb_home_detail.c:773](../../firmware/components/orb_home/orb_home_detail.c#L773) — `"No Response"` → `"No response"`
- [orb_home_detail.c:775](../../firmware/components/orb_home/orb_home_detail.c#L775) — `"Change failed"` → `"Couldn’t reach accessory"`
- [orb_home_detail.c:825](../../firmware/components/orb_home/orb_home_detail.c#L825) — `"Awaiting Home"` → `"Updating…"`
- [orb_home_detail.c:826](../../firmware/components/orb_home/orb_home_detail.c#L826) — `"Last known: "` → `"Last known · "`
- [orb_home_detail.c:842](../../firmware/components/orb_home/orb_home_detail.c#L842) — `"Low Battery"` → `"Low battery"`
- [orb_home_view.c:359](../../firmware/components/orb_home/orb_home_view.c#L359) — `"Home unavailable"` → `"Home unavailable"` (verified canonical)
- [orb_home_view.c:362](../../firmware/components/orb_home/orb_home_view.c#L362) — `"Scene failed"` → `"Couldn’t run scene"`
- [orb_home_view.c:514](../../firmware/components/orb_home/orb_home_view.c#L514) — `"No Response"` → `"No response"`
- [orb_home_view.c:555](../../firmware/components/orb_home/orb_home_view.c#L555) — `"No Response"` → `"No response"`
- [orb_home_view.c:645](../../firmware/components/orb_home/orb_home_view.c#L645) — `!v->model->online ? "Offline" : !v->model->synced ? "Waiting" : "Unavailable"` → `"Home unavailable"`
- [orb_home_view.c:660](../../firmware/components/orb_home/orb_home_view.c#L660) — `"Shared by HomeRelay."` → `"Devices from your Mac"`
- [orb_home_view.c:867](../../firmware/components/orb_home/orb_home_view.c#L867) — `"Demo Home"` → `"Home"`
- [orb_home_view.c:870](../../firmware/components/orb_home/orb_home_view.c#L870) — `!v->model->online ? "Offline" : !v->model->synced ? "Waiting for Home" : ...` → `"Home unavailable"`
- [orb_home_view.c:874](../../firmware/components/orb_home/orb_home_view.c#L874) — `home_ready(v->model) ? "Shared by HomeRelay." : "Connect your Mac to load Home."` → `home_ready(v->model) ? "Devices from your Mac" : "Connect your Mac to load Home"`
- [orb_home_view.c:1026](../../firmware/components/orb_home/orb_home_view.c#L1026) — `"No Response"` → `"No response"`
- [orb_home_view.c:1039](../../firmware/components/orb_home/orb_home_view.c#L1039) — `"No Response"` → `"No response"`

### `firmware/components/orb_games/`
- [orb_game_view.c:140](../../firmware/components/orb_games/orb_game_view.c#L140) — `"Touch controls enabled"` → `"Motion unavailable"`
- [orb_game_view.c:831](../../firmware/components/orb_games/orb_game_view.c#L831) — `"Score not saved"` → `"Not saved"`
- [orb_game_view.c:840](../../firmware/components/orb_games/orb_game_view.c#L840) — `"Back to play"` → `"Resume"`
- [orb_game_view.c:841](../../firmware/components/orb_games/orb_game_view.c#L841) — `"End Round"` → `"End round"`
- [orb_game_view.c:852](../../firmware/components/orb_games/orb_game_view.c#L852) — `.title = "Touch"` → `.title = "Touch controls"`
- [orb_game_view.c:1324](../../firmware/components/orb_games/orb_game_view.c#L1324) — `" · COMPLETE"` → `" · Complete"`; `" · ROUND OVER"` → `" · Round over"`
- [orb_game_view.c:1325](../../firmware/components/orb_games/orb_game_view.c#L1325) — `" · PAUSED"` → `" · Paused"`; `" · GARDEN"` → `" · Garden"`; `" · PLANTED"` → `" · Planted"`
- [orb_game_view.c:1331](../../firmware/components/orb_games/orb_game_view.c#L1331) — `"Back to play"` → `"Resume"`
- [orb_game_view.c:1374](../../firmware/components/orb_games/orb_game_view.c#L1374) — `"Score not saved"` → `"Not saved"`
- [orb_game_view.c:1390](../../firmware/components/orb_games/orb_game_view.c#L1390) — `marble ? "Save unavailable" : "Score not saved"` → `"Not saved"`

### `firmware/components/orb_time_app/`
- [orb_time_view.c:352](../../firmware/components/orb_time_app/orb_time_view.c#L352) — `"Clock offline"` → `"Couldn’t snooze"`
- [orb_time_view.c:743](../../firmware/components/orb_time_app/orb_time_view.c#L743) — `"Waiting for time"` → `"Time not set"`
- [orb_time_view.c:959](../../firmware/components/orb_time_app/orb_time_view.c#L959) — `"Hold BOOT to add"` → `"Hold BOOT to talk"`
- [orb_time_view.c:978](../../firmware/components/orb_time_app/orb_time_view.c#L978) — `"Retry save"` → `"Try again"`
- [orb_time_view.c:1116](../../firmware/components/orb_time_app/orb_time_view.c#L1116) — `"Retry save"` → `"Try again"`

### `firmware/components/orb_shell/`
- [orb_shell.c:305](../../firmware/components/orb_shell/orb_shell.c#L305) — `"Exit app"` → `"Exit"`
- [orb_shell.c:320](../../firmware/components/orb_shell/orb_shell.c#L320) — `"Keep holding PWR for 10 s\nto switch off"` → `"Hold PWR 10 s to switch off"`
- [orb_nowplaying.inc:21](../../firmware/components/orb_shell/orb_nowplaying.inc#L21) — `"Control unavailable"` → `"Playback unavailable"`
- [orb_system_screens.inc:60](../../firmware/components/orb_shell/orb_system_screens.inc#L60) — `{"Talk", "Hold BOOT", ORB_ICON_MIC}` → `{"Voice", "Hold BOOT", ORB_ICON_MIC}`
- [orb_agents_primary.inc:234](../../firmware/components/orb_shell/orb_agents_primary.inc#L234) — `"Last known · stale"` → `"Last known"`
- [orb_agents_primary.inc:296](../../firmware/components/orb_shell/orb_agents_primary.inc#L296) — `"Agents"` → `"Sessions"`
- [orb_agents_primary.inc:298](../../firmware/components/orb_shell/orb_agents_primary.inc#L298) — `"Start a session on your Mac."` → `"Start a session on your Mac"`
- [orb_agents_primary.inc:452](../../firmware/components/orb_shell/orb_agents_primary.inc#L452) — `"Last known · stale"` → `"Last known"`
- [orb_agents_secondary.inc:234](../../firmware/components/orb_shell/orb_agents_secondary.inc#L234) — `"Reminder added"` → `"Saved"`
- [orb_agents_secondary.inc:235](../../firmware/components/orb_shell/orb_agents_secondary.inc#L235) — `"Delivery unknown"` → `"Unknown"`
- [orb_agents_secondary.inc:236](../../firmware/components/orb_shell/orb_agents_secondary.inc#L236) — `"Failed"` → `"Not sent"`
- [orb_agents_secondary.inc:260](../../firmware/components/orb_shell/orb_agents_secondary.inc#L260) — `"Hold to speak"` → `"Hold BOOT to talk"`
- [orb_agents_secondary.inc:265](../../firmware/components/orb_shell/orb_agents_secondary.inc#L265) — `"or hold BOOT"` → `""`
- [orb_agents_secondary.inc:349](../../firmware/components/orb_shell/orb_agents_secondary.inc#L349) — `"Connecting to Mac"` → `"Loading…"`
- [orb_agents_secondary.inc:379](../../firmware/components/orb_shell/orb_agents_secondary.inc#L379) — `"Last known · stale"` → `"Last known"`
- [orb_agents_secondary.inc:515](../../firmware/components/orb_shell/orb_agents_secondary.inc#L515) — `"Recorded"` → `"Delivered"`
- [orb_agents_secondary.inc:587](../../firmware/components/orb_shell/orb_agents_secondary.inc#L587) — `"Hold BOOT to reply"` → `"Hold BOOT to talk"`
- [orb_agents_copy.c:17](../../firmware/components/orb_shell/orb_agents_copy.c#L17) — `"This session is read only"` → `"This session is read-only"`
- [orb_agents_copy.c:18](../../firmware/components/orb_shell/orb_agents_copy.c#L18) — `"Hold BOOT to try again"` → `"Hold BOOT to talk"`
- [orb_agents_copy.c:24](../../firmware/components/orb_shell/orb_agents_copy.c#L24) — `"Orb couldn't save the reminder. Try again."` → `"Orb couldn’t save the reminder. Try again"`
- [orb_usage_copy.c:48](../../firmware/components/orb_shell/orb_usage_copy.c#L48) — `ax3 ? "5h · resets %s" : "5 hours · resets %s"` → `"5 hr · resets %s"`
- [orb_usage_copy.c:56](../../firmware/components/orb_shell/orb_usage_copy.c#L56) — `" · stale"` → `" · Last known"`
- [orb_usage_copy.c:58](../../firmware/components/orb_shell/orb_usage_copy.c#L58) — `" · stale"` → `" · Last known"`
- [orb_usage_copy.c:61](../../firmware/components/orb_shell/orb_usage_copy.c#L61) — `" · stale"` → `" · Last known"`
- [orb_usage_copy.c:62](../../firmware/components/orb_shell/orb_usage_copy.c#L62) — `" · stale"` → `" · Last known"`
- [orb_workspace.c:1487](../../firmware/components/orb_shell/orb_workspace.c#L1487) — `"Could not load"` → `"Couldn’t load"`
- [orb_workspace.c:1525](../../firmware/components/orb_shell/orb_workspace.c#L1525) — `"Recorded"` → `"Delivered"`
- [orb_workspace.c:1534](../../firmware/components/orb_shell/orb_workspace.c#L1534) — `"Your Mac didn't take this answer. Check the session."` → `"Your Mac didn’t take this answer. Check the session."`
- [orb_workspace.c:2170](../../firmware/components/orb_shell/orb_workspace.c#L2170) — `"Could not save. Try again."` → `"Couldn’t save. Try again"`
- [orb_workspace.c:2749](../../firmware/components/orb_shell/orb_workspace.c#L2749) — `!w->home->online ? "Offline" : !w->home->synced ? "Waiting for Home" : !w->home->available ? "Unavailable" : "Home"` → `"Home unavailable"`
- [orb_workspace.c:2765](../../firmware/components/orb_shell/orb_workspace.c#L2765) — `"No Response"` → `"No response"`; `"Changing…"` → `"Updating…"`

### `firmware/components/orb_faces/`
- [orb_faces.c:247](../../firmware/components/orb_faces/orb_faces.c#L247) — `"%um"` → `"%u min"`
- [orb_faces.c:248](../../firmware/components/orb_faces/orb_faces.c#L248) — `"%uh"` → `"%u hr"`
- [orb_faces.c:464](../../firmware/components/orb_faces/orb_faces.c#L464) — `"Weather stale"` → `"Last known"`
- [orb_faces.c:880](../../firmware/components/orb_faces/orb_faces.c#L880) — `"Colour"` → `"Color"`
- [orb_faces.c:988](../../firmware/components/orb_faces/orb_faces.c#L988) — `"Not saved"` (replaces color name) → keep color name, show `"Not saved"` in status caption

### `firmware/components/orb_ui/`
- [orb_kit.h:108](../../firmware/components/orb_ui/include/orb_kit.h#L108) — `"#define ORB_KIT_READ_ONLY_CAPTION \"Read only\""` → `"Read-only"`
