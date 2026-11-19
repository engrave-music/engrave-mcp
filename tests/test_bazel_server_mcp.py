import os
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import MagicMock, patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
import bazel_server_mcp as mcp


class HostBazelServerMcpTests(unittest.TestCase):
    def test_host_defaults_include_host_and_engine(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "vendor/engrave").mkdir(parents=True)
            (root / "vendor/engrave/MODULE.bazel").write_text('module(name = "engrave")\n', encoding="utf-8")

            projects = mcp.load_projects(root, None, [])
            self.assertEqual(projects["host"], root)
            self.assertEqual(projects["engine"], (root / "vendor/engrave").resolve())

    def test_mcp_start_status_and_rewarm_no_stop_tool(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            host_dir = root / "host"
            eng_dir = root / "engine"
            host_dir.mkdir()
            eng_dir.mkdir()
            out_base = root / "outbase"
            (out_base / "server").mkdir(parents=True)
            pid = os.getpid()
            (out_base / "server/server.pid.txt").write_text(f"{pid}\n", encoding="utf-8")

            manager = mcp.BazelServerManager({"host": host_dir, "engine": eng_dir})
            self.assertEqual(manager.default_project, "host")

            listed = mcp.handle_request(manager, {"jsonrpc": "2.0", "id": 0, "method": "tools/list"})
            self.assertEqual(
                [t["name"] for t in listed["result"]["tools"]], ["bazel_server_start", "bazel_server_status"]
            )

            def fake_run(cmd, **kwargs):
                res = MagicMock()
                res.returncode = 0
                res.stderr = ""
                res.stdout = f"server_pid: {pid}\noutput_base: {out_base}\n"
                return res

            with (
                patch.object(mcp, "find_bazel", return_value="bazel"),
                patch.object(mcp.subprocess, "run", side_effect=fake_run) as run_mock,
            ):
                start_resp = mcp.handle_request(
                    manager,
                    {"jsonrpc": "2.0", "id": 1, "method": "tools/call", "params": {"name": "bazel_server_start"}},
                )
                self.assertFalse(start_resp["result"]["isError"])
                self.assertEqual(start_resp["result"]["structuredContent"]["project"], "host")
                self.assertEqual(start_resp["result"]["structuredContent"]["server_pid"], pid)

                self.assertIsNone(manager.ensure_warm("host"))
                self.assertEqual(run_mock.call_count, 1)

                (out_base / "server/server.pid.txt").unlink()
                self.assertIsNotNone(manager.ensure_warm("host"))
                self.assertEqual(run_mock.call_count, 2)

                bad = mcp.handle_request(
                    manager,
                    {
                        "jsonrpc": "2.0",
                        "id": 2,
                        "method": "tools/call",
                        "params": {"name": "bazel_server_start", "arguments": {"project": "missing"}},
                    },
                )
                self.assertTrue(bad["result"]["isError"])


if __name__ == "__main__":
    unittest.main()
