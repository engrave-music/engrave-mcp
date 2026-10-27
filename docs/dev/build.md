# MuseScore Native MCP 构建与开发者指南

本文档记录基于固定（pinned）上游代码的原生 C++ MuseScore MCP 服务端构建流程、架构约束及验证规范。

---

## 1. 架构设计与约束

- **构建系统**：以 `xmake.lua` 与显式源码清单 `xmake/manifest.lua` 为**主要构建系统 (primary build)**，采用 **C++23** 标准（Windows 与 Linux Linux / Arch WSL2 x86_64 均已验证）。根目录 `CMakeLists.txt` 是冻结的 Qt 时代 / C++20 历史基线，不参与当前构建，也不构建 M6–M9 的原生宿主。
- **模块集成**：包含 `engraving`、`global`、`draw`、`mpe`、`musicxml`、`midi` (包含导入/导出适配)、`guitarpro` 导入组件、`FluidSynth` 以及 `dr_mp3`/`libvorbis`/`libogg`/`flac` 音频编码器。
- **底层适配**：采用原生文件系统、进程内设置、FreeType/HarfBuzz 字体引擎和 Cairo 绘图后端；乐谱导入、排版和 MIDI 事件生成复用上游实现。设置不持久化。

---

## 2. 环境要求

### Windows 构建环境
- **编译器**：Visual Studio x64 MSVC 工具链（推荐 VS 2022 / 18.10+，MSVC 14.51+）；构建脚本自动初始化开发环境，单独打包需在 VS 开发人员终端执行。
- **包管理器**：Pixi。
  - `native` 环境（`-e native`）：剔除了所有 Qt 依赖的纯原生编译环境。
  - 默认环境（`default`）：保留供历史 Qt 基线使用。
- **构建工具**：xmake ≥ 3.1.1（Windows 使用 3.1.2）；首次下载 HarfBuzz 需要 PATH 中的 `7z` 或 `7zz`。

### Linux 构建环境
- **验证宿主**：Linux x86_64 (`ssh <linux-host>`, kernel 7.3, Pixi GCC 15.3, C++23) 及 Arch WSL2 x86_64。
- **编译器**：Pixi GCC 15.3，采用 `-std=c++23`、`-O3`、`-fPIC`。
- **构建工具**：xmake ≥ 3.1.1。

---

## 3. 上游代码与资产准备

### 3.1 检出上游与下载音色库
运行 `bootstrap.py` 拉取固定的上游仓库、初始化 Brotli 子模块，并按需下载经哈希验证的 SF3 基础音色库：

```powershell
pixi install -e native --locked
pixi run -e native python scripts/bootstrap.py --soundfont --harfbuzz
```

### 3.2 补丁应用与资源准备
`scripts/prepare.py` 会执行以下幂等操作：
1. 校验上游修订版本（`upstream.lock.json` 与 Brotli 子模块）一致性。
2. 依次幂等校验并应用以下 5 个经审阅的补丁集：
   - `patches/native-platform.patch`
   - `patches/native-imports.patch`
   - `patches/native-edit.patch`
   - `patches/native-archive.patch`
   - `patches/native-fonts.patch`
3. 使用临时 Git index 严格验证上游源码树完全等于固定提交加上述补丁集，严禁存在任何未跟踪或未授权修改。
4. 根据 `resources.json` 将资源别名复制到 `.runtime/assets`。

xmake 的生成目标在各自构建目录写入框架配置与工具定义；增量构建重新检查 `native/tools.json`，内容未变时保留时间戳。源码清单已提交，常规构建不运行 `capture_xmake_manifest.py`，也不读取 CMake 构建目录。

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
- **路径自解析**：打包后的可执行文件已具备就近查找同级资产的能力，运行时无需额外传递 `--resources` 绝对路径。

### 5.2 独立运行验证标准
打包产物已完成严格的洁净环境验证：
- 运行时环境变量 `PATH` 仅保留 `C:\Windows\System32`。
- 完全脱离桌面版 MuseScore 安装、Node.js、WASM、Python 及 Pixi 运行环境。
- 当前 xmake 产物通过全部 19 条测试道回归。

---

## 6. 历史 Qt 基线 (Legacy Baseline)

根目录 `CMakeLists.txt` 是一个冻结的 Qt 时代 / C++20 历史基线。该文件**不构建**当前的 M6–M9 原生 C++23 宿主，仅作为早期迁移对照参考保留。

- 历史 Qt-free CMake 脚本保留在 `scripts/build-cmake-native.ps1` 和 `scripts/build-cmake-native.sh`。
- 仓库内保留的 `scripts/windows.ps1` 与 `prepare_native.py` 属于历史遗留脚本。
- 其作用严格限于维护可执行的早期原型基线用于行为比对，严禁作为生产发布构建入口。

---

## 7. Linux 原生构建与 Linux 验证

Linux 构建与验证现已常态化运行于 Linux x86_64 宿主机器（`ssh <linux-host>`，路径 `~/OSS/musescore-mcp`），同时也保留了早期的 Arch WSL2 x86_64 验证记录。宿主运行环境为 Linux kernel 7.3、Pixi GCC 15.3、C++23。

### 7.1 构建与打包命令

```bash
pixi install -e native --locked
pixi run --locked -e native python scripts/bootstrap.py --soundfont --harfbuzz
pixi run --locked -e native python scripts/prepare.py
bash scripts/build-native.sh 8
pixi run --locked -e native python scripts/package_linux.py --binary build/xmake-linux/musescore-mcp --output artifacts/linux-next
```

- **构建产物**：`build/xmake-linux/musescore-mcp`。
- **打包规格**：`package_linux.py` 提取 ELF 依赖并设置相对 RUNPATH，排除了所有 Qt 依赖。生成的打包目录包含 167 个文件，体积约为 94 MB。
- **洁净运行验证**：在完全脱离开发环境 PATH 的纯净隔离环境下，运行打包二进制完整通过官方 SDK `mcp_smoke`（20 次创建/修改/导出循环及真实可听音频输出）。全部 19 条测试道在 Linux 宿主上 100% 通过（exit=0）。

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
