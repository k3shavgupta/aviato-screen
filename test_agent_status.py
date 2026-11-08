"""Synthetic lifecycle checks. Every write uses a temporary test directory."""

import importlib.util
import json
from pathlib import Path
import stat
import subprocess
import tempfile
import unittest

SCRIPT = Path(__file__).with_name("agent-status.py")
SPEC = importlib.util.spec_from_file_location("agent_status", SCRIPT)
adapter = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(adapter)


class StatusTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="aviato-agent-synthetic-")
        self.directory = Path(self.temporary.name) / "status"

    def tearDown(self):
        self.temporary.cleanup()

    def event(self, event, session="synthetic-a", turn="synthetic-turn-a", now=1000, **extra):
        adapter.publish({"hook_event_name": event, "session_id": session,
                         "turn_id": turn, **extra}, self.directory, now)

    def status(self):
        return json.loads((self.directory / "agents.json").read_text())

    def test_overlap_approval_error_and_stop(self):
        self.event("UserPromptSubmit")
        self.event("UserPromptSubmit", session="synthetic-b")
        self.event("PermissionRequest")
        self.event("PostToolUse")
        self.assertEqual(self.status()["waiting"], 1)
        self.assertEqual(self.status()["running"], 1)
        self.event("PreModelRequest")
        self.assertEqual(self.status()["waiting"], 0)
        self.event("PostModelRequest", status="failed", error="SECRET TEST BODY")
        self.event("Stop")
        self.assertEqual(self.status()["errors"], 1)
        self.event("PreModelRequest")
        self.event("Interrupt")
        self.assertEqual(self.status()["stopped"], 1)
        self.assertEqual(self.status()["running"], 1)

    def test_old_turn_does_not_stop_new_turn(self):
        self.event("UserPromptSubmit")
        self.event("UserPromptSubmit", turn="synthetic-turn-b", now=1001)
        self.event("Stop", now=1002)
        self.assertEqual(self.status()["running"], 1)
        self.assertEqual(self.status()["updated_at"], 1001)
        self.event("PreModelRequest", now=1003)
        self.event("Stop", turn="synthetic-turn-b", now=1004)
        self.assertEqual(self.status()["stopped"], 1)

    def test_stale_sessions_not_refreshed_by_other_sessions(self):
        self.event("UserPromptSubmit")
        self.event("UserPromptSubmit", session="synthetic-b", now=2000)
        self.assertEqual(self.status()["running"], 1)
        self.event("SessionStart", session="synthetic-b", now=3000)
        self.assertEqual(self.status()["state"], "stale")
        self.assertEqual(self.status()["running"], 0)

    def test_snapshot_expires_at_earliest_contributing_session(self):
        self.event("PermissionRequest")
        self.event("UserPromptSubmit", session="synthetic-b", now=1895)
        self.assertEqual(self.status()["waiting"], 1)
        self.assertEqual(self.status()["updated_at"], 1895)
        self.assertEqual(self.status()["expires_at"], 1900)
        self.event("PreModelRequest", session="synthetic-b", now=1901)
        self.assertEqual(self.status()["waiting"], 0)
        self.assertEqual(self.status()["expires_at"], 2801)

    def test_bounded_private_content_free_cache(self):
        for number in range(40):
            self.event("UserPromptSubmit", session=f"synthetic-{number}", now=1000 + number,
                       prompt="SECRET TEST BODY", cwd="/private/secret", transcript_path="secret-path")
        public = self.directory / "agents.json"
        sessions = self.directory / ".agent-sessions.json"
        self.assertLess(public.stat().st_size, 4096)
        self.assertEqual(len(json.loads(sessions.read_text())), 32)
        for path in (public, sessions):
            self.assertEqual(stat.S_IMODE(path.stat().st_mode), 0o600)
            self.assertNotIn("secret", path.read_text().lower())
            self.assertNotIn("synthetic", path.read_text())
        self.assertEqual(stat.S_IMODE(self.directory.stat().st_mode), 0o700)

    def test_invalid_input_and_subagent_do_not_mutate(self):
        for event in ({}, [], {"hook_event_name": "Stop"}):
            adapter.publish(event, self.directory, 1000)
        self.event("PreModelRequest", agent_id="synthetic-child")
        self.assertFalse(self.directory.exists())
        result = subprocess.run(["python3", str(SCRIPT), "--state-dir", str(self.directory)],
                                input=b"malformed", capture_output=True, check=True)
        self.assertEqual(result.stdout, b"{}\n")
        self.assertEqual(result.stderr, b"")

    def test_concurrent_sessions_are_retained(self):
        processes = []
        for number in range(10):
            process = subprocess.Popen(["python3", str(SCRIPT), "--state-dir", str(self.directory)],
                                       stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            process.stdin.write(json.dumps({"hook_event_name": "UserPromptSubmit",
                                           "session_id": f"synthetic-parallel-{number}",
                                           "turn_id": "synthetic-turn"}).encode())
            process.stdin.close()
            processes.append(process)
        for process in processes:
            self.assertEqual(process.wait(timeout=5), 0)
            self.assertEqual(process.stdout.read(), b"{}\n")
            self.assertEqual(process.stderr.read(), b"")
            process.stdout.close()
            process.stderr.close()
        self.assertEqual(self.status()["running"], 10)

    def test_t3_grok_envelope_maps_to_running_waiting_and_error(self):
        adapter.publish({"hookEventName": "user_prompt_submit", "sessionId": "t3-a",
                         "promptId": "turn-1"}, self.directory, 1000)
        self.assertEqual(self.status()["state"], "running")
        adapter.publish({"hook_event_name": "PreToolUse", "sessionId": "t3-a",
                         "promptId": "turn-1"}, self.directory, 1001)
        self.assertEqual(self.status()["running"], 1)
        adapter.publish({"hookEventName": "notification", "sessionId": "t3-a",
                         "notification_type": "permission_prompt"}, self.directory, 1002)
        self.assertEqual(self.status()["state"], "approval_requested")
        adapter.publish({"hookEventName": "notification", "sessionId": "t3-a",
                         "notification_type": "idle_prompt"}, self.directory, 1003)
        self.assertEqual(self.status()["waiting"], 1)
        adapter.publish({"hookEventName": "stop_failure", "sessionId": "t3-b"},
                        self.directory, 1004)
        self.assertEqual(self.status()["errors"], 1)
        adapter.publish({"hookEventName": "session_end", "sessionId": "t3-b",
                         "subagentType": "explore"}, self.directory, 1005)
        self.assertEqual(self.status()["errors"], 1)
        adapter.publish({"hookEventName": "stop", "sessionId": "t3-a"}, self.directory, 1006)
        self.assertEqual(self.status()["stopped"], 1)
        self.assertEqual(self.status()["waiting"], 0)


if __name__ == "__main__":
    unittest.main()
