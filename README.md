# engrave-mcp

Engine cleanup uses [the C0 public API gate](docs/dev/c0-gate.md).
The v1 MCP host is frozen: its targets are opt-in, and `scripts/run_lanes.py`
is a reference runner for archived binaries under ignored `artifacts/frozen-v1/`.
The host build and transport commands below describe that retained v1 interface.


`engrave-mcp` 是基于 engrave 引擎（MuseScore 核心的裁剪分支）构建的原生 C++ 语言模型上下文协议（MCP）服务，采用 `stdio` 通信，协议版本锁定为 `2025-11-25`。

项目旨在为大语言模型提供精确可控的乐谱符号生成与排版导出能力。完全脱离 Node.js、WASM、桌面版安装与 GUI 自动化交互。

## 当前状态与里程碑

- **M1**：保留 Windows x64 + Qt 原生基线及真实 LLM 创作会话作为对照。
- **M2**：已移除 Qt，使用原生文件系统、Cairo/FreeType/HarfBuzz 和离线 FluidSynth 完成六工具、九格式工作流。
- **M3**：独立 CMake 模块图和不含 Qt 的依赖环境已经构建运行；最新 Windows 本地包 `artifacts/windows-native-002` 为 51,613,872 字节，通过仅含系统 PATH 的验证。
- **M4**：Linux 在 Arch WSL2 x86_64、GCC 15.3、glibc 2.44 完成构建及同样的 MCP/音频回归。`artifacts/linux-native-001.tar.gz` 为 40,166,751 字节；不依赖开发目录的运行也已验证。其他发行版尚未验收。
- **分发范围**：当前验证的是本地部署包；公开分发所需的完整依赖许可证和对应源码清单尚待整理。

## MCP 工具与技术规格

服务为单线程事件循环，单实例最多同时驻留 16 份乐谱文档，JSON 输入上限 8 MiB。

### 导出格式
支持 `MusicXML`、`MXL`、`MSCZ`、`MIDI`、`SVG`、`PNG`、`PDF`、`metadata` 与 `WAV`（音频合成需通过命令行参数配置 SoundFont）。

### 核心工具集
1. `score_create`：结构化新建乐谱。
2. `score_open`：加载既有乐谱。
3. `score_get`：查询乐谱状态与内容。
4. `score_replace_measures`：局部小节替换。
5. `score_export`：多格式渲染与导出。
6. `score_close`：关闭句柄并释放内存。

### 乐谱构建与编辑约束
- **新建规范**：拍号固定为 4/4；480 ticks/四分音符（全小节 1920 ticks）；声部数 1..15；小节数 1..128（各声部小节数必须一致）；GM 音色编号 1..128（跳过通道 10 打击乐）；音高范围 12..127；单和弦最多容纳 8 音；休止符使用空数组 `[]` 表示；合法时值仅限 120、240、480、960、1920 ticks。
- **局部编辑**：仅允许编辑经由 `score_create` 生成的乐谱；`partIndex` 与 `startMeasure` 采用 0 基索引；替换操作必须严格保持替换区域小节数不变；引擎在提交前先导入并校验候选乐谱，校验通过后原子生效。

## 验证与稳定性测试

- **官方 SDK 测试**：通过 MCP 官方 Python SDK 集成验证。
- **压力测试**：Qt-free 后端两种模式各 120 次创建/关闭，每十次实际导出 PDF 或 WAV；40→120 次私有内存增长约 76 KiB / 2.1 MiB，低于 16 MiB 阈值。历史 Qt 基线有一次约 19 MiB 的未收敛回归。阈值通过不等于证明没有泄漏。
- **真实 Agent 会话验证**：DeepSeek Flash 在 Qt 基线上成功编排了 3 声部 8 小节乐谱，修正终止式并导出 4 类格式；独立校验确认 MusicXML 语义、85 个 MIDI note-on 的音高/起始时间及音色一致，WAV 为 44.1 kHz 双声道、23 秒且非静音。未试听音频。
- **Qt-free Agent 会话**：DeepSeek Flash 通过 11 次真实 MCP 调用完成新建、三声部末小节修改、查询、四格式导出及关闭。103 个 MIDI note-on 与最终结构化内容一致，WAV 为 44.1 kHz 双声道、约 22.756 秒；未试听。
- **排版回归**：Qt-free 的 PDF 文字可提取，PNG/PDF 尺寸及内容边界一致，并与 Qt 基线做墨迹密度对比；属于结构和数值验证，未做人工视觉验收。嵌入图片、完整字体回退及 RTL 段落布局暂不支持。

## 构建与测试

### 环境依赖
- Git
- Pixi
- Visual Studio C++ SDK (MSVC)

### 构建命令
```powershell
# 1. 安装锁定依赖
pixi install -e native --locked
pixi run -e native python scripts/bootstrap.py --soundfont

# 2. 编译（构建脚本自动校验 vendor/musescore 子模块并准备运行时资源）
./scripts/build-native.ps1 -Jobs 12

# 3. 运行冒烟与传输测试（需 pixi 环境）
$env:MUSIC_MCP_BINARY = "$PWD/build/xmake-windows/musescore-mcp.exe"
$env:MUSIC_MCP_RESOURCES = "$PWD/.runtime/assets"
pixi run -e native python scripts/mcp_smoke.py
pixi run -e native python scripts/test_transport.py $env:MUSIC_MCP_BINARY
pixi run -e native python scripts/stress_mcp.py

# 4. 打包独立发布包（运行时无需 pixi 环境）
# 在 VS 开发人员终端执行
pixi run -e native python scripts/package_windows.py --no-qt --output artifacts/windows-next
```

`bootstrap.py --soundfont` 下载并校验音色库，打包脚本自动归档。更多细节见 [构建指南](docs/dev/build.md)。

## 客户端配置示例

在 MCP 宿主（如 Claude Desktop）配置文件中添加绝对路径：

```json
{
  "mcpServers": {
    "musescore": {
      "command": "C:/path/to/musescore-mcp/artifacts/windows-native-002/musescore-mcp.exe",
      "args": [
        "--soundfont",
        "C:/path/to/musescore-mcp/artifacts/windows-native-002/soundfonts/FluidR3Mono_GM.sf3"
      ]
    }
  }
}
```

## `score_create` 单小节调用示例

以下是 `score_create` 的 `arguments`，创建一个完整 4/4 小节：

```json
{
  "score": {
    "title": "One Bar Demo",
    "tempo": 100,
    "parts": [{
      "name": "Piano",
      "midiProgram": 1,
      "measures": [{"notes": [
        {"pitches": [60, 64, 67], "ticks": 480},
        {"pitches": [], "ticks": 480},
        {"pitches": [60, 64, 67], "ticks": 960}
      ]}]
    }]
  }
}
```
## 目录与代码规范

- **忽略目录**：`build/`、`.upstream/`、`.runtime/`、`artifacts/`、`notes/`。
- **持久化文档**：设计决策与规范归档于 `docs/dev/`。
- **上游基线**：以 Git 子模块 `vendor/musescore` 形式引入 Fork 仓库（https://github.com/Locietta/engrave ，分支 `main`，Tag `engrave-14`，Commit `ab9e33c39b`；自 `upstream-final` 起作为硬分叉演进，详见 `docs/dev/fork.md`）。第三方依赖（FreeType、HarfBuzz、FLAC、LAME、Opus/libopusenc、fdk-aac、pugixml、utfcpp、picojson、mnx-json-schema、mnxdom）为固定版本的 xmake 包，版本与校验和记录在引擎检出的 `xmake/packages/` 中（宿主专用的 asio 在本仓库 `xmake/packages/asio.lua`）。


