# engrave-mcp

`engrave-mcp` 是一个原生 C++ MCP 服务，为 LLM Agent 提供真正的制谱引擎：通过
stdio JSON-RPC 创建、查询、编辑、排版并导出乐谱。

引擎是 **engrave**，即 MuseScore 5.0 核心的固定硬分叉
（[github.com/Locietta/engrave](https://github.com/Locietta/engrave)，以子模块
`vendor/engrave` 引入），直接编译进服务。运行时不需要 Qt、Node.js、WASM，
也不需要安装桌面版 MuseScore。

目标是互操作，而不是兼容：Agent 做出的乐谱要能在 MuseScore 中打开并继续编辑，
MuseScore 的乐谱也要能被 Agent 打开并继续编辑。MuseScore 的历史包袱不是目标：
只打开 MuseScore 4.6 及以后版本的乐谱。默认 MSCZ 写入 5.00；需要与 MuseScore 4.x
互通时，可在文档规定的子集内使用有界的 4.70 写入器。

## 能力

- 打开 MSCZ/MSCX、MusicXML/MXL、MIDI、Guitar Pro（GP3–GP7/GPX/PTB）与 MNX。
- 从引擎模板创建乐谱。
- 读取大纲，查询元素/小节/段/属性/音轨，查询元素类型与样式目录。
- 以原子批次执行编辑（23 种操作），并校验严格版本号；批次失败会回滚。
- 从**内存中的实时文档**导出乐谱文件、PDF/PNG/SVG 页面、音频
  （WAV/FLAC/Opus/MP3/AAC）与位置数据，并保证持久化发布。
- 运行时热加载字体、替换 SoundFont。

## 快速开始（Windows）

需要 Git、[Pixi](https://pixi.sh/) 与 Visual Studio C++ x64 工具链。

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
pixi run -e native python scripts/mcp_smoke.py
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
[工具文档](docs/dev/tools.md)，生成的 schema 见 [native/tools.json](native/tools.json)。

下面是调用参数与返回 `structuredContent` 的示意，不是完整的线上报文：真实会话先
发送 `initialize`，每次调用都包在带 id 的 JSON-RPC 2.0 请求/响应中。

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

打包后（例如输出到 `artifacts/windows-next`），用绝对路径注册服务：

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

## 一页纸设计

- **单引擎、单 owner 线程。** 所有文档操作在一个专用 owner 线程上串行执行。
  最多同时打开 16 个文档，它们共享同一个引擎。不支持对同一文档并发编辑，这是
  有意的设计。
- **实时导出。** 导出直接作用于内存中的文档。此前的 worker/快照/任务设计已被删除：
  保存再打开会改变排版，增加数百毫秒延迟，并带来大量故障面。
- **持久化发布。** 输出先写入私有暂存目录并刷盘，再原子重命名到目标位置。多文件
  导出按文件原子，而不是一个事务；出错时会精确报告哪些文件已经发布。
- **协作式取消 + 看门狗。** 长操作轮询取消标志。被取消的导出如果继续阻塞 5 秒，
  会先取得进程级发布锁，然后以退出码 70 结束进程，而不是强杀线程。
- **明确的配额。** 请求行 4 MiB、64 个在途调用、响应 16 MiB、导出预算 1 GiB、
  解码输入 8 MiB、最多 16 个文档。

这些选择的理由见[设计文档](docs/dev/design.md)。

## 范围与非目标

支持：Windows 与 Linux 上普通的单用户本地文件系统、普通继承权限、文档规定的路径
语法，以及 MuseScore 4.6 及以后版本的乐谱。

明确的非目标：MuseScore 1.x–4.5 读取器、完整的 MuseScore 兼容性、复杂/受保护的
DACL 与 ACL 合并、SMB 认证、设备命名空间与超长路径、跨卷或多文件事务发布、
交互式/流式播放。判定规则见[范围文档](docs/dev/scope.md)。

## 仓库结构

```
native/            MCP 传输、注册表、JSON 编解码、实时导出、发布
vendor/engrave   固定的 engrave 引擎（子模块，按提交固定）
xmake.lua          宿主构建；从 vendor/engrave 引入引擎构建
scripts/           引导、构建、打包、测试道、基准
tests/             fixture、golden 基线、音频语料
docs/dev/          设计、工具契约、构建、验证、fork 台账、历史
```

## 文档

- [设计与取舍](docs/dev/design.md)
- [工具契约](docs/dev/tools.md)
- [构建与测试](docs/dev/build.md)
- [验证道与已验收证据](docs/dev/validation.md)
- [引擎分叉与 pin 台账](docs/dev/fork.md)
- [支持范围与非目标](docs/dev/scope.md)
- [未解决缺陷](docs/dev/defects.md)
- [项目历史](docs/dev/history.md)
- [分支与标签策略](docs/dev/branching.md)
- [English README](README.md)
