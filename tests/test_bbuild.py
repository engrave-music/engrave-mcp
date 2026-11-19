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
                            "-include-pch",
                            "bazel-out/engraving.pch",
                            "-fPIC",
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
            self.assertFalse(any(a.startswith("--config=") for a in args))
            self.assertIn("--target=x86_64-unknown-linux-gnu", args)
            self.assertIn(f"--sysroot={str(sysroot_dir.resolve()).replace(chr(92), '/')}", args)


if __name__ == "__main__":
    sys.exit(unittest.main())
