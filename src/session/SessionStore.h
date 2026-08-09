#pragma once

#include "SessionTypes.h"

#include <QList>
#include <QString>

class SessionStore
{
public:
    explicit SessionStore(QString filePath);

    bool save(const QList<SessionRestoreMetadata>& sessions,
              QString* error = nullptr) const;
    [[nodiscard]] QList<SessionRestoreMetadata>
    load(QString* error = nullptr) const;
    [[nodiscard]] const QString& filePath() const noexcept { return _filePath; }

private:
    QString _filePath;
};
