# Third-party notices

The root [MIT License](LICENSE) covers Orb-owned code only. Third-party source,
fonts, generated glyph data and assets retain the licenses and copyrights below.
This inventory is based on the 2026-11-03 provenance audit, extended for the
current firmware lockfile, fonts, hub and simulator. The audit covered an older
firmware-only snapshot; its version table below also records components that
are no longer selected. Presence in the LVGL source tree does not mean a library
is enabled or linked. This document is not a complete binary SBOM.

## Orb adaptations and credits

- **Meta Muse Gadgets**, Copyright (c) Meta Platforms, Inc. and affiliates,
  Apache-2.0: board/power and display-band adaptations in
  `firmware/components/orb_board/axp2101.c`, `board_ws175c.c`, and
  `firmware/components/orb_display/orb_display.c`. These are modified Orb
  adaptations; preserve their origin comments and any applicable upstream NOTICE.
- **Espressif esp_lvgl_port SIMD**, Copyright (c) 2025 Espressif Systems
  (Shanghai) CO LTD, Apache-2.0: the RGB565 assembly and memcpy macros in
  `firmware/components/orb_lv_simd/src/`. Preserve the SPDX headers.
- **LVGL**, MIT: the modified fork is in `firmware/third_party/lvgl__lvgl`;
  retain `LICENCE.txt`, `COPYRIGHTS`, nested licenses and `ORB_PATCHES.md`.
- **FreeType Team**: This product includes software developed by the FreeType
  Team. Portions of this software are copyright © 1996–2026 The FreeType
  Project (www.freetype.org). All rights reserved. Use the FreeType License
  (FTL) option for MIT coexistence, preserve its source notices, identify
  modifications and include the credit in documentation accompanying binaries.
  This also applies to the FreeType-derived rasterizer inside ThorVG.
- **rsms/esp32-dot** has no license grant. It is an ideas reference, not code
  incorporated or relicensed by Orb. Inter is a separately licensed font project.

The Apache-2.0 text is included in [licenses/Apache-2.0.txt](licenses/Apache-2.0.txt).
Keep all applicable upstream NOTICE files with redistributed source or binaries.

## Current managed firmware selection

`firmware/dependencies.lock` currently selects the following; managed components
are downloaded by ESP-IDF and are not vendored by this snapshot. Retain each
package's exact license and nested notices when distributing a build.

