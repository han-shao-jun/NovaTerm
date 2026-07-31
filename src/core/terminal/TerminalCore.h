#pragma once

#include "BoundedByteQueue.h"
#include "ScreenBuffer.h"

#include <QByteArray>
#include <QObject>
#include <QString>

#include <memory>

class QKeyEvent;
class QMouseEvent;
class QWheelEvent;

// Thread-safe Qt facade for the terminal parser runtime. Transport bytes and
// control commands are queued; a dedicated worker exclusively owns VTAdapter.
class TerminalCore : public QObject
{
    Q_OBJECT

public:
    explicit TerminalCore(int cols, int rows, QObject* parent = nullptr);
    ~TerminalCore() override;

    bool writeInput(const QByteArray& data);

    void processKeyPress(QKeyEvent* event);
    void processMousePress(QMouseEvent* event);
    void processMouseMove(QMouseEvent* event);
    void processMouseRelease(QMouseEvent* event);
    void processWheel(QWheelEvent* event);
    void focusIn();
    void focusOut();
    void pasteText(const QString& text);

    void resize(int cols, int rows);
    int columns() const;
    int rows() const;

    bool getCell(int row, int col, NovaTerm::Cell& out) const;
    NovaTerm::TerminalSnapshot snapshot() const;
    NovaTerm::RendererSnapshot rendererSnapshot(
        const QVector<bool>& dirtyRows, int scrollLine) const;
    NovaTerm::CursorState cursorState() const;
    void flushDamage();
    void setDefaultColors(const NovaTerm::TerminalColor& foreground,
                          const NovaTerm::TerminalColor& background);

    int scrollbackLineCount() const;
    bool getScrollbackCell(int lineIndex, int col, NovaTerm::Cell& out) const;
    void setScrollbackLimit(int lines);
    void clearScrollback();

    NovaTerm::Position cursorPosition() const;
    bool cursorVisible() const;
    NovaTerm::CursorShape cursorShape() const;
    bool cursorBlink() const;
    QString title() const;

    // Test/benchmark synchronization; production rendering remains signal-driven.
    bool waitForIdle(int timeoutMs = 5000) const;
    NovaTerm::BoundedByteQueue::Statistics queueStatistics() const;

signals:
    void outputData(const QByteArray& data);
    void titleChanged(const QString& title);
    void bell();
    void damage(const NovaTerm::DirtyRegion& region);
    void cursorMoved();
    void scrollbackChanged();
    void inputBackpressureChanged(bool paused);
    void inputOverload(const QString& reason);

private:
    class Runtime;
    std::unique_ptr<Runtime> _runtime;
};
