# 已执行验证与审阅处理

本文件记录可复核结果；临时日志和 agent 完整输出留在忽略的 `notes/`。

## Windows / Qt 原生基线

`artifacts/native-006`：固定上游引擎导入三声部、每声部 16 小节的 fixture，
142 个音符/休止事件的起始位置、时值、音高、声部、谱表和延音线完全匹配。
29 个字体成功注册。XML/MIDI/SVG/PDF/metadata 导出成功；运行约 0.68 秒。
单独 EXE 为 44,472,320 字节，不代表包含依赖的部署体积。
早期 native-001 的 WOFF2 注册失败，native-004/005 亦失败，均不作为有效基线。

## MCP 与真实 LLM

`scripts/mcp_smoke.py` 使用独立官方 Python MCP SDK，验证协议 2025-11-25、
六工具发现、创建/查询/替换、非法编辑回滚、七格式导出、多文档切换和 20 次释放循环。
实际 XML 的修改后音高也单独检查，不能仅凭返回的原始 JSON 判定编辑成功。
WAV 为 44.1 kHz 双声道、约 7.799 秒，峰值约 0.182，非静音。

`scripts/test_transport.py` 覆盖错误 JSON、batch、非法 ID、初始化顺序、未知方法/工具、
错误参数、通知不执行工具，以及超过 8 MiB 的输入之后流仍可继续工作。
`scripts/stress_mcp.py` 分别在有/无 SoundFont 下执行 120 次创建/释放，另测深层 JSON、
Windows 原始 0x1A 输入、非法 XML 字符和 Unicode 路径。
有音色时 40→120 次的进程私有内存增长约 6.6 MiB，小于测试设定的 16 MiB；
内存存在波动，这不是没有泄漏的证明。之后的 Qt 回归出现 19,906,560 字节增长，
超过测试阈值；该次失败已记录，不能用较早通过覆盖。

DeepSeek Flash 通过 DSH 的真实 MCP client 完成 9 次工具调用：创建、查询、替换最后
两小节、再查询、四次导出、关闭。`artifacts/llm-composition/validation.json` 独立确认：

- 三声部各 8 小节，MusicXML 的全部音符/休止、和弦、时值和位置匹配最终指令。
- 85 个 MIDI note-on 的音高/起始时间，以及实际发声通道音色一致。
- WAV 为 44.1 kHz 双声道、23 秒（20 秒音乐加尾音），峰值约 0.216；未试听。
- PDF 仅检查格式头，尚未做视觉质量验收。

`artifacts/windows-001` 为 24 文件、120,530,913 字节的本地独立部署包，包含 Qt 和
SoundFont。子进程 PATH 仅含 Windows System32、工作目录不在源码目录时，上述 SDK
音乐/音频闭环通过。生产运行不依赖 Pixi/Python、Node/WASM 或 MuseScore 桌面安装。
最新生命周期修正之后需重新打包验收，旧包用于保存基线。

## 去 Qt 进展

`artifacts/noqt-001`：Qt-free 可执行程序直接使用 EngravingProject、MuseScore MusicXML
导入/导出、排版算法和 FreeType/HarfBuzz 字体实现。三声部各 16 小节排版为一页，
142 个音乐事件语义完全匹配。编译命令没有 Qt include/link；EXE 直接依赖仅系统 CRT
及 zlib。此时仅验证 XML 和排版入口，图形、MIDI、音频及生产 MCP 尚未全部迁移。

移植复用上游 `NO_QT_SUPPORT`、字符串/JSON/XML/IO/字体抽象。原生适配文件系统、
进程内设置存储和资源路径。设置不持久化，也不读取桌面用户偏好。

随后 `artifacts/noqt-mcp-003` 通过六工具、九格式导出、MSCZ/MXL 重开语义一致及
20 次创建/关闭。WAV 44.1 kHz 双声道，约 7.558 秒，峰值约 0.048。
音频沿用 MuseScore MIDI 事件生成和上游 FluidSynth，以样本时钟离线合成；
音色、时间、非静音接受检验，音频波形不要求与旧 PlaybackController 完全相同。

