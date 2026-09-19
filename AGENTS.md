# SAMapExtender: guidance for coding agents

Shared, committed, tool-agnostic. Personal settings go in `CLAUDE.local.md`
(gitignored), which `CLAUDE.md` imports after this file.

## What this is

A GTA:SA `.asi` plugin that enables the extension of the base game map,
peds, vehicles, weapons and missions.

## How to work here

- Build with Docker only: `.\build.ps1` (or `docker build --target export
  -o dist .`). Nothing is installed on the host. The build runs the tests
  first.
- The version lives in `VERSION` and nowhere else. Bump it for any change
  to the plugin; docs-only changes leave it alone.
- Never add attribution trailers, tool advertisements, or generated-by
  notes to commits, pull requests, or code.
- Always work in branches/worktrees. Never commit straight to main.
