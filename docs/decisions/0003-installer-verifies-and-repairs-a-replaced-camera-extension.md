# Decision: The installer verifies a replaced camera extension and repairs a missing launchd job

Status: accepted
Date: 2026-11-25

## Context
Replacing a running camera extension can leave it with no launchd job: activated in `systemextensionsctl`, but never launched again until reboot. The unified log (2026-11-25, build 219 to 220) shows why. registerassistantservice starts removing the old job, which finishes asynchronously while the old process lives, and submits the new job 4 ms later. launchd refuses it ("Submit job failed ... error = 37: Operation already in progress"), registerassistantservice treats that as "already running", and the old job is then removed. Other projects report the same (OBS forum, Apple developer forums threads 756234 and 702066).

The installer already asks the extension to quit first (5f270d8), so the old process is gone when the job is removed. But registerassistantservice respawns a quit extension within 6 to 8 ms, so this only sometimes wins. The old verify step accepted any process of the new build that appeared, even briefly.

`launchctl print user/262/...` would show the job directly but needs root.

## Decision
- Keep quitting first, as best effort; poll for its exit every 5 ms so a timely activation can land in the gap.
- Verify by requiring a process of the bundled build to appear within 10 s and still be the same process 3 s later.
- If verify fails, repair once: submit an activation request for the same build and answer `.replace`, then verify again. With no job left there is nothing to race, so the submit succeeds. Only after a second failure ask for a restart.
- A same-build `.replace` is allowed only during repair; a normal install of an already active build still does nothing (7cea0df).

## Consequences
- A lost race heals without a reboot, at the cost of one more "[terminated waiting to uninstall on reboot]" entry until the next reboot.
- Verify takes at least 3 s longer.
- If Apple fixes registerassistantservice, repair never runs.
- Untested: whether a same-build replace asks for user approval again; if it does, the existing approval state handles it.

## Supersedes
None
