# musescore-mcp

Native C++ MCP host backed by a pinned, trimmed MuseScore engine. This is a
separate project from `C:/path/to/llm-music`, which holds baseline experiments.

- Reuse the C++ implementation and module selection in partitioncloud/webmscore.
  Native compilation is the target; do not introduce Node.js or WASM as runtime dependencies.
- No computer use, screenshots, simulated input, or dependence on a desktop MuseScore install.
- First validate native Windows with Qt/offscreen and upstream CMake retained.
- After the Windows baseline works, remove Qt and refactor the build system, then validate Linux.
- Avoid rewriting music algorithms; preserve an executable baseline for each later refactor.
- Pin upstream revisions. Keep downloads in ignored `.upstream/` and runtime assets in `.runtime/`.
- Working notes and worker transcripts belong in ignored `notes/`; durable decisions in `docs/dev/`.
- Workers are not alone: edit only assigned files and never revert other workers' changes.
- The user authorized Gemini 3.8 Flash via `agy` for worker/documentation tasks and
  DeepSeek Flash via `dsh headless` for worker/simple verification tasks. Honor those model choices.
- At major milestones, use Argon and Opus 5.5 via `agy` for independent reviews.
- Do not print credentials or read credential files. Use existing CLI authentication.
- State which claims are source-reviewed, compiled, or actually executed; never conflate them.
- Commit each verified milestone as it is reached. Keep review fixes in small follow-up commits; do not defer all commits to the end.