| Component | Locked version | License / attribution |
| --- | --- | --- |
| espressif/cjson | 1.7.19~2 | MIT; Dave Gamble and cJSON contributors; retain license and copyright. |
| espressif/cmake_utilities | 0.5.3 | Apache-2.0; Espressif Systems. |
| espressif/esp_codec_dev | 1.5.11 | Apache-2.0; Espressif Systems; nested Microsemi code is MIT. |
| espressif/esp_lcd_co5300 | 2.2.0 | Apache-2.0; Espressif Systems. |
| espressif/esp_lcd_touch | 1.2.1 | Apache-2.0; Espressif Systems. |
| espressif/esp_websocket_client | 1.8.0 | Apache-2.0; Espressif Systems. |
| espressif/json_generator | 2.1.0 | Apache-2.0; Espressif Systems. |
| espressif/mdns | 1.14.1 | Apache-2.0; Espressif Systems. |
| espressif/network_provisioning | 1.4.0 | Apache-2.0; Espressif Systems. |
| idf | 6.1.0 | Apache-2.0 SDK with separately licensed nested components. |
| joltwallet/littlefs | 1.22.3 | [MIT ESP-IDF wrapper](https://components.espressif.com/components/joltwallet/littlefs/versions/1.22.3/license), Copyright 2020 Brian Pugh; upstream littlefs is BSD-3-Clause (Arm Limited). Preserve both notices. |
| lvgl/lvgl | 9.6.0~1 | MIT LVGL fork plus the nested libraries/fonts listed below. |
| waveshare/esp_lcd_touch_cst9217 | 2.0.0 | Apache-2.0; Waveshare / Espressif notices. |

## Project fonts and icons

- **Inter 4.1**, Copyright (c) 2016 The Inter Project Authors, **SIL OFL-1.1**.
  Instanced/subset fonts are renamed **Orb Inter**. The font remains OFL,
  cannot be sold on its own and must respect Reserved Font Names.
  [Full license](firmware/components/orb_fonts/licenses/Inter-OFL.txt).
- **Framework7 Icons 5.0.5**, Copyright (c) 2016 Vladimir Kharlampidi, **MIT**;
  Orb offsets outlines for additional weights.
  [Full license](firmware/components/orb_fonts/licenses/Framework7-MIT.txt).
- **Phosphor Icons 2.1.1** fallback glyphs, Copyright (c) 2023 Phosphor Icons,
  **MIT**. [Full license](firmware/components/orb_fonts/licenses/Phosphor-MIT.txt).

Design HTML also requests **JetBrains Mono** via Google Fonts for some frames
(OFL-1.1, JetBrains). It is not vendored here; retain its OFL/copyright notice
if downloading and redistributing the font with exported designs.

These notices are also embedded by `orb_fonts_notices()` for device legal UI.
Preserve them with generated C fonts, the signed assets pack and firmware binaries.

## Distribution conditions and unresolved audit items

- **esp_new_jpeg 1.0.2 is restricted to Espressif products.** Its MIT-like
  license includes an Espressif-product restriction and is **not unrestricted
  MIT**. It appeared in the audited lockfile and is absent from the current
  selection. If reintroduced, keep the exact license and restrict distribution
  to its permitted product scope, or replace it for unrestricted use. The root
  MIT grant does not remove that restriction.
- **FrogFS is MPL-2.0**: its covered files remain MPL, including modifications.
  Preserve notices and provide the corresponding covered source when distributing
  binaries; do not describe the whole vendored tree as MIT-only.
- **OFL fonts inside LVGL** include Montserrat, Font Awesome 5 and Source Han
  Sans SC. Retain their copyright/OFL notices and Reserved Font Name conditions.
  DejaVu-derived data additionally needs Bitstream Vera/DejaVu/Arev notices;
  Liberation Sans already carries an OFL notice. The audit found missing
  source-specific font license texts in the vendor tree. Those remain release
  follow-up items; the project Inter license does not cover these other fonts.
- The glTF shader bundle contains tonemapping matching **Khronos glTF Sample
  Renderer (Apache-2.0)**. Restore its upstream attribution and establish exact
  source/version and full bundle provenance before redistribution.
- The original artwork grant for glTF `chromatic.c`'s embedded JPEG is unresolved.
  The FTDI AN_336 touch-engine patch embedded in `EVE_commands.c` also needs a
  verified redistribution grant. Disabled build options do not settle source
  redistribution rights. These are outstanding provenance items, not covered by
  Orb's MIT license.

The vendor and managed directories were left unchanged during this preparation.
The above audit findings must be resolved or the affected material omitted from
any distribution that cannot satisfy the original terms.

## Audited vendor and managed inventory

This table preserves the audit's historical versions and exact source/license
locations, including optional and formerly managed components. For the current
managed selection use the table above. License files under `managed_components`
are available after ESP-IDF restores the matching components.

| Component | Version | License | MIT coexistence / attribution | Evidence |
|---|---|---|---|---|
| espressif/button | 4.2.2 | Apache-2.0 | Yes, retain Apache-2.0, copyright, applicable NOTICE; mark modifications. | firmware/dependencies.lock:2; firmware/managed_components/espressif__button/license.txt |
| espressif/cmake_utilities | 0.5.3 | Apache-2.0 | Yes, retain Apache-2.0, copyright, applicable NOTICE; mark modifications. | firmware/dependencies.lock:16; firmware/managed_components/espressif__cmake_utilities/license.txt |
| espressif/esp_codec_dev | 1.5.11 | Apache-2.0 | Yes, retain Apache-2.0, copyright, applicable NOTICE; mark modifications. | firmware/dependencies.lock:26; firmware/managed_components/espressif__esp_codec_dev/LICENSE |
| espressif/esp_io_expander | 1.2.1 | Apache-2.0 | Yes, retain Apache-2.0, copyright, applicable NOTICE; mark modifications. | firmware/dependencies.lock:36; firmware/managed_components/espressif__esp_io_expander/license.txt |
| espressif/esp_lcd_co5300 | 2.2.0 | Apache-2.0 | Yes, retain Apache-2.0, copyright, applicable NOTICE; mark modifications. | firmware/dependencies.lock:46; firmware/managed_components/espressif__esp_lcd_co5300/license.txt |
| espressif/esp_lcd_panel_io_additions | 1.0.1~1 | Apache-2.0 | Yes, retain Apache-2.0, copyright, applicable NOTICE; mark modifications. | firmware/dependencies.lock:60; firmware/managed_components/espressif__esp_lcd_panel_io_additions/license.txt |
| espressif/esp_lcd_touch | 1.2.1 | Apache-2.0 | Yes, retain Apache-2.0, copyright, applicable NOTICE; mark modifications. | firmware/dependencies.lock:78; firmware/managed_components/espressif__esp_lcd_touch/license.txt |
| espressif/esp_lv_decoder | 0.4.3 | Apache-2.0 | Yes, retain Apache-2.0, copyright, applicable NOTICE; mark modifications. | firmware/dependencies.lock:88; firmware/managed_components/espressif__esp_lv_decoder/license.txt |
| espressif/esp_lv_fs | 1.0.1 | Apache-2.0 | Yes, retain Apache-2.0, copyright, applicable NOTICE; mark modifications. | firmware/dependencies.lock:122; firmware/managed_components/espressif__esp_lv_fs/license.txt |
| espressif/esp_lvgl_adapter | 0.6.4 | Apache-2.0 | Yes, retain Apache-2.0, copyright, applicable NOTICE; mark modifications. | firmware/dependencies.lock:144; firmware/managed_components/espressif__esp_lvgl_adapter/LICENSE |
| espressif/esp_mmap_assets | 2.0.1 | Apache-2.0 | Yes, retain Apache-2.0, copyright, applicable NOTICE; mark modifications. | firmware/dependencies.lock:182; firmware/managed_components/espressif__esp_mmap_assets/license.txt |
| espressif/esp_new_jpeg | 1.0.2 | LicenseRef-Espressif-MIT (product restriction) | Not unrestricted MIT; permitted Espressif product scope only; retain exact license. | firmware/dependencies.lock:196; firmware/managed_components/espressif__esp_new_jpeg/LICENSE |
| espressif/freetype | 2.14.3~1 | FTL OR GPL-2.0-or-later | Yes via FTL; retain FTL/copyright, document modifications, credit FreeType Team in binary documentation. | firmware/dependencies.lock:215; firmware/managed_components/espressif__freetype/LICENSE |
| espressif/knob | 1.1.0 | Apache-2.0 | Yes, retain Apache-2.0, copyright, applicable NOTICE; mark modifications. | firmware/dependencies.lock:225; firmware/managed_components/espressif__knob/license.txt |
| espressif/libpng | 1.6.59 | libpng-2.0 | Yes; retain source copyright/license; acknowledge origin; mark altered sources. | firmware/dependencies.lock:239; firmware/managed_components/espressif__libpng/LICENSE |
| espressif/usb | 1.6.0 | Apache-2.0 | Yes, retain Apache-2.0, copyright, applicable NOTICE; mark modifications. | firmware/dependencies.lock:253; firmware/managed_components/espressif__usb/LICENSE |
| espressif/zlib | 1.3.2~2 | Zlib | Yes; retain source notice, mark alterations; do not misrepresent origin. | firmware/dependencies.lock:270; firmware/managed_components/espressif__zlib/LICENSE |
| idf | 6.1.0 | Apache-2.0 | Yes, retain Apache-2.0, copyright, applicable NOTICE; mark modifications. | firmware/dependencies.lock:280; External ESP-IDF LICENSE |
| lvgl/lvgl | 9.6.0~1 | MIT | Yes, retain copyright and permission/disclaimer. | firmware/dependencies.lock:284; firmware/third_party/lvgl__lvgl/LICENCE.txt |
| waveshare/esp32_s3_touch_amoled_1_75c | 3.0.0 | Apache-2.0 | Yes, retain Apache-2.0, copyright, applicable NOTICE; mark modifications. | firmware/dependencies.lock:310; firmware/managed_components/waveshare__esp32_s3_touch_amoled_1_75c/LICENSE |
| waveshare/esp_lcd_touch_cst9217 | 2.0.0 | Apache-2.0 | Yes, retain Apache-2.0, copyright, applicable NOTICE; mark modifications. | firmware/dependencies.lock:352; firmware/managed_components/waveshare__esp_lcd_touch_cst9217/license.txt |
| Meta Muse Gadgets adaptations | bundled | Apache-2.0 | Yes, retain Apache-2.0, copyright, applicable NOTICE; mark modifications. | firmware/components/orb_board/axp2101.c:4; firmware/components/orb_display/orb_display.c:8; firmware/components/orb_board/board_ws175c.c:5 |
| Espressif esp_lvgl_port SIMD | bundled | Apache-2.0 | Yes, retain Apache-2.0, copyright, applicable NOTICE; mark modifications. | firmware/components/orb_lv_simd/src/lv_color_blend_to_rgb565_esp32s3.S:4; lv_rgb565_blend_normal_to_rgb565_esp32s3.S:4; lv_macro_memcpy.S:4 |
| LVGL barcode/code128 | bundled | BSD-2-Clause | Yes; retain copyright, conditions, disclaimer in source and binary notices. | firmware/third_party/lvgl__lvgl/src/libs/barcode/LICENSE.txt |
| LVGL LZ4 | bundled | BSD-2-Clause | Yes; retain copyright, conditions, disclaimer in source and binary notices. | firmware/third_party/lvgl__lvgl/src/libs/lz4/LICENSE.txt |
| LVGL LodePNG and NanoVG | bundled | Zlib | Yes; retain source notices, mark altered sources. | firmware/third_party/lvgl__lvgl/src/libs/lodepng/LICENSE.txt; src/libs/nanovg/LICENSE.txt |
| LVGL QR-Code-generator, FT800-FT813 EVE, mpaland printf | bundled | MIT | Yes, retain copyright and permission/disclaimer. | firmware/third_party/lvgl__lvgl/src/libs/qrcode/LICENSE.txt; src/libs/FT800-FT813/LICENSE; src/stdlib/builtin/LICENSE_SPRINTF.txt |
| LVGL ThorVG | bundled | MIT AND FTL (FreeType-derived rasterizer) | Yes, retain copyright and permission/disclaimer. Credit FreeType and preserve embedded FTL. | firmware/third_party/lvgl__lvgl/src/libs/thorvg/LICENSE.txt; src/libs/thorvg/tvgSwRle.cpp:27 |
| LVGL RapidJSON and msinttypes | bundled | MIT AND BSD-3-Clause | Yes, retain copyright and permission/disclaimer. Retain BSD notices in binary documentation; no endorsement. | firmware/third_party/lvgl__lvgl/src/libs/thorvg/rapidjson/rapidjson.h; src/libs/thorvg/rapidjson/msinttypes/stdint.h:4 |
| LVGL tiny_ttf/stb_truetype/stb_rect_pack | bundled | (MIT OR Unlicense) AND MIT | Yes, retain copyright and permission/disclaimer. | firmware/third_party/lvgl__lvgl/src/libs/tiny_ttf/LICENSE.txt |
| LVGL glTF stb_image | bundled | MIT OR Unlicense | Yes, retain copyright and permission/disclaimer. | firmware/third_party/lvgl__lvgl/src/libs/gltf/stb_image/stb_image.h:8330 |
| LVGL TJpgDec | bundled | LicenseRef-TJpgDec | Yes; unrestricted uses; retain source copyright. | firmware/third_party/lvgl__lvgl/src/libs/tjpgd/LICENSE.txt |
| LVGL TLSF | bundled | BSD-3-Clause | Yes; preserve copyright, conditions, disclaimer in source and binary; no endorsement. | firmware/third_party/lvgl__lvgl/src/stdlib/builtin/LICENSE_TLSF.txt; src/stdlib/builtin/lv_tlsf.h |
| LVGL AnimatedGIF and CMSIS-RTOS2 port | bundled | Apache-2.0 | Yes, retain Apache-2.0, copyright, applicable NOTICE; mark modifications. | firmware/third_party/lvgl__lvgl/src/libs/gif/LICENSE; src/osal/lv_cmsis_rtos2.c:2 |
| LVGL Vivante VG-Lite | bundled | MIT-style | Yes, retain copyright and permission/disclaimer. | firmware/third_party/lvgl__lvgl/src/libs/vg_lite_driver/LICENSE.txt |
| LVGL GLAD/Khronos generated headers | bundled | (WTFPL OR CC0-1.0) AND Apache-2.0; Khronos MIT-style | Yes, retain Apache-2.0, copyright, applicable NOTICE; mark modifications. Keep Khronos permission notices. | firmware/third_party/lvgl__lvgl/src/drivers/opengles/glad/include/glad/gl.h:4; src/drivers/opengles/glad/include/KHR/khrplatform.h |
| LVGL FrogFS | bundled | MPL-2.0 | May coexist with MIT; FrogFS covered files remain MPL; preserve notices and make their source available with binaries. | firmware/third_party/lvgl__lvgl/src/libs/frogfs/LICENSE.txt; src/libs/frogfs/src/frogfs.c:1 |
| LVGL glTF shader bundle (matches Khronos Sample Renderer tonemapping) | bundled | Apache-2.0 upstream; full bundle provenance unresolved | Yes, retain Apache-2.0, copyright, applicable NOTICE; mark modifications. Restore upstream attribution and document exact source/version; do not assume blanket LVGL MIT. | firmware/third_party/lvgl__lvgl/src/libs/gltf/gltf_view/assets/lv_gltf_view_shader.c:8 |
| LVGL Montserrat and Font Awesome 5 font glyphs | bundled | OFL-1.1 | May bundle with MIT; retain copyright and OFL; fonts remain OFL, obey reserved names. Source license texts absent for these fonts. | firmware/third_party/lvgl__lvgl/src/font/lv_font_montserrat_*.c:4; firmware/sdkconfig.defaults:36 |
| LVGL Source Han Sans SC and Font Awesome glyphs | bundled | OFL-1.1 | May bundle with MIT; preserve OFL/copyright and reserved names. Source license texts absent for these fonts. | firmware/third_party/lvgl__lvgl/src/font/lv_font_source_han_sans_sc_{14,16}_cjk.c:4 |
| LVGL DejaVu Sans and Font Awesome glyphs | bundled | Bitstream Vera/DejaVu/Arev terms AND OFL-1.1 | May bundle with MIT; preserve all font notices, naming and no-standalone-sale conditions. Source font license texts absent. | firmware/third_party/lvgl__lvgl/src/font/lv_font_dejavu_16_persian_hebrew.c:4 |
| LVGL Unscii-8 (also used for generated 16px) | bundled | Public domain / CC0 | Yes; no attribution required under CC0; do not conflate with GPL unscii-16-full. | firmware/third_party/lvgl__lvgl/src/font/lv_font_unscii_{8,16}.c:4 |
| LVGL Liberation Sans | bundled | OFL-1.1 | May bundle with MIT; preserve license/copyright and reserved font names. License is present. | firmware/third_party/lvgl__lvgl/src/libs/freetype/LiberationSans-LICENSE.txt |
| Managed esp_codec_dev Microsemi zl38063 code | bundled | MIT | Yes, retain copyright and permission/disclaimer. | firmware/managed_components/espressif__esp_codec_dev/device/zl38063/LICENSE.txt |
| Managed FreeType dlg | bundled | BSL-1.0 | Yes; preserve copyright and license in source; object-code exception. | firmware/managed_components/espressif__freetype/freetype/subprojects/dlg/LICENSE |

## Hub, simulator and tool dependencies

These are installed dependencies, not code relicensed by the root MIT grant.
Their distributions carry the authoritative notices, including transitive
libraries. Check resolved package versions when bundling wheels or executables.

| Dependency | License / attribution |
| --- | --- |
| aiohttp | Apache-2.0 AND MIT; aiohttp contributors. |
| bleak | MIT; bleak contributors. |
| cryptography | Apache-2.0 OR BSD-3-Clause; Python Cryptographic Authority; preserve bundled OpenSSL and other native notices. |
| esptool / espsecure | GPL-2.0-or-later; Espressif Systems; preserve their license and corresponding-source obligations if distributing the tools. |
| ifaddr | MIT; ifaddr contributors. |
| python-zeroconf | LGPL-2.1-or-later; preserve LGPL terms/source and replacement rights when redistributing. |
| jsonschema | MIT; jsonschema contributors. |
| mlx-whisper (optional Apple Silicon speech) | MIT; MLX examples contributors; model weights have separate source/model-card terms and are not bundled. |
| SDL2 | Zlib; SDL contributors; used by the desktop simulator. |
| zlib | Zlib; Jean-loup Gailly and Mark Adler; used by simulator/assets tooling. |
| lv_font_conv | MIT; LVGL font converter contributors; preserve its native dependency notices when bundling. |
| fontTools | MIT; fontTools authors. |
| uharfbuzz / HarfBuzz | [Apache-2.0 binding](https://github.com/harfbuzz/uharfbuzz/blob/main/LICENSE); HarfBuzz uses MIT-style terms; preserve both notices. |
| Pillow | HPND; Pillow/Python Imaging Library authors; preserve bundled native-library notices. |
| PyYAML | MIT; PyYAML authors. |
| Open-Meteo offline weather responses | CC-BY-4.0; attribution: Open-Meteo (https://open-meteo.com/). The fixture README documents public Berlin samples, not a user's location. |

## Upstream license references

- https://www.apache.org/licenses/LICENSE-2.0 (redistribution section 4)
- https://raw.githubusercontent.com/google/fonts/main/ofl/montserrat/OFL.txt
- https://raw.githubusercontent.com/FortAwesome/Font-Awesome/5.x/LICENSE.txt (font files OFL, not SVG CC-BY)
- https://raw.githubusercontent.com/adobe-fonts/source-han-sans/release/LICENSE.txt
- https://dejavu-fonts.github.io/License.html
- https://github.com/viznut/unscii (public-domain/CC0 excludes unscii-16-full)
- https://raw.githubusercontent.com/KhronosGroup/glTF-Sample-Renderer/main/LICENSE.md
- https://raw.githubusercontent.com/KhronosGroup/glTF-Sample-Renderer/main/source/Renderer/shaders/tonemapping.glsl
- https://raw.githubusercontent.com/KhronosGroup/glTF-IBL-Sampler/main/LICENSE

