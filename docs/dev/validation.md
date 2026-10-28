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

1. **动态字体注册回退 (Dynamic Font Fallback)**：
   仅含 Medium/Bold 字重的字族（如 `Ubuntu-M.ttf`）在引擎无该字族时，regular 查询回退到已注册的直立 face；常规字重（regular-weight）字体动态注册与排版生效正常。
2. **KAR 歌词分词保留上游行为 (KAR Lyrics Parity)**：
   KAR 导入器完全保留上游 `importmidi_lyrics.cpp:findBestTrack` 的音节切分逻辑，测试中出现的音节重复现象与上游 behavior-identical，保留以确保与 webmscore 行为严格对齐。
3. **MusicXML 乐器回放通道同步 (Playback Channel Sync)**：
   当导入的 MusicXML 声部名称（part name）恰好与引擎内置的其他乐器模板名称重合时，现已通过 `native/noqt_runtime.cpp` 的回放通道同步机制，强制保留用户声明的 `midiProgram`，避免默认音色覆盖。

### 3. M6–M9 里程碑完成度状态

- **M6（结构化核心）**：类型化 `MusicCore` 接口、区域读取快照（`score_query_region`）、原子音高编辑（`score_set_pitches`，支持 OCC 自动变基与重试去重）、明确的资源生命周期与错误码系统均已实现并执行验证。
- **M7（webmscore 功能覆盖）**：已实现全格式输入（MSCZ/MSCX/MXL/MusicXML/MIDI/KAR/GP3–GP7/GPX/PTB，支持 base64 内存数据导入）、14 种导出格式（含 OGG/FLAC/MP3）、分谱系统（`score_generate_excerpts`/`score_list_excerpts` 及 `excerptId` 导出）、小节与片段几何映射（涵盖多小节休止 MMRest）、动态字体与音色库装载、`titleFilenameSafe` 等价字段、`metadata.tracks` 导出生效混音、局部混音 patch。剩余差异见 `webmscore-parity.md` 第 5 节（有意排除 doLayout=false、stdio base64 约 6 MiB 传输上限、音频保真度约束）。
- **M8（音频能力）**：已实现分块流式合成（`score_audio_start`/`read`/`seek`，512 帧/块，精准秒级 seek 与取消）、多音轨混音（`score_audio_tracks`, `score_audio_get_mix`, `score_audio_set_mix`）、真实压缩音频编码（WAV/FLAC/OGG/MP3）。真实可听波形在两端执行验证。
- **M9（并发与后台任务）**：三层执行边界架构（Engine owner 线程、I/O 调度上下文、隔离工作子进程）、乐观并发编辑与冲突检测、不可变版本快照投影、后台文件导出任务（`score_export_start`/`get`/`cancel`）与流式音频进程已完整实现并通过 review 修正（commit `1992ec1`：临时目录隔离防泄漏、所有者线程安全 join、单文档毒化隔离、`ok:false` 会话保活、输出路径互斥）。后续最终 review 修正见 547d02f、bcce79（含 stdin EOF 带未完成请求时正常退出）。保留开放项：所有者线程耗时未基准测试（流式音频与导出已分离各 2 个独立 worker 槽位预算，voice-filter spanner 回归已覆盖）。

### 4. 最终状态（2026-10-27，bcce79）

- Windows（MSVC，xmake）与 Linux（Linux `linux-host`，GCC 15.3）在 `ebcce79` 均通过 20 条测试道（新增 `open_data`）。
- Linux 重新打包为 167 个文件 / 94 MB，打包产物在洁净环境中通过 `mcp_smoke`（20 次循环与可听音频）。
- 最终 review：Opus 5.5 与 Argon 独立审阅 `c5b5563..364544d`，确认的问题均已修正并回归（`547d02f`、`ebcce79`）。

## Roadmap step 0: golden baseline (2026-10-27/28)

Tools: `scripts/golden.py` (capture/compare/pin; allow-list `tests/golden/allow.json`)
and `scripts/test_roundtrip.py`; inputs in `fixtures/golden/` (provenance in its
README). Lanes `golden` and `roundtrip` added to both suite runners. Baseline tag:
local `pre-fork-baseline` on `f8e7d26` (not pushed).

