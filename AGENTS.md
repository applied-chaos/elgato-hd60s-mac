# elgato-hd60s-mac

Userspace driver for the Elgato HD60 S on Apple silicon: C core (`src/`), camera extension, Core Audio plug-in and installer app (`mac/`). See `README.md` and `docs/PROTOCOL.md`.

## Commands

- Build CLI: `make`
- Tests: `make test`
- App and extension: `make app` (signed, needs Xcode team) or `make app XCODEFLAGS=CODE_SIGNING_ALLOWED=NO`
- Install locally: `make install-app`, `make install-audio`
- Dev-only AudioDriverKit prototype: `make dext-dev` (Apple Development signed, not in release)
- Release: push a `vX.Y.Z` tag on `main`; `.github/workflows/release.yml` signs, notarises and publishes

<!-- shared-rules:start (source: mintopia/home AGENTS.md; edit there, then copy) -->
## Shared rules

### Talking to me
- Be extremely concise; sacrifice grammar for concision.
- British English in prose, docs, UI copy and commits; identifiers follow the language's convention. No em dashes.
- Plain industry-standard terms.

### Working
- Finish the task. Own problems found on the way, including pre-existing failures.
- Fix root causes.
- Prove it works against the real artifact (run it, read the value, inspect the diff) before saying done.
- Match UI to its mockup exactly; raise a wrong or missing mockup on the ticket.
- Type-system discipline: illegal states unrepresentable, parse at boundaries, exhaust variants, derive types from the authoritative schema.
- Model the domain in data structures, not scattered conditionals.
- Make operations idempotent across partial runs, crashes and retries.

### Quality gate
- Red/green TDD for new behaviour and bug fixes.
- Before commit: formatter, linter, static analysis and tests pass at the repo's level. The repo's AGENTS.md names commands and any coverage bar.
- Comments: non-obvious why only, one or two lines. Run /comment-check before finishing.

### Subagents
- Delegate to subagents; run parallel teams that coordinate by messaging.
- Set model and subagent type explicitly:
  - Planning, reasoning, deep debugging: Opus / Sol
  - Explore, coding, review: Sonnet / Terra
  - Trivial edits, docs: Haiku / Luna
- Long results go to a scratchpad file; return one line plus the path.

### Git
- Work on the branch you started on; parallel work goes in a worktree off it.
- Rebase only onto your own base branch.
- Delete merged or abandoned branches and worktrees, locally and on the remote.

### Decisions


Before proposing architecture, tooling, style, testing, repo structure, or workflow changes in any project:

1. Check for a `docs/decisions/` directory in the project root.
2. If it exists, search it for relevant ADRs (Architecture Decision Records).
3. Treat accepted ADRs as binding unless explicitly overridden by the user.
4. If a decision is outdated, propose a new ADR that supersedes it rather than silently ignoring.
5. Do not re-litigate accepted decisions unless new constraints are introduced.

When creating new ADRs, use this format:

```markdown
# Decision: [Title]

Status: [proposed|accepted|deprecated|superseded]
Date: [YYYY-MM-DD]

## Context
[Why this decision is needed]

## Decision
[What was decided]

## Consequences
[Trade-offs and implications]

## Supersedes
[Reference to previous ADR, or "None"]
```

### Code exploration

Always use jCodeMunch-MCP for code navigation. Never fall back to Read, Grep, Glob, or Bash for code exploration.
**Exception:** use `Read` when you are about to edit a file — the harness requires a `Read` before `Edit`/`Write`. Use jCodeMunch to *find and understand* code, then `Read` only the file you are changing.

This server runs the **front door** surface: three tools reach every jCodeMunch capability, so the tool list stays small and the catalogue is fetched only when you need it.

**Start any session:**
1. `order { "action": "resolve_repo", "args": { "path": "." } }` — confirm the project is indexed. If it is not: `order { "action": "index_folder", "args": { "path": "." } }`

**Then, for any task:**
- Know what you want → `order { "action": "<name>", "args": { ... } }`
- Know the goal, not the tool → `route { "query": "your task in a sentence" }` picks the action and shapes the arguments
- Want to see what exists → `menu { "query": "what you are trying to do" }` returns matching actions with example arguments
- Want the whole catalogue and the usage rules → `jcodemunch_guide`

`menu` and `jcodemunch_guide` list every action this server can run, including ones absent from your tool list. That is expected: the front door is the way to call them.

**Interpreting results:**
- A `verdict` of `no_implementation_found` is evidence of absence. Report the gap; do not re-search with different wording.
- A `verdict` of `degraded` means a channel was unavailable, so absence is NOT proven. Read the note before relying on the result.
- `source: ""` alongside `source_status` means the body could not be read, not that the symbol is empty.

**After editing files:**
- With PostToolUse hooks installed (Claude Code), edited files are reindexed automatically.
- Otherwise `order { "action": "register_edit", "args": { "paths": [...] } }` after an edit, batched for bulk changes.

**Announce your model once per session** so the server can size its answers: `announce_model { "model": "<your-model-id>" }`.

<!-- shared-rules:end -->

## Agent skills

### Issue tracker

GitHub issues on `applied-chaos/elgato-hd60s-mac` via the `gh` CLI. See `docs/agents/issue-tracker.md`.

### Triage labels

Default five canonical roles, label string = role name. See `docs/agents/triage-labels.md`.

### Domain docs

Single-context: `GLOSSARY.md` + `docs/decisions/` at the repo root. See `docs/agents/domain.md`.
