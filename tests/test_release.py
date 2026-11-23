"""Exercise release archive and checksum contracts without a host executable."""

import contextlib
import io
import json
import os
import stat
import subprocess
import sys
import tarfile
import tempfile
import unittest
import zipfile
from pathlib import Path
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))

import release_archive  # noqa: E402


class ReleaseTest(unittest.TestCase):
    def setUp(self):
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        self.work = Path(temp.name)

    def declare_version(self, version):
        (self.work / "engrave_mcp").mkdir(exist_ok=True)
        (self.work / "engrave_mcp/version.h").write_text(f'const auto host_version = "{version}";\n', encoding="utf-8")
        (self.work / "MODULE.bazel").write_text(f'module(version = "{version}")\n', encoding="utf-8")
        (self.work / "pixi.toml").write_text(f'[workspace]\nversion = "{version}"\n', encoding="utf-8")

    def run_cli(self, *args):
        with contextlib.redirect_stdout(io.StringIO()) as stdout:
            self.assertEqual(release_archive.main(list(map(str, args))), 0)
        return stdout.getvalue().strip()

    def assert_cli_error(self, pattern, *args):
        with contextlib.redirect_stderr(io.StringIO()) as stderr, self.assertRaises(SystemExit) as result:
            release_archive.main(list(map(str, args)))
        self.assertEqual(result.exception.code, 1)
        self.assertRegex(stderr.getvalue(), pattern)

    def test_check_tag(self):
        with mock.patch.object(release_archive, "ROOT", self.work):
            for version, prerelease in (("0.1.0", False), ("0.1.0-rc.1", True)):
                with self.subTest(version=version):
                    self.declare_version(version)
                    self.assertEqual(self.run_cli("check-tag", "--tag", f"v{version}"), version)
                    self.assertEqual(
                        json.loads(self.run_cli("check-tag", "--json")),
                        {"version": version, "prerelease": prerelease},
                    )
                    self.assert_cli_error(f"expected tag v{version}", "check-tag", "--tag", version)
                    self.assert_cli_error(f"expected tag v{version}", "check-tag", "--tag", "v9.9.9")

    def test_check_binary(self):
        self.declare_version("0.1.0")
        binary = self.work / "engrave-mcp.exe"
        binary.touch()
        with (
            mock.patch.object(release_archive, "ROOT", self.work),
            mock.patch.object(release_archive.subprocess, "run", autospec=True) as run,
        ):
            run.return_value = subprocess.CompletedProcess([], 0, "engrave-mcp 0.1.0\r\n", "")
            self.assertEqual(self.run_cli("check-tag", "--binary", binary, "--clean-env"), "0.1.0")
            self.assertEqual(run.call_args.args[0], [binary.resolve(), "--version"])
            self.assertEqual(run.call_args.kwargs["cwd"], binary.resolve().parent)
            self.assertEqual(
                run.call_args.kwargs["env"]["PATH"],
                str(Path(os.environ["SystemRoot"]) / "System32") if os.name == "nt" else "/usr/bin:/bin",
            )
            run.return_value = subprocess.CompletedProcess([], 0, "engrave-mcp 0.2.0\n", "")
            self.assert_cli_error("expected 'engrave-mcp 0.1.0'", "check-tag", "--binary", binary)
            run.return_value = subprocess.CompletedProcess([], 1, "engrave-mcp 0.1.0\n", "loader error")
            self.assert_cli_error("exited with code 1: loader error", "check-tag", "--binary", binary)

    def test_pack_round_trip(self):
        bundle = self.work / "bundle"
        (bundle / "r").mkdir(parents=True)
        executable = "engrave-mcp.exe" if os.name == "nt" else "engrave-mcp"
        files = {"r/score.mscx": b"\x00score\x80\xff", executable: b"\x00\x00binary", "about.txt": b"hi\r\n"}
        for name, payload in files.items():
            (bundle / name).write_bytes(payload)
        (bundle / executable).chmod(0o755)

        for archive_format, mtime in (("zip", 1_700_000_000), ("tar.gz", 1_710_000_000)):
            with self.subTest(archive_format=archive_format):
                path = self.work / f"release.{archive_format}"
                command = ("pack", "--format", archive_format, "--source", bundle)
                root, root_args = ("bundle", ()) if archive_format == "zip" else ("release", ("--root-name", "release"))
                self.run_cli(*command, *root_args, "--output", path)
                self.assert_cli_error("[Ee]xists", *command, *root_args, "--output", path)
                copy = self.work / f"copy.{archive_format}"
                os.utime(bundle / "about.txt", (mtime, mtime))
                self.run_cli(*command, *root_args, "--output", copy)
                self.assertEqual(path.read_bytes(), copy.read_bytes())

                expected_names = [
                    root,
                    f"{root}/about.txt",
                    f"{root}/{executable}",
                    f"{root}/r",
                    f"{root}/r/score.mscx",
                ]
                extracted = self.work / archive_format
                if archive_format == "zip":
                    with zipfile.ZipFile(path) as archive:
                        self.assertEqual([item.rstrip("/") for item in archive.namelist()], expected_names)
                        binary = archive.getinfo(f"{root}/{executable}")
                        self.assertEqual(binary.compress_type, zipfile.ZIP_DEFLATED)
                        self.assertEqual(stat.S_IMODE(binary.external_attr >> 16), 0o755)
                        self.assertEqual(stat.S_IMODE(archive.getinfo(f"{root}/about.txt").external_attr >> 16), 0o644)
                        archive.extractall(extracted)
                else:
                    with tarfile.open(path) as archive:
                        self.assertEqual(archive.getnames(), expected_names)
                        self.assertEqual(archive.getmember(f"{root}/{executable}").mode, 0o755)
                        self.assertEqual(archive.getmember(f"{root}/about.txt").mode, 0o644)
                        for member in archive.getmembers():
                            self.assertEqual((member.uid, member.gid, member.uname, member.gname), (0, 0, "", ""))
                        archive.extractall(extracted, filter="data")
                self.assertEqual(
                    {
                        item.relative_to(extracted / root).as_posix(): item.read_bytes()
                        for item in (extracted / root).rglob("*")
                        if item.is_file()
                    },
                    files,
                )

    def test_pack_rejects_invalid_sources_and_names(self):
        bundle = self.work / "bundle"
        for archive_format in ("zip", "tar.gz"):
            with self.subTest(archive_format=archive_format):
                output = self.work / f"release.{archive_format}"
                command = ("pack", "--source", bundle, "--format", archive_format)
                self.assert_cli_error("bundle", *command, "--output", output)
                bundle.mkdir()
                self.assert_cli_error("empty", *command, "--output", output)
                self.assert_cli_error("unsafe", *command, "--output", output, "--root-name", "../escape")
                self.assert_cli_error("unsafe", *command, "--output", output, "--root-name", "NUL")
                self.assert_cli_error("unsafe", *command, "--output", output, "--root-name", "con.txt")
                self.assert_cli_error("outside", *command, "--output", bundle / output.name)
                bundle.rmdir()
                self.assertFalse(output.exists())

    def test_pack_rejects_links(self):
        bundle = self.work / "bundle"
        bundle.mkdir()
        outside = self.work / "outside"
        outside.write_bytes(b"outside")
        try:
            (bundle / "escape").symlink_to(outside)
        except OSError as error:
            self.skipTest(f"symlink creation unavailable: {error}")
        output = self.work / "release.zip"
        self.assert_cli_error(
            "links and reparse points", "pack", "--source", bundle, "--output", output, "--format", "zip"
        )
        self.assertFalse(output.exists())

    def test_checksums(self):
        first = self.work / "a.zip"
        second = self.work / "z.tar.gz"
        first.write_bytes(b"abc")
        second.write_bytes(b"")
        manifest = self.work / "SHA256SUMS.txt"
        self.run_cli("checksums", "--output", manifest, second, first)
        self.assertEqual(
            manifest.read_bytes(),
            b"ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad  a.zip\n"
            b"e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855  z.tar.gz\n",
        )
        verify = ("verify-checksums", "--checksums", manifest, "--dir", self.work)
        self.run_cli(*verify)
        duplicate = self.work / "other" / first.name
        duplicate.parent.mkdir()
        duplicate.write_bytes(b"different")
        self.assert_cli_error("duplicate", "checksums", "--output", self.work / "duplicate.txt", first, duplicate)
        first.write_bytes(b"tampered")
        self.assert_cli_error("SHA-256 mismatch", *verify)
        first.unlink()
        self.assert_cli_error("expected a regular file", *verify)

    def test_verify_rejects_malformed_manifests(self):
        manifest = self.work / "SHA256SUMS.txt"
        for text, error in (
            ("", "empty checksum manifest"),
            ("bad-digest  file.txt\n", "malformed checksum line"),
            (f"{'0' * 64} file.txt\n", "malformed checksum line"),
            (f"{'0' * 64}  ../outside\n", "unsafe release filename"),
        ):
            with self.subTest(text=text):
                manifest.write_bytes(text.encode("utf-8"))
                self.assert_cli_error(error, "verify-checksums", "--checksums", manifest, "--dir", self.work)

    def test_release_workflow_yaml(self):
        workflow = ROOT / ".github/workflows/release.yml"
        try:
            import yaml
        except ModuleNotFoundError:
            subprocess.run(["pixi", "run", "--locked", "prettier", "--check", str(workflow)], cwd=ROOT, check=True)
        else:
            spec = yaml.safe_load(workflow.read_text(encoding="utf-8"))
            triggers = spec.get("on", spec.get(True))  # PyYAML uses YAML 1.1 booleans.
            self.assertEqual(triggers, {"push": {"tags": ["v*"]}, "workflow_dispatch": None})
            self.assertEqual(set(spec["jobs"]), {"validate", "package", "checksums", "publish"})
            self.assertEqual(spec["permissions"], {"contents": "read"})
            for name, job in spec["jobs"].items():
                if name == "publish":
                    self.assertEqual(job.get("permissions"), {"contents": "write"})
                    self.assertIn("is_release == 'true'", str(job.get("if", "")))
                else:
                    self.assertNotIn("permissions", job)


if __name__ == "__main__":
    unittest.main()
