#pragma once

#include "BoundedByteQueue.h"
#include "ScreenBuffer.h"
#include "core/scrollback/LineLayout.h"
#include "core/search/SearchEngine.h"

#include <QByteArray>
#include <QByteArrayView>
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
    struct InputWriteResult
    {
        qsizetype requestedBytes{0};
        qsizetype acceptedBytes{0};
        bool backpressured{false};

        bool fullyAccepted() const
        {
            return acceptedBytes == requestedBytes;
        }
    };

    explicit TerminalCore(int cols, int rows, QObject* parent = nullptr);
    ~TerminalCore() override;

    InputWriteResult writeInput(QByteArrayView data);

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
        const QVector<bool>& dirtyRows, int scrollLine,
        NovaTerm::LineId anchorLine = 0,
        qsizetype anchorWrap = 0) const;
    quint64 modelRevision() const;
    NovaTerm::CursorState cursorState() const;
    void flushDamage();
    void setDefaultColors(const NovaTerm::TerminalColor& foreground,
                          const NovaTerm::TerminalColor& background);

    int scrollbackLineCount() const;
    bool getScrollbackCell(int lineIndex, int col, NovaTerm::Cell& out) const;
    void setScrollbackLimit(int lines);
    void clearScrollback();
    NovaTerm::ScrollbackSnapshot scrollbackSnapshot() const;
    NovaTerm::ScrollbackStatistics scrollbackStatistics() const;
    void searchScrollback(NovaTerm::SearchRequest request);
    void cancelSearch(quint64 generation);
    void requestScrollbackReflow(int columns, quint64 generation,
                                 qsizetype batchLines = 1024);
    void cancelScrollbackReflow(quint64 generation);

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
    // The revision identifies the immutable model publication that produced
    // this half-open damage region. Renderers can detect that a snapshot is
    // newer than the damage delivered so far and conservatively rebuild.
    void damage(const NovaTerm::DirtyRegion& region, quint64 revision);
    void cursorMoved();
    void scrollbackChanged();
    // Exact active-screen upward scroll count for the publication. This is
    // separate from scrollbackChanged(), which is intentionally coalesced.
    void screenScrolled(int rows);
    void inputBackpressureChanged(bool paused);
    void inputOverload(const QString& reason);
    void searchResultsReady(const NovaTerm::SearchBatch& batch);
    void reflowBatchReady(const NovaTerm::ReflowBatch& batch);

private:
    class Runtime;
    std::unique_ptr<Runtime> _runtime;
    std::unique_ptr<NovaTerm::SearchEngine> _searchEngine;
    std::unique_ptr<NovaTerm::ReflowEngine> _reflowEngine;
};
