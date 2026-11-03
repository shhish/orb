"""Exercise production ESP LVGL/JSON allocation policy with a capability heap."""

import os
import shlex
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[5]
LVGL = ROOT / "firmware/third_party/lvgl__lvgl"
COMPONENTS = ROOT / "firmware/components"
CJSON = ROOT / "firmware/managed_components/espressif__cjson/cJSON"


def main():
    with tempfile.TemporaryDirectory(prefix="orb-lvgl-heap-") as temporary:
        directory = Path(temporary)
        (directory / "esp_heap_caps.h").write_text(
            "#include <stddef.h>\n#define MALLOC_CAP_SPIRAM 1\n#define MALLOC_CAP_8BIT 2\n"
            "void *heap_caps_malloc(size_t bytes, unsigned caps);\n"
            "void heap_caps_free(void *pointer);\n"
            "size_t heap_caps_get_free_size(unsigned caps);\n"
        )
        (directory / "esp_attr.h").write_text("")
        (directory / "sdkconfig.h").write_text("")
        (directory / "freertos").mkdir()
        (directory / "freertos/task.h").write_text(
            "static inline void *xTaskGetCurrentTaskHandle(void) { return (void *)1; }\n"
        )
        display = (COMPONENTS / "orb_display/orb_display.c").read_text()
        begin = display.index("static void refresh_memory(bool started, int64_t now)")
        end = display.index("\n}", begin) + 2
        (directory / "orb_refresh_memory_impl.inc").write_text(display[begin:end])
        binary = directory / "heap"
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
                "-Wno-deprecated-declarations",
                "-DESP_PLATFORM",
                "-DLV_CONF_INCLUDE_SIMPLE",
                "-DLV_KCONFIG_IGNORE",
                f"-I{ROOT / 'sim'}",
                f"-I{LVGL / 'include'}",
                f"-I{directory}",
                f"-I{COMPONENTS / 'orb_memory/tests/host/stubs'}",
                f"-I{COMPONENTS / 'orb_memory/include'}",
                f"-I{COMPONENTS / 'orb_link/include'}",
                f"-I{CJSON}",
                str(LVGL / "src/stdlib/clib/lv_mem_core_clib.c"),
                str(COMPONENTS / "orb_memory/orb_memory.c"),
                str(COMPONENTS / "orb_link/link_memory.c"),
                str(CJSON / "cJSON.c"),
                str(Path(__file__).with_suffix(".c")),
                "-o",
                str(binary),
            ],
            check=True,
        )
        subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    main()
