# engrave-mcp 构建与开发者指南

本文档记录基于固定（pinned）上游代码的原生 C++ engrave-mcp 服务端构建流程、架构约束及验证规范。

---

## 1. 架构设计与约束

- **构建系统**：以 `xmake.lua` 与显式源码清单为**构建系统**，采用 **C++23** 标准（Windows 与 Linux Linux x86_64 均已验证）。引擎的 xmake 构建（引擎 targets、目录通配符与固定版本包 recipe）位于引擎 Fork 检出内（`xmake/engrave.lua`、`xmake/packages/`）；本仓库 `xmake.lua` 只保留宿主 targets，并通过 `includes()` 从 `vendor/musescore`（或 `--musescore_source` 指定的检出）引入引擎构建。旧版 CMake 根配置及历史脚本已移除，最后可执行的 pre-fork 状态留存于本地 Git 标签 `pre-fork-baseline`。构建支持 `release`、`debug` 以及带符号调试的 `releasedbg` 模式，并通过 `--musescore_source=DIR` 选项覆盖引擎源码目录（默认为 `vendor/musescore` 子模块；该检出必须带有 `xmake/engrave.lua`）。
- **模块集成**：包含 `engraving`、`global`、`draw`、`mpe`、`musicxml`、`midi` (包含导入/导出适配)、`guitarpro` 导入组件，以及 MuseScore 4 音频引擎（`muse_audio_engine`：FluidSynth 合成、混音、混响与 SoundTrackWriter）及其编码器 LAME（MP3）、libFLAC、Opus/libopusenc（Ogg）、fdk-aac（AAC），全部来自子模块并以 Qt-free 方式编译；音频仅在 `--worker` 子进程中渲染。libsndfile 已从构建依赖和 pixi 环境中移除；测试分析用的 Python `soundfile` wheel 自带其运行时副本，因此解码检查不依赖系统 libsndfile。捆绑的原生依赖包括 FreeType、HarfBuzz、FluidSynth、FLAC、Opus/libopusenc、LAME 与 fdk-aac（Brotli 依赖已移除）。
- **底层适配**：采用原生文件系统、进程内设置、FreeType/HarfBuzz 字体引擎和 Cairo 绘图后端；乐谱导入、排版和 MIDI 事件生成复用上游实现。设置不持久化。

---

## 2. 环境要求

### Windows 构建环境
- **版本控制与工具**：Git（用于管理工程与拉取 `vendor/musescore` 子模块）。全新克隆需运行 `git submodule update --init --depth 1` 或 `python scripts/bootstrap.py`。
- **编译器**：Visual Studio x64 MSVC 工具链（推荐 VS 2022 / 18.10+，MSVC 14.51+）；构建脚本自动初始化开发环境，单独打包需在 VS 开发人员终端执行。
- **包管理器**：Pixi。构建与验证在 `native` 环境（`-e native`，纯原生无 Qt 编译环境）下执行。
- **构建工具**：xmake ≥ 3.1.1（Windows 使用 3.1.2）。第三方依赖已改为固定版本的 xmake 包：引擎依赖的 recipe 在引擎检出的 `xmake/packages/` 内，宿主专用的 asio 在 `xmake/packages/asio.lua`。首次配置会下载并本地构建它们，不再需要 `7z` 解压 HarfBuzz。
  > [!TIP]
  > **下载器配置（机器级）**：在部分 Windows 宿主上，xmake 内置的 aria2 在下载第三方依赖包时可能遭遇 TLS 握手失败（`SSL/TLS handshake failure`）。若遇到包下载中断，可配置 xmake 全局改用 curl：
  > ```powershell
  > xmake g --downloader=curl
  > ```

### Linux 构建环境
- **验证宿主**：Linux x86_64 (`ssh <linux-host>`, kernel 7.3, Pixi GCC 15.3, C++23)。本项目不使用 WSL（早期 Arch WSL2 验证记录仅作历史保留）。
- **编译器**：Pixi GCC 15.3，采用 `-std=c++23`、`-O3`、`-fPIC`。
- **构建工具**：xmake ≥ 3.1.1。

---

## 3. 上游代码与资产准备

### 3.1 检出引擎子模块与下载音色库
引擎源码采用 Git 子模块 `vendor/musescore` 形式引入（锁定至 Fork 仓库 https://github.com/Locietta/engrave ，分支 `mcp/v4.7.6`，Tag `mcp-4.7.6-4`，Commit `6cd717f2`，提交台账参见 `docs/dev/fork.md`）。旧版 webmscore、`.upstream/webmscore`、`patches/*.patch` 以及 Brotli 依赖均已废弃移除。

全新克隆的项目可执行 `git submodule update --init --depth 1` 初始化子模块，或直接运行 `bootstrap.py`。`scripts/bootstrap.py` 会自动执行浅子模块更新（`git submodule update --init --depth 1 -- vendor/musescore`），并按需下载经哈希验证的基础 SF3 音色库；`scripts/prepare.py` 会校验子模块提交与工作区清洁，并从 Git blob 同步运行时资源，**子模块尚未初始化时它会直接拒绝运行**（见 `scripts/prepare.py` 的 `verify_submodule()`）。因此构建脚本必须 bootstrap 在 prepare 之前，`scripts/build-native.ps1` 已按此顺序执行：

```powershell
pixi install -e native --locked
python scripts/bootstrap.py --soundfont   # 先初始化子模块
python scripts/prepare.py                 # 再校验并分发资源
```

