#pragma once

#include "transport/SshTransport.h"

#include <QHash>
#include <QEvent>
#include <QStackedWidget>
#include <functional>
#include <utility>

/** 每个 SSH transport 保留独立面板及后台任务，标签切换只更换可见页面。 */
template<class PanelType>
class SshPanelStack final : public QStackedWidget
{
public:
    using Setup = std::function<void(PanelType*)>;

    explicit SshPanelStack(QWidget* parent = nullptr, Setup setup = {})
        : QStackedWidget(parent), _setup(std::move(setup))
    {
        setFrameShape(QFrame::NoFrame);
        _placeholder = createPanel();
        connect(this, &QStackedWidget::currentChanged, this, [this] {
            setWindowTitle(currentPanel()->windowTitle());
            setMaximumHeight(currentPanel()->maximumHeight());
        });
        setWindowTitle(_placeholder->windowTitle());
    }

    [[nodiscard]] PanelType* currentPanel() const
    {
        return static_cast<PanelType*>(currentWidget());
    }
    [[nodiscard]] PanelType* panelFor(SshTransport* transport) const { return _panels.value(transport); }

    void setSessionContext(const QString& label, SshTransport* transport)
    {
        PanelType* panel = _placeholder;
        if (transport) {
            panel = _panels.value(transport);
            if (!panel) {
                panel = createPanel();
                _panels.insert(transport, panel);
                // 面板在 transport 销毁前解除上下文；不让延迟删除持有旧连接。
                connect(transport, &QObject::destroyed, this, [this, transport, panel] {
                    _panels.remove(transport);
                    panel->setSessionContext({}, nullptr);
                    if (currentWidget() == panel)
                        setCurrentWidget(_placeholder);
                    removeWidget(panel);
                    panel->deleteLater();
                });
            }
        }
        panel->setSessionContext(label, transport);
        setCurrentWidget(panel);
        setWindowTitle(panel->windowTitle());
    }

protected:
    bool eventFilter(QObject* watched, QEvent* event) override
    {
        if (watched == currentWidget() && event->type() == QEvent::LayoutRequest)
            setMaximumHeight(currentPanel()->maximumHeight());
        return QStackedWidget::eventFilter(watched, event);
    }

private:
    PanelType* createPanel()
    {
        auto* panel = new PanelType(this);
        if (_setup)
            _setup(panel);
        addWidget(panel);
        panel->installEventFilter(this);
        connect(panel, &QWidget::windowTitleChanged, this, [this, panel](const QString& title) {
            if (currentWidget() == panel)
                setWindowTitle(title);
        });
        return panel;
    }

    Setup _setup;
    PanelType* _placeholder{nullptr};
    QHash<SshTransport*, PanelType*> _panels;
};
