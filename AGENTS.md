# engrave-mcp

Native C++ MCP host for the engrave engine, a pinned, trimmed fork of MuseScore
core. This is a separate project from the separate llm-music project, which holds baseline
experiments.

Goal: give LLM agents a tool to compose music, with enough MuseScore interop that
agent-made scores can be edited by humans in MuseScore and vice versa. Full MuseScore
compatibility and its legacy are not a goal: only scores from MuseScore 4.6 on open,
and nothing is kept or validated just because MuseScore has it. The engine operates
as a hard fork (cherry-picks only, no bulk upstream merges).

- The engine is the fork github.com/Locietta/engrave, consumed as the shallow submodule
  `vendor/engrave` (governance rules in `docs/dev/fork.md`; never force-push fork branches;
  pin commits on fork `main` by SHA; push fork `main` first and record the commit range
  and validation evidence in the host bump commit message, no pin tags).
  Native compilation is the target; do not introduce Node.js or WASM as runtime dependencies.
- No computer use, screenshots, simulated input, or dependence on a desktop MuseScore install.
- The build is Bazel + hermetic `xclang` (no Qt): the engine build lives in the fork
  (`MODULE.bazel`, `BUILD.bazel`, and pinned package archives under `bazel/`), the host
  build in this repository's `MODULE.bazel` and `BUILD.bazel`, which consume `@engrave`
  from `vendor/engrave`; validate Windows and Linux.
  Work natively on Windows (PowerShell, `.pixi/envs/default/python.exe`). Linux means the
  physical lab host `ssh user@<linux-host>` (checkout `~/engrave/engrave-mcp`, environment
  `source ~/engrave/env.sh`); never use WSL for this project.
  Both repositories are on GitHub (host: `Locietta/engrave-mcp`) with bare mirrors under
  `lab` on `user@<linux-host>`; move commits between machines by pushing and fetching branches,
  never git bundles or copied worktrees (`docs/dev/branching.md`).
  Current design and rationale: `docs/dev/design.md`; open issues: `docs/dev/defects.md`;
  branch and tag policy: `docs/dev/branching.md`; code style guide: `docs/dev/code-style-guide.md`.
  Keep only `main` as a long-lived branch: merge topic branches and delete them; tag
  formal releases instead of keeping phase branches.
- Avoid rewriting music algorithms; preserve an executable baseline for each refactor.
  Check engine changes against the engine gate (`vendor/engrave/tests/gate.py`, wrapped by
  `tests/engine_gate.py`): `--quick` per step, the full gate at milestones and engine bumps.
  The gate compares goldens in `vendor/engrave/tests/data/golden` with the explicit,
  hash-pinned allow-list (`vendor/engrave/tests/data/golden/allow.json`).
- Build with `scripts/bbuild.py --profile <release|debug|releasedbg|asan>` into `build/<profile>`;
  do not bypass `scripts/bbuild.py`'s shared Bazel caches (`build/.bazel_repo_cache` and `build/.bazel_disk_cache`).
- Pin upstream revisions. Keep downloads in ignored `.upstream/` and runtime assets in `.runtime/`.
- Working notes and worker transcripts belong in ignored `notes/`; gate and lane `--output`
  directories go under `notes/runs/<name>`; durable decisions in `docs/dev/`.
- Temporary worktrees and review clones go under the ignored `.worktrees/<name>` of the checkout
  (also on the Linux lab host), never at drive roots, in `$TEMP` or beside the repository. Do not junction or
  symlink into the main checkout: call the main checkout's `.pixi/envs/default/python.exe` by path.
  Remove them when done (`git worktree remove`, then delete the clone directory).
- Workers are not alone: edit only assigned files and never revert other workers' changes.
- Worker models: GPT-6.1 Sol, GPT-6.1 Terra, GPT-6 Astra, Opus 5.5, DeepSeek Flash and
  Gemini 3.8 Flash. Pick the cheapest model likely to finish a task in one pass, and escalate on a stall rather
  than retrying. Default implementation worker: GPT-6.1 Sol (stronger and more token-efficient than
  Terra for implementation). Default step reviewer and validation worker: GPT-6.1 Terra; keep
  Terra reviews independent of the Sol worker that wrote the code. Hard rules: Terra only at
  reasoning effort max, in fast mode (fast mode is for Terra only, never Astra); DeepSeek via Pi
  `opencode-go/deepseek-v4.1-flash` (the paid `deepseek/deepseek-flash` only while opencode-go
  is down).
- Reviews: each implementation step ends with a full Terra validation; each phase gets a
  final GPT-6 Astra review before it is accepted, looking for remaining defects and poor
  implementation approaches across the phase. GPT reviews over-flag: triage findings by
  real impact, and record out-of-scope findings as non-goals instead of chasing them.
- Do not print credentials or read credential files. Use existing CLI authentication.
- State which claims are source-reviewed, compiled, or actually executed; never conflate them.
- Tautological labels, tests and comments are harmful; do not add them, and remove them when
  touching code. Comments say why or state an invariant, never restate the code or a name.
  Tests check behaviour against an independent expectation (fixture, golden, hand-computed
  value), never the implementation against itself (setter-then-getter, expected values
  derived by the code under test, counts of what was just inserted). No section labels,
  headers or doc lines that only repeat identifiers.
- Commit each verified milestone as it is reached. Keep review fixes in small follow-up commits; do not defer all commits to the end.
