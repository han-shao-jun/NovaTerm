# ConPTY 句柄泄漏排查（2026-10-19，Windows）

## 结论与范围

本机 Windows 10 Enterprise 22H2（19045.6456；KernelBase.dll
10.0.19041.5915）的 `CreatePseudoConsole` / `ClosePseudoConsole` 调用对，
每次生命周期遗留一个 **已退出 conhost.exe 的 Process 句柄**。进程已经正常
退出，留在 NovaTerm 宿主中的是引用该进程对象的句柄，不是仍运行的 Shell。

不依赖 Qt、NovaTerm、应用子进程或工作线程的独立程序同样复现；
`CreatePseudoConsole` 本身仍会启动系统 conhost，所以不能称为“完全无子进程”。
本次定位到系统 API 边界，未取得 Windows 内部调用栈，也未验证其他 Windows
版本或独立 conpty.dll 已修复。没有修改生产关闭逻辑或宣称泄漏已经消除。

## 独立对照

MSVC x64 `/O2 /W4` 编译，预热 3 次后测量 200 次。
`--inspect` 只在第一个计量周期读取本进程句柄快照；不创建工作线程，
不关闭 API 内部句柄，也不读取不透明 HPCON 的私有布局。

| 场景 | 基线 | 循环后 | 等待 5 秒后 | 增量 |
| --- | ---: | ---: | ---: | ---: |
| 原复现顺序：先 ClosePseudoConsole，再关闭宿主输入端 | 66 | 266 | 266 | +200 |
| 输入优先：先关闭宿主输入端，再 ClosePseudoConsole | 66 | 266 | 266 | +200 |
| 对照：仅创建/关闭两条管道，不调用 ConPTY | 61 | 61 | 未等待 | 0 |

同一个周期观察到：

```text
created handle=0x16C type=Process access=0x1fffff pid=4676
        exitKnown=1 exit=259 wait=258 image=C:\Windows\System32\conhost.exe
remaining handle=0x16C type=Process access=0x1fffff pid=4676
          exitKnown=1 exit=0 wait=0
```

`259` 是 `STILL_ACTIVE`，`258` 是 `WAIT_TIMEOUT`，关闭后的 `0` 分别是
正常退出码与 `WAIT_OBJECT_0`。句柄值和 PID 在关闭前后相同；此观察区间
没有另一个应用线程创建或回收句柄。等待后总数也不下降。

原始控制台日志已按[归档规则](../../tests/benchmarks/README.md)清理；上文保留独立复现的计数和句柄观察，后文保留命令。
独立程序在沙箱内成功运行；Qt 测试在沙箱内 DLL 初始化失败（0xc0000142），
以下项目测试是在沙箱外运行。没有连接用户服务器或串口。

## 项目测试与修正

初始二项定向复测仍是两处句柄失败，200 次循环句柄 `268 -> 465`。
总进程计数受其他资源退出影响，`+197` 不能解释成只有 197 次 ConPTY 泄漏。

旧用例在句柄宏断言失败后立即返回，后面的线程/子进程检查未执行。
因此旧注释“线程数与子进程数断言均通过”不能由这些失败日志推出。
本次把两项检查移到句柄检查之前，并增加实际资源计数输出。

这暴露了第二个问题：线程数 `8 -> 6` 也被原来的严格相等断言判为失败。
线程减少不构成泄漏，现改为有效计数且不高于基线；子进程仍要求严格等于
基线。**句柄断言不放宽、不按循环数扣除泄漏、不跳过。**
线程总数检查只是粗粒度护栏，不证明每一个应用线程都已回收。

最终重建 `novaterm_conpty_tests` 成功，两个资源用例定向复验 60.461 秒：
启动回滚句柄 `227 -> 264`，循环句柄 `286 -> 483`，仍是 **两个句柄断言失败**。
循环线程 `7 -> 6`、测试子进程 `0 -> 0`，两项检查已实际执行并通过。
QTest 总计 2 passed / 2 failed，其中 passed 是 init/cleanup，不代表资源用例通过。
未跑全套 CTest 或 ConPTY 的其他功能用例；生产代码未改。
独立工具 `/W4` 构建无警告，三组对照及非法参数退出码检查通过；
对照中的 ConPTY 两组按设计返回 1，表示仍检测到增长。

生产代码审查：管道子端在创建后关闭；宿主管道、Shell 进程、主线程、Job
经 RAII 或显式关闭；工作线程和专用 closer 线程经 join 回收；HPCON 只交给
`ClosePseudoConsole`。本轮没有发现能解释此固定增量的应用层漏关路径。
这不等于对所有失败路径提供完整无泄漏证明。

## 重现命令

在仓库根目录的 MSVC 开发者命令行：

```bat
mkdir build\conpty-investigation
cl /nologo /W4 /O2 /utf-8 tests\transport\conpty_handle_leak_repro.c /Fobuild\conpty-investigation\repro.obj /Febuild\conpty-investigation\repro.exe
build\conpty-investigation\repro.exe --inspect --settle-ms 5000
build\conpty-investigation\repro.exe --input-first --settle-ms 5000
build\conpty-investigation\repro.exe --pipes-only
cmake --build build/Release --target novaterm_conpty_tests
set "PATH=C:\Programs\Qt\6.8.3\msvc2022_64\bin;%PATH%"
build\Release\bin\novaterm_conpty_tests.exe injectedStartupStagesRollBack repeatedLifecycleReturnsResourcesToBaseline -o D:/qt/NovaTerm/build/conpty-investigation/resource-checks.txt,txt
```

复现工具默认 200 次，可用 `--iterations N`（1～10000）调整。
`--settle-ms N` 上限 60000。退出码：0 无净增长、1 有增长、2 参数错误、
3 API/诊断查询失败。NT 快照查询只用于此手工工具，不进入生产实现。

## 后续处理边界

增加等待或交换已测试的关闭顺序均未消除泄漏。不能在生产中靠扫描句柄表
强关“看起来像 conhost”的句柄：这些句柄不属于应用公开取得的资源，
存在并发复用和误关风险；也不能假定 HPCON 的私有字段布局。

可在升级 Windows 或评估微软独立 ConPTY 实现时用此程序做同机对照，
再跑完整 ConPTY/终端联通专项；本轮未做系统升级、引入新依赖或进程隔离改造，
不声称这些方案已验证有效。宿主进程退出时操作系统会回收遗留句柄。