Allow-list contract: an entry names fixture/output globs, the roadmap step, the
reason, the kind (`different`/`missing`/`extra`) and, except for `missing`, the
sha256 of the accepted actual output per platform. A different hash fails, so
an entry cannot absorb unrelated changes. `golden.py pin` prints skeletons for
reviewed differences.

- Executed, Windows (MSVC, engine code of `f8e7d26`): `tests/golden/windows`,
  15 fixtures, 186 outputs, 1.97 MB; two captures byte-identical; compare passes;
  a hand-edited golden fails and prints its hash; a pinned entry passes; a
  wrong pin fails; an unmatched entry is reported stale.
- Executed, Linux (GCC 15.3): `tests/golden/linux` captured twice,
  byte-identical. Outputs with float formatting (SVG/PNG/PDF) and WAV features
  differ from Windows, so goldens are per platform.
- Executed: full suite, 22/22 lanes exit 0 on both hosts (`step0c-win`,
  `step0c-linux`).
- Round trip, 16 cases (create+edit, 14 imports, 1 excerpt), identical digests on
  both hosts. MSCZ round trips are exact on the semantic model and, except for
  two pinned cases, on normalized MSCX text. Baseline losses (pinned by digest,
  evidence in the script): MusicXML has no grace-after notes (bend.gp,
  linked_parts, slurs); an invisible `fff` is not exported; layout_elements
  harmony moves to staff 1 and a hairpin text becomes `mf`; the linked_parts tie
  on the moved grace note is lost on a second import; the drums title gains
  `<size>24</size>`.
- Known host defect (executed, baseline): exporting an excerpt as MSCZ drops the
  excerpt's `<Style><spatium>`, instrument `<name>` and `<linkedTo>` links, so a
  reopened excerpt lays out with the default spatium.
- Source-reviewed: `pedal.mscx` carries a user placement/offset override that
  v4.7.5 drops on write; v4.7.6 (twrite Pedal fix) is expected to keep it.
- Not exercised: v4.7.6's corrupted-measure flagging (check.cpp) is not visible
  through any current output; no fixture triggers it.

## Roadmap step 1: MuseScore fork submodule (2026-10-28)

Pin: `vendor/musescore` = Locietta/MuseScore `mcp/v4.7.5`, tag `mcp-4.7.5-1`
(`e70fdef0`), ledger in fork.md. Commits `4303f71` (submodule, webmscore's trimmed
font metadata kept in-repo), `67bced6` (qrc-generated resources), `e14e09d`
(packaging fix).

- Executed, Windows: fork tree with the old staged resources reproduced all 159
  then-current goldens; after the submodule switch 186/186 identical; after the
  qrc resources (full font metadata, orders.xml, string tunings, chord files)
  still 186/186 identical. The roadmap expected layout changes from the full
  metadata; they do not occur for these fixtures. The trimmed files lacked only
  glyphBBoxes, glyphAdvanceWidths, ligatures, optionalGlyphs and sets.
- Executed, Linux: `4303f71` and `67bced6`, 22/22 lanes each, goldens identical.
- Executed: full suite 22/22 on both hosts after each commit. `async_session`
  failed once in four Windows runs at `score_audio_seek` with "Cannot open
  worker file" (worker_protocol.cpp read of a stream result file); not
  reproduced in three reruns or on Linux. That code is removed in step 4.
- Executed, packaging: Windows 147 files, 61 MB, mcp_smoke passes with
  PATH=System32 only (needed `e14e09d`: dumpbin reports `libmp3lame.DLL` in
  upper case). Linux 181 files, 94 MB, mcp_smoke passes under `env -i`.
