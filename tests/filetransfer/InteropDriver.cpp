/**
 * @file InteropDriver.cpp
 * @brief Linux 隔离互通测试驱动；协议库不依赖本文件的文件与 POSIX I/O。
 */
#include "filetransfer/XmodemEngine.h"
#include "filetransfer/YmodemEngine.h"
#include "filetransfer/ZmodemEngine.h"
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <string>
#include <vector>
#include <cerrno>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

using namespace NovaTerm::FileTransfer;
namespace {
/** @brief 仅在夹具临时目录使用的文件宿主，不是产品文件保存策略。 */
class FileHost {
public:
    explicit FileHost(bool send, std::filesystem::path destination,
                      std::vector<std::filesystem::path> paths)
        : _send(send), _destination(std::move(destination)), _paths(std::move(paths)) {}
    OperationResult perform(const TransferAction& action)
    {
        OperationResult result;
        if (action.kind == ActionKind::OfferFile) {
            const auto& name = action.file.name;
            if (_send || name.empty() || name == "." || name == ".."
                || name.find('/') != std::string::npos
                || name.find('\\') != std::string::npos
                || name.find('\0') != std::string::npos) {
                result.success = false; return result;
            }
            _output.close(); _output.clear();
            _output.open(_destination / name, std::ios::binary | std::ios::trunc);
            result.success = _output.is_open();
        } else if (action.kind == ActionKind::ReadAt) {
            if (!_send || action.fileIndex >= _paths.size() || action.count > MaxInputBytes) {
                result.success = false; return result;
            }
            std::ifstream input(_paths[action.fileIndex], std::ios::binary);
            input.seekg(static_cast<std::streamoff>(action.offset));
            result.bytes.resize(action.count);
            input.read(reinterpret_cast<char*>(result.bytes.data()),
                       static_cast<std::streamsize>(result.bytes.size()));
            result.bytes.resize(static_cast<std::size_t>(input.gcount()));
            result.success = !input.bad();
        } else if (action.kind == ActionKind::WriteAt) {
            if (_send || !_output.is_open()) { result.success = false; return result; }
            _output.seekp(static_cast<std::streamoff>(action.offset));
            _output.write(reinterpret_cast<const char*>(action.bytes.data()),
                          static_cast<std::streamsize>(action.bytes.size()));
            result.success = _output.good();
        } else if (action.kind == ActionKind::FinishFile) {
            if (!_send) {
                _output.flush(); result.success = _output.good(); _output.close();
            }
        }
        return result;
    }
private:
    bool _send;
    std::filesystem::path _destination;
    std::vector<std::filesystem::path> _paths;
    std::ofstream _output;
};
TimePoint nowMs()
{
    return static_cast<TimePoint>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}
}
int main(int argc, char** argv)
{
    try {
        if (argc < 4) { std::cerr << "usage: driver protocol send|receive path...\n"; return 2; }
        const std::string protocol(argv[1]);
        const std::string direction(argv[2]);
        TransferRequest request;
        std::unique_ptr<ITransferEngine> engine;
        if (protocol == "x-checksum" || protocol == "x-crc" || protocol == "x-1k") {
            engine = std::make_unique<XmodemEngine>();
            request.config.xmodemMode = protocol == "x-checksum" ? XmodemMode::Checksum
                : protocol == "x-1k" ? XmodemMode::OneK : XmodemMode::Crc;
        } else if (protocol == "y") {
            engine = std::make_unique<YmodemEngine>();
        } else if (protocol == "z" || protocol == "z16") {
            engine = std::make_unique<ZmodemEngine>();
            request.config.zmodemCrc32 = protocol == "z";
        } else { std::cerr << "unsupported protocol\n"; return 2; }
        request.direction = direction == "send" ? Direction::Send : Direction::Receive;
        if (direction != "send" && direction != "receive") return 2;
        const bool send = request.direction == Direction::Send;
        std::vector<std::filesystem::path> paths;
        const std::filesystem::path destination(argv[3]);
        if (send) {
            for (int index = 3; index < argc; ++index) {
                paths.emplace_back(argv[index]);
                request.files.push_back({paths.back().filename().u8string(),
                                          std::filesystem::file_size(paths.back())});
            }
        } else {
            if (!std::filesystem::is_directory(destination)) return 2;
            request.receiveName = "received.bin";
            if (argc == 5) request.expectedSize = std::stoull(argv[4]);
        }
        FileHost host(send, destination, std::move(paths));
        const auto flagsIn = fcntl(STDIN_FILENO, F_GETFL);
        const auto flagsOut = fcntl(STDOUT_FILENO, F_GETFL);
        if (flagsIn < 0 || flagsOut < 0
            || fcntl(STDIN_FILENO, F_SETFL, flagsIn | O_NONBLOCK) < 0
            || fcntl(STDOUT_FILENO, F_SETFL, flagsOut | O_NONBLOCK) < 0) return 2;
        if (!engine->start(request, nowMs())) return 2;
        Bytes inbound;
        std::size_t inputHead = 0;
        const auto totalDeadline = nowMs() + 60000;
        while (nowMs() < totalDeadline) {
            for (unsigned actions = 0; actions < 32; ++actions) {
                auto action = engine->takeAction();
                if (!action) break;
                const auto id = action->id;
                if (!engine->completeOperation(id, host.perform(*action), nowMs())) return 3;
            }
            if (inputHead < inbound.size()) {
                const auto consumed = engine->consume(
                    {reinterpret_cast<const char*>(inbound.data() + inputHead),
                     static_cast<NovaTerm::isize>(inbound.size() - inputHead)}, nowMs());
                inputHead += consumed.consumed;
                if (inputHead == inbound.size()) { inbound.clear(); inputHead = 0; }
                if (consumed.consumed) continue;
            }
            const auto output = engine->pendingOutput();
            if (!output.empty()) {
                const auto count = ::write(STDOUT_FILENO, output.data,
                                            static_cast<std::size_t>(output.size));
                if (count > 0) {
                    if (!engine->acknowledgeOutput(static_cast<std::size_t>(count), nowMs())) return 3;
                    continue;
                }
                if (count < 0 && errno != EAGAIN && errno != EINTR) return 4;
            }
            engine->advance(nowMs());
            const auto progress = engine->progress();
            if (terminal(progress.state) && engine->pendingOutput().empty()) {
                if (progress.state != State::Completed) {
                    std::cerr << "protocol failed: state=" << static_cast<int>(progress.state)
                              << " error=" << static_cast<int>(progress.error) << '\n';
                    return 5;
                }
                std::cerr << "completed files=" << progress.completedFiles
                          << " bytes=" << progress.transferredBytes << '\n';
                return 0;
            }
            pollfd descriptors[2]{{STDIN_FILENO, 0, 0}, {STDOUT_FILENO, 0, 0}};
            if (inbound.empty()) descriptors[0].events = POLLIN;
            if (!engine->pendingOutput().empty()) descriptors[1].events = POLLOUT;
            int timeout = 100;
            if (const auto deadline = engine->nextDeadline()) {
                const auto now = nowMs();
                timeout = *deadline <= now ? 0 : static_cast<int>(std::min<TimePoint>(100,*deadline-now));
            }
            if (::poll(descriptors, 2, timeout) < 0 && errno != EINTR) return 4;
            if (descriptors[0].revents & POLLIN) {
                std::uint8_t buffer[4096];
                const auto count = ::read(STDIN_FILENO, buffer, sizeof(buffer));
                if (count > 0) inbound.assign(buffer, buffer + count);
                else if (count < 0 && errno != EAGAIN && errno != EINTR && errno != EIO) return 4;
            }
        }
        std::cerr << "integration deadline exceeded\n";
        return 6;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n'; return 7;
    }
}