`artifacts/windows-noqt-001` 共 51,785,512 字节，包含资源及音色；DLL 闭包检查排除 Qt，
纯 System32 PATH 部署回归通过。PNG/PDF 均为正常 A4 尺寸，PDF 文字可提取；
SVG 115 个 path、75 个 use，直接 PNG 和独立 PDF 栅格化墨迹比例接近。
相对旧 Qt PNG 的墨迹比例差约 1.4%，归一化内容边界差小于 0.1%。
这些是结构/数值验证，不是人工视觉或音乐品质评价。

当前 Qt-free 后端的明确限制：嵌入图片导入暂不支持；文字只使用打包字体，缺字显示
缺字形，无系统字体回退和完整 RTL 双向段落布局。Cairo 支持实色绘图及 PNG 图像，
不支持所有 Qt 画刷/合成模式；旋转遮罩使用包围框近似。典型五线谱创作场景已验证。
离线 WAV 设有 10 分钟总时长限制，超过时返回错误，不静默截断。

## Argon / Opus 5.5 审阅

M0、M1 均已进行独立源码审阅，不能视为审阅模型实际运行测试。
经主代理复核已修正：argc 引用生命周期、UTF-8 manifest/路径、结果缓冲和指针宽度、
文件写入/空结果检查、JSON 深度限制、非法 UTF-8 错误序列化、Windows 二进制 stdio、
MusicXML 音高最小值/非法字符、结构化结果对象约束、文档所有权异常路径及关闭非活动文档。

未采纳的判断也经过核对：`_load` 的布尔参数表示排版，不是音频开关；
不能为了避免音频初始化而把它直接改成 `has_soundfont`。上游引擎注册音频服务与启动
播放控制器是不同步骤，实际采用后者按音色启用。无声部导致下溢的报告被完整输入验证挡住。
运行时模块采用单 Engine/进程生命周期；尚不支持在同一进程反复销毁并重建整个 Engine。

Qt-free Opus 审阅已修正独立合成粗体/斜体 FT_Face、Cairo clip 分配异常清理，以及
PNG 单边尺寸溢出。其“缺少 DPI 缩放”判断不适用当前上游：paintScore 已设置不同的
viewport/window；重复缩放会造成错误。空遮罩语义也核对了实际 QPainterProvider，
上游是移除遮罩，并非始终裁剪 background。Argon 首次超时，重试已获得报告并修正
初始化前路径检查、MXL 解压后根文件限额、XML 扫描错误检查和导出参数检查顺序。
其空 parts/bars 下溢报告仍被 composition_xml 验证阻断；JSON 缺键异常由 MCP 统一
转为工具错误，属于已处理路径。render_score 本来已有 PDF 零页检查，入口也补充同样检查。

Qt-free 的加强压力测试每十次实际导出 PDF 或 WAV，再关闭文档。每种模式均 120 次，
40→120 次私有内存增长分别 77,824 字节和 2,211,840 字节；不会把“仅传入音色路径”
误当作实际反复合成的测试。

## 独立 CMake 构建

根 CMakeLists.txt 在完全不含 Qt 的 Pixi native 环境完成 Windows 构建。
`artifacts/native-cmake-002` 通过六工具、九格式、20 次循环及独立 PDF 栅格化检验；
文件系统/设置检查和四类畸形输入拒绝后恢复均通过。
`artifacts/windows-native-001` 为 117 文件、51,610,288 字节，DLL 闭包排除 Qt，
只含 System32 PATH 的 SDK 全流程及原始传输测试通过。

Argon、Opus 5.5 均进行了独立源码审阅。已修正 schema 的构建依赖和构建目录隔离、
强制 NDEBUG、Pixi 锁文件约束、完整源码树校验，以及首次 bootstrap 入口。
prepare.py 用临时 Git index 校验整个源码树等于固定提交加补丁，不改用户真实 index。
bootstrap.py 的已存在源码/音色幂等路径已运行；干净网络 clone 路径尚未单独验收。

