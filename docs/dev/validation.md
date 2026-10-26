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
上游是移除遮罩，并非始终裁剪 background。Argon 首次超时无有效报告，已重新发起。
