# MCP Terminal Loop Implementation Plan

> For agentic workers: use superpowers:executing-plans to implement task by task.

Goal: implement the approved terminal observation/action loop, compact event waiting,
command evidence, scoped credentials and fail-closed managed elevation.

Architecture: Parser encodes standard input batches retaining origin; Session owns
control and command tracking; a dedicated MCP interaction backend owns authorization,
tokens, snapshots and asynchronous waits. Existing tools remain compatible.

Tech Stack: C++17, Qt 6.8.3, existing libvterm/HTTP/credential components.

Spec: ../specs/2026-10-16-mcp-terminal-interaction-design.md

## Constraints

- Same terminal/Transport and one InputPump; no free-command exec side channel.
- Bounded queues; user preemption; reject late callbacks after generation changes.
- No new Qt types in core data structures. Existing QObject facade performs bridging.
- New input permission defaults off; old risk-confirmed grants never auto-upgrade.
- Compact: 4 KiB/24 lines, final tool result <=8 KiB; waits <=30 seconds.
- Passwords never accepted from or returned through MCP; unavailable trusted adapters
  reject before reading a secret or typing a command.
- Follow AGENTS.md test matrix; full suite for this cross-module change.

## Review Focus

- Retries after response loss or partial submission must never duplicate input.
- An expired/read-only token cannot acquire input or renew another client's ownership.
- Continuous output must not starve waiting or produce false command completion.
- Old captures must not outlive revocation, reconnect or authentication privacy changes.
- Saving a credential cannot grant privilege or upgrade previous input authorization.

## Tasks

1. Parser input provenance and Session controller.
   Files: core/terminal/TerminalInput.h, TerminalCore.*, session/SessionTerminalController.*,
   TerminalSession.*, SessionInputArbiter.*, tests/core and tests/session.
   Interface: encodeAgentInput(operationId, vector<InputAction>), encodedAgentInput signal;
   controller submit/release/reset/observation and changed notification.
   - [ ] Write regression for Agent encoding, protocol replies and user preemption; observe RED.
   - [ ] Implement bounded atomic admission and tagged results, lifecycle-safe Session routing.
   - [ ] Run Core/Session tests, record results.
2. MCP grants, interaction/observation backend and compact event waits.
   Files: mcp/TerminalInteraction.*, McpAccess.*, McpService.*, McpProtocol.*, WireSession.*.
   Interface: handle(client, tool, arguments, completion); cancel/revoke; capabilities.
   - [ ] Add offline HTTP cases for grants, actions, replay, waits, compact size and expansion; RED.
   - [ ] Implement client-scoped tokens and bounded capture/wait state, nested action validation,
     command/readiness observations, attention/completion/change and deadline alignment.
   - [ ] Run MCP business/wire/HTTP/endpoint and security tests; record results.
3. MCP UI and credential lifecycle/elevation boundary.
   Files: McpSettingsDialog.*, McpAccess.*, TerminalInteraction.*, translations as applicable.
   Interface: grant terminalControl/useElevation; configure/delete method credentials locally;
   request_elevation rejects ELEVATION_ADAPTER_UNAVAILABLE before credential retrieval.
   - [ ] Add UI permission roundtrip and independent sudo/su credential isolation cases; RED.
   - [ ] Add capability columns, private credential editor, persistence status and revocation.
   - [ ] Run targeted tests; do not claim real sudo/su support without a trusted adapter.
4. Whole change validation and docs.
   - [ ] Build NovaTerm and all default targets, run full registered CTest suite.
   - [ ] Review state ownership, response budgets and secret paths independently.
   - [ ] Update P8/current architecture and record verified scope and remaining platform dependencies.

## Progress / rulings

- User explicitly requested execution of the written design; implement inline without another
  permission gate for routine implementation decisions.
- Feature branch creation was rejected by the user. Work stays uncommitted in the current
  checkout; no alternate branch/worktree or repository metadata writes will be attempted.
- Trusted SSH sudo/su adapter requires an explicit target-side installation/pairing mechanism.
  This session does not authorize installing one on user servers. Implement its discovery and
  fail-closed boundary; do not substitute password-prompt matching or claim this is functional
  real-host elevation. A real authenticated adapter remains a separately verifiable dependency.