审阅中的条件性风险经核对：VorbisDecoder 使用内置 stb_vorbis，不依赖系统 Ogg/Vorbis；
DRAW_NO_INTERNAL 使 draw 跳过字体库初始化，因此根目录初始化没有重复。
QTTEXTDRAW 仅影响移入 noqt_support 的字体内部类，该宏在这些编译单元中一致。
Cairo 的原始 library 查找配合独立 dumpbin 递归打包已实际通过洁净运行，
并非依赖 TARGET_RUNTIME_DLLS。Linux 静态库顺序和工具链的实测结果见下文。

## Linux 与最终部署验收

Arch WSL2 x86_64 使用锁定的 GCC 15.3、glibc 2.28 sysroot 完成原生编译，运行于
glibc 2.44。只需补齐上游 logger 的 `<memory>` 和 Unix 栈日志的流操作；GNU 静态库
顺序实际链接通过，无需调整音乐算法。HarfBuzz 下载失败会给出明确错误并移除空文件。

Linux 官方 SDK 六工具、九格式、20 次循环，畸形 XML/MXL 拒绝后恢复，原始传输及
文件系统/设置测试全部通过。PNG 尺寸、墨迹比例和内容边界与 Windows 数值一致；
PDF 文字可提取，独立栅格化通过。相同短曲 WAV 在两端均为 44.1 kHz 双声道、
7.558095 秒，峰值 0.047760；仍未做听觉或人工视觉质量评价。

首次 Linux 音频压力测试私有驻留内存在 111–172 MB 间波动，40→120 次增长约 19 MB，
未通过原阈值。FluidSynth 对象销毁后调用 glibc `malloc_trim(0)` 释放空闲页，保持原阈值：
两种模式各 120 次、每十次导出 PDF/WAV，增长分别为 0 和 -524,288 字节。
额外通过测试期 LD_PRELOAD 观测 mallinfo2：40→120 次实际存活分配仅增长 227,104 字节，
trim 前后存活分配相同。由此支持“大幅 RSS 波动来自空闲堆页保留”的解释；
这仍是有限次数测试，不是零泄漏证明。非 glibc 平台不启用此回收策略。

Argon 和 Opus 5.5 独立审阅了 Linux 打包与原子导出，Opus 另审阅回收策略。
已修正：系统库意外混入、glibc 分库分类、ldd 符号/版本错误漏检、真实可执行路径解析、
临时文件名长度、O_CLOEXEC、零字节写入错误和 fsync 被 EINTR 中断的处理。
POSIX 导出保证同目录 rename 的原子可见性；未承诺崩溃后的目录项持久性或保留旧文件权限。

最终 Windows 包 `artifacts/windows-native-002` 为 117 文件、51,613,872 字节；
最终 Linux 包为 152 文件、90,692,740 字节，压缩归档
`artifacts/linux-native-001.tar.gz` 为 40,166,751 字节。两者都通过不带开发环境 PATH
的完整 SDK 回归。Linux 符号链接启动通过；另外临时移走开发副本的 `.pixi`、`.runtime`
和 `.upstream`，开启 LD_BIND_NOW，使用系统 Python 的原始 JSON-RPC 客户端再次完成
九格式导出、XML/MIDI/PDF 和非静音 WAV 检查，证明所测流程不依赖这些目录。
库中的构建路径字符串（诊断信息、fontconfig/X11 默认值）记录在 manifest，不据此声称
不存在任何未测试功能的外部资源依赖。没有验证其他发行版或 glibc 版本。

证据副本在 `artifacts/linux-validation/`；构建、审阅及网络绕行日志在忽略的 notes/。
最终 Windows SDK 记录为 `artifacts/windows-final-validation/`。

Qt-free 真实 LLM 会话见 `artifacts/llm-native/`：DeepSeek Flash 的 11 次 MCP 调用完成
三声部八小节创建、三个末小节替换、查询、四格式导出及关闭，所有调用成功。
独立校验全部 XML 音符/休止/和弦/时值/位置，103 个 MIDI note-on 和实际 GM 音色；
WAV 为 44.1 kHz 双声道、22.755556 秒，峰值约 0.058533。该会话 PDF 仅检查格式头。

