# musescore-mcp

Native C++ MCP host backed by a pinned, trimmed MuseScore engine. This is a
separate project from `C:/path/to/llm-music`, which holds baseline experiments.

- The engine is the fork github.com/Locietta/engrave (renamed from Locietta/MuseScore on 2026-10-28), consumed as the shallow submodule
  `vendor/musescore` (ledger and rules in `docs/dev/fork.md`; never force-push fork branches,
  tag every pinned SHA). webmscore and `patches/` are retired.
  Native compilation is the target; do not introduce Node.js or WASM as runtime dependencies.
- No computer use, screenshots, simulated input, or dependence on a desktop MuseScore install.
- The build is Qt-free xmake (`xmake.lua`, `xmake/manifest.lua`); validate Windows and Linux.
  Work natively on Windows (PowerShell, `.pixi/envs/native/python.exe`). Linux means the
  physical host `ssh <linux-host>` (repo `~/OSS/musescore-mcp`); never use WSL for this project.
  The plan of record is `docs/dev/roadmap-engrave.md` (`roadmap-4.7.6.md` is done/superseded).
- Avoid rewriting music algorithms; preserve an executable baseline for each later refactor.
  Compare every engine change against `tests/golden` (`scripts/golden.py`) with an explicit,
  hash-pinned allow-list.
- Pin upstream revisions. Keep downloads in ignored `.upstream/` and runtime assets in `.runtime/`.
- Working notes and worker transcripts belong in ignored `notes/`; durable decisions in `docs/dev/`.
- Workers are not alone: edit only assigned files and never revert other workers' changes.
- The user authorized Gemini 3.8 Flash via `agy` for worker/documentation tasks and
  DeepSeek Flash for worker/simple verification tasks (via Pi, `opencode-go/deepseek-v4.1-flash`,
  thinking max; formerly `dsh headless`). Honor those model choices.
- At major milestones, use GPT-6.1 Terra via Codex for independent reviews (replaced
  Opus 5.5 on 2026-10-28; Argon reviews are paused for quota).
- Do not print credentials or read credential files. Use existing CLI authentication.
- State which claims are source-reviewed, compiled, or actually executed; never conflate them.
- Commit each verified milestone as it is reached. Keep review fixes in small follow-up commits; do not defer all commits to the end.