第三方**库**依赖由引擎检出内的本地 xmake recipe 固定（`vendor/musescore/xmake/packages/`，或 `--musescore_source` 指定检出的同名目录）；每个 recipe 都写明版本与归档 SHA-256：freetype 2.14.1（`174d9e53…`）、harfbuzz 12.3.0、flac 1.4.3、lame 3.100（`ddfe36ca…`）、opus 1.5.2、opusenc 0.2.1、fdk-aac 2.0.3、pugixml 1.15、utfcpp v4.1.1（`1ca68016…`）、picojson 111c9be 与 asio 1.36.0（asio 留在本仓库 `xmake/packages/asio.lua`；引擎单独构建时另加固定版本 zlib 1.3.1）。freetype/lame/utfcpp 的 recipe 是从 xmake-repo（修订 `e96d4103633e161d259aa307e02a4d0842207651`）同名 recipe 逐行复制并裁剪到单一版本，版本、归档 URL 与 SHA-256 均未改动，因此依赖解析不再取决于某台机器上的 xmake-repo 修订。

注意区分：只有上述库依赖由本仓库固定版本与校验和；xmake-repo 仍提供构建工具包（cmake、nasm、pkgconf）与仅用于开发测试道的 gtest，它们的 recipe 修订记录在每个已安装包的 `manifest.txt`（`repo.commit`）中，不进入发布产物（freetype 的可选 zlib/libpng/woff2 支持在 `add_requires()` 中全部关闭，因此不会额外引入 xmake-repo 的库依赖）。因此“所有输入都已固定校验和”的说法仅对这些库依赖成立，工具包不在此列。

### 3.2 引擎资产清单与运行时分发 (Runtime Asset Staging)
引擎在 Phase C 移除了历史 Qt 资源（`.qrc`），改由引擎自身维护权威资产清单 `vendor/musescore/engrave/assets.json`（version 1，包含字体、样式、乐器定义、许可协议与乐谱模板，共 345 个文件，合计 28,380,646 字节）与配套安装校验工具 `vendor/musescore/tools/install_assets.py`。

资产准备与分发：
1. **主检出（production）**：`scripts/prepare.py` 作为宿主统一的资产准备与校验入口。它先校验 `vendor/musescore` 子模块状态与提交，随后使用引擎安装器验证 `.runtime/assets`。若资产目录缺失或文件哈希不匹配，则在临时目录完整构建后原子替换安装，并写入 `.runtime/assets/assets.json`；若已就绪且哈希完全匹配则跳过写入（保持幂等，退出码 0）。宿主不再解析 QRC，亦不再依赖 `resources.json` 与 `.runtime/assets-manifest.json`。
2. **独立校验与自定义分发**：可直接调用引擎安装工具进行校验，或分发到构建目录下的独立暂存目录（在多 worktree 如 `engrave/a2` 或测试独立引擎时，**切勿直接修改或覆盖主检出的 `.runtime`**）：
   ```powershell
   # 校验已安装的资产目录是否与引擎清单一致
   python -B vendor/musescore/tools/install_assets.py --verify .runtime/assets

   # 安装到独立暂存目录
   python -B vendor/musescore/tools/install_assets.py build/scratch_assets
   ```
   随后通过命令行将暂存路径传给二进制文件运行：
   ```powershell
   build/noqt_probe.exe build/scratch_assets <input.musicxml> <output.musicxml>
   build/assets_test.exe build/scratch_assets artifacts/assets_test <test_font_path>
   ```

xmake 的 `muse_generated` 在构建目录写入引擎版本信息；宿主 v2 定义由独立的 `registry_dump` 生成到 `native/tools.json`，`registry_check` 检查生成结果和处理器。`native/legacy_tools.json` 只属于历史 v1 基线，不参与生产构建。引擎各模块源码通过 `xmake/engrave.lua` 中的目录通配符直接引入。

引擎适配层与其回归测试、探针位于 fork，由 `engrave` 及 `xmake/tests.lua` 构建。本仓库 `native/` 的生产代码和测试都只能包含 `engrave/...` 公共引擎头文件；`native_include_check` 在构建宿主目标前检查这一规则，lane runner 的源码预检也执行它。v2 store、registry、worker、job manager 与 transport 不再链接 v1 facade。zlib 与 Cairo 由包含方提供：本仓库取自 pixi `native` 环境；宿主 `xmake.lua` 接受 `--cairo_prefix=<prefix>`（`<prefix>/include/cairo`、`<prefix>/lib`，非 Windows 平台写入 rpath），linux-host 构建须传入该参数。

增量测试检查长 UTF-8 registry 描述、修改 schema 只重编译 `schema_defs.cpp`，以及恢复后的无变化构建不编译/链接。验收命令：

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

包装脚本默认同时构建 `musescore_mcp`、`musescore_mcp_test`、`registry_dump`、
`registry_check`、`host_api_test` 和 `export_job_test`；可用 `-Targets` 指定子集。
S2 的独立验收（必须在原生 DLL 目录已加入 PATH 的环境中运行）：

```powershell
.pixi/envs/native/python.exe scripts/test_host_api.py --binary build/xmake-windows/host_api_test.exe --resources .runtime/assets --soundfont .runtime/soundfonts/FluidR3Mono_GM.sf3 --replacement-soundfont tests/engine_audio/sine.sf2 --output notes/a5/s2
```

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
pixi run --locked -e native python scripts/bootstrap.py --soundfont
pixi run --locked -e native python scripts/prepare.py
bash scripts/build-native.sh 8 build/xmake-linux "$HOME/OSS/musescore-mcp-a4s1/.pixi/envs/native"
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
