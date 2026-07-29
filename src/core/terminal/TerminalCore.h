#pragma once

#include "ScreenBuffer.h"
#include "ScrollbackBuffer.h"

#include <QByteArray>
#include <QObject>
#include <QString>

#include <memory>

class QKeyEvent;
class QMouseEvent;
class QWheelEvent;

namespace NovaTerm {
class VTAdapter;
}

// Qt-facing terminal facade. libvterm is isolated behind VTAdapter; consumers
// only observe NovaTerm-owned screen, cursor, color, and damage types.
class TerminalCore : public QObject
{
    Q_OBJECT

public:
    explicit TerminalCore(int cols, int rows, QObject* parent = nullptr);
    ~TerminalCore() override;

    void writeInput(const QByteArray& data);

    void processKeyPress(QKeyEvent* event);
    void processMousePress(QMouseEvent* event);
    void processMouseMove(QMouseEvent* event);
    void processMouseRelease(QMouseEvent* event);
    void processWheel(QWheelEvent* event);
    void focusIn();
    void focusOut();
    void pasteText(const QString& text);

    void resize(int cols, int rows);
    int columns() const { return _screen.columns(); }
    int rows() const { return _screen.rows(); }

    bool getCell(int row, int col, NovaTerm::Cell& out) const;
    const NovaTerm::ScreenBuffer& screenBuffer() const { return _screen; }
    NovaTerm::TerminalSnapshot snapshot() const;
    void flushDamage();
    void setDefaultColors(const NovaTerm::TerminalColor& foreground,
                          const NovaTerm::TerminalColor& background);

    int scrollbackLineCount() const;
    bool getScrollbackCell(int lineIndex, int col, NovaTerm::Cell& out) const;
    void setScrollbackLimit(int lines);
    void clearScrollback();

    NovaTerm::Position cursorPosition() const { return _cursor.position; }
    bool cursorVisible() const { return _cursor.visible; }
    NovaTerm::CursorShape cursorShape() const { return _cursor.shape; }
    bool cursorBlink() const { return _cursor.blink; }

    QString title() const { return _title; }

signals:
    void outputData(const QByteArray& data);
    void titleChanged(const QString& title);
    void bell();
    void damage(const NovaTerm::DirtyRegion& region);
    void cursorMoved();
    void scrollbackChanged();

private:
    NovaTerm::ScreenBuffer _screen;
    ScrollbackBuffer _scrollback;
    NovaTerm::CursorState _cursor;
    QString _title;
    std::unique_ptr<NovaTerm::VTAdapter> _adapter;
};
