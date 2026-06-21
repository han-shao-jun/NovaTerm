#pragma once
#include <QObject>
#include <memory>

class MainWindow;

// 进程级应用程序外观（单例）。持有 MainWindow 并将一次性启动逻辑
// 集中在 init() 中，使 main() 保持简洁。通过 instance() 访问。
class Application : public QObject
{
    Q_OBJECT
public:
    static Application& instance();

    // 一次性启动：初始化 ElaApplication，加载翻译，构建 MainWindow。
    // 在显示窗口之前调用，仅调用一次。
    void init();

    // 仅在 init() 之后有效。返回所持有的窗口引用。
    MainWindow& mainWindow() { return *_mainWindow; }

    // 在 QApplication 仍存活时主动销毁 MainWindow。
    // 必须在 main() 中 a.exec() 返回后、QApplication 析构前调用。
    //
    // 根因：Application 是函数内 static 单例（Meyers 单例），它通过
    // unique_ptr 持有 MainWindow，默认要到进程 atexit 阶段才析构 —— 那时
    // 栈上的 QApplication 早已析构。MainWindow 的子 widget 链在此时析构会
    // 触发 destroyed 信号并访问已失效的 Qt 全局状态（platform plugin /
    // eTheme 单例等），导致退出时 SIGSEGV。提前 reset 可让窗口在
    // QApplication 生命周期内安全销毁。
    void shutdown() { _mainWindow.reset(); }

private:
    Application() = default;
    std::unique_ptr<MainWindow> _mainWindow;
};
