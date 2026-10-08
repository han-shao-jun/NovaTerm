/** @file YmodemTests.cpp @brief YMODEM 批次、元信息和结束握手测试。 */
#include "filetransfer/YmodemEngine.h"
#include "filetransfer/XyPacketCodec.h"
#include "ProtocolSizeTestSupport.h"
#include <cstdlib>
#include <iostream>
#include <stdexcept>
using namespace NovaTerm::FileTransfer;
namespace {
void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
Bytes output(ITransferEngine& engine) { const auto view = engine.pendingOutput(); Bytes bytes; if (view.size) bytes.assign(view.data, view.data + view.size); check(engine.acknowledgeOutput(bytes.size(), 0), "output ack"); return bytes; }
void feed(ITransferEngine& engine, const Bytes& bytes) { for (const auto byte : bytes) { const auto value = static_cast<char>(byte); check(engine.consume({&value, 1}, 0).consumed == 1, "consume fragment"); } }
TransferAction action(ITransferEngine& engine, ActionKind kind) { auto result = engine.takeAction(); check(result && result->kind == kind, "action kind"); return *result; }
void complete(ITransferEngine& engine, const TransferAction& value, Bytes bytes = {}) { check(engine.completeOperation(value.id, {true, true, std::move(bytes)}, 0), "operation complete"); }
Bytes header(const std::string& name, const std::string& size) { Bytes bytes(128, 0); std::copy(name.begin(), name.end(), bytes.begin()); std::copy(size.begin(), size.end(), bytes.begin() + static_cast<std::ptrdiff_t>(name.size() + 1)); return XyPacketCodec::encode(0, bytes, true); }
void receiveBatch() {
    YmodemEngine engine; TransferRequest request; check(engine.start(request, 0), "batch start"); check(output(engine) == Bytes{'C'}, "batch C");
    for (const auto size : {3U, 0U}) {
        const auto metadata = header(size ? "binary" : "empty", std::to_string(size));
        feed(engine, metadata); const auto offer = action(engine, ActionKind::OfferFile); check(offer.file.size == size, "metadata size"); complete(engine, offer);
        check(output(engine) == Bytes({0x06, 'C'}), "metadata ack C");
        feed(engine, metadata); check(!engine.takeAction(), "duplicate header no offer"); check(output(engine) == Bytes({0x06, 'C'}), "duplicate header ack C");
        if (size) { Bytes payload(1024, 0x1a); payload[0] = 0x18; payload[1] = 0x04; payload[2] = 0;
            const auto packet = XyPacketCodec::encode(1, payload, true); feed(engine, packet); const auto write = action(engine, ActionKind::WriteAt); check(write.bytes == Bytes({0x18, 0x04, 0}), "declared size trim"); complete(engine, write); output(engine);
            feed(engine, packet); check(!engine.takeAction(), "duplicate data no write"); check(output(engine) == Bytes{6}, "duplicate data ack"); }
        feed(engine, {4}); check(output(engine) == Bytes{0x15}, "first EOT NAK");
        feed(engine, {4}); complete(engine, action(engine, ActionKind::FinishFile)); check(output(engine) == Bytes({6, 'C'}), "second EOT ack C");
        feed(engine, {4}); check(!engine.takeAction(), "duplicate eot no finish"); check(output(engine) == Bytes({6, 'C'}), "duplicate EOT ack C");
    }
    feed(engine, header("", "")); check(output(engine) == Bytes{6}, "batch terminator ack");
    engine.advance(3000); check(engine.progress().state == State::Completed && engine.progress().completedFiles == 2 && engine.progress().transferredBytes == 3, "batch complete");
}
void unknownLengthAndInvalidMetadata() {
    YmodemEngine engine; TransferRequest request; check(engine.start(request, 0), "unknown start"); output(engine);
    feed(engine, header("unknown", "")); const auto offer = action(engine, ActionKind::OfferFile); check(!offer.file.size, "missing size unknown"); complete(engine, offer); output(engine);
    for (unsigned block = 1; block <= 256; ++block) { feed(engine, XyPacketCodec::encode(static_cast<std::uint8_t>(block), Bytes(128, 0x1a), true)); const auto write = action(engine, ActionKind::WriteAt); check(write.bytes.size() == 128, "unknown retains padding"); complete(engine, write); output(engine); }
    check(engine.progress().transferredBytes == 32768 && !engine.progress().lengthKnown, "data block zero stays data"); engine.cancel(0); output(engine);
    for (const auto& metadata : {header("bad", "4294967296"), header("bad", "-1"), header(std::string(1, static_cast<char>(0xff)), "1"), XyPacketCodec::encode(0, Bytes(128, 'a'), true)}) {
        check(engine.start(request, 0), "invalid restart"); output(engine); feed(engine, metadata); check(engine.progress().state == State::Failed, "invalid metadata rejected");
    }
    check(engine.start(request, 0), "G restart"); output(engine); feed(engine, {'G'}); check(engine.progress().error == Error::UnsupportedVariant, "YMODEM G rejected");
}
void recoveryAndBudgets() {
    TransferRequest request; YmodemEngine receiver; check(receiver.start(request, 0), "recovery start"); output(receiver);
    auto damaged = header("a", "1"); damaged[2] ^= 1; feed(receiver, damaged);
    check(output(receiver) == Bytes{0x15} && !receiver.takeAction(), "bad complement no offer");
    feed(receiver, header("a", "1")); complete(receiver, action(receiver, ActionKind::OfferFile)); output(receiver);
    feed(receiver, {4}); check(receiver.progress().error == Error::SizeMismatch, "early EOT fails");
    check(receiver.start(request, 0), "extra block start"); output(receiver); feed(receiver, header("a", "1")); complete(receiver, action(receiver, ActionKind::OfferFile)); output(receiver);
    feed(receiver, XyPacketCodec::encode(1, Bytes(128, 0), true)); complete(receiver, action(receiver, ActionKind::WriteAt)); output(receiver);
    feed(receiver, XyPacketCodec::encode(2, Bytes(128, 0), true)); check(receiver.progress().error == Error::SizeMismatch, "extra full block fails");
    check(receiver.start(request, 0), "file budget start"); output(receiver);
    for (unsigned file = 0; file <= MaxFiles; ++file) {
        feed(receiver, header(std::to_string(file), "0"));
        if (file == MaxFiles) { check(receiver.progress().error == Error::ResourceLimit, "257th file rejected"); break; }
        complete(receiver, action(receiver, ActionKind::OfferFile)); output(receiver);
        feed(receiver, {4}); output(receiver); feed(receiver, {4}); complete(receiver, action(receiver, ActionKind::FinishFile)); output(receiver);
    }
    for (unsigned cycle = 0; cycle < 100; ++cycle) {
        check(receiver.start(request, 0), "Y restart"); output(receiver); feed(receiver, header("a", "1")); const auto stale = action(receiver, ActionKind::OfferFile); receiver.cancel(0);
        check(output(receiver).size() >= 2 && !receiver.completeOperation(stale.id, {}, 0), "Y cancel expires operation");
    }
    request.direction = Direction::Send; request.files = {{"a", 1025}}; YmodemEngine sender;
    check(sender.start(request, 0), "adaptive sender"); feed(sender, {'C'}); output(sender); feed(sender, {6, 'C'});
    auto read = action(sender, ActionKind::ReadAt); check(read.count == 1024, "large read"); complete(sender, read, Bytes(1024, 0)); check(output(sender)[0] == 2, "large STX packet");
    feed(sender, {6}); read = action(sender, ActionKind::ReadAt); check(read.count == 1 && read.offset == 1024, "small remainder exact read");
    complete(sender, read, {0x1a}); const auto tail = output(sender); check(tail[0] == 1 && tail[3] == 0x1a, "small SOH packet preserves real 1A");
    sender.advance(10000); check(output(sender) == tail, "data timeout exact replay"); sender.cancel(0); output(sender);
    check(sender.start(request, 0), "Y short read restart"); feed(sender, {'C'}); output(sender); feed(sender, {6, 'C'});
    complete(sender, action(sender, ActionKind::ReadAt), {1}); check(sender.progress().error == Error::SizeMismatch, "Y source short read fails");
    request.files = {{std::string(255, 'a'), 0}}; check(sender.start(request, 0), "long UTF8 name start"); feed(sender, {'C'});
    const auto metadata = output(sender); check(metadata[0] == 2, "long name uses 1K metadata"); sender.cancel(0); output(sender);
    request.files = {}; check(sender.start(request, 0), "empty batch start"); feed(sender, {'C'}); check(output(sender) == header("", ""), "empty batch terminator"); feed(sender, {6}); check(sender.progress().state == State::Completed, "empty batch complete");
}
void closingDeadlineStartsAfterAckDrain() {
    YmodemEngine receiver; TransferRequest request; check(receiver.start(request, 0), "Y closing deadline start"); output(receiver);
    feed(receiver, header("", "")); feed(receiver, {1});
    check(!receiver.nextDeadline(), "Y closing partial frame cannot arm response deadline"); output(receiver);
    check(receiver.nextDeadline() == 3000U, "Y closing uses closing deadline"); receiver.advance(3000);
    check(receiver.progress().state == State::Completed, "Y closing partial frame expires on time");
}
void senderBlockNumberWraps() {
    YmodemEngine sender; TransferRequest request; request.direction = Direction::Send; request.files = {{"wrap", 256 * 1024 + 1}};
    check(sender.start(request, 0), "Y sender wrap start"); feed(sender, {'C'}); output(sender); feed(sender, {6, 'C'});
    for (unsigned number = 1; number <= 257; ++number) {
        const auto read = action(sender, ActionKind::ReadAt); complete(sender, read, Bytes(read.count, 0x18));
        const auto packet = output(sender); check(packet[1] == static_cast<std::uint8_t>(number), "Y sender modulo 256 number"); feed(sender, {6});
    }
    check(output(sender) == Bytes{4}, "Y wrapped EOT"); feed(sender, {6}); complete(sender, action(sender, ActionKind::FinishFile));
    feed(sender, {'C'}); output(sender); feed(sender, {6});
    check(sender.progress().state == State::Completed && sender.progress().transferredBytes == 256 * 1024 + 1, "Y wrapped sender exact progress");
}
void delayedAckCannotConfirmUnsentBlock() {
    YmodemEngine sender; TransferRequest request; request.direction = Direction::Send; request.files = {{"a", 1025}};
    check(sender.start(request, 0), "delayed Y ACK start"); feed(sender, {'C'}); feed(sender, {6, 'C'});
    check(!sender.takeAction(), "header ACK before drain ignored");
    check(sender.acknowledgeOutput(1, 0), "header partial drain"); feed(sender, {6, 'C'}); check(!sender.takeAction(), "header ACK after partial drain ignored");
    output(sender); feed(sender, {6, 'C'}); complete(sender, action(sender, ActionKind::ReadAt), Bytes(1024, 0));
    feed(sender, {6}); check(sender.progress().transferredBytes == 0 && !sender.takeAction(), "Y data ACK before drain ignored");
    output(sender); feed(sender, {6}); complete(sender, action(sender, ActionKind::ReadAt), {7});
    feed(sender, {6}); check(sender.progress().transferredBytes == 1024 && !sender.takeAction(), "old Y ACK ignores unsent remainder");
    check(sender.acknowledgeOutput(1, 0), "Y remainder partial drain"); feed(sender, {6});
    check(sender.progress().transferredBytes == 1024 && !sender.takeAction(), "old Y ACK after partial drain ignored");
    output(sender); feed(sender, {6}); check(sender.progress().transferredBytes == 1025, "Y actual remainder ACK accepted");
    feed(sender, {6}); check(!sender.takeAction(), "Y EOT ACK before drain ignored"); output(sender); feed(sender, {6}); complete(sender, action(sender, ActionKind::FinishFile));
    feed(sender, {'C'}); feed(sender, {6}); check(sender.progress().state != State::Completed, "batch terminator ACK before drain ignored");
    check(sender.acknowledgeOutput(1, 0), "batch terminator partial drain"); feed(sender, {6}); check(sender.progress().state != State::Completed, "batch terminator ACK after partial drain ignored");
    output(sender); feed(sender, {6}); check(sender.progress().state == State::Completed, "Y actual final ACK accepted");
}
void senderBatch() {
    YmodemEngine engine; TransferRequest request; request.direction = Direction::Send; request.files = {{"a", 2}, {"empty", 0}};
    check(engine.start(request, 0), "sender start"); feed(engine, {'C'}); auto metadata = output(engine); check(metadata == header("a", "2"), "sender metadata");
    feed(engine, {0x15}); check(output(engine) == metadata, "header retry"); feed(engine, {6, 'C'});
    const auto read = action(engine, ActionKind::ReadAt); check(read.count == 2, "sender read count"); complete(engine, read, {7, 8}); const auto packet = output(engine); check(packet[0] == 1 && packet.size() == 133, "small final packet uses SOH");
    feed(engine, {6}); check(output(engine) == Bytes{4}, "sender EOT"); feed(engine, {0x15}); check(output(engine) == Bytes{4}, "sender second EOT");
    feed(engine, {6}); complete(engine, action(engine, ActionKind::FinishFile)); feed(engine, {'C'}); check(output(engine) == header("empty", "0"), "next metadata");
    feed(engine, {6, 'C'}); check(output(engine) == Bytes{4}, "empty EOT"); feed(engine, {6}); complete(engine, action(engine, ActionKind::FinishFile));
    feed(engine, {'C'}); check(output(engine) == header("", ""), "terminator metadata"); feed(engine, {6}); check(engine.progress().state == State::Completed && engine.progress().completedFiles == 2, "sender batch complete");
}
void declaredSizeBoundaries() {
    for (const auto size : {0xfffffffeULL, 0xffffffffULL, 0x100000000ULL}) {
        YmodemEngine receiver; TransferRequest request;
        check(receiver.start(request, 0), "Y size declaration start"); output(receiver);
        feed(receiver, header("limit.bin", std::to_string(size)));
        if (size <= 0xffffffffULL) {
            const auto offer = action(receiver, ActionKind::OfferFile);
            check(offer.file.size == size, "Y maximum size parsed without truncation");
        } else {
            check(receiver.progress().error == Error::Protocol && !receiver.takeAction(),
                  "Y oversized metadata rejected before file offer");
        }
    }
}
}
int main() { try { ProtocolSizeTests::matrix(ProtocolSizeTests::Mode::Y); declaredSizeBoundaries(); receiveBatch(); unknownLengthAndInvalidMetadata(); recoveryAndBudgets(); closingDeadlineStartsAfterAckDrain(); senderBlockNumberWraps(); delayedAckCannotConfirmUnsentBlock(); senderBatch(); std::cout << "YMODEM tests PASS\n"; } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return EXIT_FAILURE; } }
