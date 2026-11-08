The Aviato display receives lifecycle events from T3 Code (Grok) running as
`keshav` on Aviato, and still accepts Codex events from the same adapter. The
adapter is `/home/keshav/aviato-screen/agent-status.py`. Grok user hooks in
`/home/keshav/.grok/hooks/aviato-screen.json` run it with `/usr/bin/python3` and
a two-second timeout. Codex user hooks in `/home/keshav/.codex/hooks.json` do
the same. The existing Codex SessionStart hook was preserved. The renderer
hides idle and stale snapshots, so the clock stays blank unless a turn is
running, waiting, finished, or failed.

The adapter writes `/home/keshav/.cache/aviato-screen/agents.json` atomically.
The directory has mode 0700 and the cache has mode 0600. It contains `version=1`,
`updated_at` and `expires_at` in Unix seconds, `stale_after_seconds=900`, a fixed `state`, and
integer counts named `running`, `waiting`, `stopped`, and `errors`. It stays
under 4096 bytes. The renderer chooses all visible wording itself.

After updating the live cache, the adapter reads the private
`/run/user/<uid>/aviato-screen/native.pid` record, verifies its owner and that
the process executable is Aviato's `screen`, then sends `SIGUSR2`. The renderer
re-reads the cache and redraws only the affected status region. `SIGUSR1` remains
the full-frame wake used after monitor power transitions. Synthetic events in
temporary directories do not signal the live display.

These hooks expose turn and tool lifecycle states. They do not expose typing
keystrokes or microphone/speech activity. The screen does not attempt to infer
either state from prompts or other private content.

A separate private `.agent-sessions.json` holds at most 32 session fingerprints,
turn fingerprints, timestamps, and fixed states. The adapter never saves prompt
text, replies, file paths, tool arguments, error messages, or raw session IDs.
File locking prevents concurrent sessions from losing one another's updates.
Each session expires individually after 900 seconds without an event, even if
other sessions continue producing events. Records older than one day are pruned
on the next update. The renderer must also mark the entire cache stale when its
`updated_at` exceeds `stale_after_seconds` or is in the future. It must also mark
the cache stale once `expires_at` passes. That field is the earliest expiration
of any session contributing to the current aggregate. An unrelated session's
update therefore cannot prolong an older approval indication. `updated_at`
is the latest accepted event time, so rejected late events do not refresh it.

| Event | Recorded meaning |
| --- | --- |
| SessionStart | Idle for a newly observed session |
| UserPromptSubmit, PreModelRequest, PreToolUse | Running |
| PermissionRequest, Grok `permission_prompt` | Approval requested |
| PostToolUse, PostToolUseFailure | Running, while retaining a previous approval request until a model request or terminal event |
| PostModelRequest completed | Running, because a completed model request can lead to more work |
| PostModelRequest failed, Grok StopFailure | Model request error |
| PostModelRequest cancelled | Stopped |
| Stop, Interrupt, StopCancelled, SessionEnd | Activity ended, preserving a preceding model request error |

Approval requests may be resolved by another hook or automatic review. That
state means the request occurred. A failed model attempt may subsequently be
retried. The next model request clears the error. Activity ending does not
establish that the requested task succeeded. After 15 minutes without new
events, the state becomes stale. The integration covers top-level T3/Grok and
Codex sessions on Aviato. Hooks carrying a Codex `agent_id` or a Grok
`subagentType` are ignored.

The nine new hooks were reviewed and trusted through the ordinary Codex CLI
`/hooks` interface on 2026-11-05. No trust hashes were edited directly. A fresh
empty CLI session was opened and archived through `codex archive`, which fired
a real SessionEnd event and produced the live cache. This verification used no
model request. Merely opening the empty session did not produce a SessionStart
cache during the check. Full running, approval, interruption, and model-error
transitions were tested with synthetic events in isolated temporary directories.
The currently installed hook definitions are active for new sessions. Hook
reload behavior inside sessions that predate this installation remains
unverified. Start a fresh session to exercise the installed configuration.

Run the seven synthetic regression tests with
`python3 /home/keshav/aviato-screen/test_agent_status.py`. They cover overlapping
sessions, parallel writes, approval/error recovery, late old-turn events,
per-session staleness, input rejection, privacy, permissions, and bounded size.
They leave the live cache untouched. Config validation also passed using
`codex app-server --strict-config --print-startup-config`; keep that command's
output private because it describes the local configuration.

The original hook file was backed up at
`/home/keshav/.codex/hooks.json.aviato-backup-20261105T100214Z`.
For a reversible pause, open `/hooks` in Codex and disable the handlers whose
command is `/usr/bin/python3 /home/keshav/aviato-screen/agent-status.py`.
Other handlers should retain their current settings. For removal, delete only
those same handlers from `hooks.json`, retaining all other groups. Restoring the
whole backup is appropriate only after confirming no subsequent hook edits
would be lost. With the adapter paused, the renderer will mark the previous
cache stale after 15 minutes. The adapter script and private cache may then be
removed if the integration is being retired.

The supported event contracts and trust interface are documented in the
[Codex hook reference](https://learn.chatgpt.com/docs/hooks).
