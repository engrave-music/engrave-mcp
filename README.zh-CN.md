# engrave-mcp

`engrave-mcp` 是一个原生 C++ MCP 服务，为 LLM Agent 提供真正的制谱引擎：通过
stdio JSON-RPC 创建、查询、编辑、排版并导出乐谱。

引擎是 **engrave**，即 MuseScore 5.0 核心的固定硬分叉
（[github.com/Locietta/engrave](https://github.com/Locietta/engrave)，以子模块
`vendor/engrave` 引入），直接编译进服务。运行时不需要 Qt、Node.js、WASM，
也不需要安装桌面版 MuseScore。

目标是互操作，而不是历史兼容：Agent 制作的乐谱要能在 MuseScore 中打开并继续编辑，
MuseScore 的乐谱也要能被 Agent 打开并继续编辑。只打开 MuseScore 4.6 及以后版本的乐谱。
默认 MSCZ 写入 5.00 格式；需要与 MuseScore 4.x 互通时，可在文档规定的子集内使用有界的 4.70 写入器。

## 能力

- 打开 MSCZ/MSCX、MusicXML/MXL、MIDI、Guitar Pro（GP3–GP7/GPX/PTB）与 MNX。
- 从引擎模板创建乐谱。
- 读取大纲，查询元素/小节/段/属性/音轨，查询元素类型与样式目录。
- 以原子批次执行编辑（23 种操作），并校验严格版本号；批次失败会回滚。
- 从**内存中的实时文档**直接导出乐谱文件、PDF/PNG/SVG 矢量与栅格页面、合成音频
  （WAV/FLAC/Opus/MP3/AAC）与排版位置数据，并保证持久化发布。
- 运行时热加载字体、替换 SoundFont。

## 快速开始（Windows）

环境要求：Git、[Pixi](https://pixi.sh/) 与 Visual Studio C++ x64 工具链（MSVC 14.51+）。

```powershell
pixi install -e native --locked
pixi run -e native python scripts/bootstrap.py --soundfont
pixi run -e native python scripts/prepare.py
pwsh -NoProfile -File scripts/build-native.ps1 -Jobs 8
```

对构建产物运行协议冒烟测试：

```powershell
# 开发构建依赖 Pixi 原生库，先把它们加入 PATH。
$env:PATH = "$PWD\.pixi\envs\native\Library\bin;$env:PATH"

$env:ENGRAVE_MCP_BINARY   = "$PWD/build/xmake-windows/engrave-mcp.exe"
$env:ENGRAVE_MCP_RESOURCES = "$PWD/.runtime/assets"
$env:ENGRAVE_MCP_SOUNDFONT = "$PWD/.runtime/soundfonts/FluidR3Mono_GM.sf3"
pixi run -e native python tests/integration/test_mcp_smoke.py
```

手动启动服务时显式传入资源目录；`--soundfont` 可选：

```powershell
build/xmake-windows/engrave-mcp.exe --resources .runtime/assets `
  --soundfont .runtime/soundfonts/FluidR3Mono_GM.sf3
```

打包后的产物会自动查找同级的 `assets/` 目录。Windows 与 Linux 的构建、测试和打包
细节见[构建指南](docs/dev/build.md)。

## 九个工具

| 工具             | 作用                                                                                           |
| ---------------- | ---------------------------------------------------------------------------------------------- |
| `score_open`     | 从 `path` 或 base64 `data` 打开（可带 `format`、`filename_hint`、`strict_ids`）。              |
| `score_create`   | 从 `template_id` 或 `template_path` 创建（可带 `title`、`subtitle`、`composer`、`lyricist`）。 |
| `score_close`    | 关闭文档并释放内存。                                                                           |
| `score_outline`  | 标题/元数据、声部、小节、拍号调号、页数、版本号。                                              |
| `score_query`    | `mode`：`elements`、`properties` 或 `audio_tracks`。                                           |
| `score_describe` | `scope`：`types`、`type`（需带 `type`）或 `styles`。                                           |
| `score_edit`     | 原子 `ops` 批次，可带 `expected_revision`。                                                    |
| `score_export`   | `kind`：`score`、`render`、`audio` 或 `positions`。                                            |
| `runtime_info`   | `action`：`get`、`add_font` 或 `set_soundfonts`。                                              |

成功结果带 `ok: true`；失败结果带 `ok: false` 与
`error: {code, message, ...}`，并设置 MCP 的 `isError` 标志。完整契约见
[工具文档](docs/dev/tools.md)，生成的 schema 见 [engrave_mcp/tools.json](engrave_mcp/tools.json)。

下面是调用参数与返回 `structuredContent` 的示意：

```json
{"method":"tools/call","params":{"name":"score_create",
  "arguments":{"template_id":"Treble_Clef","title":"Prelude"}}}
{"ok":true,"document":"doc-807a1e3391c253a993f2c46ac95aedb4-1","revision":"1","diagnostics":[]}
```

```json
{"method":"tools/call","params":{"name":"score_export",
  "arguments":{"document":"doc-807a1e3391c253a993f2c46ac95aedb4-1","kind":"render",
    "destination":{"kind":"file","path":"C:/Scores/prelude.pdf"},
    "options":{"format":"pdf"}}}}
{"ok":true,"source_revision":"1",
 "artifacts":[{"path":"C:/Scores/prelude.pdf","mime":"application/pdf","size":"48213"}]}
```

## 客户端配置

打包后（例如输出到 `artifacts/windows-next`），在客户端配置中使用绝对路径注册服务：

```json
{
  "mcpServers": {
    "engrave-mcp": {
      "command": "C:/path/to/engrave-mcp/artifacts/windows-next/engrave-mcp.exe",
      "args": [
        "--soundfont",
        "C:/path/to/engrave-mcp/artifacts/windows-next/soundfonts/FluidR3Mono_GM.sf3"
      ]
    }
  }
}
```

## 架构概览

- **单引擎、单 owner 线程。** 所有乐谱引擎操作在一个专用 owner 线程上串行执行。
  最多同时打开 16 个文档，它们共享同一个引擎。不支持对同一文档并发编辑。
- **内存实时导出。** 导出直接作用于内存中的实时文档，避免保存再打开引起的排版重排，
  并将导出延迟缩短至毫秒级。
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

明确的非目标：MuseScore 1.x–4.5 读取器、桌面完整功能同等性、复杂/受保护的
DACL 与 ACL 合并、SMB 网络共享认证、设备命名空间与超长路径、跨卷或多文件事务发布、
交互式/流式播放。判定规则见[范围文档](docs/dev/scope.md)。

## 仓库结构

```
engrave_mcp/     MCP 传输、工具注册表、JSON 编解码、实时导出、发布
vendor/engrave/  固定的 engrave 引擎（子模块，按提交固定）
xmake.lua        宿主构建；从 vendor/engrave 引入引擎构建
scripts/         引导、构建、打包等开发工具
tests/           验收测试道、集成测试、支持库、性能基准、测试数据
docs/dev/        架构设计、工具契约、构建指南、测试验证、分叉指南
```

## 文档索引

- [架构设计与取舍](docs/dev/design.md)
- [工具契约说明](docs/dev/tools.md)
- [构建与测试指南](docs/dev/build.md)
- [验证测试道与规范](docs/dev/validation.md)
- [引擎分叉指南](docs/dev/fork.md)
- [支持范围与非目标](docs/dev/scope.md)
- [已知缺陷与限制](docs/dev/defects.md)
- [分支与标签策略](docs/dev/branching.md)
- [English README](README.md)
