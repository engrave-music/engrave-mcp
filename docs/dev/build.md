# MuseScore Native MCP 构建与开发者指南

本文档记录基于固定（pinned）上游代码的原生 C++ MuseScore MCP 服务端构建流程、架构约束及验证规范。

---

## 1. 架构设计与约束

- **构建系统**：采用代码库内直接维护的 `CMakeLists.txt` 模块图谱，无需动态生成 CMake 文件，亦不重写上游 `web/main.cpp`。编译标准为 **C++20**。
- **模块集成**：包含 `engraving`、`global`、`draw`、`mpe`、`musicxml` 以及选定的 `MIDI` 与 `FluidSynth` 组件。
- **底层适配**：采用原生文件系统、进程内设置和 Cairo 绘图后端；乐谱导入、排版和 MIDI 事件生成复用上游实现。设置不持久化。

---

## 2. 环境要求

### Windows 构建环境
- **编译器**：Visual Studio x64 MSVC 工具链；构建脚本自动初始化开发环境，单独打包需在 VS 开发人员终端执行。
- **包管理器**：Pixi。
  - `native` 环境（`-e native`）：剔除了所有 Qt 依赖的纯原生编译环境。
  - 默认环境（`default`）：保留供历史 Qt 基线使用。

---

## 3. 上游代码与资产准备

### 3.1 检出上游与下载音色库
运行 `bootstrap.py` 拉取固定的上游仓库、初始化 Brotli 子模块，并按需下载经哈希验证的 SF3 基础音色库：

```powershell
pixi install -e native --locked
pixi run -e native python scripts/bootstrap.py --soundfont
```

### 3.2 补丁应用与资源准备
`scripts/prepare.py` 会执行以下幂等操作：
1. 校验上游修订版本（revision）一致性。
2. 幂等应用 `patches/native-platform.patch`。
3. 根据 `resources.json` 将资源别名复制到 `.runtime/assets`。

CMake 跟踪 `native/tools.json` 并在各自构建目录生成工具定义，修改 schema 后普通增量构建也会更新。

---

## 4. Windows 原生构建流程

使用原生脚本进行编译：

```powershell
./scripts/build-native.ps1 -Jobs 12
```

### 直接运行构建产物
构建产物位于 `build/native-windows/musescore-mcp.exe`。未经打包直接从构建目录启动时，必须传入资源的绝对路径：

```powershell
pixi run -e native build/native-windows/musescore-mcp.exe --resources "C:/path/to/musescore-mcp/.runtime/assets" --soundfont "C:/path/to/musescore-mcp/.runtime/soundfonts/FluidR3Mono_GM.sf3"
```
*(注：`--soundfont` 为可选参数。)*

---

## 5. 打包与独立部署验证

### 5.1 自动化打包
在 Visual Studio 开发人员终端与 Pixi 原生环境下执行打包脚本：

```powershell
pixi run -e native python scripts/package_windows.py --no-qt --output artifacts/windows-next
```

- **动作规范**：自动解析并提取运行时完整的 DLL 闭包、静态资源及 SoundFont；**严格拒绝混入任何 Qt 相关动态链接库**。
- **路径自解析**：打包后的可执行文件已具备就近查找同级资产的能力，运行时无需额外传递 `--resources` 绝对路径。

### 5.2 独立运行验证标准
打包产物已完成严格的洁净环境验证：
- 运行时环境变量 `PATH` 仅保留 `C:\Windows\System32`。
- 完全脱离桌面版 MuseScore 安装、Node.js、WASM、Python 及 Pixi 运行环境。
- 当前验证包体积为 51,610,288 字节（包含 Conda 元数据）。当前为本地验证包，公开分发所需的完整许可证与对应源码清单尚待整理。

---

## 6. 历史 Qt 基线 (Legacy Baseline)

- 仓库内保留的 `scripts/windows.ps1` 与 `prepare_native.py` 属于历史遗留脚本。
- 其作用严格限于维护可执行的 Qt 原型基线用于对照验证，不属于当前的 Native 生产发布链路。

---

## 7. Linux 适配进展

- Linux 平台的原生构建与环境验证正在进行中。
- 当前阶段未完成验收。

---

## 8. 当前功能边界与已知局限性

| 维度 | 现状说明 |
| :--- | :--- |
| **乐谱渲染算法** | 完整保留原生排版、分行、跨小节与记谱逻辑。 |
| **图像元素** | 暂不支持嵌入式光栅图及外部图片对象（images unsupported）。 |
| **排版与文字** | 尚未实现完整的字体回退（font fallback）机制；不支持从右到左（RTL）书写文字。 |
| **绘制模型** | 仅支持定义明确的纯色画刷（solid brushes）与指定混合模式，渐变/复杂贴图绘制受限。 |
