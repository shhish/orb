# Implementing Orb Design 1.0

The source of truth is each `<Screen>.dc.html` file here. `png/<Screen>.png` is a render of it, for comparison, made with headless Chrome at 1× (466 × 466). `Main.dc.html` is the system sheet: grid, type, colour, components and the button map. The canonical, commentable copy is the private claude.ai canvas linked from docs/REQUIREMENTS.md.

## How to read a frame

- **Coordinates are device pixels.** Every element in a screen is absolutely positioned inside the 466 × 466 circle. `left`/`top`/`width`/`height` in the HTML are the firmware's pixel geometry. Match them within ±2 px.
- **Type:** `font-size` is the device pixel size, using Inter at the stated weight (`font-weight` 400, 500, 600 or 700). `font-variant-numeric: tabular-nums` means tabular figures. Map each size to the nearest orb_fonts size and say in your report where you rounded. Never invent a new size.
- **Colour:** hex values come from hig.md's palette. Secondary text is `rgba(235,235,245,0.6)` (#EBEBF5 at 60%). Accents follow `Main.dc.html`: orange = Claude / needs you, blue = Codex / system, indigo = Dot, yellow = Home, teal = Time, green = Play / done, red = listening / deny, pink = Beads. Each game has its own accent.
- **Glass:** `rgba(120,120,128,0.26)` with an inset 1 px top highlight is clear glass. An accent at about 26% with an accent inner border is tinted glass. On device, use orb_glass REGULAR or TINTED; because controls sit on true black, use the static/solid glass path, not a live blur.
- **Rows near the rim** are drawn smaller and at 50% opacity, for example the faded first and last rows. That's a static presentation per row position, not a continuous per-pixel scale effect: implement it as a per-row opacity/size step, which keeps scrolling a cheap translate.
- **Rim arcs** (progress, usage, the dial, scroll position) are given as SVG arcs: centre (233, 233), radius and stroke width as written, starting at 12 o'clock and running clockwise unless the path says otherwise.
- **No gradients, no glows**, anywhere. If a frame seems to need one, ask.

## Process

1. Build the screen to match the frame. Use orb_ui components (glass, round list, chord width) instead of hand-rolled ones.
2. Render it in the simulator and with the emulator (`sim/build/orb_emu --headless-frames N --out DIR`), and produce a side-by-side contact sheet: design PNG on the left, your render on the right, for every screen you touched, in Default and AX3 + Bold + Reduce Transparency. AX3 may reflow (bigger type, fewer rows), but must keep the same structure.
3. Write the per-screen differences you could not close, with the reason (font size not available, data not provided by the hub yet, and so on).
4. The lead reviews the sheet and then screenshots from the board before anything ships.
