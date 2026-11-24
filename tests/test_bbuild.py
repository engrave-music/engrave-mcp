"""Regression tests for scripts/bbuild.py target mapping, BEP staging, and compdb generation."""

import importlib.util
import json
import os
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import MagicMock, patch

REPO_ROOT = Path(__file__).resolve().parents[1]
BBUILD_PATH = REPO_ROOT / "scripts/bbuild.py"
SPEC = importlib.util.spec_from_file_location("bbuild_under_test", BBUILD_PATH)
assert SPEC and SPEC.loader
bbuild = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(bbuild)


class BBuildTests(unittest.TestCase):
    def test_resolve_targets_maps_host_engine_and_registry_check(self):
        labels = bbuild.resolve_targets(["engrave_mcp", "registry_check", "semantic_dump", "//:staging_test"])
        self.assertEqual(
            labels,
            [
                "//:engrave-mcp",
                "//:registry_dump",
                "//:registry_test",
                "@engrave//:semantic_dump",
                "//:staging_test",
            ],
        )

    def test_stage_bep_outputs_copies_default_group_executables_only(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            src_dir = root / "bazel-out/bin"
            src_dir.mkdir(parents=True)
            ext = ".exe" if os.name == "nt" else ""
            exe_file = src_dir / f"engrave-mcp{ext}"
            obj_file = src_dir / "main.o"
            exe_file.write_bytes(b"binary-payload")
            obj_file.write_bytes(b"object-payload")
            os.chmod(exe_file, 0o755)

            bep_events = [
                {
                    "id": {"namedSet": {"id": "0"}},
                    "namedSetOfFiles": {
                        "files": [
                            {"uri": exe_file.as_uri()},
                            {"uri": obj_file.as_uri()},
                        ]
                    },
                },
                {
                    "completed": {
                        "outputGroup": [
                            {"name": "default", "fileSets": [{"id": "0"}]},
                        ]
                    }
                },
            ]
            bep_path = root / "bep.json"
            bep_path.write_text("\n".join(json.dumps(ev) for ev in bep_events) + "\n", encoding="utf-8")

            out_dir = root / "build/release"
            rc = bbuild.stage_bep_outputs(bep_path, out_dir)
            self.assertEqual(rc, 0)
            self.assertTrue((out_dir / f"engrave-mcp{ext}").is_file())
            self.assertFalse((out_dir / "main.o").exists())
            self.assertEqual((out_dir / f"engrave-mcp{ext}").read_bytes(), b"binary-payload")

    def test_stage_bep_outputs_includes_runtime_libraries_and_preserves_default_outputs(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            out_dir = root / "build/sanitize"
            default_dir = root / "default"
            runtime_dir = root / "runtime"
            default_dir.mkdir()
            runtime_dir.mkdir()
            suffix = ".dll" if os.name == "nt" else ".so"
            runtime = runtime_dir / f"libsanitizer{suffix}"
            default = default_dir / f"libshared{suffix}"
            duplicate = runtime_dir / default.name
            for path, content in [(runtime, b"sanitizer"), (default, b"default"), (duplicate, b"duplicate")]:
                path.write_bytes(content)

            bep_events = [
                {
                    "id": {"namedSet": {"id": "0"}},
                    "namedSetOfFiles": {"files": [{"uri": default.as_uri()}]},
                },
                {
                    "id": {"namedSet": {"id": "1"}},
                    "namedSetOfFiles": {
                        "files": [{"uri": duplicate.as_uri()}, {"uri": runtime.as_uri()}, {"uri": runtime.as_uri()}]
                    },
                },
                {
                    "completed": {
                        "outputGroup": [
                            {"name": "default", "fileSets": [{"id": "0"}]},
                            {"name": "runtime_dynamic_libraries", "fileSets": [{"id": "1"}]},
                        ]
                    }
                },
            ]
            bep_path = root / "bep.json"
            bep_path.write_text("\n".join(json.dumps(event) for event in bep_events), encoding="utf-8")

            bbuild.stage_bep_outputs(bep_path, out_dir)

            self.assertEqual(
                {path.name: path.read_bytes() for path in out_dir.iterdir()},
                {default.name: b"default", runtime.name: b"sanitizer"},
            )

    def test_generate_compdb_expands_cfg_strips_pch_and_deduplicates_pic(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory).resolve()
            execroot = root / "execroot"
            (root / "engrave_mcp").mkdir(parents=True)
            src = root / "engrave_mcp/main.cpp"
            src.write_text("int main() { return 0; }\n", encoding="utf-8")

            # Simulate execroot relative paths and xclang config file.
            (execroot / "engrave_mcp").mkdir(parents=True)
            (execroot / "engrave_mcp/main.cpp").write_text("int main() { return 0; }\n", encoding="utf-8")
            cfg_dir = execroot / "external/xclang/bin"
            sysroot_dir = execroot / "external/xclang/sysroot"
            inc_dir = execroot / "external/xclang/include/c++/v1"
            cfg_dir.mkdir(parents=True)
            sysroot_dir.mkdir(parents=True)
            inc_dir.mkdir(parents=True)
            cfg_file = cfg_dir / "x86_64.cfg"
            cfg_file.write_text(
                "--target=x86_64-unknown-linux-gnu\n"
                "--sysroot=external/xclang/sysroot\n"
                "-stdlib++-isystem external/xclang/include/c++/v1\n",
                encoding="utf-8",
            )

            aquery_payload = {
                "actions": [
                    {
                        "arguments": [
                            "clang++",
                            "--config=external/xclang/bin/x86_64.cfg",
                            "-std=c++23",
                            "-Xclang",
                            "-fno-validate-pch",
                            "-Xclang",
                            "-isysroot",
                            "-Xclang",
                            ".",
                            "-fPIC",
                            "-include-pch",
                            "bazel-out/engraving.pch",
                            "-c",
                            str(src),
                            "-o",
                            "bazel-out/main.pic.o",
                        ]
                    },
                    {
                        "arguments": [
                            "clang++",
                            "--config=external/xclang/bin/x86_64.cfg",
                            "-std=c++23",
                            "-include-pch",
                            "bazel-out/engraving.pch",
                            "-c",
                            str(src),
                            "-o",
                            "bazel-out/main.o",
                        ]
                    },
                ]
            }

            def fake_run(cmd, **kwargs):
                res = MagicMock()
                res.returncode = 0
                res.stderr = ""
                if cmd[1] == "info":
                    res.stdout = str(execroot) + "\n"
                else:
                    res.stdout = json.dumps(aquery_payload)
                return res

            out_dir = root / "build/release"
            with (
                patch.object(bbuild.subprocess, "call", return_value=0),
                patch.object(bbuild.subprocess, "run", side_effect=fake_run),
            ):
                rc = bbuild.generate_compdb("bazel", root, {}, [], [], ["//:all_binaries"], out_dir)

            self.assertEqual(rc, 0)
            entries = json.loads((out_dir / "compile_commands.json").read_text(encoding="utf-8"))
            self.assertEqual(len(entries), 1)
            args = entries[0]["arguments"]
            self.assertNotIn("-include-pch", args)
            self.assertNotIn("-fno-validate-pch", args)
            self.assertNotIn("-isysroot", args)
            self.assertFalse(any(a.startswith("--config=") for a in args))
            self.assertIn("--target=x86_64-unknown-linux-gnu", args)
            self.assertIn(f"--sysroot={str(sysroot_dir.resolve()).replace(chr(92), '/')}", args)

    def test_prune_disk_cache_evicts_oldest_and_cleans_tmp(self):
        with tempfile.TemporaryDirectory() as directory:
            cache = Path(directory)
            (cache / "ac/00").mkdir(parents=True)
            (cache / "cas/aa").mkdir(parents=True)
            (cache / "tmp/partial").mkdir(parents=True)

            (cache / "tmp/orphan").write_bytes(b"x" * 100)
            (cache / "tmp/partial/blob").write_bytes(b"y" * 100)

            old_ac = cache / "ac/00/old"
            old_cas = cache / "cas/aa/old"
            new_ac = cache / "ac/00/new"
            new_cas = cache / "cas/aa/new"

            old_ac.write_bytes(b"a" * 300)
            old_cas.write_bytes(b"b" * 400)
            new_ac.write_bytes(b"c" * 200)
            new_cas.write_bytes(b"d" * 300)

            os.utime(old_ac, ns=(1_000_000_000, 1_000_000_000))
            os.utime(old_cas, ns=(2_000_000_000, 2_000_000_000))
            os.utime(new_ac, ns=(3_000_000_000, 3_000_000_000))
            os.utime(new_cas, ns=(4_000_000_000, 4_000_000_000))

            before, after, removed = bbuild.prune_disk_cache(cache, max_bytes=500)

            self.assertEqual((before, after, removed), (1200, 500, 4))
            self.assertFalse(old_ac.exists())
            self.assertFalse(old_cas.exists())
            self.assertTrue(new_ac.is_file())
            self.assertTrue(new_cas.is_file())
            self.assertEqual(list((cache / "tmp").iterdir()), [])

    def test_prune_disk_cache_noop_when_within_budget_still_cleans_tmp(self):
        with tempfile.TemporaryDirectory() as directory:
            cache = Path(directory)
            (cache / "cas/01").mkdir(parents=True)
            (cache / "tmp").mkdir(parents=True)
            blob = cache / "cas/01/blob"
            blob.write_bytes(b"z" * 256)
            (cache / "tmp/stale").write_bytes(b"t" * 64)

            before, after, removed = bbuild.prune_disk_cache(cache, max_bytes=512)

            self.assertEqual((before, after, removed), (256, 256, 1))
            self.assertTrue(blob.is_file())
            self.assertEqual(list((cache / "tmp").iterdir()), [])

    def test_prune_cache_cli_runs_shutdown_before_prune_and_rejects_bad_limit(self):
        order: list[str] = []

        def fake_call(cmd, **kwargs):
            self.assertEqual(cmd, ["fake-bazel", "shutdown"])
            order.append("shutdown")
            return 0

        def fake_prune(path, max_bytes):
            self.assertEqual(path, REPO_ROOT / "build/.bazel_disk_cache")
            self.assertEqual(max_bytes, 3 * bbuild.MIB)
            order.append("prune")
            return (0, 0, 0)

        with (
            patch.object(bbuild, "find_bazel", return_value="fake-bazel"),
            patch.object(bbuild.subprocess, "call", side_effect=fake_call),
            patch.object(bbuild, "prune_disk_cache", side_effect=fake_prune),
        ):
            rc = bbuild.main(["--max-disk-cache-mb", "3", "--shutdown", "prune-cache"])
        self.assertEqual(rc, 0)
        self.assertEqual(order, ["shutdown", "prune"])

        for bad_args in (["prune-cache"], ["--max-disk-cache-mb", "0", "prune-cache"]):
            with self.assertRaises(SystemExit) as ctx:
                bbuild.main(bad_args)
            self.assertNotEqual(ctx.exception.code, 0)

    def test_prune_disk_cache_rejects_symlink_root(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            target = root / "real_cache"
            (target / "cas/00").mkdir(parents=True)
            victim = target / "cas/00/blob"
            victim.write_bytes(b"a" * 200)
            link = root / "link_cache"
            try:
                link.symlink_to(target, target_is_directory=True)
            except OSError as error:
                self.skipTest(f"symlink creation unavailable: {error}")

            with self.assertRaises(ValueError):
                bbuild.prune_disk_cache(link, max_bytes=1)
            self.assertTrue(victim.is_file())


if __name__ == "__main__":
    sys.exit(unittest.main())
