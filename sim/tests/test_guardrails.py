"""Deliberately bad sources prove each architectural ratchet detects regressions."""

import contextlib
import importlib.util
import io
import json
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("orb_guardrails", ROOT / "scripts/guardrails.py")
GUARD = importlib.util.module_from_spec(SPEC)
assert SPEC.loader
SPEC.loader.exec_module(GUARD)


class GuardrailTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)

    def write(self, relative, source):
        path = self.root / "firmware/components" / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(source)
        return path

    def rules(self, relative, source, known=None):
        path = self.write(relative, source)
        return GUARD.source_rules(path, source, self.root, known or set())

    def assert_rule(self, rule, result):
        self.assertIn(rule, [item["rule"] for item in result], result)

    def test_kit_factory_and_alias_and_child_count(self):
        source = """
void screen(void) {
    lv_obj_t *chip = orb_kit_button_secondary_create(page, "9%");
    lv_obj_t *copy = (lv_obj_t *) chip;
    count = lv_obj_get_child_count(copy);
    child = lv_obj_get_child(copy, 0);
    if (lv_obj_get_child_count(copy) > 0) { result++; }
    child = lv_obj_get_child((lv_obj_t *) copy, 0);
}
"""
        result = self.rules("orb_app_example/screen.c", source)
        self.assertEqual(4, sum(item["rule"] == "kit_privacy" for item in result))
        self.assertFalse(self.rules("orb_app_example/good.c", "child = lv_obj_get_child(page, 0);"))
        self.assertFalse(self.rules("orb_ui/good.c", source))

    def test_kit_member_from_another_fragment(self):
        self.write("orb_app_example/factory.inc", "w->chip = orb_kit_disc_create(page);")
        self.write("orb_app_example/screen.c", "child = lv_obj_get_child(w->chip, 0);")
        self.assert_rule("kit_privacy", GUARD.scan(self.root))

    def test_kit_typed_and_named_child_lookups_and_counts(self):
        for call in (
            "lv_obj_get_child_by_type(copy, 0, &lv_label_class)",
            'lv_obj_get_child_by_name(copy, "caption")',
            "lv_obj_get_child_count_by_type(copy, &lv_label_class)",
        ):
            with self.subTest(call=call):
                source = (
                    "void screen(void) {\n"
                    "lv_obj_t *chip = orb_kit_contact_mark_create(page);\n"
                    "lv_obj_t *copy = (lv_obj_t *) chip;\n"
                    f"result = {call};\n"
                    "}\n"
                )
                self.assert_rule("kit_privacy", self.rules("orb_app_example/bad.c", source))
                self.assertFalse(self.rules("orb_ui/good.c", source))
                self.assertFalse(
                    self.rules("orb_app_example/good.c", call.replace("copy", "page") + ";")
                )

    def test_colour_hex_rgb_and_comment_mask(self):
        for sample in (
            "c = lv_color_hex(palette[i]);",
            "c = 0x123abc;",
            "c = 0x123abcu;",
            "c = lv_color_make(255, 12, 4);",
            "c = lv_color_make((uint8_t)255, (uint8_t)12, (uint8_t)4);",
            "c.red = 255;",
        ):
            with self.subTest(sample=sample):
                self.assert_rule("raw_colour", self.rules("orb_app_example/bad.c", sample))
        self.assertFalse(
            self.rules(
                "orb_app_example/good.c", '/* lv_color_hex(0x123456) */\nchar *s = "0x123456";'
            )
        )

    def test_colour_tokens_and_art_are_narrow(self):
        self.assertFalse(
            self.rules("orb_ui/include/orb_ui.h", "#define BLUE lv_color_hex(0x123abc)")
        )
        self.assertFalse(
            self.rules(
                "orb_games/orb_game_sprite_draw.c",
                "void game_sprite_draw(void) { c = lv_color_hex(0x123abc); }",
            )
        )
        self.assert_rule(
            "raw_colour",
            self.rules(
                "orb_games/orb_game_sprite_draw.c",
                "void button(void) { c = lv_color_hex(0x123abc); }",
            ),
        )
        self.assert_rule(
            "raw_control",
            self.rules(
                "orb_games/orb_game_sprite_draw.c",
                "void game_sprite_draw(void) { lv_button_create(page); }",
            ),
        )

    def test_scalar_token_contract_is_narrow(self):
        self.assertFalse(
            self.rules("orb_tokens/include/orb_color_tokens.h", "#define BLUE 0x123abc")
        )
        self.assert_rule(
            "raw_colour", self.rules("orb_tokens/include/other.h", "#define BLUE 0x123abc")
        )

    def test_palette_access_stays_inside_token_or_art_owners(self):
        for source in (
            "colour = lv_palette_main(LV_PALETTE_BLUE);",
            "colour = lv_palette_lighten(LV_PALETTE_BLUE, 2);",
            "colour = lv_palette_darken(LV_PALETTE_BLUE, 2);",
            "palette = &lv_palette_main;",
        ):
            with self.subTest(source=source):
                self.assert_rule("raw_colour", self.rules("orb_app_example/bad.c", source))
                self.assertFalse(self.rules("orb_ui/tokens/example.c", source))
        self.assertFalse(
            self.rules(
                "orb_app_example/good.c",
                '/* lv_palette_main(LV_PALETTE_BLUE) */\nchar *s = "lv_palette_lighten";',
            )
        )
        self.assertFalse(
            self.rules(
                "orb_games/orb_game_sprite_draw.c",
                "void game_sprite_draw(void) { colour = lv_palette_main(LV_PALETTE_BLUE); }",
            )
        )

    def test_all_raw_control_factories(self):
        for kind in ("button", "slider", "switch", "arc"):
            with self.subTest(kind=kind):
                self.assert_rule(
                    "raw_control", self.rules("orb_app_example/bad.c", f"lv_{kind}_create(page);")
                )

    def test_apps_cannot_bind_or_unbind_the_global_rotary_owner(self):
        for operation in ("bind", "unbind"):
            source = f"orb_nav_rotary_{operation}(nav, owner);"
            with self.subTest(operation=operation):
                self.assert_rule("rim_owner", self.rules("orb_app_example/bad.c", source))
                for owner in ("orb_screen", "orb_shell", "orb_nav"):
                    self.assertFalse(self.rules(f"{owner}/good.c", source))
        self.assertFalse(
            self.rules(
                "orb_app_example/good.c",
                """
/* orb_nav_rotary_bind(nav, owner); */
char *text = "orb_nav_rotary_unbind(nav);";
orb_screen_rim_acquire(instance);
""",
            )
        )

    def test_apps_cannot_alias_the_global_rotary_owner(self):
        for operation in ("bind", "unbind"):
            symbol = f"orb_nav_rotary_{operation}"
            for source in (
                f"callback = {symbol};",
                f"callback = &{symbol};",
                f"#define DIRECT_OWNER {symbol}\n",
            ):
                with self.subTest(source=source):
                    self.assert_rule("rim_owner", self.rules("orb_app_example/bad.c", source))
                    for owner in ("orb_screen", "orb_shell", "orb_nav"):
                        self.assertFalse(self.rules(f"{owner}/good.c", source))
        self.assertFalse(
            self.rules(
                "orb_app_example/good.c",
                """
/* callback = &orb_nav_rotary_bind; */
char *text = "orb_nav_rotary_unbind";
callback = orb_nav_rotary_binding_info;
""",
            )
        )

    def test_translation_unit_includes_fragment_lines(self):
        self.write("orb_app_example/body.inc", "\n".join("int x;" for _ in range(801)))
        result = self.rules("orb_app_example/bad.c", '#include "body.inc"\n')
        self.assert_rule("file_size", result)
        self.assertGreater(
            next(item["value"] for item in result if item["rule"] == "file_size"), 800
        )

    def test_psram_tasks_cannot_share_direct_flash_calls(self):
        for create in ("xTaskCreateWithCaps", "xTaskCreatePinnedToCoreWithCaps"):
            for operation in (
                "nvs_commit(h)",
                "nvs_open(name, NVS_READWRITE, &h)",
                "esp_partition_write(part, 0, buf, size)",
                "esp_partition_read(part, 0, buf, size)",
                "esp_flash_erase_region(chip, 0, size)",
                "spi_flash_disable_interrupts_caches_and_other_cpu()",
                "esp_ota_begin(part, size, &handle)",
            ):
                with self.subTest(create=create, operation=operation):
                    source = (
                        f"void init(void) {{ {create}(worker, name, 3072, NULL, 1, &task, "
                        "MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT); }\n"
                        f"void persist(void) {{ {operation}; }}\n"
                    )
                    self.assert_rule("psram_flash", self.rules("orb_sound/bad.c", source))

    def test_psram_stack_capability_argument_and_masking(self):
        source = """
void init(void) {
    void *buffer = heap_caps_malloc(1024, MALLOC_CAP_SPIRAM);
    xTaskCreateWithCaps(worker, name, 3072, NULL, 1, &task, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    nvs_commit(h);
}
"""
        self.assertFalse(self.rules("orb_sound/internal.c", source))
        self.assertFalse(
            self.rules(
                "orb_sound/internal_alias.c",
                "#define TASK_CAPS (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)\n"
                + source.replace("MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT", "TASK_CAPS"),
            )
        )
        self.assert_rule(
            "psram_flash",
            self.rules(
                "orb_sound/unconstrained.c",
                source.replace("MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT", "MALLOC_CAP_8BIT"),
            ),
        )
        self.assertFalse(
            self.rules(
                "orb_sound/comments.c",
                "/* xTaskCreateWithCaps(worker, MALLOC_CAP_SPIRAM); */\n"
                'char *text = "xTaskCreateWithCaps(worker, MALLOC_CAP_SPIRAM)";\n'
                "void save(void) { nvs_commit(h); }\n",
            )
        )
        self.assertFalse(
            self.rules(
                "orb_sound/comments.c",
                "void init(void) { xTaskCreateWithCaps(worker, MALLOC_CAP_SPIRAM); }\n"
                '/* nvs_commit(h); */\nchar *text = "esp_partition_write(part)";\n',
            )
        )

    def test_psram_alias_static_and_pthread_stacks(self):
        for declaration in (
            "#define TASK_CAPS (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)\n"
            "void init(void) { xTaskCreateWithCaps(worker, name, 3072, NULL, 1, &task, TASK_CAPS); }",
            "const unsigned caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;\n"
            "void init(void) { xTaskCreatePinnedToCoreWithCaps(worker, name, size, NULL, 1, &task, 0, caps); }",
            "EXT_RAM_BSS_ATTR static StackType_t stack[3072];\n"
            "void init(void) { xTaskCreateStatic(worker, name, 3072, NULL, 1, stack, &tcb); }",
            "void init(void) { StackType_t *stack = heap_caps_malloc(3072, MALLOC_CAP_SPIRAM);\n"
            "xTaskCreateStaticPinnedToCore(worker, name, 3072, NULL, 1, stack, &tcb, 0); }",
            "void init(void) { esp_pthread_cfg_t cfg = {.stack_alloc_caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT};\n"
            "esp_pthread_set_cfg(&cfg); }",
        ):
            with self.subTest(declaration=declaration):
                self.assert_rule(
                    "psram_flash",
                    self.rules(
                        "orb_sound/bad.c", declaration + "\nvoid save(void) { nvs_commit(h); }"
                    ),
                )

    def test_psram_guard_expands_included_task_or_flash_body(self):
        self.write(
            "orb_sound/worker.inc",
            "void init(void) { xTaskCreateWithCaps(worker, name, 3072, NULL, 1, &task, MALLOC_CAP_SPIRAM); }",
        )
        self.write("orb_sound/save.inc", "void save(void) { nvs_commit(h); }")
        result = self.rules("orb_sound/bad.c", '#include "worker.inc"\n#include "save.inc"\n')
        self.assertEqual(1, sum(item["rule"] == "psram_flash" for item in result))

    def test_flash_exceptions_are_per_call_documented_and_shrink_only(self):
        path = self.write(
            "orb_fonts/port.c",
            "void init(void) { xTaskCreateWithCaps(worker, MALLOC_CAP_SPIRAM); }\n"
            "void read(void) { esp_partition_read(part, 0, buf, size); }",
        )
        current = GUARD.psram_flash_rules(path, path.read_text(), self.root)
        exception = dict(current[0], reason="XIP read holds SPI1 mutex with cache enabled.")
        baseline = {"violations": [], "exceptions": [exception]}
        self.assertEqual([], GUARD.regressions(current, baseline))
        self.assertEqual([exception], GUARD.baseline_document(current, baseline)["exceptions"])
        self.assertEqual([], GUARD.baseline_document([], baseline)["exceptions"])
        # An undocumented exception and another call, even in the same TU, fail.
        self.assertEqual(current, GUARD.regressions(current, {"exceptions": current}))
        for extra in (
            "void other(void) { esp_partition_read(part, 0, buf, size); }",
            "void erase(void) { esp_partition_erase_range(part, 0, size); }",
        ):
            with self.subTest(extra=extra):
                result = GUARD.psram_flash_rules(path, path.read_text() + extra, self.root)
                self.assertEqual(1, len(GUARD.regressions(result, baseline)))

    def test_file_size_boundary(self):
        limit = GUARD.LIMITS["file_size"]
        self.assertFalse(self.rules("orb_app_example/good.c", "int x;\n" * limit))
        self.assert_rule("file_size", self.rules("orb_app_example/bad.c", "int x;\n" * (limit + 1)))

    def test_apps_cannot_call_foreign_factories_without_an_include(self):
        self.write("orb_app_other/other.c", "void orb_app_other_create(void) {}")
        source = """
void screen(void) {
    orb_app_other_create();
    orb_app_other_descriptor();
}
"""
        result = self.rules("orb_app_example/bad.c", source)
        self.assertEqual(2, sum(item["rule"] == "app_boundary" for item in result))
        self.assertFalse(self.rules("orb_shell/good.c", source))
        self.assertFalse(self.rules("orb_screen/good.c", source))
        self.assertFalse(
            self.rules(
                "orb_app_example/good.c",
                """
/* orb_app_other_create(); */
char *text = "orb_app_other_descriptor()";
void screen(void) { orb_app_example_descriptor(); }
""",
            )
        )
        self.assertFalse(
            self.rules(
                "orb_app_other_extra/good.c",
                "void screen(void) { orb_app_other_extra_create(); }",
            )
        )

    def test_function_logical_lines_and_included_body(self):
        # Multiple statements per physical line must not evade the limit.
        body = "\n".join("x++; y++;" for _ in range(51))
        self.write("orb_app_example/body.inc", body)
        result = self.rules("orb_app_example/bad.c", 'void bad(void) {\n#include "body.inc"\n}\n')
        self.assert_rule("function_size", result)
        self.assert_rule(
            "function_size",
            self.rules("orb_app_example/bad.h", "void bad(void) {\n" + body + "\n}\n"),
        )
        self.assertFalse(
            self.rules("orb_app_example/good.c", "void small(void) {\n/* x++; */\n}\n")
        )

    def test_service_and_ui_layering(self):
        for owner, target in (
            ("orb_model", "lvgl"),
            ("orb_home_data", "orb_ui"),
            ("orb_time", "orb_ui"),
            ("orb_link", "orb_screen"),
            ("orb_sound", "orb_shell"),
            ("orb_voice", "orb_app_example"),
            ("orb_ui", "orb_link"),
            ("orb_ui", "orb_shell"),
            ("orb_ui", "orb_screen"),
            ("orb_ui", "orb_app_example"),
        ):
            with self.subTest(owner=owner, target=target):
                source = '#include "bad.h"\n'
                path = self.write(f"{owner}/bad.c", source)
                result = GUARD.dependency_rules(
                    path, source, self.root, {"bad.h": {target}}, {target}
                )
                self.assert_rule("include_layering", result)

    def test_cmake_layering_without_any_include(self):
        for owner, target in (
            ("orb_model", "lvgl"),
            ("orb_home_data", "orb_ui"),
            ("orb_time", "orb_ui"),
            ("orb_sound", "orb_app_example"),
            ("orb_ui", "orb_shell"),
            ("orb_ui", "orb_screen"),
            ("orb_app_example", "orb_app_other"),
        ):
            with self.subTest(owner=owner, target=target):
                self.write(f"{owner}/good.c", "void screen(void) {}")
                for kind in ("REQUIRES", "PRIV_REQUIRES"):
                    self.write(
                        f"{owner}/CMakeLists.txt", f"idf_component_register({kind} {target})"
                    )
                    result = GUARD.scan(self.root)
                    self.assertTrue(
                        any(
                            item["rule"] == "include_layering"
                            and item["path"] == f"firmware/components/{owner}/CMakeLists.txt"
                            for item in result
                        ),
                        result,
                    )
        self.assertFalse(
            GUARD.declaration_rules(
                self.write("orb_ui/CMakeLists.txt", ""), self.root, "orb_ui", {"lvgl", "orb_tokens"}
            )
        )

    def test_relative_cross_component_private_include(self):
        self.write("orb_other/private.h", "")
        source = '#include "../orb_other/private.h"\n'
        path = self.write("orb_app_example/bad.c", source)
        result = GUARD.dependency_rules(path, source, self.root, {}, {"orb_other"})
        self.assert_rule("include_layering", result)

    def test_actual_include_must_have_direct_cmake_dependency(self):
        self.write("orb_other/include/orb_other.h", "")
        source = '#include "orb_other.h"\n#include <esp_timer.h>\n'
        path = self.write("orb_app_example/bad.c", source)
        index = GUARD.header_index(self.root, None)
        index["esp_timer.h"] = {"esp_timer"}
        result = GUARD.dependency_rules(path, source, self.root, index, set())
        self.assertEqual(2, sum(item["rule"] == "undeclared_dependency" for item in result))
        self.assertFalse(
            GUARD.dependency_rules(path, source, self.root, index, {"orb_other", "esp_timer"})
        )
        self.assertEqual(
            {"orb_other", "esp_timer"},
            GUARD.declared_dependencies(
                'idf_component_register(SRCS "a.c" REQUIRES orb_other PRIV_REQUIRES esp_timer)'
            ),
        )

    def test_index_resolves_sdk_and_managed_component_owners(self):
        sdk = self.root / "sdk/components/esp_timer/include/esp_timer.h"
        sdk.parent.mkdir(parents=True)
        sdk.write_text("")
        private = self.root / "sdk/components/unrelated/private/esp_timer.h"
        private.parent.mkdir(parents=True)
        private.write_text("")
        managed = self.root / "firmware/managed_components/espressif__codec/include/codec.h"
        managed.parent.mkdir(parents=True)
        managed.write_text("")
        index = GUARD.header_index(self.root, self.root / "sdk")
        self.assertEqual({"esp_timer"}, index["esp_timer.h"])
        self.assertEqual({"codec"}, index["codec.h"])

    def test_baseline_only_shrinks_even_with_regenerate_flag(self):
        source = "void bad(void) {\n" + "x++;\n" * 110 + "}\n"
        self.write("orb_app_example/bad.c", source)
        baseline = self.root / "baseline.json"
        argv = [
            "--root",
            str(self.root),
            "--baseline",
            str(baseline),
            "--idf-path",
            str(self.root / "missing"),
        ]
        with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
            self.assertEqual(0, GUARD.main(argv + ["--regenerate-baseline"]))
            original = baseline.read_text()
            self.write("orb_app_example/bad.c", source.replace("x++;", "x++; y++;"))
            self.assertEqual(1, GUARD.main(argv))
            self.assertEqual(1, GUARD.main(argv + ["--regenerate-baseline"]))
            self.assertEqual(original, baseline.read_text())
            self.write("orb_app_example/bad.c", "void bad(void) {}\n")
            self.assertEqual(0, GUARD.main(argv + ["--regenerate-baseline"]))
            self.assertEqual([], json.loads(baseline.read_text())["violations"])
            self.write("orb_app_example/new.c", "lv_arc_create(page);\n")
            self.assertEqual(1, GUARD.main(argv + ["--regenerate-baseline"]))


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]])
