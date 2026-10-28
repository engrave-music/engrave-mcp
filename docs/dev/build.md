# MuseScore Native MCP 构建与开发者指南

本文档记录基于固定（pinned）上游代码的原生 C++ MuseScore MCP 服务端构建流程、架构约束及验证规范。

---

## 1. 架构设计与约束

- **构建系统**：以 `xmake.lua` 与显式源码清单 `xmake/manifest.lua` 为**构建系统**，采用 **C++23** 标准（Windows 与 Linux Linux x86_64 均已验证）。旧版 CMake 根配置及历史脚本已移除，最后可执行的 pre-fork 状态留存于本地 Git 标签 `pre-fork-baseline`。构建支持 `release`、`debug` 以及带符号调试的 `releasedbg` 模式，并通过 `--musescore_source=DIR` 选项覆盖引擎源码目录（默认为 `vendor/musescore` 子模块）。
- **模块集成**：包含 `engraving`、`global`、`draw`、`mpe`、`musicxml`、`midi` (包含导入/导出适配)、`guitarpro` 导入组件，以及 MuseScore 4 音频引擎（`muse_audio_engine`：FluidSynth 合成、混音、混响与 SoundTrackWriter）及其编码器 LAME（MP3）、libFLAC、Opus/libopusenc（Ogg）、fdk-aac（AAC），全部来自子模块并以 Qt-free 方式编译；音频仅在 `--worker` 子进程中渲染。libsndfile 已从构建依赖和 pixi 环境中移除；测试分析用的 Python `soundfile` wheel 自带其运行时副本，因此解码检查不依赖系统 libsndfile。捆绑的原生依赖包括 FreeType、HarfBuzz、FluidSynth、FLAC、Opus/libopusenc、LAME 与 fdk-aac（Brotli 依赖已移除）。
- **底层适配**：采用原生文件系统、进程内设置、FreeType/HarfBuzz 字体引擎和 Cairo 绘图后端；乐谱导入、排版和 MIDI 事件生成复用上游实现。设置不持久化。

---

## 2. 环境要求

### Windows 构建环境
- **版本控制与工具**：Git（用于管理工程与拉取 `vendor/musescore` 子模块）。全新克隆需运行 `git submodule update --init --depth 1` 或 `python scripts/bootstrap.py`。
- **编译器**：Visual Studio x64 MSVC 工具链（推荐 VS 2022 / 18.10+，MSVC 14.51+）；构建脚本自动初始化开发环境，单独打包需在 VS 开发人员终端执行。
- **包管理器**：Pixi。构建与验证在 `native` 环境（`-e native`，纯原生无 Qt 编译环境）下执行。
- **构建工具**：xmake ≥ 3.1.1（Windows 使用 3.1.2）；首次下载 HarfBuzz 需要 PATH 中的 `7z` 或 `7zz`。

### Linux 构建环境
- **验证宿主**：Linux x86_64 (`ssh <linux-host>`, kernel 7.3, Pixi GCC 15.3, C++23)。本项目不使用 WSL（早期 Arch WSL2 验证记录仅作历史保留）。
- **编译器**：Pixi GCC 15.3，采用 `-std=c++23`、`-O3`、`-fPIC`。
- **构建工具**：xmake ≥ 3.1.1。

---

## 3. 上游代码与资产准备

### 3.1 检出引擎子模块与下载音色库
引擎源码采用 Git 子模块 `vendor/musescore` 形式引入（锁定至 Fork 仓库 https://github.com/Locietta/engrave ，分支 `mcp/v4.7.6`，Tag `mcp-4.7.6-4`，Commit `6cd717f2`，提交台账参见 `docs/dev/fork.md`）。旧版 webmscore、`.upstream/webmscore`、`patches/*.patch` 以及 Brotli 依赖均已废弃移除。

