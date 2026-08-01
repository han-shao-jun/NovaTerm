#pragma once

#include <QHash>
#include <QVector>
#include <QtGlobal>

namespace NovaTerm {

struct VisibleRowIdentity
{
    quint64 sourceId{0};
    quint64 sourceVersion{0};
    qsizetype wrapIndex{0};
    bool activeScreen{false};

    friend bool operator==(const VisibleRowIdentity& a,
                           const VisibleRowIdentity& b)
    {
        return a.sourceId == b.sourceId
            && a.sourceVersion == b.sourceVersion
            && a.wrapIndex == b.wrapIndex
            && a.activeScreen == b.activeScreen;
    }
};

inline size_t qHash(const VisibleRowIdentity& id, size_t seed = 0) noexcept
{
    return qHashMulti(seed, id.sourceId, id.sourceVersion, id.wrapIndex,
                      id.activeScreen);
}

struct RowPlacement
{
    VisibleRowIdentity identity;
    int widgetRow{-1};
    int gpuSlot{-1};
    float yTransform{0};
    quint64 mappingRevision{0};
    bool reused{false};
};

struct RowSlotUpdate
{
    QVector<RowPlacement> placements;
    QVector<int> enteringWidgetRows;
    QVector<int> retiredSlots;
    int reusedRows{0};
    bool fullRemap{false};
};

class RowSlotMap
{
public:
    RowSlotUpdate update(const QVector<VisibleRowIdentity>& rows,
                         float rowHeight, bool forceFull = false);
    void reset();
    quint64 mappingRevision() const { return _mappingRevision; }
    int capacity() const { return _capacity; }
    const QVector<RowPlacement>& placements() const { return _placements; }

private:
    int allocateSlot(QVector<int>& freeSlots);

    int _capacity{0};
    quint64 _mappingRevision{0};
    QVector<RowPlacement> _placements;
};

} // namespace NovaTerm