## Export options increment (Windows)

The first xmake / MSVC C++23 binary (`artifacts/xmake-windows-first`) passed the
official SDK smoke suite (20 cycles), the new `test_export_options.py` suite with
and without a SoundFont, and numerical rendering comparison against the previous
Windows baseline. This validates these runtime changes; the subsequently revised
xmake build graph still requires its own final build and cross-platform checks.

- MSCX export/reopen preserves the test score's 3 parts, 2 measures per part and
  22 note events.
- PNG resolution changes from 596×842 at 72 DPI to 1191×1684 at 144 DPI. Transparent
  output has zero-alpha pixels; opaque output has alpha 255 everywhere and a white
  page corner. SVG/PNG page-background toggles are checked against artifact content.
- The two-measure repeat fixture produces 4 versus 2 MIDI note-on events. WAV
  duration changes from 6.577052 to 10.576689 seconds with repeat expansion.
- RPN output changes from 35 relevant controller events to none when disabled,
  while musical note-on events remain identical.
- Nineteen malformed/out-of-format/out-of-range options preserve existing output
  sentinels and leave the session usable. WAV without a SoundFont fails cleanly.
- Direct PNG ink density/bounds, composited on white, equal the earlier baseline;
  independently rasterized PDF differs in ink fraction by less than 0.00005.

PNG/SVG now default to no page background; PNG defaults to transparent. Use
`transparent: false` for opaque PNG or `drawPageBackground: true` to paint the
page. This is an explicit default change from the earlier always-white renderer.
The PNG default resolution remains 144 DPI; webmscore's 360-DPI output can be
requested through `pngDpi: 360`. Accepted range: 36–1200, still subject to the
existing dimension/pixel-count limits.

Evidence: `artifacts/xmake-export-options-validated/`,
`artifacts/xmake-export-options-no-sf/`, and `artifacts/xmake-smoke-first/`.
These checks do not establish full webmscore parity or asynchronous editing.

## xmake / C++23 Windows milestone

The revised xmake graph built successfully with MSVC 14.51 (VS 18.10.3), xmake
3.1.2, `/std:c++23preview`, `/MD` and `/EHsc`. Compilation is driven by explicit
per-target source/define/include lists; the regular build never invokes CMake or
reads its build directory. HarfBuzz bootstrap is independently pinned and hashed.
`scripts/build-native.ps1` was executed successfully after the full build.

Argon and Opus 5.5 independently reviewed the migration (Opus via Claude Code).
Fixes cover Windows platform macros, missing Linux FreeType system source,
per-target includes with system dependency headers, deterministic generated-file
paths, pinned source checks, and explicit compiler/linker selection on Linux.
The core is a static target but still contains the existing MCP adapter; the
typed protocol-independent interface is a later milestone, not completed here.

`artifacts/xmake-windows-001` contains 117 files / 51,628,720 bytes. Its clean-PATH
SDK smoke run passed 20 cycles. The reviewed graph also passed malformed-input,
raw protocol and export-option suites; the final schema-generation-only change
passed the large UTF-8 schema probe and another packaged SDK smoke run.

`artifacts/xmake-incremental/evidence.json` confirms a schema larger than one
MSVC string literal round-trips through `tools/list`, exactly one translation
unit recompiles, the source is restored, and a subsequent no-op build neither
compiles nor links. The test performs real schema changes, not only timestamp
inspection. Linux xmake verification remains in progress at this checkpoint.

## xmake / C++23 Linux milestone

The explicit xmake graph and all native probe targets built on Arch WSL2 x86_64
with xmake 3.1.1 and Pixi GCC 15.3. Actual C++ invocations use `-std=c++23`, `-O3`
and `-fPIC`. FluidSynth retains the upstream GNU C extension needed for `alloca`
through GNU17; Windows C11 and the C++23 standard are unchanged.
`PIXI_NO_INSTALL=true bash scripts/build-native.sh 8` passed after the full build.

