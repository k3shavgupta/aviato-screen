#!/usr/bin/env python3
"""Publish bounded, content-free T3/Grok (and Codex) lifecycle status for Aviato's display."""

import argparse
import fcntl
import hashlib
import json
import os
from pathlib import Path
import signal
import stat
import sys
import tempfile
import time


MAX_INPUT_BYTES = 1024 * 1024
MAX_SESSIONS = 32
STALE_SECONDS = 900
RETENTION_SECONDS = 86400
EVENTS = {
    "SessionStart", "UserPromptSubmit", "PreModelRequest", "PostModelRequest",
    "PermissionRequest", "PostToolUse", "PreToolUse", "Stop", "Interrupt", "SessionEnd",
}
EVENT_ALIASES = {
    "SessionStart": "SessionStart", "session_start": "SessionStart", "sessionStart": "SessionStart",
    "UserPromptSubmit": "UserPromptSubmit", "user_prompt_submit": "UserPromptSubmit",
    "userPromptSubmit": "UserPromptSubmit",
    "PreToolUse": "PreToolUse", "pre_tool_use": "PreToolUse", "preToolUse": "PreToolUse",
    "PostToolUse": "PostToolUse", "post_tool_use": "PostToolUse", "postToolUse": "PostToolUse",
    "PostToolUseFailure": "PostToolUse", "post_tool_use_failure": "PostToolUse",
    "postToolUseFailure": "PostToolUse",
    "PermissionRequest": "PermissionRequest",
    "Notification": "Notification", "notification": "Notification",
    "Stop": "Stop", "stop": "Stop",
    "StopFailure": "StopFailure", "stop_failure": "StopFailure", "stopFailure": "StopFailure",
    "StopCancelled": "Interrupt", "stop_cancelled": "Interrupt", "stopCancelled": "Interrupt",
    "Interrupt": "Interrupt",
    "SessionEnd": "SessionEnd", "session_end": "SessionEnd", "sessionEnd": "SessionEnd",
    "PreModelRequest": "PreModelRequest", "PostModelRequest": "PostModelRequest",
}


def first_text(payload, *keys):
    for key in keys:
        value = payload.get(key)
        if isinstance(value, str) and value:
            return value
    return None


def normalize_payload(payload):
    """Map T3/Grok camelCase and snake_case envelopes onto the Codex event names."""
    if not isinstance(payload, dict):
        return None
    raw = first_text(payload, "hook_event_name", "hookEventName") or os.environ.get("GROK_HOOK_EVENT")
    event = EVENT_ALIASES.get(raw) if raw else None
    if event is None:
        return None
    if (payload.get("agent_id") or first_text(payload, "subagentType", "subagent_type") or
            event in {"SubagentStart", "SubagentStop"}):
        return None
    session_id = first_text(payload, "session_id", "sessionId")
    if not session_id and os.environ.get("GROK_HOOK_EVENT"):
        session_id = os.environ.get("GROK_SESSION_ID")
    if not isinstance(session_id, str) or not session_id or len(session_id) > 256:
        return None
    if event == "Notification":
        kind = first_text(payload, "notification_type", "notificationType")
        if kind != "permission_prompt":
            return None
        event = "PermissionRequest"
    status = payload.get("status")
    if event == "StopFailure":
        event = "PostModelRequest"
        status = "failed"
    turn_id = first_text(payload, "turn_id", "promptId", "prompt_id") or ""
    normalized = {"hook_event_name": event, "session_id": session_id, "turn_id": turn_id}
    if event == "PostModelRequest":
        normalized["status"] = status
    return normalized


def private_directory(path):
    path.mkdir(mode=0o700, parents=True, exist_ok=True)
    info = path.lstat()
    if not stat.S_ISDIR(info.st_mode) or info.st_uid != os.getuid():
        raise ValueError("Unsafe status directory")
    path.chmod(0o700)


def atomic_json(path, data):
    descriptor, temporary = tempfile.mkstemp(prefix=".status-", dir=path.parent)
    try:
        with os.fdopen(descriptor, "w") as stream:
            json.dump(data, stream, separators=(",", ":"), sort_keys=True)
            stream.write("\n")
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, path)
    finally:
        if os.path.exists(temporary):
            os.unlink(temporary)


def notify_display():
    """Wake only Aviato's verified native renderer after a status change."""
    runtime = Path(f"/run/user/{os.getuid()}/aviato-screen")
    try:
        directory_info = runtime.lstat()
        if (not stat.S_ISDIR(directory_info.st_mode) or directory_info.st_uid != os.getuid() or
                directory_info.st_mode & 0o077):
            return
        descriptor = os.open(runtime / "native.pid", os.O_RDONLY | os.O_CLOEXEC | os.O_NOFOLLOW)
        with os.fdopen(descriptor, "r") as stream:
            info = os.fstat(stream.fileno())
            raw_pid = stream.read(32).strip()
        if (not stat.S_ISREG(info.st_mode) or info.st_uid != os.getuid() or
                info.st_mode & 0o077 or info.st_nlink != 1 or info.st_size > 32 or
                not raw_pid.isdigit()):
            return
        pid = int(raw_pid)
        if pid <= 1:
            return
        expected = Path(__file__).resolve().with_name("screen")
        if os.readlink(f"/proc/{pid}/exe") != str(expected):
            return
        os.kill(pid, signal.SIGUSR2)
    except (OSError, ValueError):
        return


