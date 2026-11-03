# Rounded Face companion

These frozen Nunito subsets are retained as parity resources. The old
`orb_complication_apply()` renderer had no production callers after the Face
migration and was removed in refactor20. Its font assets, source and licence
remain unchanged; the active Face renderer owns its current typography.

The actual Footnote 2 table is 21 px Default, 25 px Larger, 28 px Largest/AX3,
with weights 600 and 700 for normal/Bold Text. Each subset contains only
`0123456789waitingMacDemoUSB- ` (27 distinct characters). Digits use Nunito's
OpenType tabular figures, frozen into the cmap because LVGL does not shape
OpenType features. Tracking matches each semantic style (16, 4, -4 per mille).
Unsupported text, punctuation, and icons use the normal fallback chain.

## Source and licence

Nunito **3.602**, Copyright 2014 The Nunito Project Authors, SIL OFL 1.1.
The unmodified original and complete licence are retained alongside the six
C subsets. Upstream: [Nunito](https://github.com/googlefonts/nunito).
Google Fonts commit: `8b0a1d0f5983c89bc2b93f1b5fb55f9e252744b5`.

- [Original variable font](https://github.com/google/fonts/blob/8b0a1d0f5983c89bc2b93f1b5fb55f9e252744b5/ofl/nunito/Nunito%5Bwght%5D.ttf)
  → `Nunito-wght.ttf`, SHA256 `bb55a5ca5c2042335b3991af27c4d0705d0ef41cac6164ac737fd8f2a1e85207`.
- [Original licence](https://github.com/google/fonts/blob/8b0a1d0f5983c89bc2b93f1b5fb55f9e252744b5/ofl/nunito/OFL.txt)
  → `Nunito-OFL.txt`, SHA256 `580df76c95a1ec5ab878ceb25bb3d85c6a076804e9c970c8c6972aea775fdf65`.

Only generated C bitmap data links into the app; the original TTF and Python
generator are build inputs. The derived subset's internal family name is
“Orb Rounded Complications”; copyright/licence records are preserved.

## Reproduction

Use the font tools' existing hash-pinned Python environment (`fonttools 4.67.0`,
`uharfbuzz 0.57.0`, `skia-pathops 0.9.2`) and `lv_font_conv 1.5.3` installed
from `tools/fonts/package-lock.json`. The generator verifies both original-file
hashes and the converter version before writing. It does not change the main
font pipeline or pack.

From the repository root:

```sh
tools/fonts/.venv/bin/python firmware/components/orb_shell/fonts/generate.py \
  --converter tools/fonts/node_modules/lv_font_conv/lv_font_conv.js
```

In the wave2 worktree, the already installed tool directory may be supplied
from `../fonts/tools/fonts` for both paths. Temporary instanced fonts are
created and removed under the OS temporary directory; C output paths and
converter arguments are machine-independent. Regenerating twice must produce
identical SHA256 sums for all six `orb_nunito_*.c` files.

The retired renderer's `rounded_complications` test was removed with its API.
The parity manifest continues to fingerprint all six generated subsets.