- Executed, clean clone + bootstrap (git clone of the branch,
  `bootstrap.py --soundfont --harfbuzz`, prepare, build, mcp_smoke, golden
  compare): Linux passes. linux-host cannot reach raw.githubusercontent.com (TLS
  EOF at the time; re-run 2026-10-28 through the host's sing-box proxy:
  `bootstrap.py --harfbuzz` downloads it, executed), so `.upstream/harfbuzz` was seeded from the existing checkout; the
  submodule and SoundFont were fetched by the clean run. Windows passes too
  (HarfBuzz, submodule and SoundFont downloaded by bootstrap); xmake's aria2
  download of the Asio archive failed the TLS handshake with GitHub twice
  (curl reached the URL), so the project-local, SHA-pinned Asio cache was seeded.
- Compiled and executed: dropped webmscore deltas (bbox shim, PrintStackTrace,
  profiler/ret tweaks, brotli, renamed font/resource paths). Kept temporarily:
  the IApplication version suppression (without it MSCX/MusicXML writers
  dereference null; reproduced with a symbolized `releasedbg` build).
- Not shown by goldens: no compiled code reads orders.xml or
  string_tunings_presets.json (only the uncompiled notation module does), and
  the fixtures use no ligatures, optional glyphs or stylistic sets.
- Review fix (executed in noqt_support_test): `://` resource paths (fret.cpp's
  harmony_to_diagram.xml) now resolve like `:/`; before, they were rejected.

## Roadmap step 2: rebase to v4.7.6 (2026-10-28)

Pin `mcp-4.7.6-1` (`30387998`); range-diff identical (fork.md). Executed on Windows
and Linux: build, 22/22 lanes, round trip 16/16 with the same loss digests.
Golden differences, 23 per platform, all reviewed and pinned by hash in
`tests/golden/allow.json` (step "2 (v4.7.6)"):
- `pedal.mscx`: MSCX now writes the Pedal's user `placement`/`offset` (twrite fix,
  aef1934ee / PR #34917); the round trip keeps the override.
  Rendering was already correct; only the written file changes.
- `repeat70.mscx`: playback now continues into the open volta after the
  exhausted repeats (MIDI notes at ticks 5760/7680/9600; metadata duration 2 -> 4,
  rounded, actual 1.8 s -> 3.6 s; positions and audio follow) (repeatlist fix,
  upstream 6257400c2 + 97bc1a916; order 1;2;1;3;4;5 as in repeat_tests.cpp).
- `drums_import.musicxml`: one chord's stem `down` -> `up`, the same line upstream
  changed in testImportDrums_ref.mscx (MusicXML percussion import, e76569a8a).
- `perc_stem_dir.musicxml`: 8 stems flip; all 16 stem directions equal upstream's
  testInferredPercStemDir_ref.mscx.
Not visible through the host (source-reviewed): check.cpp's corrupted-measure flag
(setupMasterScore ignores the sanity result, isCorruptedUponLoading is never read,
and printing mode skips the overlay); Score::undoChangeVisible (edit.cpp, compiled,
but no host tool calls it); noteentry addPitch; sig.cpp tickValues callers;
chordarticulationsrenderer (only PlaybackModel, used from step 5).
Reachable but not yet covered (follow-up fixtures added after step 3): other
repeatlist volta cases (repeat71/72 and the full repeat_data set), ChordLine path
copy into generated parts, parenthesis/tab-grace crash fixes on pre-4.6 MSCX,
MusicXML named percussion noteheads, Pedal textSizeIsSpatiumDependent.
Linux evidence: suite logs on linux-host (notes/claude-tests/step2-linux, not in git).
## Roadmap step 3: version info and platform macros (2026-10-28)

Pin `mcp-4.7.6-2` (`e099f16b`, the TEMPORARY version suppression reverted).
Decision: `programRevision` is the 7-character SHA of the engine commit actually
built (the fork submodule), read by xmake from the submodule's own checkout (empty, with
a warning, when the source root is not a git checkout). xmake's release compile mode
leaves MUSE_APP_UNSTABLE undefined; this approximates upstream's release build
channel, and nothing compiled reads it today.
- Executed on Windows and Linux (`version_info` lane, new): MSCX
  `programVersion` 4.7.6 and `programRevision` e099f16; MusicXML `<software>MuseScore
  Studio 4.7.6</software>`; metadata `mscoreVersion` 4.7.6 after an MSCX export;
  platform metaTag "Microsoft Windows" / "Linux" (Q_OS_* now defined centrally in
  xmake.lua). fixtures/version/header.mscx (programVersion 4.5.0, `$v` header):
  unedited SVG shows 4.5.0; after one pitch edit it shows 4.7.6 and does not crash
  (before: `application()` was null). The `$v` check compares cairo glyph runs with
  reference runs, since cairo SVG has no text.
