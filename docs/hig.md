# Orb HIG: an Apple-style design spec for the 1.75" round display

How to apply Apple's Human Interface Guidelines, and the Liquid Glass design language that came with iOS/watchOS 26, to this device: a 466×466 px round AMOLED (about 266 ppi) with touch, two side buttons (PWR, BOOT), a 6-axis IMU, a mic and a speaker. It has no Digital Crown and no haptics. The device's jobs are agent sessions, approvals, push-to-talk replies, the clock face, notifications, Control Center, settings and small games.

Sources were read on 2026-10-26: the HIG pages (via their JSON), the Liquid Glass technology overview, the SwiftUI docs, and the WWDC25 sessions 219, 356 and 334 plus WWDC26 sessions 102 and 250. Every rule carries one of two tags:

- **[A]**: Apple states it (paraphrased).
- **[I]**: our inference or translation for this hardware. Tune these on the device.

Two Apple updates arrived during 2026:

- **WWDC26 / the 27 releases.** Glass diffuses complex content more strongly, gains a darkened edge and brighter specular highlights, and a new user slider runs from "ultra clear" to "fully tinted".
- **HIG.** The Design principles page returned (June 2026), the scroll-edge guidance changed (June 2026), and the watchOS widget specs changed (Oct 2026).

I found no watchOS-specific design changes for 27.

---

## 1. Principles

- **[I] Precedence.** This HIG is Orb's design standard. Briefs, sketches and the design gate must follow it; where they conflict, this document wins. The Orb-specific adaptations below govern the shipped interface even when an Apple behaviour is described for context.
- **[A] Hierarchy, harmony, consistency.** Liquid Glass exists to establish hierarchy, create harmony with the hardware's shape, and keep things consistent across devices.
- **[A] Content first.** Glass draws attention to the content under it. It forms a separate functional layer, for controls and navigation only, that floats above the content.
- **[A] Glass never goes in the content layer.** Lists, cards and backgrounds use standard materials or plain fills. One exception: a slider or toggle knob becomes glass only while it's being touched.
- **[A] Use glass sparingly.** Put it only on the most important functional elements, and limit how many glass effects are on screen at once.
- **[A] Express hierarchy through layout and grouping, not decoration.** Remove extra backgrounds, borders and tints from bars and buttons.
- **[A] The design principles (HIG, June 2026):** purpose, agency, responsibility, familiarity, flexibility, simplicity, craft, delight. Three matter most here:
  - Agency: make every action easy to undo.
  - Responsibility: interrupt only before real mistakes. That covers approvals.
  - Delight: it isn't decoration.
- **[A] watchOS-specific:**
  - Interactions should be glanceable, fit on one screen and finish in under a minute.
  - Keep navigation hierarchy shallow.
  - Use notifications and complications for timely information.
  - Use background colour only when it says something.

