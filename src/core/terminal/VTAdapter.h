#pragma once

#include "ScreenBuffer.h"
#include "ScrollbackBuffer.h"

#include <QByteArray>
#include <QByteArrayView>
#include <QString>

#include <functional>
#include <memory>

namespace NovaTerm {

class VTAdapter
{
public:
    struct Observer
    {
        std::function<void(QByteArrayView)> output;
        std::function<void(const DirtyRegion&)> damage;
        std::function<void(const CursorState&)> cursorChanged;
        std::function<void(const QString&)> titleChanged;
        std::function<void()> bell;
        std::function<void()> scrollbackChanged;
        std::function<void(int)> screenScrolled;
    };

    VTAdapter(int columns, int rows, ScreenBuffer& screen,
              ScrollbackBuffer& scrollback, Observer observer);
    ~VTAdapter();

    VTAdapter(const VTAdapter&) = delete;
    VTAdapter& operator=(const VTAdapter&) = delete;

    bool isValid() const;
    void writeInput(const QByteArray& data);
    void flushDamage();
    void resize(int columns, int rows);
    void setDefaultColors(const TerminalColor& foreground,
                          const TerminalColor& background);

    void keyboardUnichar(uint32_t codepoint, int modifiers);
    void keyboardKey(int key, int modifiers);
    void startPaste();
    void endPaste();
    void mouseButton(int button, bool pressed, int modifiers);
    void focusIn();
    void focusOut();

    CursorState cursor() const;
    QString title() const;

private:
    class Impl;
    std::unique_ptr<Impl> _impl;
};

} // namespace NovaTerm