- `endCmd` lays out before the undo macro marks the score dirty, so the first edit
  left `$v`/`$r` stale; the edit guard now relayouts once on the clean -> dirty edge.
- Goldens: 47 differences per platform, all only `<programVersion>`/
  `<programRevision>` elements (values normalized), metadata `mscoreVersion`, and the
  MSCZ inner MSCX hash; pinned, then rebaselined. No SVG/PNG/PDF/MIDI/audio change.
- Executed: 23/23 lanes on both hosts.

Review fixes (Opus 5.5 review of step 3, verified):
- Exports are read-only for the document. Writer::writeScore stamps the writing
  program into every score it writes; MSCX/MSCZ exports now restore
  mscoreVersion/mscoreRevision/mscVersion of the master and excerpts, excerpt
  generation keeps the source values across its reload, and background workers
  receive them with the snapshot. Executed (version_info): a 4.5.0 fixture reports
  4.5.0 before and after an MSCZ export, from a background job and after excerpt
  generation; a created score keeps an empty version, as in MuseScore Studio until
  the file is saved. Golden metadata now shows the source file's mscoreVersion and
  fileVersion (before, the earlier MSCX export in the capture order had replaced them
  with 4.7.6/470 since the webmscore baseline).
- The clean -> dirty relayout runs only when a header/footer style prints `$v`, and
  also lays out excerpts that have pages. `$r` prints an empty revision while dirty
  upstream, so only `$v` was affected.
- The revision is taken only if the source root is itself the git top level.

Follow-up fixtures from the step 2 review (executed, goldens on both hosts):
repeat71/72, named percussion noteheads, a 4.50 linked tab staff with grace notes
and parentheses, 4.10 ghost notes, Pedal `textSizeIsSpatiumDependent` (now written,
v4.7.6), and chordline_path.mscx with generated parts. Observed: the generated part
excerpts do not carry the edited ChordLine path (no `<Path>` in their MSCX) even
though v4.7.6's ChordLine copy constructor copies it; the cause is not yet known.
New round-trip baseline losses: ghost_note (first save after the 4.10 read writes
one NoteParenGroup twice) and a 4/4 time signature added to linked tab staves after
MusicXML import. New lane `repeats`: all 72 upstream repeat_tests.cpp orders
match through measurePositions (executed, Windows).

## Roadmap step 4: trim streaming audio and stateful mix (2026-10-28)

Removed tools score_audio_start/read/seek and score_audio_get_mix/set_mix with the
worker stream protocol, the stream budget and the 120 s/150 s timers; per-document
mix state and its revision bumps are gone. Audio exports (score_export and
score_export_start, wav/flac/ogg/mp3) take a `mix` option that overrides, field by
field, the document's default mix: the MSCZ audiosettings.json mix (master
volumeDb/balance; tracks by partId+instrumentId: out.volumeDb/balance,
soloMuteState.mute/solo) when opened from such an archive, otherwise neutral.
forceMute is derived. score_audio_tracks and metadata.tracks report the defaults.
Deleted: offline_audio_probe, audio_test, dead Qt-era native/engine.cpp and
native/result.h. 16 tools remain.
- Executed, Windows: 23/23 lanes; goldens unchanged (no export output changed);
  audio_facade covers solo/mute silence, volumeDb gain (within 0.2 dB), balance by
  L/R energy, master volume and rejections; fixtures/audio/mix.mscz (Cello muted
  in audiosettings.json) exports a silent Cello by default and an audible one with
  an override; a background WAV with a mix is byte-identical to the synchronous one;
  exports never advance the revision.