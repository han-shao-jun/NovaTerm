/** @file NoQtLinkCheck.cpp @brief 只链接独立协议库的三引擎生命周期验收。 */
#include "filetransfer/XmodemEngine.h"
#include "filetransfer/YmodemEngine.h"
#include "filetransfer/ZmodemEngine.h"
#include <array>
#include <memory>
using namespace NovaTerm::FileTransfer;
int main()
{
    for (unsigned cycle = 0; cycle < 100; ++cycle) {
        std::array<std::unique_ptr<ITransferEngine>, 3> engines{
            std::make_unique<XmodemEngine>(), std::make_unique<YmodemEngine>(),
            std::make_unique<ZmodemEngine>()};
        for (const auto& engine : engines) {
            TransferRequest request;
            if (!engine->start(request, 0)) return 1;
            const auto output = engine->pendingOutput();
            if (output.size > static_cast<NovaTerm::isize>(MaxOutputBytes)) return 2;
            if (!engine->acknowledgeOutput(static_cast<std::size_t>(output.size), 1)) return 3;
            while (const auto action = engine->takeAction())
                if (!engine->completeOperation(action->id, {}, 1)) return 4;
            engine->consume({"abc",3}, 2);
            engine->cancel(3);
            if (engine->progress().state != State::Cancelled) return 5;
        }
    }
    return 0;
}
