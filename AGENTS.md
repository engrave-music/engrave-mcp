# engrave-mcp

Native C++ MCP host for the `engrave` engine, a pinned hard fork of MuseScore
core (`github.com/engrave-music/engrave`, consumed as the submodule
`vendor/engrave`).

## Goal & Scope

- Give LLM agents a tool to compose music, with enough MuseScore interop that
  agent made scores can be edited by humans in MuseScore and vice versa.
- Full MuseScore compatibility and legacy formats are non-goals: only MuseScore
  4.6+ scores open, and nothing is kept or validated solely because MuseScore
  has it.
- Native compilation only: no Node.js, WASM, desktop MuseScore install,
  computer use, screenshots, or simulated input.

## Repository & Engine Fork

- Keep `main` as the only long-lived branch in both repositories; merge and
  delete topic branches, and reserve tags for formal releases (`docs/dev/fork.md`).
- The engine operates as a hard fork (cherry-picks only, no bulk upstream
  merges; never force-push fork branches). Pin commits on fork `main` by SHA
  with no pin tags; push fork `main` before pushing a host commit that bumps
  `vendor/engrave`, and record the commit range and validation evidence in the
  host bump commit message.
- Move commits between machines by pushing and fetching branches, never git
  bundles or copied worktrees.

## Build, Validation & Workspace

- Build system: Bazel + hermetic `xclang` (C++23). The engine build lives
  in `vendor/engrave` (`MODULE.bazel`, `BUILD.bazel`, `bazel/`); the host build
  consumes `@engrave`. Validate on both native Windows (PowerShell, `pixi run`)
  and native Linux — never use WSL.
- Build with `scripts/bbuild.py --profile <release|debug|releasedbg|sanitize>` into
  `build/<profile>`. Do not bypass `scripts/bbuild.py`'s shared Bazel caches
  (`build/.bazel_repo_cache` and `build/.bazel_disk_cache`).
- Avoid rewriting music algorithms; preserve an executable baseline for each
  refactor. Check engine changes against the engine gate
  (`vendor/engrave/tests/gate.py`, wrapped by `tests/engine_gate.py`): `--quick`
  per step, full gate at milestones and engine bumps. Goldens live in
  `vendor/engrave/tests/data/golden` with the hash-pinned allow-list in
  `vendor/engrave/tests/data/golden/allow.json`.
- Directory hygiene:
  - Pin upstream revisions; keep downloads in ignored `.upstream/` and runtime
    assets in `.runtime/`.
  - Working notes and transcripts belong in ignored `notes/`; gate and lane
    `--output` directories go under `notes/runs/<name>`; durable decisions go in
    `docs/dev/`.
  - Temporary worktrees and review clones go under ignored `.worktrees/<name>`
    inside the checkout — never at drive roots, in `$TEMP`, or beside the
    repository. Do not junction or symlink into the main checkout; call the main
    checkout's `.pixi/envs/default/python.exe` by path. Remove worktrees when
    done (`git worktree remove`, then delete the directory).

## Documentation Map

- Design and rationale: [docs/dev/design.md](docs/dev/design.md)
- Tool contract: [docs/dev/tools.md](docs/dev/tools.md)
- Build, formatting, and linting: [docs/dev/build.md](docs/dev/build.md)
- Validation and test lanes: [docs/dev/validation.md](docs/dev/validation.md)
- Engine fork and branching: [docs/dev/fork.md](docs/dev/fork.md)
- Filesystem scope and triage: [docs/dev/scope.md](docs/dev/scope.md)
- Open defects and limitations: [docs/dev/defects.md](docs/dev/defects.md)
- Code style guide: [docs/dev/code-style-guide.md](docs/dev/code-style-guide.md)

## Engineering Discipline

- **Multi-worker safety**: Edit only assigned files and never revert other
  workers' changes.
- **Credentials**: Do not print credentials or read credential files; use
  existing CLI authentication.
- **Evidence**: State which claims are source-reviewed, compiled, or actually
  executed; never conflate them.
- **No tautologies**: Tautological labels, tests, and comments are harmful — do
  not add them, and remove them when touching code. Comments explain _why_ or
  state an invariant, never restate code or names. Tests check behavior against
  an independent expectation (fixture, golden, hand-computed value), never the
  implementation against itself. No section banners or doc lines that only
  repeat identifiers.
- **Commits**: Commit each verified milestone as it is reached. Keep review
  fixes in small follow-up commits; do not defer all commits to the end.

## Local Overrides

If `AGENTS.local.md` (gitignored) exists in the repository root, read and follow
it for machine-specific workflows, lab hosts, mirrors, and local model/review
preferences. Local-only reference docs live in `docs/do_not_commit/`
(gitignored).
