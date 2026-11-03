# Sound and system screens — Design 1.0

Branch: `orb/w2-sound-ui`. Work is uncommitted. No board was flashed and no live hub or owner configuration was changed.

Reproduce the fixtures, SDL emulator frames and comparison sheets with `sim/sound-shots.sh`. Each sheet has the exported Design 1.0 PNG on the left and the firmware render on the right. All fourteen sheets were opened for visual inspection; the hint/typography checks were tightened after that inspection found an ellipsis.

| Screen | Built behavior | Default sheet | AX3 + Bold + Reduce Transparency sheet |
| --- | --- | --- | --- |
| SoundSettings | Volume; local spoken-reply preference; Silent; Quiet Hours and editable hours; Always On sound policy. Native scrolling avoids raster caches. | [Default](build/sound-ui/SoundSettings-default-sheet.png) | [AX](build/sound-ui/SoundSettings-ax3-bold-rt-sheet.png) |
| VolumeHUD | Local volume overlay, twist/rim, 1.5-second dismissal; confirmed Mac-volume updates use the same HUD with an explicit Mac label. HUD input cannot change a covered value or activate underlying controls. | [Default](build/sound-ui/VolumeHUD-default-sheet.png) | [AX](build/sound-ui/VolumeHUD-ax3-bold-rt-sheet.png) |
| SpokenReply | Actual streamed caption, scrollable for long replies, physical Stop and existing double-tap/talking cancellation. No invented provider or speech progress. | [Default](build/sound-ui/SpokenReply-default-sheet.png) | [AX](build/sound-ui/SpokenReply-ax3-bold-rt-sheet.png) |
| AlarmRinging | Top-layer interruption above any active screen, 12-hour clock, nine-minute Snooze, Stop, double-tap and face-down cancellation. Prior navigation remains beneath the modal. | [Default](build/sound-ui/AlarmRinging-default-sheet.png) | [AX](build/sound-ui/AlarmRinging-ax3-bold-rt-sheet.png) |
| NowPlaying | Capability-gated Smart Stack card; title/artist; previous/play/pause/next; Mac-volume twist; confirmed state only. Requests do not replay after timeout or reconnect. | [Default](build/sound-ui/NowPlaying-default-sheet.png) | [AX](build/sound-ui/NowPlaying-ax3-bold-rt-sheet.png) |
| Battery | Flagged level/charge state/health/temperature, desk charge limit, explicit unavailable states. No estimated runtime or invented health/temperature classifications. | [Default](build/sound-ui/Battery-default-sheet.png) | [AX](build/sound-ui/Battery-ax3-bold-rt-sheet.png) |
| Onboarding | Four swipe directions, BOOT/PWR teaching, Got it. Physical acknowledgement queues an atomic eight-byte orb_fs record; corrupt or failed storage cannot claim completion. | [Default](build/sound-ui/Onboarding-default-sheet.png) | [AX](build/sound-ui/Onboarding-ax3-bold-rt-sheet.png) |

Additional evidence: `build/sound-ui/{default,ax3-bold-rt}/SoundSettings-scrolled.png`, `Battery-scrolled.png`, `Battery-unknown.png`, NowPlaying idle/unknown/long-title fixtures, and `build/sound-ui/emu/<Screen>-<mode>/frame-000060.png` for every screen. These are isolated fixtures with sample telemetry/media; production views never fill missing data from them.

## Allocations

These are **measured desktop LVGL payloads**, not device heap measurements or proof of owner-budget admission. Shared font assets, allocator overhead and the already-existing shell/model/navigation tree are excluded from per-view figures. Alarm measurement includes its newly admitted glass material keys; the other controls reuse warmed standard keys. No new full-screen framebuffer is allocated by these screens.

| View | Default LVGL bytes | AX LVGL bytes | C state |
| --- | ---: | ---: | --- |
| SoundSettings | 7,295 | 7,283 | 0 additional per-view bytes |
| VolumeHUD | 1,918 | 1,910 | Shared overlay state: 88 host / 56 ESP bytes |
| SpokenReply | 4,036 | 2,763 | Same shared overlay state |
| AlarmRinging | 47,617 | 47,529 | Existing Time view: 736 host / 584 ESP bytes; no digit raster |
| NowPlaying | 5,712 | 5,701 | 0 per-view bytes; bounded media snapshot 328 host bytes, queued request 8 bytes |
| Battery | 4,436 | 5,043 | 0 additional per-view bytes |
| Onboarding | 3,925 | 3,921 | 0 per-view bytes; persistence service uses four atomic booleans and an eight-byte read-only record |

