/** @file WaitTextRequest.h @brief 明确来源、起点与预算的字面等待契约。 */
#pragma once
#include "Identifiers.h"
#include "Budget.h"
#include <QStringList>
#include <chrono>
#include <optional>

namespace NovaTerm::Mcp {
enum class WaitTextSource { Log, Screen };
enum class WaitTextMatch { Any, All };
enum class ScreenWaitStart { Current, Next };
struct WaitTextRequest {
    SessionId session;
    Epoch epoch;
    WaitTextSource source{WaitTextSource::Log};
    QStringList texts;
    WaitTextMatch match{WaitTextMatch::Any};
    bool caseSensitive{true};
    std::optional<QString> sinceToken;
    ScreenWaitStart screenStart{ScreenWaitStart::Current};
    std::chrono::milliseconds wait{0};
    int scanBytes{Budget::DefaultTextWaitScanBytes};
    std::optional<QString> resumeToken;
};
}
