/**
 * @file Budget.h
 * @brief MCP 接入、观察、执行与报文预算的唯一取值处。
 *
 * 数值来自 2026-10-18 重构设计 §10 的兼容预算表；迁移期间旧常量
 * `McpProtocol.h` 仍以别名引用这里，禁止在新代码中重复字面量。
 */
#pragma once

#include <QtGlobal>

#include <chrono>

namespace NovaTerm::Mcp::Budget {
inline constexpr int MaxFileLocalRootsPerClient = 8;
inline constexpr int MaxFileRemoteRootsPerClient = 8;
inline constexpr int MaxFileLocalRoots = 128;
inline constexpr int MaxFileRemoteRoots = 256;
inline constexpr int MaxFileRootPathBytes = 1024;
inline constexpr int MaxFileRootTitleBytes = 256;
inline constexpr quint64 MaxLocalTransferFileBytes = 64 * 1024 * 1024;
inline constexpr qsizetype LocalTransferChunkBytes = 64 * 1024;
inline constexpr int MaxLocalTransferPathParts = 32;
inline constexpr int MaxFileTransfersPerClient = 16;
inline constexpr int MaxFileTransfers = 64;
inline constexpr int MaxActiveFileTransfers = 2;
inline constexpr std::chrono::milliseconds FileTransferRetention{600000};
inline constexpr std::chrono::milliseconds FileUploadTimeout{120000};
inline constexpr std::chrono::milliseconds FileUploadStallTimeout{15000};
inline constexpr std::chrono::milliseconds FileUploadPublicationTimeout{2000};

// --- 接入与请求调度 -------------------------------------------------------
inline constexpr int MaxClients = 4;
inline constexpr int MaxActiveRequests = 2;
inline constexpr int MaxQueuedRequests = 4;
inline constexpr int MaxWireCalls = 6;
inline constexpr int MaxClearRecordsPerClient = 32;
inline constexpr int MaxClearRecords = 128;
inline constexpr std::chrono::milliseconds ClearRecordRetention{600000};
inline constexpr int MaxTerminalTasksPerClient = 32;
inline constexpr int MaxTerminalTasks = 128;
inline constexpr std::chrono::milliseconds TerminalTaskRetention{600000};
inline constexpr std::chrono::milliseconds TerminalTaskDefaultTimeout{600000};
inline constexpr std::chrono::milliseconds TerminalTaskMaxTimeout{3600000};
inline constexpr int MaxTextWaitCandidates = 8;
inline constexpr int MaxTextWaitCandidateBytes = 256;
inline constexpr int DefaultTextWaitScanBytes = 262144;
inline constexpr int MaxTextWaitScanBytes = 1048576;
inline constexpr int MaxTextWaitStatesPerClient = 8;
inline constexpr int MaxTextWaitStates = 32;
inline constexpr std::chrono::milliseconds TextWaitStateLifetime{600000};

// --- 观察与 capture -------------------------------------------------------
inline constexpr std::chrono::milliseconds SnapshotCoalesceInterval{250};
inline constexpr int MaxCapturesPerConnection = 4;
inline constexpr int MaxCapturesPerClient = 4;
inline constexpr int MaxCapturesGlobal = 32;
inline constexpr std::chrono::milliseconds CaptureLifetime{60000};
inline constexpr int MaxObservationWaiters = 32;
inline constexpr int MaxObservedControlSessions = 32;
inline constexpr std::chrono::milliseconds ControlIdleRenewal{60000};
inline constexpr qsizetype MaxTextBytes = 256 * 1024;
inline constexpr int MaxFullLines = 1024;
inline constexpr int CompactPayloadBytes = 4 * 1024;
inline constexpr int CompactLines = 24;
inline constexpr int CompactFinalPayloadBytes = 8 * 1024;
inline constexpr int ObservationDefaultPayloadBytes = 65536;
inline constexpr int ObservationDefaultLines = 256;

// --- 终端动作 -------------------------------------------------------------
inline constexpr int MaxTerminalActions = 64;
inline constexpr qsizetype MaxTerminalActionTextBytes = 16 * 1024;
inline constexpr int MaxTerminalWaitMs = 30000;
inline constexpr qsizetype MaxEncodedActionBytes = 64 * 1024;
inline constexpr std::chrono::milliseconds CancelCompletionGrace{250};

// --- 命令、确认与脚本 -----------------------------------------------------
inline constexpr int CommandOutputBytes = 65536;
inline constexpr std::chrono::milliseconds CommandTimeout{5000};
inline constexpr std::chrono::milliseconds ConfirmationTimeout{60000};
inline constexpr std::chrono::milliseconds ScriptWriteTimeout{120000};
inline constexpr qsizetype MaxScriptBytes = 2 * 1024 * 1024;
inline constexpr int MaxPendingScriptWrites = 8;
inline constexpr int MaxExecutionRecordsPerClient = 32;
inline constexpr int MaxExecutionRecords = 128;
inline constexpr std::chrono::milliseconds ExecutionRecordRetention{600000};

// --- 报文 ----------------------------------------------------------------
inline constexpr qsizetype MaxFrameBytes = 2 * 1024 * 1024;
inline constexpr qsizetype MaxOutputQueueBytes = 4 * 1024 * 1024;
/** @brief JSON 字符串最坏每 UTF-8 字节展开为六字节；其余有界字段另留信封额度。 */
inline constexpr qsizetype MaxRequestEnvelopeBytes = 256 * 1024;
inline constexpr qsizetype MaxRequestBytes = 6 * MaxScriptBytes + MaxRequestEnvelopeBytes;
inline constexpr std::chrono::milliseconds WireDeadlineMargin{5000};

} // namespace NovaTerm::Mcp::Budget
