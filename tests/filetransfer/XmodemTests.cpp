/** @file XmodemTests.cpp @brief XMODEM 分片、模式、长度与生命周期测试。 */
#include "filetransfer/XmodemEngine.h"
#include "filetransfer/XyPacketCodec.h"
#include <cstdlib>
#include <iostream>
#include <stdexcept>
using namespace NovaTerm::FileTransfer;
namespace {
void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
Bytes output(ITransferEngine& engine) {
    const auto view = engine.pendingOutput();
    Bytes bytes;
    if (view.size) bytes.assign(view.data, view.data + view.size);
    check(engine.acknowledgeOutput(bytes.size(), 0), "output ack");
    return bytes;
}
void feed(ITransferEngine& engine, const Bytes& bytes) {
    for (const auto byte : bytes) {
        const auto value = static_cast<char>(byte);
        check(engine.consume({&value, 1}, 0).consumed == 1, "one-byte consume");
    }
}
TransferAction action(ITransferEngine& engine, ActionKind kind) {
    auto result = engine.takeAction();
    check(result && result->kind == kind, "action kind");
    return *result;
}
void complete(ITransferEngine& engine, const TransferAction& value, Bytes bytes = {}) {
    check(engine.completeOperation(value.id, {true, true, std::move(bytes)}, 0), "complete operation");
}
void receiveModes() {
    for (const auto mode : {XmodemMode::Checksum, XmodemMode::Crc, XmodemMode::OneK}) {
        XmodemEngine engine;
        TransferRequest request;
        request.config.xmodemMode = mode;
        check(engine.start(request, 0), "receive start");
        complete(engine, action(engine, ActionKind::OfferFile));
        check(output(engine) == Bytes{static_cast<std::uint8_t>(mode == XmodemMode::Checksum ? 0x15 : 'C')}, "initial handshake");
        Bytes payload(mode == XmodemMode::OneK ? 1024 : 128, 0x1a);
        payload[0] = 0x04; payload[1] = 0x18; payload[2] = 0x18;
        const auto frame = XyPacketCodec::encode(1, payload, mode != XmodemMode::Checksum);
        feed(engine, frame);
        const auto write = action(engine, ActionKind::WriteAt);
        check(write.offset == 0 && write.bytes == payload, "unknown length preserves bytes");
        check(engine.progress().transferredBytes == 0, "write confirmation gates progress");
        check(output(engine).empty(), "no premature ack");
        complete(engine, write);
        check(output(engine) == Bytes{0x06}, "data ack");
        feed(engine, frame);
        check(!engine.takeAction(), "duplicate no write");
        check(output(engine) == Bytes{0x06}, "duplicate ack");
        feed(engine, {0x04}); complete(engine, action(engine, ActionKind::FinishFile));
        check(output(engine) == Bytes{0x06}, "eot ack");
        feed(engine, {0x04}); check(output(engine) == Bytes{0x06}, "duplicate eot ack");
        feed(engine, frame); check(engine.progress().state == State::Closing, "closing payload controls remain data");
        engine.advance(3000);
        check(engine.progress().state == State::Completed && !engine.progress().lengthKnown, "receive complete unknown length");
    }
}
void exactLengthAndWrap() {
    XmodemEngine engine; TransferRequest request; request.expectedSize = 256 * 128 - 17;
    check(engine.start(request, 0), "exact start"); complete(engine, action(engine, ActionKind::OfferFile)); output(engine);
    std::uint64_t total = 0;
    for (unsigned block = 1; block <= 256; ++block) {
        feed(engine, XyPacketCodec::encode(static_cast<std::uint8_t>(block), Bytes(128, 0x1a), true));
        const auto write = action(engine, ActionKind::WriteAt);
        check(write.offset == total, "wrapped offset"); total += write.bytes.size(); complete(engine, write); output(engine);
    }
    check(total == *request.expectedSize, "exact final trim");
    feed(engine, XyPacketCodec::encode(1, Bytes(128, 0), true));
    check(engine.progress().error == Error::SizeMismatch, "extra full block rejected");
}
void sendModesAndErrors() {
    for (const auto mode : {XmodemMode::Checksum, XmodemMode::Crc, XmodemMode::OneK}) {
        XmodemEngine engine; TransferRequest request; request.direction = Direction::Send;
        request.files = {{"source", 3}}; request.config.xmodemMode = mode;
        check(engine.start(request, 0), "send start");
        feed(engine, {static_cast<std::uint8_t>(mode == XmodemMode::Checksum ? 0x15 : 'C')});
        auto read = action(engine, ActionKind::ReadAt); check(read.count == 3, "exact read count");
        complete(engine, read, {1, 2, 3}); const auto packet = output(engine);
        check(packet[0] == (mode == XmodemMode::OneK ? 2 : 1), "packet mode");
        feed(engine, {0x15}); check(output(engine) == packet, "nak identical retry");
        feed(engine, {0x06}); check(output(engine) == Bytes{0x04}, "send eot");
        feed(engine, {0x06}); complete(engine, action(engine, ActionKind::FinishFile));
        check(engine.progress().state == State::Completed && engine.progress().transferredBytes == 3, "send complete");
    }
    XmodemEngine shortRead; TransferRequest request; request.direction = Direction::Send; request.files = {{"a", 3}};
    check(shortRead.start(request, 0), "short start"); feed(shortRead, {'C'});
    complete(shortRead, action(shortRead, ActionKind::ReadAt), {1});
    check(shortRead.progress().error == Error::SizeMismatch, "short read fails");
    XmodemEngine fallback; request.config.xmodemMode = XmodemMode::OneK;
    check(fallback.start(request, 0), "fallback start"); feed(fallback, {0x15});
    complete(fallback, action(fallback, ActionKind::ReadAt), {1, 2, 3});
    check(output(fallback).size() == 132, "1k checksum fallback");
}
void deadlinesPartialWritesAndSizes() {
    TransferRequest request; request.direction = Direction::Send; request.files = {{"empty", 0}};
    XmodemEngine sender; check(sender.start(request, 0), "empty sender start"); feed(sender, {'C'});
    check(output(sender) == Bytes{4}, "empty sender EOT"); feed(sender, {6}); complete(sender, action(sender, ActionKind::FinishFile));
    check(sender.progress().completedFiles == 1 && sender.progress().transferredBytes == 0, "empty sender done");
    request.files = {{"a", 129}}; check(sender.start(request, 0), "partial output start"); feed(sender, {'C'});
    complete(sender, action(sender, ActionKind::ReadAt), Bytes(128, 0x42));
    const auto view = sender.pendingOutput(); const Bytes packet(view.data, view.data + view.size);
    check(!sender.nextDeadline(), "deadline waits for full output");
    check(sender.acknowledgeOutput(7, 100), "partial prefix accepted");
    const auto suffix = sender.pendingOutput(); check(Bytes(suffix.data, suffix.data + suffix.size) == Bytes(packet.begin() + 7, packet.end()), "output suffix preserved");
    sender.advance(20000); check(sender.progress().retransmissions == 0, "partial output no premature retry");
    check(sender.acknowledgeOutput(static_cast<std::size_t>(suffix.size), 20000), "output suffix accepted");
    check(sender.nextDeadline() == 30000, "deadline based on drain"); sender.advance(30000);
    check(output(sender) == packet && sender.progress().retransmissions == 1, "timeout retries identical packet"); sender.cancel(0); output(sender);
    check(sender.start(request, 0), "long read start"); feed(sender, {'C'}); complete(sender, action(sender, ActionKind::ReadAt), Bytes(129, 0));
    check(sender.progress().error == Error::SizeMismatch, "oversized read rejected");
    XmodemEngine receiver; request = {}; request.expectedSize = 1; check(receiver.start(request, 0), "early EOT start");
    complete(receiver, action(receiver, ActionKind::OfferFile)); output(receiver); feed(receiver, {4});
    check(receiver.progress().error == Error::SizeMismatch, "early EOT fails exact length");
    request.expectedSize = 0; check(receiver.start(request, 0), "empty receiver start"); complete(receiver, action(receiver, ActionKind::OfferFile)); output(receiver);
    feed(receiver, {4}); complete(receiver, action(receiver, ActionKind::FinishFile)); output(receiver); receiver.advance(3000);
    check(receiver.progress().state == State::Completed, "empty receiver complete");
    request = {}; check(receiver.start(request, 0), "partial frame start"); complete(receiver, action(receiver, ActionKind::OfferFile)); output(receiver);
    feed(receiver, {1, 1, 0xfe, 0x18}); receiver.advance(3000); check(output(receiver) == Bytes{'C'}, "partial packet handshake retry");
    feed(receiver, XyPacketCodec::encode(1, Bytes(128, 0), true)); auto write = action(receiver, ActionKind::WriteAt);
    check(receiver.completeOperation(write.id, {false, true, {}}, 0), "failed write accepted");
    check(receiver.progress().error == Error::FileIo && receiver.progress().transferredBytes == 0, "failed write no progress");
}
void checksumDoesNotAcceptOneKVariant() {
    check(XyPacketCodec::encode(1, Bytes(1024, 0), false).empty(), "codec rejects 1K checksum encoding");
    XmodemEngine receiver; TransferRequest request; request.config.xmodemMode = XmodemMode::Checksum;
    check(receiver.start(request, 0), "checksum variant start"); complete(receiver, action(receiver, ActionKind::OfferFile)); output(receiver);
    Bytes packet{2, 1, 0xfe}; packet.resize(1028, 0); packet[3] = 0x18; packet[4] = 0x18; packet[5] = 4; packet.back() = 0x34;
    feed(receiver, packet); check(!receiver.takeAction() && receiver.progress().state == State::Negotiating, "1K checksum no write or payload control");
    check(output(receiver) == Bytes{0x15}, "1K checksum rejected with NAK");
}
void closingDeadlineStartsAfterAckDrain() {
    XmodemEngine receiver; TransferRequest request; check(receiver.start(request, 0), "closing deadline start");
    complete(receiver, action(receiver, ActionKind::OfferFile)); output(receiver); feed(receiver, {4});
    complete(receiver, action(receiver, ActionKind::FinishFile)); feed(receiver, {1});
    check(!receiver.nextDeadline(), "closing partial frame cannot arm response deadline"); output(receiver);
    check(receiver.nextDeadline() == 3000, "closing uses closing deadline"); receiver.advance(3000);
    check(receiver.progress().state == State::Completed, "closing partial frame expires on time");
}
void senderBlockNumberWraps() {
    XmodemEngine sender; TransferRequest request; request.direction = Direction::Send; request.files = {{"wrap", 256 * 128 + 1}};
    check(sender.start(request, 0), "sender wrap start"); feed(sender, {'C'});
    for (unsigned number = 1; number <= 257; ++number) {
        const auto read = action(sender, ActionKind::ReadAt); complete(sender, read, Bytes(read.count, 0x18));
        const auto packet = output(sender); check(packet[1] == static_cast<std::uint8_t>(number), "sender modulo 256 number"); feed(sender, {6});
    }
    check(output(sender) == Bytes{4}, "wrapped sender EOT"); feed(sender, {6}); complete(sender, action(sender, ActionKind::FinishFile));
    check(sender.progress().transferredBytes == 256 * 128 + 1, "wrapped sender exact progress");
}
void delayedAckCannotConfirmUnsentBlock() {
    XmodemEngine sender; TransferRequest request; request.direction = Direction::Send; request.files = {{"a", 129}};
    check(sender.start(request, 0), "delayed ack sender start"); feed(sender, {'C'});
    complete(sender, action(sender, ActionKind::ReadAt), Bytes(128, 0));
    feed(sender, {6}); check(sender.progress().transferredBytes == 0 && !sender.takeAction(), "ACK before first output drain ignored");
    output(sender); feed(sender, {6}); complete(sender, action(sender, ActionKind::ReadAt), {7});
    feed(sender, {6}); check(sender.progress().transferredBytes == 128 && !sender.takeAction(), "old ACK cannot confirm unsent next block");
    check(sender.acknowledgeOutput(1, 0), "next block partial drain"); feed(sender, {6});
    check(sender.progress().transferredBytes == 128 && !sender.takeAction(), "old ACK after partial drain ignored");
    output(sender); feed(sender, {6}); check(sender.progress().transferredBytes == 129, "legitimate ACK after drain accepted");
    feed(sender, {6}); check(!sender.takeAction(), "EOT ACK before drain ignored");
    check(output(sender) == Bytes{4}, "EOT remains queued"); feed(sender, {6}); complete(sender, action(sender, ActionKind::FinishFile));
    check(sender.progress().state == State::Completed, "legitimate EOT ACK completes");
}
void corruptionCancellationAndTimeout() {
    XmodemEngine engine; TransferRequest request;
    check(engine.start(request, 0), "bad packet start"); complete(engine, action(engine, ActionKind::OfferFile)); output(engine);
    auto frame = XyPacketCodec::encode(1, Bytes(128, 0), true); frame.back() ^= 1;
    feed(engine, frame); check(output(engine) == Bytes{0x15}, "crc nak");
    feed(engine, {0x18}); check(engine.progress().state != State::Cancelled, "single CAN ignored");
    feed(engine, {0, 0x18, 0x18}); check(engine.progress().state == State::Cancelled, "double CAN cancels");
    for (unsigned cycle = 0; cycle != 100; ++cycle) {
        check(engine.start(request, 0), "restart"); const auto stale = action(engine, ActionKind::OfferFile); engine.cancel(0);
        check(!engine.completeOperation(stale.id, {}, 0), "stale result rejected");
        check(output(engine).size() >= 2, "cancel sends CAN");
    }
    check(engine.start(request, 0), "timeout restart"); complete(engine, action(engine, ActionKind::OfferFile)); output(engine);
    engine.advance(30000); check(engine.progress().error == Error::Timeout, "handshake timeout");
}
}
int main() { try { receiveModes(); exactLengthAndWrap(); sendModesAndErrors(); deadlinesPartialWritesAndSizes(); checksumDoesNotAcceptOneKVariant(); closingDeadlineStartsAfterAckDrain(); senderBlockNumberWraps(); delayedAckCannotConfirmUnsentBlock(); corruptionCancellationAndTimeout(); std::cout << "XMODEM tests PASS\n"; } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return EXIT_FAILURE; } }