`artifacts/xmake-linux-001` in the Linux validation checkout contains 152 files /
87,458,652 bytes. Package dependency closure and `ldd -r` checks passed, with no Qt.
The packaged binary passed the official SDK suite (20 cycles), all export-option
checks including audible WAV, malformed-input/recovery tests, and raw JSON-RPC
checks (zero reported failures). Direct PNG ink/bounds match the previous Linux
baseline; independent PDF rasterization remains within the rendering threshold.
The SDK WAV is 44.1 kHz stereo, 7.558095 seconds, peak approximately 0.047760.

The Linux large UTF-8 schema test also passed: one translation unit recompiles,
the schema round-trips through MCP, original bytes are restored, and the no-op
build does not compile/link. Evidence copies live in
`artifacts/xmake-linux-evidence/`; compiler/reviewer transcripts remain ignored.
This completes the two tested platforms for M5, not Linux-distribution coverage,
a typed core, webmscore parity, or asynchronous editing.

## Position mapping increment (Windows and Linux)

`measurePositions` and `segmentPositions` export the upstream position schema.
The adapted algorithm uses LayoutData bounds, explicit expanded repeat-list
access, and one cached list reference throughout event generation. Opus 5.5
reviewed this through Claude Code, including a follow-up on fixes; Argon reviewed
the original adaptation independently. Findings were checked against source and
tests. This is not a claim of complete playback-navigation coverage.

The SDK test `scripts/test_positions.py` passes on both native binaries and both
standalone packages with a clean child PATH. The two-measure fixture includes a
rest, 120→60 BPM tempo changes and a repeat: measure times are 0/2000/6000/8000 ms;
12 segment events have exact independently specified times. Widths in these
visible-note/rest fixtures are positive, segment positions fall inside their measures, and PNG dimensions
match the documented 1200-DPI layout conversion. The 60-measure two-part fixture
spans two pages and produces 240 correctly timed segment events. Invalid
format-specific options preserve an output sentinel; repeated queries are equal.

The C++ `score_positions_test` directly verifies both initial expand-repeats
settings stay unchanged, plus null-score rejection. The separate SDK MIDI byte
comparison proves unchanged MIDI output, not the internal flag invariant.
Evidence: `artifacts/positions-windows-acceptance/` and
`artifacts/positions-linux-acceptance/`, including the native probe logs.
Pinned upstream fixtures repeat06/07/08/14 also pass on both packages, matching
upstream's expected measure order for volta, D.C. al Fine, D.S. al Coda and a
combined roadmap. Segment geometry and monotonic event times are checked too.
Multi-measure rests still need dedicated coverage; hidden/gap-only segments may
legitimately have zero width. Segment x/y JSON integer types are tested explicitly.
Both formats reject three incompatible options each without changing sentinels.

Final position-enabled packages are `artifacts/positions-windows-001` and
`artifacts/positions-linux-001` (the latter in the Linux validation checkout).
The reviewed implementation additionally passed the general 20-cycle SDK
regression on both platforms before the final integer-type/defensive-check fixes;
the final packages passed the full position suite and direct flag-invariant probe.

## 2026-10-27: M6–M9 全面集成与跨平台验证 (Windows & Linux Linux)

代码基线在提交 `1992ec1`（包含 `c5b5563`、`cbff482`、`1992ec1`）完成了 M6（结构化核心）、M7（webmscore 功能覆盖）、M8（音频能力）与 M9（并发与后台任务）的集成实现与跨平台双端验证。

### 1. 执行证据 (Executed Evidence)

- **Windows 平台 (MSVC 14.51 / VS 18.10.3, xmake 3.1.2)**：
  通过全部 19 条测试道（19 test lanes pass at `1992ec1`）：
  `music_core`、`score_edit`、`score_audio`、`score_positions`、`noqt_support`、`audio`、`excerpts`、`assets`、`archives`（34 种畸形 ZIP 归档在 <30 ms 内被有界缓冲安全拒绝）、`native_edit`、`audio_facade`、`async_session`（包含临时目录无泄漏、单文档毒化隔离、输出路径预留冲突、`ok:false` 错误恢复等断言）、`imports_midi`、`imports_gp`、`tool_schema`、`positions`、`export_options`、`transport`、`mcp_smoke`。
