# Round, small, useful, playful: interaction research for Orb

Research checked **2026-10-27**. Scope: interaction research and proposals, not a firmware change or a claim of device validation.

**Recommendation:** make Orb feel like a small instrument with a readable centre, an explorable circumference, and a few physical rituals. Use the centre for the thing that needs understanding; use the rim for position, continuity and continuous adjustment; use pick-up and the buttons for clear intent. A circle should change how the device works, not merely clip a rectangular dashboard.

Reading path: [Apple Watch](#1-apple-watch-borrow-hierarchy-and-continuity-then-reshape-them) · [Round watches](#2-round-watch-systems) · [Instruments and companions](#3-instruments-tiny-companions-and-ambient-interfaces) · [IoT and hobby systems](#4-circular-iot-smart-rings-and-hobby-systems) · [HCI evidence](#5-what-academic-hci-actually-supports) · [Primitives](#6-interaction-primitives-for-orb) · [Ten concepts](#7-ten-concrete-orb-concepts) · [Anti-patterns and evaluation](#8-anti-patterns-and-a-practical-evaluation-order).

## Reading this research

The reference sections distinguish **documented behaviour** from **analysis and Orb translation**. “Why it works” is a design interpretation unless an experiment is explicitly identified. Costs include discoverability, occlusion, accessibility, attention, power and implementation effort, not just drawing time. Product announcements describe intended behaviour, not independent usability evidence. Historical systems remain useful references; their navigation should not be mistaken for the latest OS behaviour.

Sources prioritise official guidelines, manufacturer manuals, original developer accounts and academic papers. Rebble preserves Pebble's historical developer documentation. Talks and videos are described through retrieved transcripts, official descriptions or an explicitly attributed written report; no frame-by-frame viewing or timing experiment is claimed. The concepts later in this document are original combinations for Orb, not claims that their underlying interaction techniques are new inventions.

### Orb's actual boundary

Read against [HANDOFF](../HANDOFF.md), [design](../design.md), [protocol](../protocol.md), [HIG](../hig.md), [Dot](../dot.md), [physical-panel feedback](../ux-feedback.md) and the [design gate](../agent-briefs/wave2/design-gate.md).

| Constraint | Consequence for this research |
| --- | --- |
| 1.75-inch, 466 × 466 round AMOLED; ESP32-S3, 8 MB PSRAM; software-rendered LVGL | Use polar geometry, cached sprites, bounded lists and small invalidation regions. Do not assume a watch GPU, full-screen video or live full-screen effects. |
| Touch, QMI8658 6-axis IMU, PWR and BOOT | Tangential touch can approximate rotary input, but there is no physical crown or rotating bezel. Pickup/tilt sensing is available; reliable finger-pinching in free space is not established. |
| Speaker and microphone; **no vibration motor** | Every sound needs visible feedback. Sound cannot recreate tactile detents or be required in quiet mode. |
| Desk and hand use | Design a distance-readable resting face and a deliberate interactive state. An IMU detects motion/orientation, not viewing distance, gaze or owner identity. |
| Agent approvals and voice | Movement, navigation and recording must not grant permission or send a transcript. `card.answer` is first-answer-wins; recorded, delivered and executed are different states. Respect `can_message` and Dot's listening state. |
| Smart home | Render only reported values and writable controls. Offline disables writes. Scene acceptance does not prove every accessory reached its final state. Unlock requires the protocol's deliberate one-second hold in lock detail. |

The governing visual rules remain true black, Inter, Framework7-style icons, colour for meaning, large targets, and Liquid Glass only on controls/navigation. **No gradients or glows anywhere**, including icons and games. Some borrowed references conflict: Palette gradients, neon hobby dashboards and literal Glyph light bloom do not translate. The owner instruction and design gate also override older glow suggestions within HIG §2.2. Use a crisp press response and shape/material change instead. Apply the gate's stricter **three visible glass elements** ceiling when it differs from HIG's older four-shape rendering budget.

There are also differing button descriptions in existing documents: `design.md` assigns PWR hold to the power menu, while HIG's games paragraph mentions holding PWR to exit. These proposals add no button reassignment. Resolve that inconsistency before implementation; keep a visible game exit and the established shell controls meanwhile.

The [integrated performance checkpoint](../PERFORMANCE.md) reports approximately **18.56–18.57 fps scrolling and 39.95–40.45 fps animated glass** for its October 26 stress scene, with networking active. HANDOFF describes newer work on another firmware branch. Neither those numbers nor older renderer-only 50–60 fps results guarantee a new concept's performance. All costs and timing/gesture thresholds proposed below are estimates to test, not new measurements.

## 1. Apple Watch: borrow hierarchy and continuity, then reshape them

Apple Watch is a rounded rectangle. It is useful for small-screen interaction, but literal corner placement and complication density are not a round-screen specification.

### Input, navigation and glanceability

| Reference and documented behaviour | Why it helps; what it costs | Translation to Orb |
| --- | --- | --- |
| **Brief interactions.** Apple encourages glanceable information, simple tasks and shallow navigation. [Designing for watchOS](https://developer.apple.com/design/human-interface-guidelines/designing-for-watchos) | A person can answer “does this need me?” without reading an app. The hard cost is choosing what to omit and keeping the summary truthful. | Face: time plus pending count and a last-step summary on entry. Approval: exact action, source and scope before decision. Send long reading to detail/the hub rather than shrinking everything. |
| **Digital Crown.** Scroll, zoom and navigate without occupying the display. [Apple Watch introduction, 2014](https://www.apple.com/newsroom/2014/09/09Apple-Unveils-Apple-Watch-Apples-Most-Personal-Device-Ever/) | Continuous motion handles large ranges; the same control can serve lists and values. It requires hardware, a hand grip and a consistent mapping. | Introduce a common rotary-delta abstraction driven by rim touch, alongside ordinary vertical scrolling. Unlike the Crown, touch still occludes the screen and lacks Crown-style tactile feedback. |
| **Navigation continuity.** watchOS 10 combines a source/detail view, vertical pages and hierarchy; an identifying object can persist between positions. [Official UI example](https://developer.apple.com/documentation/watchos-apps/creating-an-intuitive-and-effective-ui-in-watchos-10) | Carrying the subject through a transition reduces “where did I go?” The cost is maintaining identity and reversible state. | A session token becomes the detail header and returns to its original slot. Cache the token and destination; avoid continuously rescaling every text glyph. |
| **Smart Stack.** Expressive or data-rich faces both lead to useful widgets; relevance and user pins coexist. [WWDC23: Design widgets for the Smart Stack](https://developer.apple.com/videos/play/wwdc2023/10309/) | The face need not contain everything. Ranking costs freshness logic, predictability and navigation stability. | Preserve the existing swipe-up entry. Put waiting work first using explicit rules; offer fixed favourites. Freeze target order during a gesture, then visibly reconcile with the hub's authoritative order. |
| **Widget timelines.** Entries and relevance validity determine what appears when. [WWDC23: Build widgets for the Smart Stack](https://developer.apple.com/videos/play/wwdc2023/10029/) | Scheduled selection supports a useful glance without a permanent animation loop. It costs data lifetime and expiry handling. | Use timestamps and bounded snapshots already available. Do not invent a new relevance/expiry wire field silently; any richer scheduling model needs a protocol change first. |
| **Complications / Live Activities.** Small summaries and direct entry points select the most critical information; timed activities have a lifecycle. [WWDC24: Design Live Activities for Apple Watch](https://developer.apple.com/videos/play/wwdc2024/10098/) | A stable little gauge can replace an app visit. Many simultaneous gauges become tiny targets and competing meanings. | A pending-count complication opens the queue. Known timer progress gets an arc; an agent with no completion fraction gets a static state symbol and last-step text, never a fabricated progress ring. |
| **Double Tap.** Thumb/index taps are detected using accelerometer, gyroscope, optical heart sensor and machine learning on the Neural Engine. [Apple's technical explanation](https://www.apple.com/newsroom/2023/10/apple-watch-double-tap-gesture-now-available-with-watchos-10-1/) | A low-effort shortcut is helpful when the other hand is occupied. It costs specialised sensing, recognition and an unambiguous primary action. | Do not promise Double Tap from Orb's IMU. Keep BOOT push-to-talk and visible actions; experimental knocks may only wake or preview after testing, never approve or send. |
| **Wrist raise.** Raising wakes; touch/Crown provide alternatives. [Wake and unlock](https://support.apple.com/guide/watch/wake-and-unlock-apd0e1e73b6f/watchos) | Attention follows an intentional bodily action. Bumps and unusual postures cause false wakes. | Pick-up-to-wake with hysteresis, cooldown and touch/button fallback. Lifting does not identify the owner and must not reveal private text automatically. |
| **Always On.** Lower brightness/update frequency, preserve a useful resting presentation. [Always On design](https://developer.apple.com/documentation/watchos-apps/designing-your-app-for-the-always-on-state/) | Useful information remains available without an active session. Power still depends on lit pixels, refresh, CPU and communications. | A still clock with a count is a distinct state, not an animation running dimly. Verify actual panel power and burn-in strategy; black alone does not prove low energy use. |

**Talk described in text:** [WWDC23, “Design and build apps for watchOS 10”](https://developer.apple.com/videos/play/wwdc2023/10138/) has a TabView chapter at 7:53 and layout at 11:19. Its transcript describes Activity Rings moving to the upper-left toolbar while remaining visible across pages, and dial/infographic/list grids shaped by the display. The lesson is persistent subject identity and hardware-derived geometry. Orb can accomplish that with a token, a chord-aware list and a short iris, without duplicating Apple's render pipeline.

**Zoom nuance:** Apple's [fluid zoom transition documentation](https://developer.apple.com/documentation/uikit/enhancing-your-app-with-fluid-transitions) is an iOS/UIKit reference and explicitly notes context-dependent defaults on other platforms. It supports the general origin-to-detail and interruptibility principles; it is not evidence that this API runs on watchOS or LVGL. Orb should animate a bounded subject or clip region, not assume a cheap arbitrary full-screen scale.

### Faces as information architectures

| Face | What it does / why it fits a small display | Costs and Orb lesson |
| --- | --- | --- |
| **Modular** | Six complications organise direct values typographically. | Competing numbers and configuration. Borrow one dominant value plus a few summaries, using chords rather than a rectangular grid. |
| **Infograph** | Up to eight complications/subdials establish recognisable positions. | Small labels/targets and clutter. Borrow spatial memory, not eight interactive items on Orb. |
| **Solar Dial / Solar Graph** | A 24-hour solar dial or daylight curve relates time to sunrise/sunset; Crown exploration changes the selected time. | Requires data and a clear distinction between now and exploration. Borrow a labelled workday/timer arc with a visible return-to-now action. |

These three descriptions come from [Apple Watch faces and their features](https://support.apple.com/guide/watch/faces-and-features-apde9218b440/watchos). The transferable idea is choosing a face around a question—“what time?”, “what needs me?”, “where in the day?”—rather than layering every metric onto one face.

| Face | What it does / why it fits | Costs and Orb lesson |
| --- | --- | --- |
| **Astronomy** | watchOS 9 added a star map and current cloud information: the face is an explanatory scene. [watchOS 9 announcement](https://www.apple.com/newsroom/2022/09/watchOS-9-is-available-today/) | Textured globes, downloads and perpetual rendering are expensive. A few agent bodies positioned with polar coordinates borrow scene coherence cheaply; they need not orbit continuously. |
| **Palette** | Three overlapping colour layers change with time. [watchOS 10 announcement](https://www.apple.com/uk/newsroom/2023/06/introducing-watchos-10-a-milestone-update-for-apple-watch/) | Expressive but less precise; its gradients conflict with Orb's mandatory visual rules. Borrow time-sensitive expression using discrete flat symbols, not its appearance. |
| **Snoopy** | Characters interact with clock hands and respond to context such as weather/workouts. [Same official announcement](https://www.apple.com/uk/newsroom/2023/06/introducing-watchos-10-a-milestone-update-for-apple-watch/) | Art, choreography, licensing and animation demand. Borrow an original character reacting briefly to real events, then resting. Keep it away from approval decisions. |

## 2. Round watch systems

This section compares native round patterns with small-screen patterns that come from other shapes. Historical Tizen and Pebble are particularly useful because their guidelines explain geometry and input compromises explicitly.

### Wear OS / Material: let the boundary shape the composition

| Pattern and source | What it does and why it helps | Costs and Orb translation |
| --- | --- | --- |
| **Edge-hugging layout and transforming lists.** [Material 3 Expressive walkthrough](https://android-developers.googleblog.com/2025/08/introducing-material-3-expressive-for-wear-os.html) | A bottom button follows the circular edge; list items scale/morph toward the poles. The article's animation descriptions show these changes. Focus occupies the widest region. | Transformations can make text unstable or too small; a bottom action consumes space. Use existing round-row caches and one concentric bottom action. Borrow geometry and continuity, while retaining Orb's own visual system. |
| **Arc typography.** [Typography application](https://developer.android.com/design/ui/wear/guides/styles/typography/apply) | Short signposts such as time/headings can follow the top/bottom arc, leaving straight central lines for reading. | Rotated glyphs, spacing, localisation and text enlargement need special handling. Use a short cached label only when it earns its place. Commands, agent names and transcripts remain straight. |
| **Polar components.** [Wear Compose foundation](https://developer.android.com/reference/kotlin/androidx/wear/compose/foundation/package-summary) | Curved layouts/text and scaling/transforming lists are explicit components, rather than accidental clipping. | Android widgets are not an LVGL implementation. Translate polar anchors and chord calculations, not the framework. Avoid live text rotation when a cached short label will do. |
| **Rotary input.** [Official rotary guidance](https://developer.android.com/training/wearables/compose/rotary-input) | Side controls, physical bezels and touch bezels feed scrolling with a visible position indicator. Peripheral manipulation leaves more central content visible. | Touch lacks physical indexing and still occludes. One focused consumer owns rotation; distinguish list scrolling from adjusting a value. Keep ordinary drag and visible alternatives. |
| **Swipe to dismiss / back.** [Exit](https://developer.android.com/training/wearables/views/exit), [navigation](https://developer.android.com/training/wearables/views/navigation) | Left-to-right swiping returns through the back stack; vertical content avoids competition. | Horizontal carousels can fight navigation. Retain Orb's established left-edge back, and make edge versus centre origins explicit. Back from an approval is navigation only, never a provider decision. |
| **Tiles.** [Get started](https://developer.android.com/training/wearables/tiles/get_started), [common layouts](https://developer.android.com/design/ui/wear/guides/foundations/common-layouts/tiles) | Fixed-height, non-scrolling quick experiences commonly use title, main content and bottom action. A single glance need not become an app session. | Long Tile carousels become hunting; prose does not fit. Use this content contract inside Orb's existing Smart Stack, with detail on a separate screen. Do not add a competing system carousel. |
| **Circular reveal.** [Android's implementation](https://developer.android.com/develop/ui/views/animations/reveal-or-hide-view) | A growing clipping circle can originate at the selected object and connect it to the new view. | It is general Android guidance, not a requirement that every Wear transition be circular. Two scenes and a changing mask can invalidate the entire display. Prototype a cached, scanline-clipped LVGL version and measure. |
| **Fit and accessibility.** [Wear quality](https://developer.android.com/develop/adaptive-apps/quality-guidelines/wear-app-quality), [clipping guidance](https://developer.android.com/design/ui/wear/guides/m2-5/behaviors-and-patterns/clipping) | Enlarged text, substantial targets and predictable close navigation are functional requirements. | Android dp is not Orb px. Use Orb's physical sizing and chord-safe line boxes; switch layouts at large text sizes rather than merely scaling the picture. |

**Video/talk lead:** [Google I/O 2024, “Building for the future of Wear OS”](https://io.google/2024/explore/19faa1a5-5b4f-4e7a-ac06-8564a5a324af/) covers Wear OS 5, Watch Face Format and variation in device sizes. Its accessible abstract establishes that scope, not particular animation details. The Material walkthrough above provides the stronger retrieved visual descriptions for this report.

### Samsung Tizen / One UI Watch: make focus and direction consistent

Samsung's detailed Tizen documents are **historical**; they explicitly note that Tizen watch-app distribution has ended. They remain unusually clear descriptions of circular interaction, not instructions to adopt Tizen today.

| Pattern and source | What / why | Costs and Orb translation |
| --- | --- | --- |
| **Physical versus touch bezel.** [Bezel types](https://developer.samsung.com/one-ui-watch-tizen/bezel-less-ux.html) | Touch bezel means swiping the outer touch region; physical rotation is a different controller. Both preserve the centre for content. | A touch bezel can be disabled and needs alternative controls. Orb is analogous to this branch, not a motorised/mechanical ring. |
| **One rotary owner.** [Bezel interaction](https://developer.samsung.com/one-ui-watch-tizen/interaction/bezel.html) | Clockwise increases/advances, counter-clockwise decreases/returns among peers. A circular indicator shows position. | Switching the controlled quantity needs explicit focus. Crucially, Samsung limits its accept/reject rotation patterns to physical bezels and excludes touch bezels. Orb rim gestures must never approve or deny. |
| **Centred list.** [List guidelines](https://developer.samsung.com/one-ui-watch-tizen/ui-components/list.html) | The centre row gains focus and neighbours demagnify. Reading lands on the largest chord. | Peripheral rows become previews, and dynamic reflow can disrupt reading. Give Orb's focused agent actual state and last step; keep text stable and target bounds generous while moving. |
| **Radial selector.** [Rotary selectors](https://developer.samsung.com/one-ui-watch-tizen/ui-components/rotary-selectors.html) | Icons surround a large central title/description of the selected option. Stable directions support spatial memory. | The historical eleven-icon layout is too dense to copy for direct finger selection. Start Orb with four broad destinations and a central label; more requires physical evidence. |
| **Circular motion.** [Circular UX](https://developer.samsung.com/one-ui-watch-tizen/circular-ux.html) | Rotation suggests peers/flow; outward expansion and inward contraction express entry/removal. | Repeated circular motion becomes decoration when no relation is conveyed. Use one source-to-detail expansion and its reverse; do not turn agent activity into a spinning ornament. |

**Manufacturer case study:** Samsung's [One UI 9 Watch design story](https://design.samsung.com/global/contents/view/one-ui-9-watch) discusses radial importance, adaptive information density and deeper exploration on a companion phone. Its images are identified as illustrative. The useful lesson for Orb is brief understanding/action locally and extensive code/log analysis on the Mac. Adaptive states cost design and testing; they are not achieved by uniformly shrinking a desktop layout.

### Pebble Time Round and Timeline: geometry plus a temporal organising idea

**Round menus and text, documented:** the highlighted item stays centred and can contain extra detail. Pebble's round guide explains that continuous text reflow across changing chord widths disrupts reading, and favours pagination. It also warns that very thin perimeter rings can make manufacturing eccentricity conspicuous. [Preserved Pebble Round App Design](https://developer.rebble.io/guides/design-and-interaction/in-the-round/)

**Why / cost / Orb:** focus gets the widest readable area, pages retain stable line breaks and inset/thicker arcs tolerate optical mismatch. Costs are fewer simultaneous items and explicit page continuity. Show a clear next-page affordance for long command/transcript detail. Never wrap text differently every animation frame, and verify rim alignment on the physical panel.

**Timeline, documented:** Up from the face reaches past pins; Down reaches future pins. Integrations contribute events, reminders and actions outside their own apps. [Pebble Timeline](https://developer.rebble.io/guides/pebble-timeline/)

**Why / cost / Orb:** time unifies otherwise unrelated apps. It also competes with urgency: a chronological build-completion receipt must not bury an unanswered approval. Borrow a separate history/time exploration mode, not Pebble's exact directions, which conflict with Orb's established shell. Distinguish provider event time from arrival time and show when records are incomplete.

Pebble additionally advocates single-purpose experiences, bold key information, brief purposeful animation and discoverable “more” affordances. [Core Experience Design](https://developer.rebble.io/guides/design-and-interaction/core-experience/) Orb should expose an Actions button even if long-press opens the same radial shortcut. Hidden expertise cannot be the only route.

### Garmin, Fitbit and Pixel Watch

| Reference | What / why | Costs and Orb translation |
| --- | --- | --- |
| **Garmin Glances.** [Developing the Concepts](https://developer.garmin.com/connect-iq/articles/user-experience-guidelines/Developing_the_Concepts.html) | Scroll key metric summaries, select for detail. A compact entry layer saves opening every app. | Metric-oriented timeouts must not erase unresolved decisions. Borrow concise agent/Home summaries and explicit drill-down, keeping approvals in the queue after sleep. |
| **Garmin watch-face power states.** [Watch Faces](https://developer.garmin.com/connect-iq/articles/user-experience-guidelines/Watch_Faces.html), [app types](https://developer.garmin.com/connect-iq/connect-iq-basics/app-types/) | Low-rate idle updates and brief richer presentation after a look gesture; AMOLED guidance considers lit pixels/static content. | Different display/SDK budgets do not transfer. Use event-driven/minute-based idle updates and short wake moments. Keep Orb's legible font weights even where Garmin's own guidance suggests thinner idle art. |
| **Fitbit Sense 2 / Versa 4 navigation.** [Official guide](https://support.google.com/googlehealth/answer/14225687?hl=en) | Clock root, directional surfaces, user-ordered Tiles and a physical return route. These are **small-screen, non-round** references. | Invisible directional destinations must be learned; copying each vendor's mapping creates conflict. Preserve Orb's one map, provide visible entries and let users prioritise a few cards. |
| **Fitbit goal celebration.** [Versa manual](https://www.fitbit.com/content/assets/help/manuals/manual_versa_en_US.pdf) | Completion pairs a celebration with vibration, giving closure. | Repeated nudging and streak pressure cost attention; Orb lacks vibration. Use a brief visual acknowledgment of real completion, optional quiet sound and no guilt for ignoring it. |
| **Pixel Watch object design.** [Google design case study](https://blog.google/products-and-platforms/devices/pixel/design2022/) | Domed glass/water-droplet inspiration connects the object to round-watch familiarity. | Industrial-design appeal is not usability proof. Let true black merge with Orb's panel; keep controls inset instead of painting a shiny fake background. |
| **Pixel faces, complications, Tiles.** [Customisation guide](https://support.google.com/googlepixelwatch/answer/12662644?hl=en) | User-chosen face information and favourite shortcuts create a personal glance hierarchy. | A fully configurable dashboard increases setup burden. Offer Agents, Home and Quiet clock presets with a few editable complications; avoid a tiny on-device layout editor. |
| **Pixel voice access.** [Gemini on Pixel Watch](https://support.google.com/googlepixelwatch/answer/16393978?hl=en) | Physical and contextual entry paths vary with device/model, preserving quick access. | Ambient/raise-to-talk adds recognition, power and privacy costs. BOOT push-to-talk is the honest Orb translation; the IMU alone does not establish speech intent. |

Across these watches, the useful common structure is **face → glance → focused task**, with a consistent escape route. Their precise swipe directions are mutually incompatible; Orb should borrow the structure, not accumulate all their gestures.

## 3. Instruments, tiny companions and ambient interfaces

### Nest thermostat: one variable, one large dial

**Documented:** Nest Learning Thermostat / Thermostat E use a physical ring to change temperature, turn through options and press to select. This differs from the non-learning Nest Thermostat's side touch strip. [Google's beginner guide](https://support.google.com/googlehome/answer/9248184?hl=en)

**Why / cost / Orb:** the physical and displayed circles describe the same scalar variable; a large centre exposes its value. This works beautifully for a focused light level, fan speed or setpoint, less well for unrelated commands around a menu. A simulated dial loses mechanical precision and can hide its value under the finger. Use rim scrubbing with a large centre value, named accessory and explicit commit; let the authoritative Home response settle the final value.

**Distance hierarchy:** Farsight on the fourth-generation Learning Thermostat can show additional near-view information within three feet. [Official Farsight instructions](https://support.google.com/googlehome/answer/16633204?hl=en) The principle is progressive disclosure across attention states. Orb cannot measure that distance with its IMU: use resting/picked-up/manual modes, without claiming proximity detection.

### iPod Click Wheel: separate movement from commitment

**Documented:** the fourth-generation iPod combined a touch-sensitive continuous scrolling wheel with five push buttons. [Apple, July 2004](https://www.apple.com/ca/newsroom/2004/07/19Apple-Introduces-the-New-iPod/)

**Why / cost / Orb:** circular relative motion permits repeated movement without reaching a slider end; a separate click commits. The wheel occupies its own surface, so it does not conceal the list. Orb shares input and display, so it cannot inherit that advantage. Borrow clockwise-next/increase, counter-clockwise-previous/decrease, a stable centre preview and a separate labelled touch action. Use acceleration for browsing only after testing; precise setpoints need slower steps. A finger moving around a rim is not permission to activate the item it passes.

### Rabbit r1 and Humane Ai Pin: small AI interfaces need inspectable state

**Rabbit, documented:** the current manufacturer guide describes voice, touch, text and a scroll wheel with a card-oriented interface; holding PTT speaks and releasing hands the request onward. It also exposes explicit home/quick-setting gestures. This has evolved since launch, so claims that r1 is exclusively wheel-operated are outdated. [r1 user guide, updated September 22, 2026](https://www.rabbit.tech/r1-user-guide)

**Why / cost / Orb:** a dedicated talk control makes capture intent tangible; a small screen can show context and results without becoming a phone. Costs are transcription latency, social/privacy limits of speech, recovery and ambiguity about what the AI actually did. Orb already has the useful hardware pattern. Improve recipient visibility, recording feedback, transcription error recovery and explicit send confirmation; do not copy a release-to-submit flow into consequential agent messages.

**Humane, historical concept:** the TED page presents the goal of a computer that recedes from attention and now marks the product discontinued in February 2025. [Imran Chaudhri, TED2023](https://www.ted.com/talks/imran_chaudhri_the_disappearing_computer_and_a_world_where_you_can_take_ai_everywhere?view=transcript) This is a concept pitch, not usability evidence. A contemporary written description of the manufacturer's demonstration reports palm tilt to highlight and finger pinch to select, with several steps to compose/send. [Attributed video report, November 2023](https://9to5google.com/2023/11/21/humane-ai-pin-display/) This report is not a measurement made for Orb.

**Why / cost / Orb:** multimodal input can reduce touch when hands are occupied, but recognition, arm fatigue and serial steps can outweigh novelty. Orb has direct touch and should keep it. Put tilt in optional toys and mild depth cues, not primary navigation or approvals.

**Teardown evidence:** iFixit's original [Rabbit / Humane teardown article](https://www.ifixit.com/News/95474/rabbit-r1-and-humane-ai-pin-teardown-the-beginning-of-a-new-device-category) describes Rabbit's wheel sensing and Humane's MEMS projector, and distinguishes local hardware operation from remote AI. It also links the teardown video. The important translation is the boundary: a charming physical shell cannot hide remote dependencies. Orb's clock and games can work offline; agent/voice/Home availability must remain explicit. No projector, camera or cloud AI capability is implied for Orb.

### Nothing Glyph: compress meaning before adding pixels

**Documented:** Glyph pairs light patterns with calls/notifications. The manufacturer's retrospective distinguishes basic strips, progress/essential notifications and a circular symbol matrix. [Official support](https://support.nothing.tech/hc/en-us/articles/16770135458193-What-is-the-Glyph-Interface), [Nothing's design retrospective](https://nothing.community/d/53209-the-evolution-of-glyph-interface)

**Why / cost / Orb:** a small vocabulary of recognisable signals can answer “anything important?” without inviting a long interaction. The cost is learning opaque codes, missing signals outside view and sensory overload from blinking. Translate this into **steady**, semantically distinct rim marks plus a readable count/state on wake. Use shape and position as well as colour. Orb has no rear LED array; reproduce neither light bloom nor an always-pulsing perimeter. Sound remains optional.

### Teenage Engineering: understandable instruments, not tiny desktop apps

**OP-1, documented:** four coloured encoders map to corresponding graphical elements/values. [Original OP-1 layout guide](https://teenage.engineering/guides/op-1/original/layout) The [tape-mode guide](https://teenage.engineering/guides/op-1/original/tape-mode) describes a four-track recording metaphor.

**Why / cost / Orb:** a coherent metaphor and direct control-to-picture mapping let users learn one little instrument. The cost is mode learning and colour-only ambiguity; complex modifier functions can become an expert language. Borrow a timeline that visibly winds or a dial that visibly adjusts a named value. Add symbols/text and one focus owner; Orb has neither four encoders nor room for four simultaneous parameter editors.

**TP-7, documented:** its motorised reel scrubs, pauses and navigates, while also indicating recording/playback; a memo button can start capture directly. [Manufacturer's product explanation](https://teenage.engineering/products/tp-7)

**Why / cost / Orb:** one object being both control and feedback makes cause and effect legible. The physical motor and touch interaction cannot be recreated by animation. A restrained recording spool or elapsed-time arc can express capture; actual mic state remains a separate explicit indicator. Reuse BOOT hold rather than overloading PWR or inventing a new mode chord.

### Playdate: make the unusual input the point of play

**Documented:** Playdate's guidelines discuss crank precision, accessible alternatives, accelerometer calibration, avoiding costly runtime sprite transforms, dirty-region rendering and the difference between simulator and device. [Designing for Playdate](https://help.play.date/developer/designing-for-playdate/)

**Why / cost / Orb:** a small device can be memorable because a simple mechanic belongs to its body. Control gain and grip comfort matter more than spectacle. Orb has no crank: use a rim-controlled orbital paddle, reversible timeline toy or calibrated tilt marble, with visible touch alternatives. Pre-render costly orientations and stop rendering still scenes. Those are implementation strategies, not transferable Playdate frame-rate promises.

**Creator case study:** the [Crankin's Time Travel Adventure developer podcast transcript](https://podcast.play.date/episodes/s01e04/transcript/) explains that a film-projector association inspired forward/backward time control; characters were rendered into sprite sheets. The [official game page](https://play.date/games/crankin/) notes the mechanic requires precision. Orb's lesson is to derive one surprising rule from continuous input—rewind only your marble's path, for example—rather than bolt tilt onto ordinary buttons. The costs are original level/asset work and testing whether touch really provides enough precision.

### Tamagotchi and Pikmin Bloom: delight through consequence and memory

**Tamagotchi, documented:** Bandai's Connection manual uses A to select, B to confirm and C to cancel; a tiny character communicates hunger, happiness and attention and changes with care. [Official manual](https://www.bandai.com/amfile/file/download/file/242/product/1304247/)

**Why / cost / Orb:** a consistent character plus a few meaningful states creates attachment with little screen area. The cost is obligation, interruptions and opaque state if the character is the only indicator. Borrow brief contextual behaviours from an original character, not care penalties. The companion must never become sad because the owner denied an approval, stopped coding or ignored Orb.

**Pikmin Bloom, documented:** its lifelog collects daily experiences; flower planting is connected to gradual growth/friendship. [Lifelog help](https://niantic.helpshift.com/hc/en/23-pikmin-bloom/faq/2856-lifelog/?l=en), [Flower planting help](https://niantic.helpshift.com/hc/en/23-pikmin-bloom/faq/2855-planting-flowers/?l=en)

**Why / cost / Orb:** ordinary activity becomes a small visible history rather than a demand to manage another app. Costs include persistence, privacy and pressure from streak/reward systems. An optional desk garden can remember a timer finished or a toy played. Do not equate a local IMU with phone step/location tracking, invent work achievements, or reward granting agent permissions. Store only the minimum local keepsake state.

## 4. Circular IoT, smart rings and hobby systems

| Reference and evidence | What / why | Costs and Orb translation |
| --- | --- | --- |
| **M5Stack Dial.** [Official hardware](https://docs.m5stack.com/en/core/Dial), [Home Assistant integration](https://docs.m5stack.com/en/homeassistant/devices/dial) | ESP32-S3, round 240 × 240 touch display and a rotary encoder form a compact IoT control instrument. One scalar can dominate the centre. | The physical encoder is the crucial difference. A touch rim plus large value and ± controls is plausible on Orb; matching its physical feel is not. Good dimmer/timer precedent, weak justification for dense dashboards. |
| **SmartKnob.** [Creator's repository and demonstration link](https://github.com/scottbez1/smartknob) | A motor and magnetic encoder create programmable detents/endstops; the control's physical feel changes by task. | Orb cannot generate resisting torque or tactile stops. Borrow semantic snapping and optional ticks only. This is an exploratory hardware project, not validation of a production home UI. Its software/electronics/docs and mechanics have different licences; preserve those if reusing anything. |
| **Nenya magnetic ring, CHI 2011.** [Paper DOI](https://doi.org/10.1145/1978942.1979238), [authors' project index](https://hpi.de/baudisch/projects/) | Turning the ring selects; movement along the finger clicks. Input is separated from the display. | Needs a magnetic ring and wrist sensing. Orb's 6-axis accelerometer/gyro is **not a magnetometer**. Borrow turn-to-preview/separate-commit semantics only. The research apparatus also separated pointing/click timing in part of its evaluation. |
| **Galaxy Ring double pinch.** [Samsung support](https://www.samsung.com/ie/support/mobile-devices/what-is-the-double-pinch-gesture-on-the-galaxy-ring/) | A narrow, practised gesture controls specific compatible-phone contexts such as capture/alarm dismissal. | Worn sensing and device compatibility matter. Borrow a practice screen and one clear contextual meaning; use Orb touch/button input, not imagined free-air finger sensing. |
| **SkinTrack, CHI 2016.** [Author project](https://www.chrisharrison.net/index.php/Research/SkinTrack) | An active ring and wrist electrodes track skin contact to extend input beyond the screen. | Extra electronics make this a concept reference rather than an Orb implementation. It reinforces the value of keeping the reading area clear; it does not show that ordinary capacitive glass senses the case or hand. |
| **HomeGenie Mini round smart display.** [First-party project documentation](https://homegenie.it/mini/1.2/examples/smart-display/) | ESP32-S3/PSRAM LVGL activities include level/colour control and round-display thermostat experiments. A named activity provides a focused task. | Different boards, stack and assets; reported features are not Orb performance evidence. Borrow one-accessory-at-a-time controls and activity separation. Do not import a constantly streaming dashboard just because a demo supports one. |
| **Waveshare 1.75C examples.** [Vendor repository](https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-1.75C) | LVGL examples, motion-driven blocks and audio visualisation demonstrate local touch/motion/audio possibilities on the device family. | Feature bring-up is not UX, energy or network-load validation. Use as feasibility reference only; keep Orb's existing board layer and partitioning. No board flash is needed for this research. |
| **Chronos / esp32-c3-mini.** [Creator repository](https://github.com/fbiego/esp32-c3-mini), [related face project](https://github.com/fbiego/esp32-lvgl-watchface) | An LVGL watch implementation combines faces, notifications, weather and media controls; the project links native builds and demos. | BLE/phone assumptions differ from Orb's hub. Some face features are experimental or version-limited. Learn state organisation; a repository's code licence does not automatically license every imported watch-face asset. |
| **Muse Gadgets.** [Supported-device account](https://github.com/facebookincubator/muse-gadget-sdk/blob/main/esp32/devices/README.md), [licence](https://github.com/facebookincubator/muse-gadget-sdk/blob/main/LICENSE) | Small AI-device implementations connect board support, voice and compact presentation. The nearby hardware lineage makes it a useful engineering reference. | Device reports are configuration-specific, not a UI benchmark. Orb already identifies allowable reuse with attribution; inspect each reused component and dependency. Retain the local thin-client and truthful-status boundary. |

**LVGL provides useful building blocks, not the interaction design.** Its [circular scroll example](https://github.com/lvgl/lvgl/blob/master/examples/scroll/lv_example_scroll_translate.c) changes child displacement/opacity with distance from centre and enables snapping. Its [Arc widget](https://lvgl.io/docs/open/widgets/arc) provides a scalar control with ranges, rotation and change-rate behaviour. These make a focus list and dial cheap prototypes. The example's transform alone does not guarantee chord-safe text; Arc is not a universal gesture recogniser. Reuse Orb's existing chord/layout/caching work and adapt to the pinned LVGL fork rather than replacing it with moving upstream code.

Unlicensed round-device concept work can inspire an interaction, not supply code/art/text. In particular, the repository's instructions identify `rsms/esp32-dot` as ideas-only. Nothing in this research requires copying it or installing another firmware.

## 5. What academic HCI actually supports

### Evidence with a direct circular-screen connection

| Study | Technique / useful result | Limits, costs and Orb interpretation |
| --- | --- | --- |
| **Ashbrook, Lyons & Starner, MobileHCI 2008: An Investigation into Round Touchscreen Wristwatch Interaction.** [Original proceedings PDF](https://docbox.etsi.org/STF/Archive/STF322_HF_EC_3G_MobileUserInterface/STFworkarea/FoldersFiles/MobileHIC%202008%20Proceedings/papers/p311-ashbrook.pdf) | Compares tapping and straight/rim-following sliding. Radial depth and angular width trade off rim selection against central space. Fingers did not reliably reach the literal outer edge. | Fifteen young participants, mostly male; roughly 30 mm aperture over a resistive phone touchscreen. Endpoint data simulated button hit rates, rather than validating a shipping button menu. Use thick inset hit regions; do not import its error estimates to Orb's larger capacitive panel. |
| **Wong et al., CHI 2020: Exploring Eyes-free Bezel-initiated Swipe on Round Smartwatches.** [Author-hosted paper](https://hongbofu.people.ust.hk/doc/BIS.Round.CHI2020.pdf) | Swipes start on the physical bezel and enter the screen. Table 1 reports **81.80% raw accuracy for eight origin segments, 25% for 32**. Direction and participant affect performance. | Twelve right-handed users, left-wrist 1.4-inch Ticwatch 2, hands horizontal on a table, view blocked. These are not Orb gesture success rates. Dense universal eyes-free menus are a poor default; use few broad origins, visible feedback and measure mistakes. Orb's passive case cannot detect the initial off-glass contact. |
| **Neshati et al., ICMI 2022: EdgeSelect.** [Lab publication account](https://hci.cs.umanitoba.ca/Publications/details/edgeselect-smartwatch-data-interaction-with-minimal-screen-occlusion) | Studies occlusion around the finger and a layered selection method for watch graphs. Separates a small input location from a visible result elsewhere. | Graph exploration and multiple selection layers differ from approvals. Do not assume one best quadrant across hands/postures. Borrow a large central preview while the finger manipulates the rim; avoid putting the selected value under the finger. |

### Menus, crossing and precision

| Study / source | What / why | Limits, costs and Orb translation |
| --- | --- | --- |
| **Kurtenbach, Sellen & Buxton: pie and marking menus.** [Full paper](https://www.billbuxton.com/PieMenus.html), [hierarchical-menu work](https://www.billbuxton.com/MMExpert.html) | A visible radial menu teaches the same directions an expert later marks quickly. Direction matters more than an exact small endpoint. | Original pen/mouse/trackball tasks are not tiny finger-touch circles. Breadth/depth increase learning and error costs. Use one stable four-action level, labelled preview and inward cancel. Restrict fast release selection to opening a view or reversible local operations. |
| **Accot & Zhai, CHI 2002: More than dotting the i's.** [Google Research record](https://research.google/pubs/more-than-dotting-the-is-foundations-for-crossing-based-interfaces/), [paper](https://doi.org/10.1145/503376.503390) | Crossing a boundary replaces entering a target and clicking; measured tasks support a Fitts-like model with task-specific parameters. Continuous sequences can be fluid. | Width, direction and accidental crossings still matter. Use a broad crossing to reveal a preview or select a local mode, never as an implicit grant. It is not universally faster or immune to finger occlusion. |
| **Vogel & Baudisch, CHI 2007: Shift.** [Author-hosted paper](https://www.patrickbaudisch.com/publications/2007-Vogel-CHI07-Shift.pdf) | An offset callout reveals the obscured selection point; a person can correct before lifting. Large targets remain direct taps. | Designed for finger use of pen-oriented interfaces, not specifically watches. Callouts consume precious area and create another mode. Orb should first enlarge targets, then show rim value/name in the clear centre; routine approvals should not need magnification. |
| **Holz & Baudisch, CHI 2010: Generalized Perceived Input Point.** [Author project](https://www.christianholz.net/generalized_perceived_input_point_model.html) | Touch error depends on finger posture and the user's perceived contact point, not merely a fixed “fat finger” circle. | The prototypes use sensing Orb does not have. Do not promise their correction accuracy from software alone. Test desk taps, held use, thumbs and different approach angles; use stable nonoverlapping hit regions and hysteresis. |
| **Parhi, Karlson & Bederson, MobileHCI 2006: thumb target sizes.** [Original paper](https://static.aminer.org/pdf/PDF/000/495/289/target_size_study_for_one_handed_thumb_use_on_small.pdf) | Recommendations around **9.2 mm for discrete and 9.6 mm for serial** thumb targets demonstrate the importance of physical dimensions. | A one-handed HP iPAQ study is not an Orb minimum-target standard. Orb's local 72 px target is about **6.9 mm**, assuming a 1.75-inch active diameter. This is a reason to test larger frequent/critical controls, not to claim its current minimum is empirically validated. |
| **WatchIt, Perrault et al., CHI 2013.** [Institutional publication](https://researchportal.ip-paris.fr/fr/publications/watchit-simple-gestures-and-eyes-free-interaction-for-wristwatche/) | Instrumented watchband gestures keep fingers off the display for commands and continuous input. | Requires a sensing strap. Borrow separation of manipulation/reading through real buttons and a touch annulus; it does not justify sensing taps on Orb's enclosure. |
| **BezelCursor, Li, Zhu & Fu.** [Authors' project and demo links](https://meilab-hk.github.io/projectpages/bezel.html) | Bezel initiation enters cursor control to reach distant mobile-screen targets without changing grip. | Large-phone reach is a different problem; indirect cursor control may add work on Orb. Borrow deliberate edge initiation, while keeping ordinary tasks direct. Retrieved text supports the technique, not particular video timings or animation appearances. |

### Fitts' law without the usual overclaim

The useful framework is movement distance, **effective target width in the direction of approach**, errors and physical scale. A thin annular segment has both a radial-depth constraint and angular separation from neighbours. A wide arc is not automatically an easy target if the finger must land in a very thin radial strip. [MacKenzie's Fitts' law reference](https://www.yorku.ca/mack/hhci2018.html)

The “screen edges are infinitely large targets” desktop-pointer argument does not hold for a finger that can slide entirely off a capacitive sensor. Also, circular arrangements of targets in standard Fitts tests do not imply a circular display was studied. Small-display evaluation has its own task limitations. [Small-display evaluation reference](https://www.yorku.ca/mack/ijhcs2018.html)

Ashbrook's dissertation investigates microinteractions taking under four seconds to initiate and complete. [Institutional abstract](https://repository.gatech.edu/handle/1853/33986) Treat that as an aspiration for checking one agent or changing a lamp, **not** a timer pressuring the owner to read and approve a command.

The experiments collectively support broad inset input regions, a visible preview away from the finger, consistent directional semantics and modest menu breadth. They do **not** establish universal angular thresholds, target sizes, gesture accuracy or performance for Orb. Those require physical testing in both desk and held use.

## 6. Interaction primitives for Orb

These are design deductions from the preceding evidence, subject to Orb's HIG. They describe one coherent interaction language, not a requirement to ship every idea together.

### Input: continuous exploration, separate intent

- **Touch centre:** tap to inspect; drag vertically to browse. Normal controls remain the primary discoverable path.
- **Touch rim:** relative one-dimensional movement for the current scroll, scrub or value owner. Clockwise means next/increase/later; counter-clockwise means previous/decrease/earlier. One gesture controls one thing.
- **Long-press:** reveal a small, stable contextual menu, also available through a visible Actions button. It is not a second hidden hierarchy.
- **BOOT:** preserve hold-to-talk with an explicit recipient, mic state and transcript confirmation. Do not turn a short tap into an approval shortcut.
- **PWR:** preserve shell/power behaviour. Do not ask people to remember new double/triple/chord actions.
- **IMU:** pick-up wake, face-down quieting, optional material response and games. No primary navigation, permission, message send or home write from motion.

**Gesture ownership needs a state machine.** Hit-test visible controls first. A left-edge inward stroke belongs to Back; other established edge destinations retain their map. For an eligible unclaimed rim start, wait for directional evidence: predominantly tangential motion captures the rotary owner, while a straight inward stroke remains edge navigation. A recogniser that has claimed the contact keeps it until lift/cancel; it never switches midway into Approve, Back or a different dial. BOOT recording suppresses competing gestures except explicit cancel. Handle lost touch, interrupted animation, sleep, overlays and reconnection as cancellation, never as activation.

For an initial experiment, an annulus around **r = 173–223 px** is a candidate continuous-drag region, not a row of tap targets. Its 50 px depth needs comparison with a wider 72 px band; neither is a measured optimum. Visible toolbar buttons take precedence where they overlap. Do not reserve the whole circumference if that makes the centre too small. If arbitration is confusing, enable rotation only after entering an explicit dial/scrub mode; ordinary scrolling remains available.

Compute angle around (233,233), unwrap across ±π and accumulate delta. Apply a movement deadband and snap hysteresis; keep the initial value/focus to support cancel. Treat lift/re-entry as a new gesture, not an angular jump. Never let one noisy sample make a full revolution. Step gain may vary by task but direction must stay consistent. List acceleration needs a bounded maximum; setpoints and approvals never inherit fling behaviour. These are engineering proposals, not recognition results.

### Layout: centre for reading, circumference for relationships

| Primitive | Orb rule |
| --- | --- |
| **Centre focus** | One readable subject/value; neighbouring items provide orientation. Expand detail on focus, not all rows simultaneously. |
| **Circle chords** | For centre (233,233), available width at y is `2 * sqrt(r*r - (y-233)*(y-233))`. Use the narrower chord at a line box's top and bottom, then include icon/padding. Text's safe radius is smaller than controls'. Reuse `orb_chord_width`. |
| **Physical targets** | HIG default 72 px, minimum 46 px, with required spacing. Tiny state marks can be visual summaries, but cannot silently become tiny independent hit targets. Enlarge critical controls where space allows. |
| **Polar anchors** | A few stable positions support spatial memory. Use the rim for a timer arc, page position or aggregate state; label the active meaning. Never overload one ring with three unrelated quantities. |
| **Straight reading** | Body text and commands stay straight, prewrapped into stable blocks/pages. Short peripheral labels may curve; essential content must survive enlarged text without requiring curvature. |
| **Distance hierarchy** | Resting: time and aggregate attention. Active: one agent/accessory and its action. Detail: stable pages. Large text can switch the orrery/menu to a list. |

At an assumed active diameter of 44.45 mm, 466 pixels mean about 10.48 px/mm. The 330 px inscribed square is **not the only usable area**, but neither does the 466 px diameter permit a full-width rectangular paragraph at every y. Round-first means using the extra side area intentionally while respecting the narrowing poles.

### Navigation and transition vocabulary

Keep **Face → Smart Stack / Apps → focused task → detail** shallow. Preserve the established shell directions, provide visible entries, and make every gesture's destination reachable through a button. A focus list is vertical; horizontal motion is not casually repurposed over Back. Card dismissal, back navigation and provider denial are three separate actions.

Use motion to answer a question:

| Relationship | Transition | Cheap first implementation |
| --- | --- | --- |
| “This object opened this detail.” | Expand its token into a circular reveal; reverse on back. | Cached token plus band-local clip; stable final text. |
| “These items are peers.” | Short arc movement / centre-focus snapping. | Move a few cached rows/sprites; avoid per-frame prose layout. |
| “This value follows my finger.” | Immediate indicator/value tracking, then a restrained snap. | Native arc and one dirty value region. |
| “This operation finished.” | Brief acknowledgement resolving to a still receipt. | One small sprite sequence or shape change. |
| “The device is waiting for me.” | Static count/status. | No permanent loop, spinner, heartbeat or simulated progress. |

Retarget an animation from its current state; don't queue gestures behind it. Normal mode follows HIG springs/material behaviour. Reduce Motion uses the prescribed short cross-fade with no depth/tilt movement; Reduce Transparency substitutes the prescribed solid surface. Content remains content, not glass. Do not stack effects merely because individually they are cheap.

### Feedback, glanceability and delight

Use **press → pending → acknowledged → settled** as distinct states. A local tap animation acknowledges touch, not network success. A failed home write returns to the last confirmed value; an agent answer with `delivered:false` remains a delivery failure even if the choice was durably recorded. Stale/disconnected state must be visible; no animation can stand in for a missing event.

Use silent visual detents; optional short quiet ticks only at meaningful steps. Make the centre readout visible away from the operating finger. Keep sound off during recording if it would enter the microphones. An elapsed recording arc measures known time; an unbounded agent task cannot use the same arc as invented completion.

Delight should be **caused by something**: a character notices a timer ending, an orbit settles after selection, a toy responds precisely to tilt. It must stop, yield to attention cards and work with reduced motion. Idle stillness is part of the character. Separate playful keepsakes from productivity scoring and permission decisions. The interaction should remain understandable after disabling every sound and animation.

## 7. Ten concrete Orb concepts

Relative costs below mean **L**: bounded layout/widgets/assets; **M**: new recogniser/component/state machine; **H**: shell coordination, compositing or hub/data work. They are implementation judgments, not schedules, heap measurements or frame-rate promises. Each concept needs a visible alternative and the design gate before implementation can be called complete.

### 1. Agent Orrery — a clock with a small constellation of work

**Interaction.** The resting face shows time and an aggregate “needs you” mark. On entering Agents, up to four prominent session bodies occupy fixed orbital slots; overflow opens a list. Rim movement brings a body into the readable focus region. Its name, state and actual last step appear centrally. Tap opens detail; the same body becomes its header. A waiting body bears a distinct attention symbol, a disconnected one a stale/disconnected treatment. No body must move perpetually to prove that an agent is busy.

**Why it fits.** It turns the circle into a spatial model and offers the owner a recognisable “my agents” object rather than a miniature process table. The orbit is for selecting peers; the centre is for reading. Astronomy's scene coherence and Infograph's stable locations inspire it, while its session mapping is specific to Orb.

**LVGL / data / cost: M.** Polar positions, a few cached flat sprites, stable session IDs and one focused text block. Existing session events suffice. Respect the hub's eight-session focus cap and authoritative order; overflow/idle summaries must not disappear. Allocate visual positions from the current ordered snapshot and keep them stable during a contact, then reconcile visibly. No textured 3D or physics engine.

**Risks / acceptance.** Four bodies are a starting density, not proof. Attention counts must still account for all pending work; multiple cards can belong to one session. Controls retain HIG-sized hit regions. Large text/Reduce Motion gets a list presentation. Test “which agent needs me?” against the ordinary list, and reject it if spatial theatre slows the answer.

### 2. Rim as Crown — one continuous control language

**Interaction.** In a list, trace clockwise to advance; in an entered timer/light dial, the same movement increases the selected value. A small local indicator makes the current owner visible. The centre previews selection/value. Browsing settles on lift; reversible value editors expose Cancel and an explicit Apply. Rotation never commits an agent answer or unlocks a door.

**Why it fits.** It uses Orb's most distinctive boundary and keeps the middle readable. Unlike a row of tiny edge buttons, a relative stroke can begin over a broad region and travel through many values.

**LVGL / data / cost: M–H.** A shell-level rotary-delta service with per-screen ownership, angle unwrapping, deadband, snap hysteresis, bounded gain, cancellation and edge-gesture arbitration. Start with one explicit dial screen using LVGL Arc before enabling shell-wide starts. Rendering is cheap compared with getting recognition right. Ordinary vertical drag, ± buttons and visible previous/next controls remain available.

**Risks / acceptance.** A touch bezel has no tactile landmarks and can miss edge contact. Test both hands, desk versus held use, rapid reversal, diagonal Back attempts, lift/re-entry and accidental palm contact. Disable global rim capture if it harms established navigation. “Everywhere” means a shared semantic abstraction, not an unrestricted gesture over security controls.

### 3. Iris Rooms — opening a place from its circular object

**Interaction.** Tap an agent, app or home entity: its token holds identity while a circular window expands into detail. Back contracts toward the source, with its last position preserved. A reversal follows the finger/current animation state. Modal alerts do not acquire a theatrical entrance that delays reading.

**Why it fits.** A round screen can reveal a round room. It communicates source/detail and returning to the same place, rather than making every task feel like a sideways phone page.

**LVGL / data / cost: H for a general compositor, M for a bounded prototype.** Try a scanline span mask over cached content, rendering only visible parts in the existing bands. An RGB565 square frame alone is `466*466*2 = 434,312 B`; two are 868,624 B before masks, fonts, raster caches or glass. A full-screen byte-per-pixel mask adds 217,156 B. Prefer analytic circle spans or small cached tokens over allocating all those surfaces. These are byte calculations, not measured allocator demand. No GPU is required, but memory bandwidth and invalidation dominate.

**Risks / acceptance.** Full-screen reveal may be slower than a short slide on the integrated build. Start with a token expansion and final-content cut/cross-fade fallback; retain HIG material behaviour for glass. Measure final DMA and touch interruption under networking/voice. Reduce Motion uses the specified cross-fade. If it cannot sustain coherent pacing, reduce the affected area before adding frames.

### 4. Focus Lens — a circular list that becomes readable where it matters

**Interaction.** Sessions or cards form a short vertical sequence. The central item has its full title, status and last step; neighbours preview identity. Drag normally or use the rim. The selected item settles on the widest chord, then exposes a visible Open/Details action. It does not reveal Approve merely because a row passed through centre.

**Why it fits.** Samsung's centred focus and Pebble's stable detail solve the real problem: little space for reading. The “lens” is information priority, not a magnifying glass shader. It can also be the accessible alternative to the Orrery.

**LVGL / data / cost: L–M.** Extend existing round-row/chord and cache infrastructure. Precompute compact and expanded row presentations; avoid per-frame rewrapping. Move/scale bounded cached row art while reserving readable text at rest. Use the session/card snapshots already defined. Freeze target identity for the duration of contact; do not answer a newly inserted card under an old finger-down.

**Risks / acceptance.** Scaling can shrink text below HIG minima; peripheral previews should omit optional text before shrinking essential text. Invisible hit boxes must not overlap adjacent rows. At AX3 show fewer rows or stable pages. Compare focus acquisition, accidental opens and reading comfort with the current list; the physical-panel clipping/scroll-edge feedback remains a required check.

### 5. Time Tide — scrub the circumference to revisit the day

**Interaction.** Open a clearly labelled History view. A notch at the top means Now; clockwise moves later, counter-clockwise earlier. As the owner scrubs, large central receipts appear: a build completed, a request arrived, a timer ended. Sparse perimeter ticks show density. A visible Now action restores live mode. Past receipts have distinct “History” labelling and cannot answer an expired or previously resolved approval.

**Why it fits.** It combines Pebble's temporal organising idea, Solar's time exploration and Playdate's reversible control. The rim gives time a physical path, while the centre shows one legible event rather than a tiny scrolling log.

**LVGL / data / cost: M locally, H for trustworthy history.** A bounded event array, timestamp index and cached receipt view are simple. Current session snapshots are **not an event history**, and the protocol has no general history query. A local prototype can show only events observed since connection, explicitly labelled incomplete and capped. A durable/multi-device product needs hub retention, event identity, timestamps and paging specified in `protocol.md` before both sides change. Future timer marks must represent actual known schedules, never predicted agent finish times.

**Risks / acceptance.** Event time versus arrival time, reconnect gaps, retention/privacy and confusion between past and live. Test whether the owner can always identify mode and return to Now. Do not quietly reconstruct history from `updated` fields or keep unbounded logs in PSRAM/flash.

### 6. Pocket Compass — four contextual shortcuts that teach their directions

**Interaction.** Long-press a safe content area, or tap visible Actions. Four broad directions appear: for example Agents, Home, Timers and Play. Drag highlights a labelled destination centrally; moving inward cancels. Release may open that destination. For any operation that changes external state, release only opens its preview/detail and a separate action commits. No approval, unlock or message-send shortcut lives here.

**Why it fits.** A one-level marking menu uses the circle and can become fast through spatial memory. One consistent set is more learnable than four different long-press meanings scattered through every app.

**LVGL / data / cost: M.** Four cached glyphs/labels, directional sectors, a centre cancellation zone and a recogniser that cannot also scroll. Treat wedges as a selection map, not four glass panels: use plain content highlights with at most the permitted glass controls. Keep the standard back/close button accessible. No hub change for navigation shortcuts.

**Risks / acceptance.** Finger occlusion, accidental long-press, edge truncation and memory burden. Four is an initial choice; the cited studies do not certify its accuracy on Orb. On large text, switch to a conventional action list. Keep destinations stable across normal use and provide a practice preview. Do not require experts to wait for a long animation, or novices to memorise an invisible gesture.

### 7. Desk / Hand — two attention states, one continuous object

**Interaction.** On the desk, a quiet clock presents a large time and aggregate attention count. A deliberate pickup wakes it into a richer glance; tap to reveal sensitive detail and choose a task. Set it down and let inactivity return to the quiet view. Face-down silences and sleeps according to existing policy. A visible Quiet/Active control gives the same choices without motion.

**Why it fits.** A desk object should be readable across the desk and become more detailed when held. This borrows wrist raise and Farsight's hierarchy, while avoiding the false claim that motion equals distance or authentication. A tiny original character can wake/stretch once, then settle away from the information.

**LVGL / data / cost: M.** Two bounded layouts, current attention summary, pickup state machine, filtered gravity and event-driven invalidation. Keep sensitive text redacted until deliberate reveal. Do not let a continuously changing IMU force perpetual redraw; thresholds, settling and cooldown need measurement. No new protocol field is required for presentation alone.

**Risks / acceptance.** Desk bumps, carrying, an angled stand and a charging cable can resemble pickup. Returning to a resting mode must not dismiss a pending card or discard a recording. Test false wake frequency and battery effects; allow disabling motion wake. Reduce Motion removes the transition/parallax but preserves the readable state change.

### 8. Home Satellites — select one room, adjust one real thing

**Interaction.** Home shows a few favourite entity/room tokens. Select “Desk lamp” to enter its dial: a large centre value, a simple ring, a named unit and ± controls. Rim dragging previews brightness; Apply sends once and leaves “pending” visible until the actual result. The return path leads to the same entity. Thermostats use the resolved display unit; sensor-only objects never become adjustable.

**Why it fits.** Nest and M5Dial suggest a delightful small instrument for one scalar. Room/entity context prevents a generic circular slider from changing the wrong lamp. A snap into the accepted value provides closure without lighting up the whole display.

**LVGL / data / cost: M.** Use `home.snapshot/update`, writable keys and `home.set`; keep values as preview versus last confirmed. The hub's accepted response and later authoritative updates govern settlement. Temperature wire values remain Celsius; convert for display and back correctly. Quantisation/min/max ultimately follow the bridge's accepted normalisation. Do not add local favourites editing without the protocol/owner preference workflow.

**Risks / acceptance.** External changes during dragging, write timeouts, unknown values and offline mode. Disable writes when unavailable/unreachable, retain unknown as unknown and never replay a timed-out toggle/scene automatically. Locks get their existing dedicated detail with explicit one-second unlock hold, not this dial or a radial menu. Scene acceptance is not a guarantee that every physical accessory reached its target.

### 9. Voice Spool — speech with a visible recipient and a deliberate send

**Interaction.** Focus a message-capable agent and hold BOOT. A mic symbol, recipient and elapsed recording arc stay visible. Release stops capture; the spool becomes a clearly labelled transcription-wait state, then a stable transcript page. The owner reads and chooses Send or Cancel. Quiet/no-speech failure invites another try. A short settling motion after accepted delivery closes the interaction.

**Why it fits.** Rabbit/TP-7 make voice physically immediate; the spool makes recording bounded and tangible. The circular arc is honest because elapsed time is known, unlike an agent's completion percentage. Centre text and explicit recipient prevent “who am I talking to?” ambiguity.

**LVGL / data / cost: M.** Existing `voice.begin/end/cancel/result/error` and confirmed `message.send` supply the lifecycle. The arc can show elapsed time against the protocol's 60-second capture limit, with a numeric duration/limit; no waveform is needed. If a waveform is added, it reflects measured audio amplitude only, not comprehension or speech validity. Use cached glyphs and a few arc updates, with audio buffers owned by the existing services.

**Risks / acceptance.** Stale target, `can_message:false`, Dot `not_listening`, disconnect, transcription latency, >280-byte result, cancellation races and acoustic feedback. Recording and transcription must not look the same. Never auto-send on release or animate successful delivery before acknowledgment. Large-text transcript pages remain stable. Provide visible start/stop recording controls as the gesture alternative, subject to existing service support, and test simultaneous capture/network/render memory.

### 10. Marble Conservatory — a circular toy that leaves a quiet keepsake

**Interaction.** Open Play and tip Orb to roll one marble through a circular, rotating-gate garden. A rim drag or visible hold-to-rewind control rewinds **the marble's own recorded path** while gates continue forward, creating a simple timing puzzle. Finish a short stage to place one original flat flower/stone in an optional clock-side garden. The owner can also plant through play without any coding activity. The toy pauses and exposes a visible exit when interrupted.

**Why it fits.** It combines actual tilt, round boundaries and continuous time control into a rule that belongs to the object. A small persistent garden borrows Pikmin's memory and Tamagotchi's attachment without needing a needy pet, productivity score or huge game world. This extends the Marble direction in HANDOFF rather than assuming its renderer is finished.

**LVGL / data / cost: M–H.** One marble, a few analytic collision shapes, a fixed-step simulation, a **bounded** path ring buffer and cached original sprites. Start with a static maze/tilt stage; add rewind only after responsiveness is demonstrated. Example storage calculation: 10 seconds × 30 samples/s × 8 bytes/sample is 2,400 B of path data before metadata—not a measured total game budget. Persist only a tiny versioned keepsake record at stage completion, not every frame. Entire toy/clock decoration works offline; it needs no agent-history protocol.

**Risks / acceptance.** Tilt calibration, grip fatigue, physics jitter, flash-write frequency, animation cost and reduced-motion access. Define deterministic rewind release: restore velocity deliberately and resolve a marble path intersecting a gate that has moved, rather than ejecting the marble unpredictably. Offer visible touch steering and a non-motion puzzle variant; keep controls large and the scene sparse. Notifications must not cause accidental actions on resume. No glow, particles obscuring controls or perpetual garden animation. Never award flowers for approving commands, penalise Deny, or imply a game event proves an agent's work succeeded.

## 8. Anti-patterns and a practical evaluation order

### Anti-patterns

| Avoid | Why it fails here | Prefer |
| --- | --- | --- |
| A phone dashboard clipped to a circle | Pole clipping, wasted side space and too many tiny values. | Centre focus, chord-aware blocks, a few semantic arcs. |
| Making every object radial | Labels become hard to read and navigation becomes an aim test. | Straight text in the centre; polar structure only where it represents a relation. |
| Eight or twelve tiny rim commands | Angular width, shallow radial depth, occlusion and memory all hurt selection. | A continuous annulus or a small stable action menu with a clear preview. |
| Quoting “infinite edge targets” | Fingers can leave the sensor; case contact is not input. | Inset, substantial hit regions tested on the device. |
| Hiding all navigation behind long-press or gesture | New users cannot discover it; motor/access limitations block it. | Visible Actions, Back, Open, ± and mode controls. |
| Letting rim, Back, swipe-up and row drag all win | One touch can trigger several interpretations or commit after a mode switch. | One captured recogniser and deterministic cancellation. |
| Rotating everything forever | Distracts, consumes energy, harms target stability and becomes an agent spinner. | Stable slots and brief event/input-driven movement. |
| Showing “AI thought” as fake progress | Activity is not a completion fraction or evidence of correctness. | Actual state, last step, timestamp and truthful availability. |
| Tilt/pick-up/knock to approve, send or unlock | Accidental motion and ambiguous recognition become consequential commands. | Explicit readable decisions; the protocol's lock hold stays in lock detail. |
| Treating navigation away as Deny/Dismiss | Changes provider state without an explicit decision. | Preserve pending work and separate navigation from card options. |
| Tinting a risky approval as the obvious default | Encourages reflex confirmation and hides scope. | No default/tinted grant; Deny present; actual operation/turn scope visible. |
| A moving queue under a finger | A newly arrived item can inherit an old tap. | Stable semantic identity during contact, then reconcile the snapshot. |
| Text that rewraps as it scrolls around the circle | Line identity changes while being read. | Stable detail pages/blocks and short neighbour previews. |
| Constant live blur, rotating text or scaled full screens | Software rendering, PSRAM traffic and invalidation can overwhelm the integrated build. | Small cached elements, band-local composition and measured fallbacks. |
| Glass cards on glass backgrounds | Breaks Orb's content/control hierarchy and burns render budget. | Plain content, true black and a few control surfaces. |
| Neon, gradient sunsets, painted sheen or light bloom | Conflicts with the mandatory design language and can reduce clarity. | Flat meaningful colour, original sparse art and the approved material renderer. |
| Literal Apple fonts/icons or branded characters | Reference quality does not provide a non-Apple firmware/character licence. | Inter, Framework7 and original characters/assets. |
| Sound-only detents or success | Quiet mode, deafness and ambient noise erase feedback. | Visible value/state first, optional brief sound. |
| Pickup reveals private text | Physical motion is not authentication or viewing intent. | Aggregate wake glance and deliberate detail reveal. |
| Pet guilt, approval rewards, coding streak pressure | Deliberation and rest become emotionally penalised. | Optional memory/play, never reinforcement of permission grants. |
| Demo video as a frame-rate/battery claim | Different hardware, scene and services invalidate the comparison. | Capture touch/render/DMA/memory/power on the integrated dev board. |

### Suggested order of exploration

| Stage | Concrete prototype | What it resolves |
| --- | --- | --- |
| **First: readability and control** | Focus Lens plus an explicit timer/light dial using Rim as Crown; keep ordinary drag/± alternatives. | Does the circle make reading and adjustment better in both desk and held use? Can input ownership remain predictable? |
| **Second: identity** | Agent Orrery with at most four prominent bodies and list fallback; compare with Focus Lens. | Does spatial identity help recognise work, or merely add navigation? |
| **Third: continuity** | One Iris Rooms transition, limited to a cached token and a bounded scene. | What is the real compositor/memory/frame cost under integrated load? |
| **Fourth: voice and home trust** | Voice Spool and one named Home dial, exercising all pending/error/offline states. | Are capture, transcript confirmation, accepted state and actual delivery understandable? |
| **Fifth: personality** | Desk/Hand state change and a short Marble Conservatory stage. | Can pickup and play feel responsive without false wake, excessive redraw or interruption loss? |
| **Later: expert/history layers** | Pocket Compass, then Time Tide only with an explicit history contract. | Do the shortcuts beat visible navigation, and is the history complete enough to be trusted? |

This order does not authorise implementation or protocol changes. It avoids requiring a new renderer, a history backend and a complex recogniser before the basic round experience can be judged.

### Evidence to capture when prototyping

Run a short comparison with the owner, then broaden if needed. Include desk use at normal viewing distance, held use with both hands, a charging stand, thumb/fingertip approaches and silent mode. Use representative long names, multiple approvals, missing values, a busy hub and disconnected state. Do not ask someone to speed-read security decisions to improve a time score.

Record:

- **Understanding:** can the person identify what needs attention, which entity is focused, whether a value is preview/confirmed and whether time is live/history?
- **Input errors:** wrong destination, accidental commit, cancelled intended gesture, touch lost near the edge, rim/Back confusion, overshoot and false pickup. Keep a confusion table, not just a recogniser's ideal-trace pass rate.
- **Task effort:** steps, corrections and time for checking one agent, opening the oldest waiting request, changing one lamp, confirming/cancelling speech and returning home. Compare with the current UI; subjective fun/readability matter alongside speed.
- **Rendering:** frame-time distribution through final DMA, pacing during gesture reversal, touch-to-visible response, dirty area, cache allocation/eviction, minimum internal RAM and audio/network concurrency. Capture on the development board; do not infer from the simulator.
- **Idle/power:** redraws and wake frequency plus actual current/energy measurements before claiming savings. Test charging and battery use separately.
- **Design gate:** own simulator/device PNGs at default and AX3 text and Reduce Transparency for Face, list and card; verify all checklist headings, Reduce Motion, no clipping and physical-panel legibility. Independent UI review is still required before merge.

No firmware was built, board flashed, service changed or screenshot gate passed for this research-only document. The proposals deliberately leave empirical thresholds and performance claims open until captured. The highest-value next question is whether **a broad rim control plus a stable readable centre** feels better than ordinary touch scrolling; the rest of the personality can grow around that result.
