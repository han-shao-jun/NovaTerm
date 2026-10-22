#pragma once
class TerminalSession;
namespace NovaTerm::Recording { class HistoryService; }
// Same-thread composition glue; Session itself has no History dependency.
void bindHistoryRecording(TerminalSession*, NovaTerm::Recording::HistoryService*);