- **Linux 平台 (Linux x86_64 host `linux-host`, kernel 7.3, Pixi GCC 15.3, C++23)**：
  在独立的 Linux 宿主环境 (`ssh <linux-host>`, `~/OSS/musescore-mcp`) 下，全部 19 条测试道同样完整通过。
- **Linux 打包与独立部署运行**：
  `package_linux.py` 生成了 167 个文件 / 94 MB 的独立分发包；在完全隔离的洁净环境（clean environment，移除开发工具链环境变量）下成功运行并完整通过官方 SDK `mcp_smoke`（20 次创建/修改/导出循环及可听音频生成）。

### 2. 实测已知局限与上游对齐特征 (Known Limitations & Parity Findings)

1. **动态字体注册缓存机制 (Dynamic Font Fallback)**：
   动态注册中等字重字体（`Ubuntu-M.ttf`，family "Ubuntu"）时，当前不会自动替换之前 regular 字重查询已缓存的 fallback 字体对象；常规字重（regular-weight）字体动态注册与排版生效正常。
2. **KAR 歌词分词保留上游行为 (KAR Lyrics Parity)**：
   KAR 导入器完全保留上游 `importmidi_lyrics.cpp:findBestTrack` 的音节切分逻辑，测试中出现的音节重复现象与上游 behavior-identical，保留以确保与 webmscore 行为严格对齐。
3. **MusicXML 乐器回放通道同步 (Playback Channel Sync)**：
   当导入的 MusicXML 声部名称（part name）恰好与引擎内置的其他乐器模板名称重合时，现已通过 `native/noqt_runtime.cpp` 的回放通道同步机制，强制保留用户声明的 `midiProgram`，避免默认音色覆盖。

### 3. M6–M9 里程碑完成度状态

- **M6（结构化核心）**：类型化 `MusicCore` 接口、区域读取快照（`score_query_region`）、原子音高编辑（`score_set_pitches`，支持 OCC 自动变基与重试去重）、明确的资源生命周期与错误码系统均已实现并执行验证。
- **M7（webmscore 功能覆盖）**：已实现全格式输入（MSCZ/MSCX/MXL/MusicXML/MIDI/KAR/GP3–GP7/GPX/PTB）、14 种导出格式（含 OGG/FLAC/MP3）、分谱系统（`score_generate_excerpts`/`score_list_excerpts` 及 `excerptId` 导出）、小节与片段几何映射、动态字体与音色库装载、`titleFilenameSafe` 等价字段。剩余缺口：`metadata["tracks"]` 仍为空数组（需由 `score_audio_tracks` 获取）、多小节休止位置测试未单独覆盖、中等字重 fallback 替换限制、全量混音参数与局部 patch 差异。
- **M8（音频能力）**：已实现分块流式合成（`score_audio_start`/`read`/`seek`，512 帧/块，精准秒级 seek 与取消）、多音轨混音（`score_audio_tracks`, `score_audio_get_mix`, `score_audio_set_mix`）、真实压缩音频编码（WAV/FLAC/OGG/MP3）。真实可听波形在两端执行验证。
- **M9（并发与后台任务）**：三层执行边界架构（Engine owner 线程、I/O 调度上下文、隔离工作子进程）、乐观并发编辑与冲突检测、不可变版本快照投影、后台文件导出任务（`score_export_start`/`get`/`cancel`）与流式音频进程已完整实现并通过 review 修正（commit `1992ec1`：临时目录隔离防泄漏、所有者线程安全 join、单文档毒化隔离、`ok:false` 会话保活、输出路径互斥）。保留开放项：音频流与导出共享 2 个运行 worker 槽位、stdin EOF 退出清理、缺少 voice-filter spanner 显式测试、所有者线程耗时未基准测试。
