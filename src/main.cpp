#include "app/Application.h"
#include "app/MainWindow.h"
#include <QApplication>

int main(int argc, char *argv[])
{
    // 高 DPI 处理。Qt 6 默认启用高 DPI 缩放，以下属性仅在 Qt 5 上需要。
    // PassThrough 保留分数缩放比例（如 150%）而非取整，确保渲染清晰。
#if (QT_VERSION < QT_VERSION_CHECK(6, 0, 0))
    QGuiApplication::setAttribute(Qt::AA_UseHighDpiPixmaps);
#if (QT_VERSION >= QT_VERSION_CHECK(5, 14, 0))
    QGuiApplication::setHighDpiScaleFactorRoundingPolicy(
        Qt::HighDpiScaleFactorRoundingPolicy::PassThrough);
#endif
#endif
    QApplication a(argc, argv);
    // 应用程序标识，用于 QSettings 存储路径和任务栏入口名称。
    a.setApplicationName("NovaTerm");
    a.setApplicationVersion("0.1.0");
    a.setOrganizationName("NovaTerm");

    // init() 必须在 show() 之前调用：启动 ElaApplication，安装翻译器，
    // 并构建主窗口（参见 Application::init）。
    Application::instance().init();
    Application::instance().mainWindow().show();
    return a.exec();
}
