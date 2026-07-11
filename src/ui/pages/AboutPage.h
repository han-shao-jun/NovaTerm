#pragma once
#include "ElaScrollPage.h"

// 全页"关于"视图（ElaScrollPage）。注意：应用中关于页面实际以模态对话框形式
// 从 MainWindow::showAboutDialog() 弹出；本页面保留作为备选可嵌入视图，
// 未注册到导航中。
class AboutPage : public ElaScrollPage
{
    Q_OBJECT
public:
    explicit AboutPage(QWidget* parent = nullptr);
private:
    void retranslateUi();
    ElaText* _titleText{nullptr};
    ElaText* _versionText{nullptr};
    ElaText* _descText{nullptr};
    ElaText* _licenseText{nullptr};
    QWidget* _centralWidget{nullptr};
};