全新克隆的项目可执行 `git submodule update --init --depth 1` 初始化子模块，或直接运行 `bootstrap.py`。`scripts/bootstrap.py` 会自动执行浅子模块更新（`git submodule update --init --depth 1 -- vendor/musescore`），并按需下载经哈希验证的 HarfBuzz 源码（解压至 `.upstream/harfbuzz`）与基础 SF3 音色库：

```powershell
pixi install -e native --locked
pixi run -e native python scripts/bootstrap.py --soundfont --harfbuzz
```

### 3.2 子模块校验与资源准备
`scripts/prepare.py` 会执行以下幂等操作：
1. 校验 `vendor/musescore` 子模块已初始化、指向仓库记录的提交，且工作区绝对洁净（无本地修改或未跟踪文件；若状态不符则拒绝执行）。
2. 根据 `resources.json` 清单（共 69 项资源），直接从已提交的 Git blob 中读取资源文件并同步到 `.runtime/assets`，同时清理其中不再匹配的旧版残留文件。此流程不再应用任何 patch。

运行时资源清单 `resources.json` 由 `scripts/generate_resources.py` 从 `vendor/musescore` 的 Qt 资源清单（`src/engraving/engraving.qrc`、`src/engraving/data/fonts/*.qrc` 以及 MPE 的 `qt_add_resources` 配置块）外加一项显式历史回退样式（`legacy-style-defaults-v450.mss`）自动解析生成。可执行以下命令验证其时效性与一致性：

```powershell
python scripts/generate_resources.py --check
```

xmake 的生成目标在各自构建目录写入框架配置与工具定义；增量构建重新检查 `native/tools.json`，内容未变时保留时间戳。源码清单已提交，常规构建不运行 `capture_xmake_manifest.py`，也不读取已移除的旧版 CMake 目录。

Windows 和 Linux 已验证长 UTF-8 schema 的完整传递、修改 schema 只重编译 `noqt_engine.cpp`，以及恢复后的无变化构建不编译/链接。生成器分块编码字符串，避免 MSVC 单个字符串长度限制。验收命令：

```powershell
pixi run -e native python scripts/test_xmake_incremental.py --binary build/xmake-windows/musescore-mcp.exe --output artifacts/xmake-incremental
```

---

## 4. Windows 原生构建流程

使用原生包装脚本进行编译（禁止直接调用原始 xmake）：

```powershell
pwsh -NoProfile -File notes/claude-scripts/build.ps1 musescore_mcp
```
*(日常开发可使用 `./scripts/build-native.ps1 -Jobs 12`。)*

构建系统支持通过 `--musescore_source=DIR` 选项覆盖引擎源码目录（默认指向 `vendor/musescore`），并提供 `releasedbg` 模式用于带符号排错与性能分析。

### 直接运行构建产物
构建产物位于 `build/xmake-windows/musescore-mcp.exe`。未经打包直接从构建目录启动时，必须传入资源的绝对路径：

```powershell
pixi run -e native build/xmake-windows/musescore-mcp.exe --resources "C:/path/to/musescore-mcp/.runtime/assets" --soundfont "C:/path/to/musescore-mcp/.runtime/soundfonts/FluidR3Mono_GM.sf3"
```
*(注：`--soundfont` 为可选参数。)*

---

## 5. 打包与独立部署验证

### 5.1 自动化打包
在 Visual Studio 开发人员终端与 Pixi 原生环境下执行打包脚本：

```powershell
pixi run -e native python scripts/package_windows.py --no-qt --binary build/xmake-windows/musescore-mcp.exe --output artifacts/windows-next
```

- **动作规范**：自动解析并提取运行时完整的 DLL 闭包、静态资源及 SoundFont；**严格拒绝混入任何 Qt 相关动态链接库**。
- **打包规格**：Windows 打包目录包含 139 个文件，体积约为 61 MB。
- **路径自解析**：打包后的可执行文件已具备就近查找同级资产的能力，运行时无需额外传递 `--resources` 绝对路径。

