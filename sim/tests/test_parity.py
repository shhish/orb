"""Exercise the evidence tool with real artifacts and deliberate regressions."""

import argparse
import contextlib
import importlib.util
import io
import json
import shutil
import struct
import sys
import tempfile
import unittest
import zlib
from pathlib import Path
from unittest import mock

spec = importlib.util.spec_from_file_location("orb_parity", Path(__file__).parents[1] / "parity.py")
parity = importlib.util.module_from_spec(spec)
spec.loader.exec_module(parity)


class ParityTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)

    def artifact(self, name="base"):
        directory = self.root / name
        destination = directory / "face" / "default"
        destination.mkdir(parents=True)
        png, trace = destination / "settled.png", destination / "settled.json"
        parity.write_png(png, 3, 2, b"\x00\x10\x20" * 6)
        parity.write_json(
            trace, {"objects": [{"id": "send", "role": "button"}], "cues": [], "rim": "none"}
        )
        frame = {
            "scenario": "face",
            "profile": "default",
            "frame": "settled",
            "png": png.relative_to(directory).as_posix(),
            "trace": trace.relative_to(directory).as_posix(),
            "sha256": parity.sha256(png),
            "trace_sha256": parity.sha256(trace),
            "width": 3,
            "height": 2,
        }
        parity.write_json(directory / "manifest.json", {"format_version": 1, "frames": [frame]})
        return directory

    def compare(self, base, new):
        output = self.root / "diff"
        with contextlib.redirect_stdout(io.StringIO()):
            status = parity.compare(argparse.Namespace(base_dir=base, new_dir=new, out=output))
        return status, parity.read_json(output / "report.json"), output

    def update_hashes(self, directory):
        manifest = parity.read_json(directory / "manifest.json")
        for frame in manifest["frames"]:
            frame["sha256"] = parity.sha256(directory / frame["png"])
            frame["trace_sha256"] = parity.sha256(directory / frame["trace"])
        parity.write_json(directory / "manifest.json", manifest)

    def test_identical_tree(self):
        base = self.artifact()
        new = self.root / "new"
        shutil.copytree(base, new)
        status, report, _ = self.compare(base, new)
        self.assertEqual(status, 0)
        self.assertEqual(report["trace_differences"], [])
        self.assertEqual(report["pixel_differences"], [])

    def test_one_pixel_count_and_inclusive_bbox(self):
        base = self.artifact()
        new = self.root / "new"
        shutil.copytree(base, new)
        png = new / "face/default/settled.png"
        pixels = bytearray(parity.read_png(png)[2])
        pixels[15] += 1  # x=2, y=1, one channel changes by one level.
        parity.write_png(png, 3, 2, bytes(pixels))
        self.update_hashes(new)
        status, report, output = self.compare(base, new)
        self.assertEqual(status, 1)
        difference = report["pixel_differences"][0]
        self.assertEqual(difference["count"], 1)
        self.assertEqual(difference["bbox"], [2, 1, 2, 1])
        diff = parity.read_png(output / difference["diff"])[2]
        self.assertEqual(diff[:15], b"\x00" * 15)
        self.assertEqual(diff[15:], b"\xff\x00\x00")

    def test_every_semantic_leaf(self):
        base = self.artifact()
        new = self.root / "new"
        shutil.copytree(base, new)
        parity.write_json(
            new / "face/default/settled.json",
            {
                "objects": [{"id": "send", "role": "label", "disabled": True}],
                "cues": [{"role": "sent", "count": 2}],
                "rim": "volume",
            },
        )
        self.update_hashes(new)
        status, report, _ = self.compare(base, new)
        self.assertEqual(status, 1)
        self.assertEqual(len(report["trace_differences"]), 4)
        self.assertTrue(
            any(item["path"].endswith('["role"]') for item in report["trace_differences"])
        )

    def test_missing_extra_and_hash_corruption(self):
        base = self.artifact()
        new = self.root / "new"
        shutil.copytree(base, new)
        (new / "face/default/settled.json").unlink()
        (new / "unexpected.txt").write_text("extra")
        png = new / "face/default/settled.png"
        png.write_bytes(png.read_bytes()[:-1] + b"\0")
        status, report, _ = self.compare(base, new)
        self.assertEqual(status, 1)
        errors = "\n".join(report["artifact_errors"]["new"])
        self.assertIn("missing", errors)
        self.assertIn("hash mismatch", errors)
        self.assertIn("invalid PNG", errors)
        self.assertIn("unlisted file unexpected.txt", errors)

    def test_dimensions_are_differences(self):
        base = self.artifact()
        new = self.root / "new"
        shutil.copytree(base, new)
        parity.write_png(new / "face/default/settled.png", 2, 2, b"\x00\x10\x20" * 4)
        self.update_hashes(new)
        status, report, _ = self.compare(base, new)
        self.assertEqual(status, 1)
        self.assertEqual(report["pixel_differences"][0]["count"], 2)
        self.assertEqual(report["pixel_differences"][0]["bbox"], [2, 0, 2, 1])

    def test_png_filters_and_crc(self):
        # Two two-pixel RGB rows, encoded under all five legal filter kinds.
        rgb = bytes(range(12))
        for kind in range(5):
            raw = bytearray()
            previous = b"\0" * 6
            for y in range(2):
                row = rgb[y * 6 : y * 6 + 6]
                raw.append(kind)
                for x, value in enumerate(row):
                    a = row[x - 3] if x >= 3 else 0
                    b, c = previous[x], previous[x - 3] if x >= 3 else 0
                    prediction = (0, a, b, (a + b) // 2, parity._paeth(a, b, c))[kind]
                    raw.append((value - prediction) & 255)
                previous = row
            png = self.root / f"filter-{kind}.png"
            png.write_bytes(
                parity.PNG_MAGIC
                + parity._chunk(b"IHDR", struct.pack(">IIBBBBB", 2, 2, 8, 2, 0, 0, 0))
                + parity._chunk(b"IDAT", zlib.compress(raw))
                + parity._chunk(b"IEND", b"")
            )
            self.assertEqual(parity.read_png(png), (2, 2, rgb))

    def test_paths_and_output_safety(self):
        for value in ("../secret", "/tmp/secret"):
            with self.assertRaises(ValueError):
                parity._safe_relative(value)
        base = self.artifact()
        with self.assertRaises(ValueError):
            parity.compare(argparse.Namespace(base_dir=base, new_dir=base, out=base / "diff"))
        with self.assertRaises(ValueError):
            parity.capture(argparse.Namespace(out_dir=base))

    def test_capture_reproducibility_parallel_and_shards(self):
        binary = self.root / "fixture"
        tool = Path(__file__).parents[1] / "parity.py"
        binary.write_text(
            f"#!{sys.executable}\n"
            "import importlib.util, json, pathlib, sys\n"
            f"s=importlib.util.spec_from_file_location('p',{str(tool)!r})\n"
            "p=importlib.util.module_from_spec(s); s.loader.exec_module(p)\n"
            "if sys.argv[1]=='--list':\n"
            " print(json.dumps([{'id':'b','family':'face','state':'normal','rim':'none'},"
            "{'id':'a','family':'face','state':'normal','rim':'none'}]));sys.exit(0)\n"
            "out=pathlib.Path(sys.argv[3]);p.write_png(out/'settled.png',2,2,b'\\0'*12)\n"
            "p.write_json(out/'settled.json',{'scenario':sys.argv[1],'profile':sys.argv[2]})\n"
        )
        binary.chmod(0o755)
        with contextlib.redirect_stdout(io.StringIO()):
            for name, jobs in (("a", 1), ("b", 4)):
                parity.capture(
                    argparse.Namespace(
                        out_dir=self.root / name, binary=binary, jobs=jobs, shard=(1, 1), filter="*"
                    )
                )
            parity.capture(
                argparse.Namespace(
                    out_dir=self.root / "shard", binary=binary, jobs=4, shard=(2, 3), filter="*"
                )
            )
        first = self.root / "a"
        second = self.root / "b"
        self.assertEqual(parity._files(first), parity._files(second))
        for file in parity._files(first):
            self.assertEqual((first / file).read_bytes(), (second / file).read_bytes())
        manifest = parity.read_json(first / "manifest.json")
        self.assertEqual(len(manifest["frames"]), 8)
        shard = parity.read_json(self.root / "shard/manifest.json")
        self.assertEqual(len(shard["frames"]), 3)
        self.assertEqual(shard["selection"]["shard"], [2, 3])

    def test_manifest_schema_and_duplicate_frames_rejected(self):
        base = self.artifact()
        new = self.root / "new"
        shutil.copytree(base, new)
        manifest = parity.read_json(new / "manifest.json")
        manifest["frames"].append(dict(manifest["frames"][0]))
        parity.write_json(new / "manifest.json", manifest)
        status, report, _ = self.compare(base, new)
        self.assertEqual(status, 1)
        self.assertTrue(
            any("duplicate frame" in error for error in report["artifact_errors"]["new"])
        )
        parity.write_json(new / "manifest.json", {"frames": [None]})
        with contextlib.redirect_stderr(io.StringIO()):
            status = parity.main(["parity-compare", str(base), str(new)])
        self.assertEqual(status, 2)

    def test_effective_compiler_settings_and_path_normalization(self):
        build = self.root / "build"
        compiler = build / "CMakeFiles/4.4.3"
        compiler.mkdir(parents=True)
        (compiler / "CMakeCCompiler.cmake").write_text(
            'set(CMAKE_C_COMPILER_ID "AppleClang")\n'
            'set(CMAKE_C_COMPILER_VERSION "21.0.0")\n'
            "set(CMAKE_C_COMPILER_ARCHITECTURE_ID arm64)\n"
        )
        (compiler / "CMakeSystem.cmake").write_text('set(CMAKE_SYSTEM_PROCESSOR "arm64")\n')
        (build / "CMakeCache.txt").write_text(
            "CMAKE_BUILD_TYPE:STRING=\nCMAKE_C_COMPILER:FILEPATH=/usr/bin/cc\n"
            "CMAKE_C_FLAGS:STRING=-ffast-math\n"
            "CMAKE_C_FLAGS_RELWITHDEBINFO:STRING=-O2 -g -DNDEBUG\n"
        )
        (build / "build.ninja").write_text(
            "CONFIGURATION = RelWithDebInfo\n"
            "build CMakeFiles/orb.dir/example.c.o: C_COMPILER__orb_unscanned_RelWithDebInfo example.c\n"
            "  CONFIG = RelWithDebInfo\n"
            f"  DEFINES = -DASSET={self.root}/font.pack\n"
            "  FLAGS = -ffast-math -O2 -g -DNDEBUG -std=gnu11\n"
        )
        metadata = parity.compiler_metadata(build / "orb_parity", self.root)
        self.assertEqual(metadata["compiler"]["compiler_version"], "21.0.0")
        self.assertEqual(metadata["compiler"]["compiler_architecture_id"], "arm64")
        self.assertEqual(metadata["effective_global_c_flags"], "-ffast-math -O2 -g -DNDEBUG")
        self.assertEqual(metadata["configuration"], "RelWithDebInfo")
        self.assertEqual(
            metadata["target_settings"]["orb"][0]["defines"], "-DASSET=$REPO/font.pack"
        )

    def test_identically_corrupt_valid_crc_pngs_fail_comparison(self):
        raster = (b"\0" + bytes(9)) * 2
        payloads = {
            "invalid-zlib": b"not a zlib stream",
            "short-raster": zlib.compress(raster[:-1]),
            "long-raster": zlib.compress(raster + b"\0"),
            "invalid-filter": zlib.compress(b"\x05" + raster[1:]),
            "trailing-stream": zlib.compress(raster) + b"trailing",
        }
        for name, compressed in payloads.items():
            with self.subTest(name=name):
                base = self.artifact("base-" + name)
                png = base / "face/default/settled.png"
                png.write_bytes(
                    parity.PNG_MAGIC
                    + parity._chunk(b"IHDR", struct.pack(">IIBBBBB", 3, 2, 8, 2, 0, 0, 0))
                    + parity._chunk(b"IDAT", compressed)
                    + parity._chunk(b"IEND", b"")
                )
                self.update_hashes(base)
                new = self.root / ("new-" + name)
                shutil.copytree(base, new)
                status, report, output = self.compare(base, new)
                self.assertEqual(status, 1)
                self.assertEqual(report["file_differences"], [])
                for side in ("base", "new"):
                    self.assertTrue(
                        any("invalid PNG" in error for error in report["artifact_errors"][side])
                    )
                shutil.rmtree(output)

    def test_capture_rejects_asset_or_executable_drift(self):
        binary = self.root / "fixture"
        binary.write_bytes(b"fixture-v1")

        def run_fixture(_binary, *arguments):
            if arguments == ("--list",):
                return json.dumps(
                    [{"id": "fixture", "family": "test", "state": "normal", "rim": "none"}]
                )
            destination = Path(arguments[2])
            parity.write_png(destination / "settled.png", 2, 2, bytes(12))
            parity.write_json(destination / "settled.json", {"case": arguments[0]})
            return ""

        args = argparse.Namespace(
            out_dir=self.root / "asset-drift", binary=binary, jobs=1, shard=(1, 1), filter="*"
        )
        with (
            mock.patch.object(parity, "_run", side_effect=run_fixture),
            mock.patch.object(
                parity, "capture_metadata", side_effect=[{"font": "before"}, {"font": "after"}]
            ),
        ):
            with self.assertRaisesRegex(ValueError, "assets or renderer configuration changed"):
                parity.capture(args)
        self.assertFalse((args.out_dir / "manifest.json").exists())

        def rebuilding_fixture(_binary, *arguments):
            result = run_fixture(_binary, *arguments)
            if arguments != ("--list",):
                binary.write_bytes(b"fixture-v2")
            return result

        args.out_dir = self.root / "binary-drift"
        with (
            mock.patch.object(parity, "_run", side_effect=rebuilding_fixture),
            mock.patch.object(parity, "capture_metadata", return_value={"font": "same"}),
        ):
            with self.assertRaisesRegex(ValueError, "executable changed"):
                parity.capture(args)
        self.assertFalse((args.out_dir / "manifest.json").exists())

    def test_trace_whitespace_is_still_byte_difference(self):
        base = self.artifact()
        new = self.root / "new"
        shutil.copytree(base, new)
        trace = new / "face/default/settled.json"
        trace.write_text(json.dumps(parity.read_json(trace)))
        self.update_hashes(new)
        status, report, _ = self.compare(base, new)
        self.assertEqual(status, 1)
        self.assertEqual(report["trace_differences"], [])
        self.assertTrue(report["file_differences"])


if __name__ == "__main__":
    unittest.main()
