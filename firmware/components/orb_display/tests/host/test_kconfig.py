"""Check experiment combinations with IDF's parser without configuring firmware."""

import itertools
import os
import tempfile
from pathlib import Path

import kconfiglib

root = Path(__file__).resolve().parents[5]
idf = Path(os.environ.get("IDF_PATH", Path.home() / "esp/esp-idf"))
options = (
    "ORB_NETWORK_BSS_PSRAM",
    "ORB_SMALL_ALLOCATIONS_PSRAM",
    "ORB_DISPLAY_TALL_BANDS",
    "ORB_DISPLAY_INTERNAL_DOUBLE_BUFFER",
    "ORB_DISPLAY_INTERNAL_IMAGE_SCRATCH",
    "ORB_DISPLAY_INTERNAL_BLUR_SCRATCH",
    "ORB_DISPLAY_INTERNAL_DRAW_MASKS",
    "ORB_DISPLAY_DRAW_PROFILE",
    "ORB_DISPLAY_GLASS_SINGLE_PASS",
    "ORB_DISPLAY_GLASS_MOTION_QUALITY",
    "ORB_DISPLAY_RASTER_SCALE_CACHE",
    "ORB_DISPLAY_RGB565_SCALE_FAST",
)
# Parse the real IDF PSRAM choices/selects; only unrelated platform leaves are
# stubbed. This catches choice dependencies that a list of symbols would miss.
with tempfile.TemporaryDirectory(prefix="orb-speed-kconfig-") as tmp:
    path = Path(tmp) / "Kconfig"
    path.write_text(
        f'''mainmenu "Orb experiment validation"
config IDF_TARGET_ESP32S3
    bool
    default y
config SPIRAM
    bool "PSRAM"
    default y
config PM_ENABLE
    bool
    default y
config ORB_DEVTOOLS
    bool "Developer tools"
    default y
config FREERTOS_SUPPORT_STATIC_ALLOCATION
    bool
config ESP_ALLOW_BSS_SEG_EXTERNAL_MEMORY
    bool
config SOC_PSRAM_ENCRYPTION_PAGE_CONFIGURABLE
    bool
if SPIRAM
source "{idf}/components/esp_psram/Kconfig.spiram.common"
endif
source "{root}/firmware/components/orb_display/Kconfig"
source "{root}/firmware/components/orb_ui/Kconfig"
source "{root}/firmware/main/Kconfig.projbuild"
'''
    )
    checks = 0
    for flags in itertools.product((False, True), repeat=len(options)):
        config = kconfiglib.Kconfig(str(path), warn_to_stderr=False, info=False)
        for option in options:
            assert config.syms[option].str_value == (
                "y" if option == "ORB_DISPLAY_INTERNAL_DOUBLE_BUFFER" else "n"
            )
            checks += 1
        for option, enabled in zip(options, flags, strict=True):
            config.syms[option].set_value("y" if enabled else "n")
        for option, enabled in zip(options, flags, strict=True):
            assert config.syms[option].str_value == ("y" if enabled else "n")
            checks += 1
        for selected in (
            "SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY",
            "ESP_ALLOW_BSS_SEG_EXTERNAL_MEMORY",
        ):
            assert config.syms[selected].str_value == ("y" if flags[0] else "n")
            checks += 1
        assert not config.warnings, config.warnings

    for accounting, retained in itertools.product((False, True), repeat=2):
        config = kconfiglib.Kconfig(str(path), warn_to_stderr=False, info=False)
        for option in ("ORB_DISPLAY_RENDER_ACCOUNTING", "ORB_DISPLAY_RETAINED_MATERIAL"):
            assert config.syms[option].str_value == "n"
            checks += 1
        for option, value in (
            ("ORB_DISPLAY_RENDER_ACCOUNTING", accounting),
            ("ORB_DISPLAY_RETAINED_MATERIAL", retained),
        ):
            config.syms[option].set_value("y" if value else "n")
            assert config.syms[option].str_value == ("y" if value else "n")
            checks += 1
        assert not config.warnings, config.warnings

    pacing_options = (
        "ORB_DISPLAY_RETAINED_MATERIAL",
        "ORB_DISPLAY_RETAINED_SETTLED_ONLY",
        "ORB_DISPLAY_GLASS_SOLID_FAST",
        "ORB_DISPLAY_GLASS_SPARSE_MASKS",
        "ORB_DISPLAY_TE_PACING",
    )
    for flags in itertools.product((False, True), repeat=len(pacing_options)):
        config = kconfiglib.Kconfig(str(path), warn_to_stderr=False, info=False)
        for option in pacing_options:
            assert config.syms[option].str_value == (
                "y" if option == "ORB_DISPLAY_GLASS_SOLID_FAST" else "n"
            )
            checks += 1
        for option, enabled in zip(pacing_options, flags, strict=True):
            config.syms[option].set_value("y" if enabled else "n")
        for index, (option, enabled) in enumerate(zip(pacing_options, flags, strict=True)):
            # The moving-backdrop policy is meaningful only with retained maps.
            effective = enabled and (index != 1 or flags[0])
            assert config.syms[option].str_value == ("y" if effective else "n")
            checks += 1
        assert not config.warnings, config.warnings

    config = kconfiglib.Kconfig(str(path), warn_to_stderr=False, info=False)
    assert config.syms["ORB_DISPLAY_RASTER_COVER_CHECK"].str_value == "y"
    config.syms["ORB_DISPLAY_RASTER_COVER_CHECK"].set_value("n")
    assert config.syms["ORB_DISPLAY_RASTER_COVER_CHECK"].str_value == "n"
    config.syms["ORB_DISPLAY_INTERNAL_DOUBLE_BUFFER"].set_value("y")
    config.syms["ORB_DISPLAY_PSRAM_PIPELINE"].set_value("y")
    assert config.syms["ORB_DISPLAY_INTERNAL_DOUBLE_BUFFER"].str_value == "n"
    for internal, live in itertools.product((False, True), repeat=2):
        config = kconfiglib.Kconfig(str(path), warn_to_stderr=False, info=False)
        assert config.syms["ORB_DISPLAY_LIVE_DOUBLE_BUFFER"].str_value == "y"
        config.syms["ORB_DISPLAY_INTERNAL_DOUBLE_BUFFER"].set_value("y" if internal else "n")
        config.syms["ORB_DISPLAY_LIVE_DOUBLE_BUFFER"].set_value("y" if live else "n")
        assert config.syms["ORB_DISPLAY_LIVE_DOUBLE_BUFFER"].str_value == (
            "y" if internal and live else "n"
        )
        config.syms["ORB_DISPLAY_PSRAM_PIPELINE"].set_value("y")
        assert config.syms["ORB_DISPLAY_LIVE_DOUBLE_BUFFER"].str_value == "n"
        checks += 3
        assert not config.warnings, config.warnings
    config = kconfiglib.Kconfig(str(path), warn_to_stderr=False, info=False)
    config.syms["ORB_SMALL_ALLOCATIONS_PSRAM"].set_value("y")
    config.syms["SPIRAM_USE_CAPS_ALLOC"].set_value("y")
    assert config.syms["ORB_SMALL_ALLOCATIONS_PSRAM"].str_value == "n"
    config.syms["ORB_NETWORK_BSS_PSRAM"].set_value("y")
    config.syms["ORB_DISPLAY_RENDER_ACCOUNTING"].set_value("y")
    config.syms["ORB_DISPLAY_RETAINED_MATERIAL"].set_value("y")
    config.syms["SPIRAM"].set_value("n")
    assert config.syms["ORB_DISPLAY_RENDER_ACCOUNTING"].str_value == "n"
    assert config.syms["ORB_DISPLAY_RETAINED_MATERIAL"].str_value == "n"
    checks += 2
    assert config.syms["ORB_NETWORK_BSS_PSRAM"].str_value == "n"
    assert config.syms["SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY"].str_value == "n"
    config.syms["ORB_DISPLAY_DRAW_PROFILE"].set_value("y")
    assert config.syms["ORB_DISPLAY_DRAW_PROFILE"].str_value == "n"
    config.syms["SPIRAM"].set_value("y")
    config.syms["ORB_DEVTOOLS"].set_value("n")
    assert config.syms["ORB_DISPLAY_DRAW_PROFILE"].str_value == "n"
    assert not config.warnings, config.warnings
    checks += 8
    hotspot_options = ("ORB_RENDER_FUSED_GLASS", "ORB_RENDER_ROWS_AT_SIZE")
    for flags in itertools.product((False, True), repeat=2):
        config = kconfiglib.Kconfig(str(path), warn_to_stderr=False, info=False)
        for name, enabled in zip(hotspot_options, flags, strict=True):
            assert config.syms[name].str_value == (
                "y" if name == "ORB_RENDER_ROWS_AT_SIZE" else "n"
            )
            config.syms[name].set_value("y" if enabled else "n")
            assert config.syms[name].str_value == ("y" if enabled else "n")
            checks += 2
        config.syms["SPIRAM"].set_value("n")
        for name in hotspot_options:
            assert config.syms[name].str_value == "n"
            checks += 1
        assert not config.warnings, config.warnings
    config = kconfiglib.Kconfig(str(path), warn_to_stderr=False, info=False)
    assert config.syms["ORB_UI_SCROLL_EDGE_SOFT"].str_value == "y"
    checks += 1
    for enabled in (False, True):
        config.syms["ORB_UI_SCROLL_EDGE_SOFT"].set_value("y" if enabled else "n")
        assert config.syms["ORB_UI_SCROLL_EDGE_SOFT"].str_value == ("y" if enabled else "n")
        checks += 1
    assert not config.warnings, config.warnings
    for devtools, spiram in itertools.product((False, True), repeat=2):
        config = kconfiglib.Kconfig(str(path), warn_to_stderr=False, info=False)
        assert config.syms["ORB_DISPLAY_SCROLL_TRACE"].str_value == "n"
        config.syms["ORB_DEVTOOLS"].set_value("y" if devtools else "n")
        config.syms["SPIRAM"].set_value("y" if spiram else "n")
        config.syms["ORB_DISPLAY_SCROLL_TRACE"].set_value("y")
        assert config.syms["ORB_DISPLAY_SCROLL_TRACE"].str_value == (
            "y" if devtools and spiram else "n"
        )
        assert not config.warnings, config.warnings
        checks += 2
    print(
        f"Kconfig: {checks} checks passed, {2 ** len(options)} original + "
        "32 pacing/solid/sparse/settled combinations, zero parser warnings"
    )
