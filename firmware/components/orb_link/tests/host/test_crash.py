"""Read a synthetic 31 MiB flash dump and exercise the actual report send path."""

import os
import subprocess
import sys
from pathlib import Path

component = Path(__file__).resolve().parents[2]
work = Path(sys.argv[1] if len(sys.argv) > 1 else "build").resolve() / "crash"
cjson = Path(
    os.environ.get("CJSON_DIR", component.parents[1] / "managed_components/espressif__cjson/cJSON")
).resolve()
work.mkdir(parents=True, exist_ok=True)
stubs = {
    "esp_err.h": """#pragma once
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_INVALID_ARG 1
#define ESP_ERR_INVALID_SIZE 2
#define ESP_ERR_INVALID_RESPONSE 3
#define ESP_ERR_NOT_FOUND 4
#define ESP_ERR_INVALID_CRC 5
const char *esp_err_to_name(esp_err_t);
""",
    "esp_core_dump.h": """#include <stddef.h>
#include "esp_err.h"
esp_err_t esp_core_dump_image_check(void);
esp_err_t esp_core_dump_image_get(size_t *, size_t *);
""",
    "esp_partition.h": """#pragma once
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"
typedef struct {uint32_t address, size;} esp_partition_t;
#define ESP_PARTITION_TYPE_DATA 1
#define ESP_PARTITION_SUBTYPE_DATA_COREDUMP 3
const esp_partition_t *esp_partition_find_first(int, int, const char *);
esp_err_t esp_partition_read(const esp_partition_t *, size_t, void *, size_t);
""",
    "esp_system.h": "#define ESP_RST_PANIC 4\nint esp_reset_reason(void);\n",
    "esp_memory_utils.h": "#include <stdbool.h>\nbool esp_ptr_executable(const void *);\n",
    "freertos/FreeRTOS.h": "typedef struct {char padding[32]; char ucDummy7[16];} StaticTask_t;\n",
    "xtensa_context.h": "typedef struct {uint32_t exit, pc, ps, a0, a1;} XtExcFrame;\n",
}
for name, body in stubs.items():
    path = work / name
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(body)
link = (component / "orb_link.c").read_text()
report = link[link.index("static void send_report(void)") : link.index("/* ---- What the device")]
source = work / "report.c"
source.write_text(
    (Path(__file__).with_name("test_crash.c")).read_text().replace("/* REPORT */", report)
)
binary = work / "test_crash"
subprocess.run(
    [
        "clang",
        "-std=c11",
        "-Wall",
        "-Wextra",
        "-Werror",
        "-Wno-deprecated-declarations",
        "-g",
        "-O1",
        "-fsanitize=address,undefined",
        "-fno-sanitize-recover=all",
        f"-I{work}",
        f"-I{component}",
        f"-I{component / 'include'}",
        f"-I{cjson}",
        str(source),
        str(component / "link_crash.c"),
        str(component / "link_crash_elf.c"),
        str(component / "link_proto.c"),
        str(cjson / "cJSON.c"),
        "-o",
        str(binary),
    ],
    check=True,
)
for boot in ("panic", "normal", "missing", "corrupt"):
    subprocess.run([str(binary), boot], check=True)
