# P2 第二轮审查修复证据

2026-10-22 Windows Release / Qt 6.8.3。说明、约束与修复映射见 [实施记录](../../../../../docs/architecture/stages/P8_AI_MCP_Interface/P2_Implementation_and_Verification.md)。

| 文件 | 含义 |
| --- | --- |
| `store-red.txt` / `service-red.txt` | 最初的索引连续性、读失败、查询计划、保留、配额、退出、空闲扫描复现 |
| `binding-red.txt` / `ui-red.txt` | 身份复用、代际及配置加载竞态复现 |
| `regression-initial.txt` | 初次广泛回归 41/42；不是全绿记录 |
| `storage-repeat-initial.txt` | 修复目录清理后，进一步捕获索引替换失败的重复运行记录 |
| `delete-sharing-red.txt` / `index-sharing-red.txt` | 真实 Windows 临时读句柄拒绝删除共享的两个确定性复现 |
| `storage-stability.txt` | 最终存储目标 20 轮连续通过 |
| `final-five.txt` | 最终相关 5 项 CTest 通过 |
| `store.txt` / `service.txt` / `ui.txt` | 最终 QTest 明细：33/33、13/13、8/8 |
| `binding-final.txt` | 身份、实际背压和代际专项 |
| `off.json` / `on.json` | 中期单次合成录制复验；64 MiB 零丢失，不代表真实性能目标通过 |

所有夹具使用临时私有目录、假连接或假字节源。未启动主程序或真实设备；ConPTY 已知失败不在本轮通过项中。

`partial-edit-red.txt` 另保存未形成有效数值时被后台刷新覆盖的复现；最终 UI 输出包含其修复结果。
