"""Exercise production task/layer allocation lifetimes with ESP capabilities."""

import os
import re
import shlex
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[5]
LVGL = ROOT / "firmware/third_party/lvgl__lvgl"
SOURCE = LVGL / "src/draw/lv_draw.c"


def function(source, name):
    match = re.search(rf"^\w[^;\n]*\b{name}\([^;]*?\n\{{", source, re.M)
    assert match, name
    begin = match.start()
    offset = match.end()
    depth = 1
    while depth:
        depth += (source[offset] == "{") - (source[offset] == "}")
        offset += 1
    return source[begin:offset] + "\n"


def main():
    source = SOURCE.read_text()
    names = (
        "lv_draw_metadata_alloc",
        "lv_draw_metadata_free",
        "get_draw_dsc_size",
        "lv_draw_add_task",
        "lv_draw_layer_create",
        "lv_draw_layer_delete",
        "lv_draw_cleanup_task",
    )
    fixture = Path(__file__).with_suffix(".c").read_text()
    fixture = fixture.replace(
        "/* PRODUCTION_FUNCTIONS */",
        "\n".join(function(source, name) for name in names)
        + function((LVGL / "src/draw/lv_draw_label.c").read_text(), "lv_draw_label"),
    )
    with tempfile.TemporaryDirectory(prefix="orb-draw-metadata-") as temporary:
        directory = Path(temporary)
        (directory / "esp_heap_caps.h").write_text(
            "#include <stddef.h>\n#include <stdint.h>\n"
            "#define MALLOC_CAP_8BIT 1\n#define MALLOC_CAP_SPIRAM 8\n"
            "void *heap_caps_aligned_alloc(size_t alignment, size_t size, uint32_t caps);\n"
            "void heap_caps_free(void *memory);\n"
        )
        (directory / "sdkconfig.h").write_text("")
        (directory / "esp_attr.h").write_text("")
        path = directory / "metadata.c"
        path.write_text(fixture)
        for esp in (False, True):
            binary = directory / f"metadata-{int(esp)}"
            subprocess.run(
                [
                    *shlex.split(os.environ.get("CC", "clang")),
                    "-std=c11",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-O1",
                    "-g",
                    "-fsanitize=address,undefined",
                    "-DLV_CONF_INCLUDE_SIMPLE",
                    "-DLV_KCONFIG_IGNORE",
                    *(["-DESP_PLATFORM"] if esp else []),
                    f"-I{ROOT / 'sim'}",
                    f"-I{LVGL}",
                    f"-I{LVGL / 'include'}",
                    f"-I{directory}",
                    str(path),
                    "-o",
                    str(binary),
                ],
                check=True,
            )
            subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    main()
