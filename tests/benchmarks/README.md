# 基准测试目录

按用途分类；根目录只保留本导航。所有构建目标名称保持不变，日常测试按改动范围选择。

| 目录 | 内容 | 主要入口 / 目标 |
| --- | --- | --- |
| [core/](core/) | 终端内核吞吐、回滚与搜索性能 | `novaterm_core_benchmark`、`novaterm_scrollback_benchmark` |
| [renderer/](renderer/) | P3 CPU/GPU、P5 GPU、大输入渲染及参数测试 | `novaterm_renderer_p3_benchmark`、`novaterm_renderer_p3_gpu_benchmark`、`novaterm_renderer_p5_gpu_benchmark`、`novaterm_renderer_large_input_benchmark`、`novaterm_renderer_large_input_tests` |
| [history/](history/README.md) | 分为 `lz4/` 压缩与滚动基准、`mcp/` 历史模块录制基准 | `novaterm_history_benchmark`、`novaterm_history_recording_benchmark`、`novaterm_history_gui_benchmark` |
| [repro/](repro/) | 终端渲染残影与 ConPTY 残留问题复现 | `novaterm_residue_repro`、`novaterm_conpty_residue_repro` |
| [serial-stress/](serial-stress/README.md) | 真实串口压力、CPU 采样及分析 | 独立 CMake 工程与 `run.ps1` |
| [archive/](archive/README.md) | 按主题、日期保存历史报告、复现脚本、源码快照及本地输出 | `archive/lz4/`、`archive/mcp-history/` |

常规构建目标由 [tests/CMakeLists.txt](../CMakeLists.txt) 声明；历史压缩/录制基准另见
[tests/history/CMakeLists.txt](../history/CMakeLists.txt)。串口压力工具使用独立构建入口。

新测量结果写入 `build/` 下的新目录。测试输出 `.txt` 只保留本地，Git 不跟踪；
`CMakeLists.txt`、`requirements.txt`、历史 CMake 源码快照和 `cpu-topology.txt` 环境说明保留。
历史报告不能代替当前版本验收，归档脚本需要准备匹配版本及目录布局的重跑输入。
