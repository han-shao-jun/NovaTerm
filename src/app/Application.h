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

private:
    Application() = default;
    std::unique_ptr<MainWindow> _mainWindow;
};
