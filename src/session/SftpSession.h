#pragma once

// SFTP 会话 — 占位（P6 步骤之后实现）。
//
// 需求背景：SSH 会话补全的第一阶段只做 shell/PTY 通路，SFTP 明确留白。
// libssh 的 SFTP 支持已随构建开启（WITH_SFTP=ON，sftp_init / sftp_open /
// sftp_read 等可用），但 NovaTerm 尚未规划 SFTP 的文件浏览 UI 布局，
// 因此这里只声明接口轮廓，不实现任何传输逻辑。
//
// 后续接入点：
//   • 复用现有 ITransport 的 SSH channel（ssh_channel_request_sftp）。
//   • 文件操作必须走工作线程，禁止阻塞 GUI。
//   • 目录/文件浏览 UI 规划后，在此填充枚举/回调契约。

class SftpSession
{
public:
    // TODO: 实现 SFTP 文件操作（list / get / put / mkdir / rm）。
    // 当前为占位桩，调用即表示"未实现"。
};
