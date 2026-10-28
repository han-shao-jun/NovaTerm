# Linux CPack 打包修复（2026-10-29）

## 范围与根因

基于0.2.100修复当前Linux Release构建目录的默认CPack流程，实施阶段版本0.2.100；用户随后要求创建提交并推送，CPack主题提交递增至0.2.101。

`cpack -C Release` 首先生成DEB，在 `dpkg-shlibdeps` 阶段失败：Qt部署收集了
全部SQL驱动，未使用的MySQL、PostgreSQL、ODBC驱动分别要求缺失的
`libmysqlclient.so.21`、`libpq.so.5`、`libodbc.so.2`；Mimer驱动另有缺失库警告。
历史索引只使用QSQLITE，故通过 `qt_import_plugins(... INCLUDE_BY_TYPE sqldrivers
Qt6::QSQLiteDriverPlugin)` 选择SQLite，保留自动Depends生成和依赖错误检查。
不安装无关数据库客户端，不修改生成的CPack配置或第三方源码。

包内容复核还发现诊断helper安装在公共bin，但SessionDirectory从应用同目录
定位helper。独立运行旧包helper会加载系统Qt并报 `Qt_6.8 not found`。
Linux私有目录变量提前至工具子目录之前定义，helper安装到主程序同目录，
INSTALL_RPATH使用 `$ORIGIN/../lib`；Windows/macOS安装分支保持原状。

## 验证

- `cmake --build build/Release --target all -j 6` 成功。
- `cd build/Release && cpack -C Release` 成功，DEB及ZIP分别生成。
- 相关CTest：`novaterm_history_recorder_tests`、`novaterm_history_store_tests`、
  `novaterm_history_service_tests`，3/3通过，总11.97秒。
- 两个包各解压到独立临时目录，不安装到系统；确认SQL插件目录仅含
  `libqsqlite.so`，helper与主程序同目录，公共bin不再含helper。
- 清除LD_LIBRARY_PATH和Qt插件路径环境覆盖后，随包helper的
  `system.uptime`成功；在包内临时编译的Qt SQL探针使用随包库和qt.conf，
  QSQLITE内存库建表、插入、读取值42均成功。
- 两个包分别以隔离XDG配置通过公共启动器在本机X11启动，4秒内进程持续运行，
  随后仅终止本次启动的进程；未见空连接、缺库或插件加载错误。
- `git diff --check`通过。未运行无关全套、其他平台打包、目标机安装或设备测试；
  本机短暂启动不代表跨发行版兼容验收。

临时探针首次使用开发库链接名 `-lQt6Sql/-lQt6Core` 链接失败；发行包仅带
版本化运行库，改用 `-l:libQt6Sql.so.6/-l:libQt6Core.so.6` 后两个包均通过。
此处是验证程序修正，未为打包结果添加开发库链接。

产物：`build/Release/novaterm-0.2.100-linux-x64.deb`、同名`.zip`，均约36MiB。
原始失败 `/tmp/novaterm-cpack-red.log`；最终构建、打包、CTest及解包验证日志
分别为 `/tmp/novaterm-cpack-final-build.log`、`/tmp/novaterm-cpack-final.log`、
`/tmp/novaterm-cpack-history-tests.log`、`/tmp/novaterm-cpack-package-verification.log`。
临时验证脚本为 `/tmp/novaterm-package-verify.py`。以上本机临时证据与构建产物不纳入提交。

## 0.2.101提交收口

按用户要求将CPack修复独立提交，根版本0.2.100→0.2.101。以上all、CPack、历史CTest3/3和两包运行验证均为实施阶段0.2.100的真实成绩。本次没有为该中间提交重建隔离工作树，也不把同时含MCP清理的工作区结果记成此提交重测；完整后续版本的重新验证另记于MCP清理记录。不纳入产物、日志或本机依赖。