ESP structure sizes were read from the compiled Xtensa `orb_time_view_state_size` and `orb_sound_overlay_size` functions. SoundSettings initially retained about 250 KB of row caches and AX Battery about 190 KB; both were replaced with native safe-chord scrolling.

## Design gate

| Heading | Evidence/result |
| --- | --- |
| Two layers | Plain rows/cards/captions. Glass counts: Sound 0, Volume 0, Speech 1 regular, Alarm 2 with one tint, Media 3 with one tint, Battery 0, Onboarding 1 regular. |
| Canvas | True black; no gradients, halos or glows added. Tinted actions use `orb_glass_set_tint_opacity(71)` and accent ink. |
| Round-first | Every new surface has chord-fit assertions in both sizes. Long captions use a safe central scroll region; AX setting rows scroll within a narrow chord. |
| Sizes | Existing Inter styles, no new fonts. Physical targets meet the 46px minimum; common controls use 72px or larger targets. |
| Colour | Semantic pink/indigo/teal/green and system label ink. Toggle geometry/text and explicit state messages accompany colour. Onboarding preserves the owner-approved Home/Agents identity mapping. |
| Approvals and agents | No synthesized click is used for motion. Synthetic controls cannot send, approve, change the desk charger, control media, stop speech through the physical button path, or persist tutorial completion. |
| Motion/accessibility | Both requested modes render and test. Reduce Transparency uses opaque glass surfaces. AX reflows caption guidance and settings; no shrinking body text to fit. |
| Navigation | BOOT/PWR and native gestures retained. Home detail keeps its native rim ownership. Alarm modal prevents navigation beneath it until cancellation. No added Back chevrons. |
| Visual review | Opened all comparison sheets and the scrolled/unknown cases. Independent agents reviewed the media, alarm and system sheets and audited the shell interruption/input paths. Physical panel review remains the lead's release gate. |

## Remaining differences from the frames

- HIG takes precedence over the reference's 15–19px text. These use existing 21px Footnote2 and its AX variant. Related text is shortened or reflowed so complete essential labels fit.
- SoundSettings shows the requested Quiet Hours and Always On policy instead of the reference's unsupported Voice/Ava value and System sounds row. Always On is reached by scrolling. Spoken-reply opt-out is local and persisted through the existing sound preference worker.
- Volume's 72px number maps to existing 65px Large Title. The speaker/transport/arrow glyphs use Framework7 forms.
- Speech status has no provider identity or per-word timing. The view says Spoken reply and shows the entire real caption at readable contrast; it does not claim Dot identity or dim supposedly unspoken words. The waveform is a static speech symbol, not fabricated audio levels. AX moves the complete stop guidance above Stop.
- Alarm's 120px clock maps to the existing 96px clock. One-time alarm replaces Weekdays because the service supports one-shot alarms. The AM/PM suffix remains actual 12-hour logic.
- Media's 30px title maps to existing 33px Title3; the music-note glyph is absent from the bundled icon pack, so the header uses source text. Unknown duration has no progress or time labels.
- Battery's 64px number maps to 65px. Charging/On battery replaces the unavailable runtime estimate; health and temperature are numeric only when their validity flags permit. The board API can mark health/NTC telemetry unavailable; the view follows those flags.
- Onboarding's text uses supported minimum sizes, with an AX reflow. Storage failures permit the tutorial this boot and retry after reboot.

## Validation

Final verification: `cmake --build sim/build -j4` passed; `ctest --test-dir sim/build -j4` passed **735/735** (29.42 seconds). `scripts/idf.sh build` passed with four jobs; the unsigned app is 0x290000 bytes with 57% of the 6 MiB app slot free. `shellcheck sim/sound-shots.sh` and `git diff --check` passed. The production media delivery callback tests passed 140 ASan/UBSan checks; the Time service tests passed 1,852 checks and 10,000 concurrent snapshots. Sound fixtures passed 114 default / 110 AX checks; system fixtures 114 / 134; media fixtures 154 / 156. Build/test commands use `nice` and `-j4`. The sandbox refuses `setpriority`, so the first sandboxed runs report that priority could not be lowered. ESP-IDF configuration also needed a reviewed sandbox escalation for its CMake-parent process enumeration. The authorized build writes only build artifacts; it does not sign, flash or change live state.
