/**
 * @file   ConPtyApi.cpp
 * @brief  ConPTY API 动态解析与错误描述实现。
 *
 * resolve() 通过 GetModuleHandleW("kernel32.dll") + GetProcAddress 查找
 * CreatePseudoConsole/ResizePseudoConsole/ClosePseudoConsole 三个函数指针，
 * 用 QMutex 保护，原子发布完整解析集。windowsErrorMessage 通过 FormatMessageW
 * 取系统本地化描述。
 */
#include "ConPtyApi.h"

#include <QMutex>
#include <QMutexLocker>

namespace NovaTerm::Windows {
namespace {

QMutex apiMutex;
CreatePseudoConsoleFunction createFunction = nullptr;
ResizePseudoConsoleFunction resizeFunction = nullptr;
ClosePseudoConsoleFunction closeFunction = nullptr;

} // namespace

QString windowsErrorMessage(unsigned long error)
{
    wchar_t* message = nullptr;
    const DWORD length = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM
            | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, error, 0, reinterpret_cast<wchar_t*>(&message), 0, nullptr);
    const QString result = length > 0 && message
        ? QString::fromWCharArray(message, static_cast<qsizetype>(length)).trimmed()
        : QStringLiteral("Windows error %1").arg(error);
    if (message)
        LocalFree(message);
    return result;
}

QString hresultMessage(HRESULT result)
{
    return windowsErrorMessage(static_cast<unsigned long>(result));
}

bool ConPtyApi::resolve(QString* error)
{
    QMutexLocker locker(&apiMutex);
    if (createFunction && resizeFunction && closeFunction)
        return true;

    const HMODULE module = GetModuleHandleW(L"kernel32.dll");
    if (!module) {
        if (error)
            *error = windowsErrorMessage(GetLastError());
        return false;
    }

    const auto candidateCreate = reinterpret_cast<CreatePseudoConsoleFunction>(
        GetProcAddress(module, "CreatePseudoConsole"));
    const auto candidateResize = reinterpret_cast<ResizePseudoConsoleFunction>(
        GetProcAddress(module, "ResizePseudoConsole"));
    const auto candidateClose = reinterpret_cast<ClosePseudoConsoleFunction>(
        GetProcAddress(module, "ClosePseudoConsole"));
    if (!candidateCreate || !candidateResize || !candidateClose) {
        if (error)
            *error = QStringLiteral("ConPTY requires Windows 10 version 1809 or newer");
        return false;
    }

    // 仅在三个函数指针全部就绪后才发布，部分查找绝不污染全局缓存，
    // 保证后续重试不会观察到被污染的中间状态。
    createFunction = candidateCreate;
    resizeFunction = candidateResize;
    closeFunction = candidateClose;
    return true;
}

CreatePseudoConsoleFunction ConPtyApi::create()
{
    QMutexLocker locker(&apiMutex);
    return createFunction;
}

ResizePseudoConsoleFunction ConPtyApi::resize()
{
    QMutexLocker locker(&apiMutex);
    return resizeFunction;
}

ClosePseudoConsoleFunction ConPtyApi::close()
{
    QMutexLocker locker(&apiMutex);
    return closeFunction;
}

void PseudoConsoleHandle::reset(HPCON handle) noexcept
{
    if (_handle) {
        if (const auto closePseudoConsole = ConPtyApi::close())
            closePseudoConsole(_handle);
    }
    _handle = handle;
}

} // namespace NovaTerm::Windows