Sources: [Liquid Glass overview](https://developer.apple.com/documentation/technologyoverviews/liquid-glass) · [Materials](https://developer.apple.com/design/human-interface-guidelines/materials) · [Design principles](https://developer.apple.com/design/human-interface-guidelines/design-principles) · [Designing for watchOS](https://developer.apple.com/design/human-interface-guidelines/designing-for-watchos) · [WWDC25 356](https://developer.apple.com/videos/play/wwdc2025/356/)

## 2. Liquid Glass

### 2.1 What Apple says it is [A]

- **Lensing, not scattering.** Earlier materials blurred, or scattered, light. Liquid Glass bends and concentrates light in real time, so its edges show its shape while the content behind stays visible.
- **Highlights layer.** Virtual light sources create highlights that follow the shape's geometry. In some cases the highlights move with device motion.
- **Adaptive layers.**
  - Shadows get stronger over text and weaker over flat light backgrounds.
  - Tint and dynamic range shift to keep labels legible.
  - Small elements flip between a light and a dark look depending on what's behind them, and their symbols flip with them.
  - Large elements (menus, sheets) never flip. They are thicker instead: deeper shadow, stronger lensing, softer scattering, and more opacity.
- **Two variants, never mixed.**
  - *Regular* blurs the background and adjusts its luminosity. It works anywhere and suits anything with a lot of text.
  - *Clear* is permanently more transparent and has no adaptive behaviours. Use it only when all three hold: the background is rich media, a dimming layer won't hurt the content, and the content on top is bold and bright. Over bright content, add a **35 % black** dimming layer.
- **Motion and morphing.**
  - On touch, Apple describes glass as flexing and energising with light, including a spreading glow. **Orb adaptation [I]: no touch glow.** Use geometric press response and a bounded material/rim change, as specified in §2.2.
  - Glass elements morph between states as one floating plane. A menu pops open out of the button that opened it.
  - Glass appears and disappears by **ramping the lensing up or down, not by fading its opacity**.
- **No glass on glass.** Anything placed on glass uses fills, transparency and vibrancy.
- **Tint only the primary action, and tint its background.** Apple's tinting maps a range of tones to the brightness underneath, like stained glass. A solid fill breaks the material. If you want colour, put it in the content layer.
- **Scroll edge effect.**
  - It is not decorative. It dissolves content where it scrolls under floating controls. Over dark content it becomes a subtle dimming instead.
  - Use one per view. *Soft* is the default style; *hard* is for pinned headers.
  - Use it only where floating UI sits over the scroll view.
  - In the 27 releases, standard toolbars add a uniform band at the top.
- **Shapes.** Three shape types:
  - Fixed: a constant corner radius.
  - Capsule: radius = height ÷ 2.
  - Concentric: radius = parent radius − padding. A concentric shape can take a fallback radius for when it stands alone.
  - Near a device edge, use a capsule with extra margin. Avoid corners that look pinched or flared.
- **Steady state.** At rest, for example on launch, content and glass shouldn't overlap.

Sources: [WWDC25 219 Meet Liquid Glass](https://developer.apple.com/videos/play/wwdc2025/219/) · [WWDC25 356](https://developer.apple.com/videos/play/wwdc2025/356/) · [Materials](https://developer.apple.com/design/human-interface-guidelines/materials) · [Color › Liquid Glass color](https://developer.apple.com/design/human-interface-guidelines/color) · [Scroll views](https://developer.apple.com/design/human-interface-guidelines/scroll-views) · [Applying Liquid Glass to custom views](https://developer.apple.com/documentation/swiftui/applying-liquid-glass-to-custom-views) · [Glass.clear](https://developer.apple.com/documentation/swiftui/glass/clear) · [WWDC26 Platforms State of the Union](https://developer.apple.com/videos/play/wwdc2026/102/)

### 2.2 Rendering on the ESP32-S3 (LVGL 9, software only) [I]

Apple publishes no numbers for glass. Every value below is a starting point to tune.

The budget comes from Apple's own advice to limit how many glass effects are on screen at once:

- At most **4 glass shapes** on screen.
- Live glass covers at most **about 20 % of the screen** per frame.
- Each shape is drawn as a signed-distance-field (SDF) shape inside its bounding box.

**Per-frame for small glass (buttons, toolbars, page indicator):**

1. **Sample.** Copy the content layer under the shape's bounding box plus 16 px, downsampled 4×.
2. **Regular blur.** Run 3 box-blur passes of radius 3 at ¼ resolution. That approximates a Gaussian with σ ≈ 20 px (about 12 pt) at full resolution.
   - Work at ≥ 8 bits per channel and **dither when writing RGB565** (4×4 Bayer or blue noise). Banding in dark blurs is the fastest way to look cheap.
3. **Lensing.**
   - Edge band: 6 pt (10 px) wide. Sample the background through a per-shape displacement table of int8 (dx, dy) pairs, up to 4 pt (6–7 px), so content compresses towards the rim the way it does under a convex edge.
   - Interior: magnify 1.00–1.03×.
   - Precompute the table once per shape size.
4. **Adaptive tone.**
   - Measure the mean luminance and the variance of the ¼-resolution sample.
   - Compress its dynamic range.
   - Flip to the light look above a mean luminance of 0.60 and back below 0.45, with about 150 ms of hysteresis. Labels flip with the glass.
   - Over pure black, regular glass must still read as an object. Lift it to roughly **#1C1C1E–#2C2C2E** (systemGray6/5, dark).
5. **Edge and highlights.**
   - A 1 px inner edge at 15–25 % black, which is the darkened edge from the 27 releases.
   - A 1–1.5 px specular rim with intensity = Imax·max(0, n·l)^p, where p ≈ 4–8 and Imax ≈ 40–60 % white, added on top.
   - An opposite rim at about 30 % of the main one.
   - The light vector l comes from the IMU's gravity vector projected onto the screen, low-pass filtered (τ ≈ 150 ms) and clamped to ±30°.
   - The rim normals n are precomputed per shape.
6. **Shadow.**
   - Offset y = 1–2 pt, blur 8 pt.
   - Alpha scales with the background's variance: 0.15 over flat content, 0.35 over text.
   - Skip it when the background is black, since it would be invisible there.

**Precomputed or cached:**

- Per-shape SDF, coverage, normal and displacement tables, held in PSRAM.
- Font bitmaps.
- Scroll-edge alpha ramps.
- **Large glass** (sheets, menus, Control Center panels):
  - Blur a snapshot when the sheet opens and re-blur only when the content under it changes.
  - Use stronger blur and lensing and a deeper shadow than small glass, and never flip light/dark.
  - Full-screen live blur at 60 fps is not achievable on this chip.

**Clear variant:**

- No blur, or ≤ 1 px at ¼ resolution, and no adaptive tone.
- Add a 35 % black dimming layer under bright content.
- Use it only over games or media.

**Touch:**

- Design 2.0 controls use `orb_kit_press_apply()`: contract to 94% in 80 ms,
  brighten the existing glass, then return on a 160 ms critically damped spring.
  Reduce Motion changes only the material. Retire a cancelled contact immediately:
  reset its visual state and reject activation until a fresh press.
- Change the existing material/rim within the control's bounds. Do not add a radial glow, halo or illuminated background at the touch point.
- Let the shape stretch elastically while it's dragged.

**Morph, materialise, tint:**

- **Morph.** Smooth-union the SDFs with k equal to the container spacing (12–16 pt) and move the shape centres on springs. This matches Apple's rule that the container spacing decides when shapes blend.
- **Materialise.** Ramp one "glass amount" g from 0 to 1 that scales displacement, blur and highlight together; labels follow g. Plain alpha fades are not allowed.
- **Tint (primary action only).** Keep the hue fixed and let brightness and saturation follow the luminance underneath, within ±15 % of the base colour. Never use an opaque fill.

**Scroll edge:**

- The canvas is black, so the soft style is an alpha ramp that fades content to black under the top and bottom floating controls: the bar's height plus 8 pt.
- Apply it in the content compositor and only where floating controls exist. It's a content mask, not a background treatment.

**What not to fake badly:**

- **No content-independent frost.** A flat translucent grey shape is iOS-7 frosting, not Liquid Glass.
- **No baked sheen.** A painted-on gradient sheen that ignores the content underneath reads as fake glass.
- **No static highlights.** Highlights that never move look printed on, and they also burn in on AMOLED.
- **Lensing stays.** If performance forces a cut, reduce how often the blur updates. Keep lensing and highlights, since those are what make it read as glass.

## 3. Units, targets, safe area

- **Device density [I]:** 466 px ÷ 1.75 in = **266.3 ppi**. This assumes 1.75" is the active-area diameter, so confirm it against the panel datasheet.
- **Apple Watch:** 2 px/pt at about 326 ppi, so 1 pt = 2/326 in = 0.156 mm.
- **px per pt on this device [I]:** 266.3 × (2 / 326) = **1.634 px/pt**. Use `px = round(pt × 1.634)`.
  - The screen is **285 pt across**.
  - The inscribed square is 330 px (202 pt).
  - This keeps physical size equal to Apple Watch at wrist distance. For desk viewing at 40–60 cm, consider a global scale of 1.15–1.25.

| Apple rule | pt | px here |
|---|---|---|
| Default control size [A] | 44×44 | **72×72** |
| Minimum control size [A] | 28×28 | **46×46** |
| Padding around bezelled elements [A] | ~12 | 20 |
| Padding around borderless elements [A] | ~24 | 39 |
| Default text / minimum text [A] | 16 / 12 | 26 / 20 |
| Smart Stack widget, 46 mm [A, Oct 2026] | 194×80.5 | 317×132 |

**[A] watchOS layout rules:**

- At most 3 glyph buttons or 2 text buttons in a row.
- Prefer full-width primary buttons.
- Give one- and two-line buttons in a stack the same height.

**[I] Round safe area:**

- **Rim margin:** 6 pt (10 px) from the bezel to any glass shape; 10 pt (16 px) to any text.
- **Usable width:** for a line box at height y, the width is 2·√(r² − (y − 233)²), with r = 233 − margin. Check it at both the top and bottom edge of the line and use the narrower.
- **Shapes near the rim:**
  - Circles and capsules, with the extra margin Apple recommends at device edges.
  - Full-bleed panels use the screen circle inset by d, which gives a concentric radius of 233 − d px.
  - Nested shapes use radius = parent radius − padding.

## 4. Typography

**[A] Fonts and defaults:**

- watchOS uses SF Compact; complications use SF Compact Rounded.
- System fonts have dynamic optical sizes and adjust tracking at every point size.
- Avoid Ultralight, Thin and Light. Prefer Regular through Bold.

**Default scale [I]:** watchOS **xLarge**, which Apple ships as the default on 44/45/49 mm watches. This screen is larger than any of those.

| Style [A] | Weight [A] | pt / leading [A] | **px / leading** | Tracking [A] (1/1000 em) | Tracking px |
|---|---|---|---|---|---|
| Large Title | Regular (bold variant: Bold) | 40 / 42.5 | **65 / 69** | −20 | −1.3 |
| Title 1 | Regular | 38 / 40.5 | **62 / 66** | −20 | −1.2 |
| Title 2 | Regular | 30 / 32.5 | **49 / 53** | −14 | −0.7 |
| Title 3 | Regular | 20 / 22.5 | **33 / 37** | 0 | 0 |
| Headline | Semibold | 17 / 19.5 | **28 / 32** | −4 | −0.1 |
| Body | Regular | 17 / 19.5 | **28 / 32** | −4 | −0.1 |
| Caption 1 | Regular | 16 / 18.5 | **26 / 30** | 0 | 0 |
| Caption 2 | Regular | 15 / 17.5 | **25 / 29** | +4 | +0.1 |
| Footnote 1 | Regular | 14 / 16.5 | **23 / 27** | +14 | +0.3 |
| Footnote 2 | Regular | 13 / 15.5 | **21 / 25** | +16 | +0.3 |

The tracking values are SF Compact's; re-tune them for the substitute font.

**Bold variants [A].** For emphasis, Large Title becomes Bold and every other style becomes Semibold.

**Dynamic Type:**

- **[A]** Support text enlargement to at least 140 % on watchOS. Keep hierarchy as sizes grow and keep truncation to a minimum.
- **[I]** Offer three steps, with Body at:
  - Default: 28 px.
  - Larger (xxxLarge): 31 px.
  - Accessibility (AX3): 38 px.
- **[I]** Prerender a bitmap font (4 bits per pixel) for each step and weight.
- **[I]** Bold Text moves every weight up one step.

**Licensing [A, quoted from the licence text on developer.apple.com/fonts]:**

- The SF Compact licence covers only "creating mock-ups of user interfaces to be used in software products running on Apple's watchOS". It forbids non-Apple operating systems.
- The SF Pro licence says the same for iOS, macOS and tvOS.
- Neither may ship in this firmware.
- The HIG requires respecting the SF Symbols terms, including the ban on using symbols in icons or logos. Treat the symbols as Apple-platform-only [I: confirm with the licence bundled in the SF Symbols app].

**Substitutes [I]:**

- **Inter** (SIL OFL), with its continuous optical-size axis (14–32) set to each size's point size, to imitate SF's dynamic optical sizes. Turn on `tnum` (tabular figures) for the clock and counters.
- **Nunito** (OFL) as the rounded companion for complications.
- Icons: **Framework7 Icons 5.0.5** (MIT), with SF-style shapes and names. Offset its filled outlines to match Inter's stem width at each size and weight. Keep existing `ORB_ICON_*` names and codepoints, cap-height centring, inline fallback and shared widths for state changes. Retain Phosphor only for the exceptions listed in `orb_fonts/README.md`.
- **Live status [I]:** use `orb_status` for battery charge and Wi-Fi strength. The battery has a rounded body, terminal nub and proportional fill; charging is green (#30D158) with a knocked-out bolt, low power yellow (#FFD600), and charge at or below 20% red (#FF4245). Tiny tabular digits inside the battery are an exception to the normal text minimum, following the iOS status-bar convention; show a normal-sized percentage nearby when it is essential information. Wi-Fi uses concentric fan arcs with inactive bars at 30% opacity and a slashed disconnected state. Thresholds and precedence are documented in the component README.

Sources: [Typography](https://developer.apple.com/design/human-interface-guidelines/typography) · [Accessibility](https://developer.apple.com/design/human-interface-guidelines/accessibility) · [Apple Fonts](https://developer.apple.com/fonts/) · [SF Symbols](https://developer.apple.com/design/human-interface-guidelines/sf-symbols)

## 5. Colour

**[I] Design 2.0 kit palette.** New surfaces use `orb_kit.h` and
`docs/design/2.0/Main.dc.html`: Claude/orange #FF9F0A, Codex/blue #0A84FF,
Dot/indigo #5E5CE6, Home/yellow #FFD60A, Time/teal #64D2FF, Play/green
#30D158, Sound/pink #FF375F, Calendar/red #FF453A and system grey #8E8E93.
These override the older accent table below. Secondary remains #EBEBF5 at
60% opacity; never substitute a preblended grey such as #8D8D93.

**[A] Rules:**

- watchOS has no Dark Mode setting: the UI is always dark.
- Use system colours by meaning, not by look. Values can change between releases.
- Provide increased-contrast variants.
- Don't convey anything by colour alone.
- Avoid full-screen colour on views that stay up a long time.

The system colours were re-tuned for Liquid Glass in June 2025. The values in `docs/design.md` (#0A84FF and so on) predate that and should be updated.

| [A] Colour (dark) | Default | Contrast on black | Increased contrast | Contrast |
|---|---|---|---|---|
| red | #FF4245 | 6.1:1 | #FF6165 | 7.1:1 |
| orange | #FF9230 | 9.4:1 | #FFA056 | 10.4:1 |
| yellow | #FFD600 | 14.9:1 | #FEDF43 | 15.9:1 |
| green | #30D158 | 10.4:1 | #4AD968 | 11.4:1 |
| mint | #00DAC3 | 11.8:1 | #54DFCB | 12.8:1 |
| teal | #00D2E0 | 11.3:1 | #3BDDEC | 12.7:1 |
| cyan | #3CD3FE | 11.9:1 | #6DD9FF | 13.0:1 |
| blue | #0091FF | 6.5:1 | #5CB8FF | 9.8:1 |
| indigo | #6D7CFF | 6.0:1 | #A7AAFF | 9.8:1 |
| purple | #DB34F2 | 5.8:1 | #EA8DFF | 9.8:1 |
| pink | #FF375F | 6.0:1 | #FF8AC4 | 9.7:1 |
| brown | #B78A66 | 6.8:1 | #DBA679 | 9.7:1 |

**Greys (dark) [A]:**

| Grey | Default | Increased contrast |
|---|---|---|
| gray | #8E8E93 | #AEAEB2 |
| gray2 | #636366 | #7C7C80 |
| gray3 | #48484A | #545456 |
| gray4 | #3A3A3C | #444446 |
| gray5 | #2C2C2E | #363638 |
| gray6 | #1C1C1E | #242426 |

**Labels [I: UIKit values, not shown on the HIG page]:** white; secondary #EBEBF5 at 60 % (6.4:1); tertiary at 30 % (2.3:1, so never for essential text).

**[A] Contrast minimums:**

- 4.5:1 for text up to 17 pt (28 px).
- 3:1 at 18 pt (29 px) and larger, or for bold text.
- Aim for 7:1 for custom colours and small text in dark UIs.

**[I] Contrast on glass:**

- White on a blue-tinted button is 3.2:1, so prominent button labels must be Semibold or heavier.
- Measure contrast against the glass's *computed* backdrop each frame. The flip and dimming rules in §2.2 exist to keep it at 4.5:1 or better.

**[I] Identity discs and accessory colours:**

- Launcher icons use a flat identity-colour disc and a Framework7-style glyph. Choose black or white glyph ink adaptively for contrast against that disc; white is not mandatory on bright yellow, orange, green or actual lamp colours. Preserve glyph shape and text identity independently of hue.
- Ordinary task views use at most two meaningful accents and at most one tinted glass action. The launcher catalogue is an explicit exception to the two-accent limit because each disc identifies a different app. Home's accessory-state colours are another exception because they report the actual light or device state. Design 1.0 (owner decision, 2026-10-28) also permits provider identity colours on Agents/Sessions and green done/red failed step glyphs alongside the provider accent. Notifications use their source identity colours too: Home yellow, Claude orange, Codex and System/Updates blue, Dot indigo (owner's board-fix request, 2026-10-28). These content exceptions do not permit multiple tinted glass controls, coloured paragraphs, gradients or glows.

**[A] Long-look notifications.** Use white at 18 % for the content background.

**[A] Smart Stack cards.** Default to black. Colour a card only when the colour means something.

Sources: [Color](https://developer.apple.com/design/human-interface-guidelines/color) · [Dark Mode](https://developer.apple.com/design/human-interface-guidelines/dark-mode) · [Accessibility](https://developer.apple.com/design/human-interface-guidelines/accessibility)

## 6. Motion

**Spring values from Apple's docs [A]:**

- `Animation.default`, since watchOS 10, is a spring with response 0.55 and damping fraction 1.0, which is critically damped.
- `spring()` defaults to response 0.5 and damping 0.825.
- `interactiveSpring` is response 0.15, damping 0.86 and blend 0.25. Use it for finger tracking.
- `spring(duration:bounce:)` defaults to 0.5 s with no bounce.
- The presets: `smooth` has no bounce, `snappy` a small bounce, `bouncy` more.

**How to choose [A, WWDC23 "Animate with springs"]:**

- Pick the duration first, then add bounce if needed.
- About 0.15 bounce feels brisk; about 0.3 is noticeably bouncy.
- Be careful above about 0.4.
- If unsure, use bounce 0.

**[A] Behaviour:**

- Springs keep their velocity when retargeted, so motion is always interruptible and a gesture hands its speed over to the animation.
- Never wait for a spring to settle before changing the UI. Use the perceptual duration instead.
- Keep feedback motion brief, avoid motion on frequent interactions, and let people cancel any animation.
- Games run at a consistent 30–60 fps.

**Implementation [I]:**

- Per animated property, keep a position and a velocity.
- Use k = (2π/d)², c = 4πζ/d, and ζ = 1 − bounce, or 1/(1 + bounce) when bounce is negative. Apple's example of 0.5 s with 0.3 bounce gives k = 157.9 and c = 17.6, which matches.
- Integrate at a fixed 240 Hz substep.
- Presets:

| Use | Duration | Bounce |
|---|---|---|
| Default | 0.5 s | 0 |
| Morphs and toggles | 0.5 s | 0.15 |
| Playful (games) | — | 0.3 |
| Finger tracking | response 0.15 | ζ 0.86 |

**[A] Reduce Motion:**

- Tighten springs to remove bounce.
- Track gestures directly.
- Don't animate depth.
- Replace x/y/z slides with fades.
- Don't animate into or out of blurs.
- Glass reduces some effects and drops its elastic behaviour.
- **[I]** Freeze the IMU light, and replace materialising and morphing with a 150 ms cross-fade.

**[A] Reduce Transparency:**

- Glass becomes frostier and hides more of what's behind it; backgrounds should become opaque.
- **[I]** Use about 4× the blur radius and lay an opaque surface on top: 90 % #1C1C1E for small glass, 100 % for sheets. Clear glass becomes regular. Keep the highlight rim.

**[A] Increase Contrast.** Elements become mostly black or white with a contrasting border **[I: 1 px at 50 % white]**, and colours switch to the increased-contrast column.

**[A, 27 releases] Glass slider.** Users choose anywhere from ultra clear to fully tinted. **[I]** Offer Settings › Glass: Clear / Default / Tinted, which changes the blur, the opacity of the lift layer, and the dimming.

Sources: [Motion](https://developer.apple.com/design/human-interface-guidelines/motion) · [Animation.default](https://developer.apple.com/documentation/swiftui/animation/default) · [spring(response:…)](https://developer.apple.com/documentation/swiftui/animation/spring(response:dampingfraction:blendduration:)) · [interactiveSpring](https://developer.apple.com/documentation/swiftui/animation/interactivespring(response:dampingfraction:blendduration:)) · [Spring](https://developer.apple.com/documentation/swiftui/spring) · [WWDC23 10158](https://developer.apple.com/videos/play/wwdc2023/10158/) · [Accessibility](https://developer.apple.com/design/human-interface-guidelines/accessibility) · [WWDC25 219](https://developer.apple.com/videos/play/wwdc2025/219/)

## 7. Navigation and components (watchOS 26, adapted to a circle)

**[I] Design 2.0 shared kit.** Build new surfaces with
[`orb_kit.h`](../firmware/components/orb_ui/include/orb_kit.h), using the
[migration guide](../firmware/components/orb_ui/README.md). Standard discs are
80 px, utility discs 56 px, row discs 40 px, artwork 180 px. Focus adds a
3 px white outline with a 4 px gap without scaling. Primary/secondary capsules
are 64/52 px tall. Rim titles/captions follow baseline radii 200/212; right
scroll position uses radius 206 and a 6 px stroke. All arc sliders share the
30 px knob. List rows remain chord fitted and 72 px tall. The kit's 94% press
response (§2.2) replaces the older swell for these controls. These component
sizes supersede the earlier 72 px default for the corresponding roles; a
40 px leading disc is content inside its 72 px row target.

**No Digital Crown.** **[A]** The Digital Crown is watchOS's main navigation input, and everything it does must have a visible response. **[I] Owner decision — Design 1.0, 2026-10-28:** vertical drag, the rim and twist scroll or step focus. The always-available physical buttons are the alternatives to navigation gestures: BOOT = Back (Apps on the Face); PWR short = lock into Always On. They satisfy the visible-alternative requirement on this hardware; do not add on-screen back, paging or scroll chevrons, redundant page dots or Control Center −/+ buttons. Preserve invisible semantic navigation actions. Never use IMU tilt for primary navigation.

- **Gesture ownership (owner decision, Gesture Model v3, 2026-10-28).**
  - **[I]** The DOWN point alone chooses ownership. Edge means distance from (233,233) ≥193 px, the outer 40 px of the 466×466 circle; the nearest top/bottom/left/right direction by angle (±45°) chooses the edge. Retain radial-versus-tangential arbitration and crown ownership in this band until lift/cancel.
  - **[I]** Inward from top opens Notifications; bottom opens Smart Stack; left is Back (including every layer); right opens Control Center. Track the finger with the existing commit, cancel and rubber-band response. Tangential travel remains the crown.
  - **[I]** Notifications dismisses upward from the bottom edge or with Back. Stack dismisses downward from the top edge or with Back. Control Center dismisses rightward from the left edge or with Back. Centre drags never open or dismiss layers, invoke Back, or move between places. Remove pull-at-list-top navigation; scrollable and non-scrollable content have the same ownership.
  - **[I]** Only Face and Always On keep centre navigation: left → Home, right → Agents, up → Stack, down → Notifications. A locked Always On Face consumes the first waking contact before these swipes become available. Compass long-press is unchanged. Inside content, centre drags remain local scrolls, taps, holds, value drags and carousels.
  - **[I]** A tap travels <10 px and lasts <500 ms. Once travel reaches the threshold, or the contact becomes a scroll/drag, it never clicks; remove its pressed state immediately without a flash. A scroll starting on a button or toggle scrolls its list without activation. Apply this to Home Rooms, room and accessory views, Settings, Time, Play, Sessions and Notifications as well as ordinary lists.
  - **[I]** Every Apps destination shares the same Back path for a left-edge swipe or BOOT: reverse iris to its launching token when available, otherwise slide right. Updates, Battery, Sound, Settings subpages, Time, Play, Home, Dot, Agents and Sessions cannot opt out.
- **Top-corner toolbar buttons.**
  - **[A]** These are glass, and the time and title move aside for them. The leading button is a standard back chevron or close X, never a text label. Titles stay under 15 characters.
  - **[I]** When a frame calls for toolbar actions, use 72 px circles centred at (101, 101) and (365, 101), on the 45° diagonals. BOOT supplies Back without an on-screen chevron (Design 1.0, owner decision, 2026-10-28).
- **Bottom toolbar.**
  - **[A]** Only one prominent, tinted primary action.
  - **[I]** Up to 3 glyph buttons along the bottom arc with the primary in the centre, or one full-chord capsule.
- **Scrolling toolbar button.** **[A]** For an important but secondary action, such as New Session, place it above the top of the list; it appears when people scroll to the top.
- **Smart Stack.**
  - **[A]** Use a control to perform an action, a widget for information, and a Live Activity for something with a start and an end. Order by relevance.
  - **[I]** A running agent turn is a Live Activity. Live session progress and talk rank first, with Claude and Codex usage at a glance. Approvals remain reachable as waiting cards. Cards are 317×132 px with concentric corners.
- **Lists.**
  - **[A]** Keep them short with the most relevant rows first and a "more" row. Rows are taller and sections rounder in the new design. Section headers use title-style capitalisation. Keep detail views short so people can page between them.
  - **[I]** Row width follows the chord at its **current centre while scrolling**. For height `h`, safe radius `R` and centre offset `dy`, a capsule's width is `h + 2√((R − h/2)² − dy²)`; its end circles, rather than its rectangular corners, fit the safe circle. Shell lists use `R = 209` (16 px text safety plus 8 px reserved travel), giving a 72 px capsule 418 px at the equator. Text retains at least 16 px clearance and each line box also fits its own top/bottom chord. Keep trailing values and switches anchored to the current trailing edge.
  - **[I]** Use the shared cached presentation: opt in with `orb_round_list_apply(list, &(orb_round_list_config_t){.margin = 24, .fade = 38, .stepped = true, .chord_fit = true})`. Build native rows at `2 * (233 − margin)` pixels, centred on the panel, with leading text insets and trailing children using `LV_ALIGN_RIGHT_MID`. Widths use conservative 4 px centre buckets and 8 px width steps. Explicit content changes call `orb_round_list_update()` while idle; scroll/draw never typesets or allocates row sources. Omitting `chord_fit` preserves the original helper for existing callers, including Home.
- **Home accessory grids.** **[I]** Show at most five 72 px accessory discs per page, with 20 px control spacing and room for the focused name and scene action. Continue additional accessories on a separate page. The earlier seven-disc sketch is superseded by this touch-safe layout; do not reduce targets or spacing to recover seven discs.
- **Sheets.**
  - **[A]** On watchOS a sheet is full screen and semi-transparent; its material blurs and desaturates what's under it. Use one only for a custom modal task. Close it with an X symbol in the top-leading corner. Show one sheet at a time, and support swipe-to-dismiss.
  - **[I]** Draw the sheet as standard material, not glass. Its buttons are the only glass.
- **Alerts.**
  - **[A]** A title of at most 2 lines, optional text, and at most 3 buttons. In a stack, the likeliest choice goes at the top and Cancel at the bottom. Avoid "OK" and use specific verbs. Use the destructive style only for destruction the person didn't ask for. Don't alert for undoable actions.
  - **[I]** Approvals of risky commands have **no default button**, which Apple recommends when people should read first, and "Deny" is always available.
- **Notifications.**
  - **[A] Short look:** shown on wrist raise, gone when the wrist drops, discreet (no sensitive titles).
  - **[A] Long look:** the sash at the top shows the icon and name, then the content, up to 4 actions, and Dismiss last.
  - **[A]** Double tap triggers the first nondestructive action.
  - **[I]** Picking the device up (IMU) counts as a wrist raise. BOOT is reserved for Back under the owner's 2026-10-27 physical-button revision. Put "Reply" first, never "Approve"; notification actions use visible controls.
- **Control Center.**
  - **[A]** Each control has a symbol, title and value. A toggle shows on and off symbols, tints its symbol when on, and animates the change. Redact the title and value when the device is locked. Security actions require authentication.
  - **[I]** Use 3 glass circles per row over a blurred snapshot of the screen, with no glass panel behind them.
- **Toggles and sliders.** **[A]** The knob turns into glass only while it's touched.
- **Page indicators.**
  - **[A]** Vertical pages are the preferred structure, one screen tall each. The indicator expands into a scroll bar on long pages.
  - **[I]** Render indicators only where the approved Design 1.0 frame includes them. They follow the rim; do not add generic page dots to other surfaces (owner decision, 2026-10-28).
- **Buttons.**
  - **[A]** Inline buttons are capsules with a material behind them. Every button needs a press state.
  - **[I]** Use the geometric press response and material/rim change from §2.2. Never add a touch glow.
- **Loading.**
  - **[A]** Avoid indeterminate spinners. If a load takes a second or two, a spinner beats a blank screen. For long jobs, promise a notification instead.
  - **[I]** Agent work shows its last step as static text.
- **Always On.**
  - **[A]** Dim the screen, redact sensitive content, keep the layout stable, and update rarely. Bring motion to rest smoothly.
  - **[I]** Also shift pixels slightly to protect against AMOLED burn-in. PWR short and idle timeout share one lock path into Always On. Touch stays locked until tap-to-wake, raise or PWR; consume the waking contact. A PWR press while off or at ≤10% brightness only wakes. Input provenance never confirms, sends or unlocks an accessory.
- **Clock face.** **[I]** Use rounded type in complications, mirroring SF Compact Rounded.
- **Games.**
  - **[A]** Games may hide bars, but always offer a reliable way back.
  - **[I] Owner revision (2026-10-28):** PWR short pauses a running game, then locks into Always On; BOOT retains Back (Apps on Face). PWR double opens Control Center. PWR hold 2 s opens the system power sheet. The independent 10 s PMU cut is unchanged. See interaction-model.md for the full mapping.

Sources: [Toolbars](https://developer.apple.com/design/human-interface-guidelines/toolbars) · [Buttons](https://developer.apple.com/design/human-interface-guidelines/buttons) · [Layout](https://developer.apple.com/design/human-interface-guidelines/layout) · [Widgets](https://developer.apple.com/design/human-interface-guidelines/widgets) · [Lists](https://developer.apple.com/design/human-interface-guidelines/lists-and-tables) · [Sheets](https://developer.apple.com/design/human-interface-guidelines/sheets) · [Alerts](https://developer.apple.com/design/human-interface-guidelines/alerts) · [Notifications](https://developer.apple.com/design/human-interface-guidelines/notifications) · [Controls](https://developer.apple.com/design/human-interface-guidelines/controls) · [Page controls](https://developer.apple.com/design/human-interface-guidelines/page-controls) · [Digital Crown](https://developer.apple.com/design/human-interface-guidelines/digital-crown) · [Gestures](https://developer.apple.com/design/human-interface-guidelines/gestures) · [Loading](https://developer.apple.com/design/human-interface-guidelines/loading) · [Always On](https://developer.apple.com/design/human-interface-guidelines/always-on) · [Going full screen](https://developer.apple.com/design/human-interface-guidelines/going-full-screen) · [WWDC25 334](https://developer.apple.com/videos/play/wwdc2025/334/) · [Adopting Liquid Glass](https://developer.apple.com/documentation/technologyoverviews/adopting-liquid-glass)

## 8. Feedback, sound and accessibility

**Feedback without haptics:**

- **[A]** Deliver feedback through several channels at once (colour, text, sound, haptics) so it still works when the device is silenced or nobody is looking.
- **[A]** Pair every audio cue with a visual one.
- **[I]** With no haptics, every one of watchOS's haptic meanings becomes a short sound plus a visual change:
  - Notification, Up, Down, Success, Failure, Retry, Start, Stop.
  - Click is used only at meaningful increments; Apple warns that overusing it destroys its meaning.

**Sound:**

- **[A]** Respect silent mode and system volume, never autoplay, and vary repeated sounds slightly. That last point is visionOS guidance, borrowed here.
- **[I]** Keep sounds under 150 ms and quiet. Mute them when the device is face-down or in quiet hours.

**Voice:**

- **[A]** Ask for permissions in context with a clear reason. Offer text alternatives to audio.
- **[I]** Show a persistent mic indicator while holding BOOT records. Show the transcript to confirm before sending. Voice replies always display as text, which serves as captions.

**Accessibility checklist:**

- [ ] Text scales to at least 140 %, with no clipping inside the round safe area.
- [ ] Controls are at least 46 px (72 px by default), with 20 px between bezelled controls.
- [ ] Contrast is at least 4.5:1 below 29 px and at least 3:1 at 29 px or larger, or when bold. It's measured against the glass's backdrop as computed each frame.
- [ ] State is never shown by colour alone; add a shape, symbol or text.
- [ ] Reduce Motion, Reduce Transparency, Increase Contrast and Bold Text are all honoured (see §6).
- [ ] Nothing that needs a decision auto-dismisses on a timer. That includes approvals.
- [ ] Navigation gestures have the always-available physical BOOT/PWR and rim/twist alternatives (Design 1.0, owner decision, 2026-10-28); no redundant on-screen navigation controls. BOOT/PWR mappings never need a two-hand chord.
- [ ] No fast flashing or blinking.
- [ ] Always On and the short look redact sensitive text.

Sources: [Feedback](https://developer.apple.com/design/human-interface-guidelines/feedback) · [Playing haptics](https://developer.apple.com/design/human-interface-guidelines/playing-haptics) · [Playing audio](https://developer.apple.com/design/human-interface-guidelines/playing-audio) · [Privacy](https://developer.apple.com/design/human-interface-guidelines/privacy) · [Accessibility](https://developer.apple.com/design/human-interface-guidelines/accessibility) · [Action button](https://developer.apple.com/design/human-interface-guidelines/action-button)

## 9. Never do

- No glass in the content layer: no glass lists, cards or backgrounds.
- No glass on glass.
- Never mix the regular and clear variants.
- Don't use clear glass without dimming over bright content.
- Don't tint more than one element per view, and never use an opaque fill in place of a tint.
- No gradients or glows anywhere, including backgrounds, icons, cards and touch feedback. Use frosted real content or plain black; bounded material/rim changes are allowed.
- Don't fade glass in or out with alpha. Materialise it.
- Don't fake glass with static sheen, content-independent frost, or blur without dithering.
- Don't add custom borders or backgrounds to bar buttons, and don't put text labels in a toolbar's back or close position.
- No indeterminate spinners for agent work.
- No alerts for undoable actions.
- Never auto-approve or auto-dismiss approvals.
- Don't put text below 20 px, Light or Thin weights, or tertiary-label grey on essential text.
- Don't ship SF Pro, SF Compact or SF Symbols.
- Don't show sensitive content in short looks or Always On.
- No motion without purpose, and nothing that ignores Reduce Motion.
