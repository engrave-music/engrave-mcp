# engrave-mcp

`engrave-mcp` 是一个原生 C++ MCP 服务，为 LLM Agent 提供真正的制谱引擎：通过
stdio JSON-RPC 创建、查询、编辑、排版并导出乐谱。

引擎是 **engrave**，即 MuseScore 5.0 核心的固定硬分叉
（[github.com/Locietta/engrave](https://github.com/Locietta/engrave)，以子模块
`vendor/engrave` 引入），直接编译为独立原生 C++ 二进制。运行时不需要 Node.js、WASM，
也不需要安装桌面版 MuseScore。

目标是互操作，而不是历史兼容：Agent 制作的乐谱要能在 MuseScore 中打开并继续编辑，
MuseScore 的乐谱也要能被 Agent 打开并继续编辑。读取器接受 MuseScore 4.60 至 5.00 格式的乐谱，
高于 5.00 或低于 4.60 的版本均会被拒绝。包含内嵌图像的 MSCZ 与 MusicXML 输入会被拒绝。
默认 MSCZ 写入 5.00 格式；需要与 MuseScore 4.x 互通时，可在文档规定的子集内使用面向 MuseScore 4.7.0–4.7.6 的有界 4.70 写入器。

## 能力

- 通过 stdio JSON-RPC 同时支持现代无状态 MCP（`2026-07-28`）与旧版会话 MCP（`2025-11-25` / `2025-06-18`）。
- 打开 MSCZ/MSCX、MusicXML/MXL、MIDI、Guitar Pro（GP3–GP7/GPX/PTB）与 MNX。
- 从引擎模板创建乐谱。
- 读取大纲，按小节窗口读回乐谱内容（`score_read_music`），查询元素/属性/页数/音轨（`score_query`），并查询模板、乐器、元素类型、样式与编辑操作目录（`runtime_info`）。
- 使用紧凑记谱语法写入乐句（`score_write_phrase`），或执行编辑批次（28 种操作）并校验严格版本号（`score_edit`）。编辑批次默认原子执行（传 `atomic: false` 可改为前缀提交）。需要回滚时使用命名检查点。
- 从**内存中的实时文档**直接导出乐谱文件、PDF/PNG/SVG 矢量与栅格页面、合成音频
  （WAV/FLAC/Opus/MP3）与排版位置数据，并保证持久化发布。
- 运行时热加载字体、替换 SoundFont（`runtime_configure`）。

## 快速开始（Windows）

环境要求：Git 与 [Pixi](https://pixi.sh/)（Bazelisk 与隔离的 `xclang` C++23 工具链会自动配置）。

克隆时使用 `--recursive`，或者在安装前初始化引擎子模块：

```powershell
git submodule update --init
pixi install --locked
pixi run python scripts/bootstrap.py --soundfont
pixi run python scripts/prepare.py
pwsh -NoProfile -File scripts/build-native.ps1 -Jobs 12
```

对构建产物运行协议冒烟测试：

```powershell
$env:ENGRAVE_MCP_BINARY   = "$PWD/build/release/engrave-mcp.exe"
$env:ENGRAVE_MCP_RESOURCES = "$PWD/.runtime/assets"
$env:ENGRAVE_MCP_SOUNDFONT = "$PWD/.runtime/soundfonts/FluidR3Mono_GM.sf3"
pixi run python tests/integration/test_mcp_smoke.py
```

手动启动开发构建产物时（通过 `pixi run` 自动将 Pixi 原生库加入 `PATH`）显式传入资源目录；`--soundfont` 可选：

```powershell
pixi run build/release/engrave-mcp.exe --resources .runtime/assets `
  --soundfont .runtime/soundfonts/FluidR3Mono_GM.sf3
```

打包后的产物会自动查找同级的 `assets/` 目录。Windows 与 Linux 的构建、测试和打包
细节见[构建指南](docs/dev/build.md)：

- **日常开发构建**：使用 `scripts/bbuild.py --profile <release|debug|releasedbg|sanitize> -j 12 build engrave_mcp`（输出到 `build/<profile>`，共享缓存在 `build/.bazel_repo_cache` 与 `build/.bazel_disk_cache`）。`scripts/build-native.ps1` 与 `scripts/build-native.sh` 会重置 `vendor/engrave`，仅在固定引擎提交上做干净构建时使用。
- **验收测试道**：使用 `tests/run_lanes.py`（自动构建所需目标；`--no-build` 跳过构建，`--profile` 选择 `build/<profile>`，`--list-lanes` 打印调度列表），并将 `--output` 指向 `notes/runs/<name>`；详见[验证测试道与规范](docs/dev/validation.md)。

## 工具

| 工具                 | 作用                                                                                                                  |
| -------------------- | --------------------------------------------------------------------------------------------------------------------- |
| `score_open`         | 从 `path` 或 base64 `data` 打开（可带 `format`、`filename_hint`、`strict_ids`）。                                     |
| `score_create`       | 从 `template_id` 或 `template_path` 创建（可带 `title`、`subtitle`、`composer`、`lyricist`）。                        |
| `score_close`        | 关闭文档并释放内存。                                                                                                  |
| `score_outline`      | 标题/元数据、声部、小节、拍号调号、页数、版本号。                                                                     |
| `score_read_music`   | 按小节窗口读回结构化音乐内容与小节对齐摘要（`measure`、`count`、可选 `staves`、`voices`、`excerpt`、`include_ids`）。 |
| `score_write_phrase` | 按小节/谱表/声部轨道用紧凑记谱语法写入乐句（`lanes`，每条轨道可设 `pitch_mode`；可选 `expected_revision`）。          |
| `score_query`        | `mode`：`elements`、`properties`、`page_count` 或 `audio_tracks`。                                                    |
| `score_edit`         | 执行最多 1,024 条 `ops`（默认原子提交，传 `atomic: false` 为前缀提交），可带 `expected_revision`。                    |
| `score_export`       | 将指定 `format`（`mscz`、`pdf`、`png`、`svg`、`wav`、`flac`、`opus`、`mp3`、位置数据等）导出到 `path`。               |
| `runtime_info`       | 查询能力、配额与目录（`scope`：`overview`、`templates`、`instruments`、`types`、`type`、`styles`、`ops`）。           |
| `runtime_configure`  | 运行时热加载字体（`add_font`）和/或替换有序 `soundfonts` 列表。                                                       |
| `checkpoint`         | 保存文档的命名内存快照。                                                                                              |
| `restore_checkpoint` | 恢复命名快照，并清除中毒状态。                                                                                        |
| `drop_checkpoint`    | 删除命名快照。                                                                                                        |

成功结果带 `ok: true`；失败结果带 `ok: false` 与
`error: {code, message, ...}`，并设置 MCP 的 `isError` 标志。完整契约见
[工具文档](docs/dev/tools.md)，生成的 schema 见 [engrave_mcp/tools.json](engrave_mcp/tools.json)。

下面是调用参数与返回 `structuredContent` 的示意：

```json
{"method":"tools/call","params":{"name":"score_create",
  "arguments":{"template_id":"Treble_Clef","title":"Prelude"}}}
{"ok":true,"document":"doc-807a1e3391c253a993f2c46ac95aedb4-1","revision":"1","outline":{"...":"..."},"diagnostics":[]}
```

```json
{"method":"tools/call","params":{"name":"score_export",
  "arguments":{"document":"doc-807a1e3391c253a993f2c46ac95aedb4-1",
    "format":"pdf","path":"C:/Scores/prelude.pdf"}}}
{"ok":true,"source_revision":"1",
 "artifacts":[{"path":"C:/Scores/prelude.pdf","mime":"application/pdf","size":48213}]}
```

## 客户端配置

打包后（例如输出到 `artifacts/windows-next`），在客户端配置中使用绝对路径注册服务。
打包产物会自动发现同级 `assets/` 目录；如果打包时一并包含了 `soundfonts/FluidR3Mono_GM.sf3`，
也会自动加载该文件（未打包该 SoundFont 或需使用其他 SoundFont 时，请传 `--soundfont`
或调用 `runtime_configure`）：

```json
{
  "mcpServers": {
    "engrave-mcp": {
      "command": "C:/path/to/engrave-mcp/artifacts/windows-next/engrave-mcp.exe"
    }
  }
}
```

## 架构概览

- **单引擎、单 owner 线程。** 所有乐谱引擎操作在一个专用 owner 线程上串行执行。
  最多同时打开 16 个文档，它们共享同一个引擎。不支持对同一文档并发编辑。
- **内存实时导出。** 导出直接作用于内存中的实时文档，避免保存再打开引起的排版重排，
  并尽可能降低导出延迟。
- **持久化发布。** 输出先写入私有暂存目录并刷盘，再原子重命名到目标位置。多文件
  导出按文件原子发布，并在出错时精确报告哪些文件已发布。
- **协作式取消与看门狗。** 长操作轮询取消标志。在发布开始前取消不会发布任何文件；发布一旦开始，
  迟到的取消仍可能成功完成或报告已发布的文件。若被取消的实时导出持续阻塞，5 秒看门狗定时器到期后
  会等待同步报告交付全程持有的发布锁（未读 stdout 会无限期推迟退出），之后以退出码 70 退出进程，而不是异步强杀线程。
- **明确的配额限制。** 请求行 4 MiB、64 个在途调用、响应 16 MiB 上限、导出预算 1 GiB、
  解码输入上限 8 MiB、最多 16 个打开文档。

设计取舍与理由见[设计文档](docs/dev/design.md)。

## 范围与非目标

支持范围：Windows 与 Linux 上普通的单用户本地文件系统、普通继承权限、文档规定的路径
语法，以及 MuseScore 4.6 及以后版本的乐谱。

明确的非目标：MuseScore 1.x–4.5 读取器、桌面完整功能同等性、服务内置文件系统沙箱或已废弃的
MCP `roots` 边界强制（工作区隔离由宿主侧 OS 沙箱与客户端工具审批负责）、复杂/受保护的
DACL 与 ACL 合并、SMB 网络共享认证、设备命名空间与超长路径、跨卷或多文件事务发布、
交互式/流式播放。判定规则见[范围文档](docs/dev/scope.md)。

## 仓库结构

```
engrave_mcp/     MCP 传输、工具注册表、JSON 编解码、实时导出、发布
vendor/engrave/  固定的 engrave 引擎（子模块，按提交固定）
licenses/        打包时随产物分发的第三方与运行时许可证声明
MODULE.bazel     Bzlmod 依赖与 xclang 工具链配置
BUILD.bazel      宿主构建目标；从 vendor/engrave 引入 @engrave
scripts/         引导、Bazel 构建封装、打包等开发工具
tests/           验收测试道、集成测试、支持库、性能基准、测试数据
docs/dev/        架构设计、工具契约、构建指南、测试验证、分叉指南
```

## 文档索引

- [架构设计与取舍](docs/dev/design.md)
- [工具契约说明](docs/dev/tools.md)
- [构建与测试指南](docs/dev/build.md)
- [验证测试道与规范](docs/dev/validation.md)
- [引擎分叉与分支策略](docs/dev/fork.md)
- [支持范围与非目标](docs/dev/scope.md)
- [已知缺陷与限制](docs/dev/defects.md)
- [代码风格指南](docs/dev/code-style-guide.md)
- [English README](README.md)
