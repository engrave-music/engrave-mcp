# musescore-mcp

`musescore-mcp` 是基于 MuseScore 裁剪引擎构建的原生 C++ 语言模型上下文协议（MCP）服务，采用 `stdio` 通信，协议版本锁定为 `2025-11-25`。

项目旨在为大语言模型提供精确可控的乐谱符号生成与排版导出能力。完全脱离 Node.js、WASM、桌面版安装与 GUI 自动化交互。

## 当前状态与里程碑

- **M1（当前）**：已完成 Windows x64 + Qt 原生基线与 6 个标准 MCP 工具实现。已通过纯净环境（`PATH` 仅含 `System32`）验证绿色发布包 `artifacts/windows-001`（24 个文件，120,530,913 字节）。
- **M2（进行中）**：解耦并剥离 Qt 依赖。
- **M3 & M4（规划中）**：构建系统现代化重构与 Linux 原生支持。
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
- **压力测试**：经受 120 次新建与关闭循环测试（覆盖挂载/未挂载 SoundFont 两种模式），早期 40→120 次循环增长低于 16 MiB，但最新 Qt 回归出现约 19 MiB 增长，压力稳定性仍在调查（注：阈值测试通过不等于完全杜绝内存泄漏）。
- **真实 Agent 会话验证**：DeepSeek Flash 驱动的真实 MCP 交互测试中，模型成功编排了 3 声部 8 小节乐谱，修正了终止式并导出 4 类格式文件；独立校验确认 MusicXML 音乐语义、85 个 MIDI note-on 的音高/起始时间及实际音色一致，WAV 为 44.1 kHz 双声道、23 秒且非静音。PDF 只检查格式，未试听音频。。

## 构建与测试

### 环境依赖
- Git
- Pixi
- Visual Studio C++ SDK (MSVC)

### 构建命令
```powershell
# 1. 安装锁定依赖
pixi install --locked

# 2. 编译（构建脚本自动应用 patch 并校验 upstream.lock.json）
./scripts/windows.ps1 -Action build -Jobs 12

# 3. 运行冒烟与传输测试（需 pixi 环境）
pixi run python scripts/mcp_smoke.py
pixi run python scripts/test_transport.py build/windows/musescore-mcp.exe
pixi run python scripts/stress_mcp.py

# 4. 打包独立发布包（运行时无需 pixi 环境）
./scripts/windows.ps1 -Action package -OutputDirectory artifacts/windows-next
```

> **音频配置说明**：WAV 渲染所需的音色库 `.runtime/soundfonts/FluidR3Mono_GM.sf3` 当前需手动放置，打包脚本会自动将其归档进发布包。

## 客户端配置示例

在 MCP 宿主（如 Claude Desktop）配置文件中添加绝对路径：

```json
{
  "mcpServers": {
    "musescore": {
      "command": "C:/path/to/musescore-mcp/artifacts/windows-001/musescore-mcp.exe",
      "args": [
        "--soundfont",
        "C:/path/to/musescore-mcp/artifacts/windows-001/soundfonts/FluidR3Mono_GM.sf3"
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
- **上游基线**：锁定自 `partitioncloud/webmscore`（Tag `webmscore-4.7.5-1`，Commit `8ec6401a60deac5582403e6914595d12c384d9ae`）。检出版本信息由 `upstream.lock.json` 固化。