### 5.2 独立运行验证标准
打包产物已完成严格的洁净环境验证：
- 运行时环境变量 `PATH` 仅保留 `C:\Windows\System32`。
- 完全脱离桌面版 MuseScore 安装、Node.js、WASM、Python 及 Pixi 运行环境。
- 当前 xmake 产物通过全部 25 条测试道回归，涵盖官方 SDK 冒烟、传输测试、版本信息（`scripts/test_version_info.py`）、上游 72 例反复播放顺序（`scripts/test_repeats.py`）、`scripts/golden.py compare`（针对 `tests/golden/<platform>` 与 `tests/golden/allow.json` 白名单）排版渲染验证，以及 `scripts/test_roundtrip.py` 往返一致性测试。

---

## 6. 历史 Qt 基线 (Legacy Baseline)

历史 Qt 时代及 C++20 阶段的根目录 `CMakeLists.txt` 与相关历史脚本（`build-cmake-native.ps1/.sh`、`windows.ps1`、`prepare_native.py`、`prepare_noqt_probe.py`、`noqt-probe.ps1`、`run_probe.py` 等）均已移除。

最后可执行的 pre-fork 历史基线状态已通过本地 Git 标签 `pre-fork-baseline` 归档固化。若需回溯比对早期原型或排查迁移差异，可检出该标签查看历史代码；当前主线工程完全基于 `vendor/musescore` 子模块与 xmake 构建体系。

---

## 7. Linux 原生构建与 Linux 验证

Linux 构建与验证现已常态化运行于 Linux x86_64 宿主机器（`ssh <linux-host>`，路径 `~/OSS/musescore-mcp`）。早期的 Arch WSL2 x86_64 记录仅作历史保留，现在不再使用 WSL。宿主运行环境为 Linux kernel 7.3、Pixi GCC 15.3、C++23。

### 7.1 构建与打包命令

```bash
pixi install -e native --locked
pixi run --locked -e native python scripts/bootstrap.py --soundfont --harfbuzz
pixi run --locked -e native python scripts/prepare.py
bash scripts/build-native.sh 8
pixi run --locked -e native python scripts/package_linux.py --binary build/xmake-linux/musescore-mcp --output artifacts/linux-next
```

- **构建产物**：`build/xmake-linux/musescore-mcp`。
- **打包规格**：`package_linux.py` 提取 ELF 依赖并设置相对 RUNPATH，排除了所有 Qt 依赖。生成的打包目录包含 172 个文件，体积约为 103 MB。
- **洁净运行验证**：在完全脱离开发环境 PATH 的纯净隔离环境下，运行打包二进制完整通过官方 SDK `mcp_smoke`（20 次创建/修改/导出循环及真实可听音频输出）。全部 25 条测试道在 Linux 宿主上 100% 通过（exit=0）。

Linux 使用显式 `gcc@` / `g++@` 工具类型识别 Pixi 的编译器，确保 xmake 正确继承 `-std=c++23`、`-O3` 与 `-fPIC` 参数。FluidSynth C 目标使用 GNU17 以保留上游 `alloca` 扩展；Windows 的该 C 目标仍为 C11。

---

## 8. 当前功能边界与已知局限性

| 维度 | 现状说明 |
| :--- | :--- |
| **乐谱渲染算法** | 完整保留原生排版、分行、跨小节与记谱逻辑。 |
| **图像元素** | 暂不支持嵌入式光栅图及外部图片对象（images unsupported）。 |
| **排版与文字** | 尚未实现完整的全字集回退（font fallback）；不支持从右到左（RTL）书写文字。 |
| **绘制模型** | 仅支持定义明确的纯色画刷（solid brushes）与指定混合模式，渐变/复杂贴图绘制受限。 |
| **音频流与导出并发槽位** | 音频流与后台导出任务拥有独立的运行子进程预算（各 2 个运行 worker 槽位，共享 16 个后台作业总上限）；流式音频不再饿死导出任务。 |
