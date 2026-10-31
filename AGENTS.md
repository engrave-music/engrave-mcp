# engrave-mcp

Native C++ MCP host for the engrave engine, a pinned, trimmed fork of MuseScore
core. Renamed from musescore-mcp on 2026-10-30; the binary, the MCP server name
and the `MUSIC_MCP_*` env vars keep their old names until A5. This is a
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
- Worker models: Opus 5.5, GPT-6 Astra, GPT-6.1 Terra, DeepSeek Flash and Gemini 3.8 Flash.
  Pick the cheapest model likely to finish a task in one pass, and escalate on a stall rather
  than retrying. Hard rules: Terra only at reasoning effort max; DeepSeek via Pi
  `opencode-go/deepseek-v4.1-flash` (the paid `deepseek/deepseek-flash` only while opencode-go
  is down); no GPT-6.1 Sol.
- Reviews: each roadmap step (A1, A2, ...) ends with a full Terra validation; each phase
  (A, I, B) gets a final GPT-6 Astra review before it is accepted, looking for remaining
  defects and poor implementation approaches across the phase. GPT reviews over-flag: triage
  findings by real impact.
- Do not print credentials or read credential files. Use existing CLI authentication.
- State which claims are source-reviewed, compiled, or actually executed; never conflate them.
- Commit each verified milestone as it is reached. Keep review fixes in small follow-up commits; do not defer all commits to the end.

