# engrave-mcp

`engrave-mcp` 是基于 [engrave](https://github.com/Locietta/engrave) 引擎的原生 C++ MCP 服务，通过 stdio JSON-RPC 提供乐谱创建、查询、编辑、排版与导出。引擎是 MuseScore 核心的固定、裁剪分支；运行时不需要 Node.js、WASM 或桌面版 MuseScore。

当前服务使用 v2 工具契约。原有 v1 工具及其调用格式不再由服务提供；历史基线单独保留。乐谱交互的目标是让人类和 Agent 都能继续编辑作品，完整的 MuseScore 旧版兼容性不是目标。

## MCP 工具

服务提供以下十个工具：

1. `score_open`：从路径或数据打开乐谱。
2. `score_create`：从引擎模板创建乐谱。
3. `score_close`：关闭文档。
4. `score_outline`：查询乐谱结构与版本。
5. `score_query`：查询元素、属性或音轨。
6. `score_describe`：查询元素属性和样式目录。
7. `score_edit`：以原子批次编辑乐谱。
8. `score_export`：导出乐谱、页面、音频或位置数据，并支持后台任务。
9. `runtime_info`：查询运行环境，或添加字体、替换 SoundFont。
10. `runtime_job`：查询或取消后台任务。

参数、输出、错误与版本约束见 [v2 registry 文档](docs/dev/a5-registry.md) 和生成的 [工具 schema](native/tools.json)。例如，`score_create` 的 `arguments` 可以是：

```json
{"template_id": "Treble_Clef", "title": "One Bar Demo"}
```

使用 `runtime_info` 查看可用模板及格式；编辑内容使用 `score_edit`。

## 构建与测试

Windows 需要 Git、Pixi 和 Visual Studio C++ x64 工具链：

```powershell
pixi install -e native --locked
pixi run -e native python scripts/bootstrap.py --soundfont
pwsh -NoProfile -File scripts/build-native.ps1 -Jobs 8

$env:ENGRAVE_MCP_BINARY = "$PWD/build/xmake-windows/engrave-mcp.exe"
$env:ENGRAVE_MCP_RESOURCES = "$PWD/.runtime/assets"
$env:ENGRAVE_MCP_SOUNDFONT = "$PWD/.runtime/soundfonts/FluidR3Mono_GM.sf3"
pixi run -e native python scripts/mcp_smoke.py
pixi run -e native python scripts/test_transport.py $env:ENGRAVE_MCP_BINARY
```

`ENGRAVE_MCP_*` 变量配置这些测试和客户端脚本；直接启动服务时通过 `--resources` 和可选的 `--soundfont` 传入路径。打包后的服务默认查找可执行文件同级的 `assets` 目录。Windows 和 Linux 的构建、打包及验收命令见 [构建指南](docs/dev/build.md)。

## 客户端配置示例

构建并打包到 `artifacts/windows-next` 后，可在 MCP 客户端中填写绝对路径：

```json
{
  "mcpServers": {
    "engrave-mcp": {
      "command": "C:/path/to/musescore-mcp/artifacts/windows-next/engrave-mcp.exe",
      "args": [
        "--soundfont",
        "C:/path/to/musescore-mcp/artifacts/windows-next/soundfonts/FluidR3Mono_GM.sf3"
      ]
    }
  }
}
```

## 仓库与基线

引擎固定为 `vendor/musescore` 子模块，版本和 fork 台账见 [fork 文档](docs/dev/fork.md)。`build/`、`.upstream/`、`.runtime/`、`artifacts/` 和 `notes/` 不纳入版本控制；持久设计记录位于 `docs/dev/`，对照数据位于 `tests/golden/`。