def fingerprint(value):
    return hashlib.sha256(value.encode()).hexdigest()[:24]


def read_sessions(path):
    try:
        descriptor = os.open(path, os.O_RDONLY | os.O_NOFOLLOW)
        with os.fdopen(descriptor, "r") as stream:
            raw = stream.read(32769)
        if len(raw) > 32768:
            return {}
        data = json.loads(raw)
        if not isinstance(data, dict):
            return {}
        result = {}
        for key, record in data.items():
            if (isinstance(key, str) and len(key) == 24 and
                    isinstance(record, dict) and
                    record.get("state") in {"running", "approval_requested", "stopped", "error", "idle"} and
                    isinstance(record.get("updated_at"), int) and
                    isinstance(record.get("turn"), str) and len(record["turn"]) <= 24):
                result[key] = {name: record[name] for name in ("state", "updated_at", "turn")}
        return result
    except (OSError, ValueError):
        return {}


def update_record(previous, payload, now):
    event = payload["hook_event_name"]
    turn_id = payload.get("turn_id")
    turn = fingerprint(turn_id) if isinstance(turn_id, str) and turn_id else ""
    # A late event from an older turn must not stop a newly submitted turn.
    if (previous and turn and previous["turn"] and turn != previous["turn"] and
            event != "UserPromptSubmit"):
        return previous
    state = previous["state"] if previous else "idle"
    if event == "SessionStart":
        if previous:
            return previous
    elif event in {"UserPromptSubmit", "PreModelRequest"}:
        state = "running"
    elif event == "PermissionRequest":
        state = "approval_requested"
    elif event == "PostModelRequest":
        status = payload.get("status")
        if status not in {"completed", "failed", "cancelled"}:
            return previous
        state = {"completed": "running", "failed": "error", "cancelled": "stopped"}[status]
    elif event in {"PostToolUse", "PreToolUse"}:
        # Other tools can finish while an approval is pending. Wait for the
        # next model request or terminal event before clearing that indication.
        if state != "approval_requested":
            state = "running"
    elif event in {"Stop", "Interrupt", "SessionEnd"}:
        if state != "error":
            state = "stopped"
    return {"state": state, "updated_at": now, "turn": turn or (previous["turn"] if previous else "")}


def publish(payload, directory, now=None):
    payload = normalize_payload(payload)
    if payload is None or payload.get("hook_event_name") not in EVENTS:
        return
    session_id = payload.get("session_id")
    if not isinstance(session_id, str) or not session_id or len(session_id) > 256:
        return
    now = int(time.time()) if now is None else now
    private_directory(directory)
    lock_descriptor = os.open(directory / ".agents.lock", os.O_CREAT | os.O_RDWR | os.O_NOFOLLOW, 0o600)
    with os.fdopen(lock_descriptor, "w") as lock:
        deadline = time.monotonic() + 0.5
        while True:
            try:
                fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
                break
            except BlockingIOError:
                if time.monotonic() >= deadline:
                    return
                time.sleep(0.01)
        sessions = read_sessions(directory / ".agent-sessions.json")
        sessions = {key: record for key, record in sessions.items()
                    if 0 <= now - record["updated_at"] <= RETENTION_SECONDS}
        key = fingerprint(session_id)
        record = update_record(sessions.get(key), payload, now)
        if record is None:
            return
        sessions[key] = record
        sessions = dict(sorted(sessions.items(),
                               key=lambda item: (item[1]["updated_at"], item[0] == key),
                               reverse=True)[:MAX_SESSIONS])
        fresh = [record for record in sessions.values() if now - record["updated_at"] <= STALE_SECONDS]
        counts = {"running": 0, "waiting": 0, "stopped": 0, "errors": 0}
        count_keys = {"running": "running", "approval_requested": "waiting", "stopped": "stopped", "error": "errors"}
        for record in fresh:
            if record["state"] in count_keys:
                counts[count_keys[record["state"]]] += 1
        states = {record["state"] for record in fresh}
        state = next((candidate for candidate in ("approval_requested", "error", "running", "stopped", "idle")
                      if candidate in states), "stale")
        updated_at = max(record["updated_at"] for record in sessions.values())
        expires_at = min((record["updated_at"] + STALE_SECONDS for record in fresh),
                         default=updated_at + STALE_SECONDS)
        status = {"version": 1, "updated_at": updated_at, "expires_at": expires_at,
                  "stale_after_seconds": STALE_SECONDS, "state": state, **counts}
        atomic_json(directory / ".agent-sessions.json", sessions)
        atomic_json(directory / "agents.json", status)
        # Synthetic tests use temporary directories; only the live private
        # cache is allowed to wake the physical display process.
        if directory.resolve() == (Path.home() / ".cache/aviato-screen").resolve():
            notify_display()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--state-dir", type=Path, default=Path.home() / ".cache/aviato-screen")
    args = parser.parse_args()
    try:
        raw = sys.stdin.buffer.read(MAX_INPUT_BYTES + 1)
        if len(raw) <= MAX_INPUT_BYTES:
            publish(json.loads(raw), args.state_dir)
    except (OSError, ValueError, TypeError):
        # Hook failures must not steer the agent or reveal any event contents.
        pass
    print("{}")


if __name__ == "__main__":
    main()
