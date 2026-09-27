/**
 * @file LocalSessionCommandExecutor.cpp
 * @brief LocalShell 独立诊断 helper 的有界进程生命周期。
 */
#include "LocalSessionCommandExecutor.h"

#include "LocalDiagnosticProtocol.h"

#include <QFileInfo>
#include <QCryptographicHash>
#include <QProcessEnvironment>
#include <QSysInfo>

#include <algorithm>

LocalSessionCommandExecutor::LocalSessionCommandExecutor(
    QString helperPath, QStringList fixedArguments, QObject* parent)
    : ISessionCommandExecutor(parent)
    , _helperPath(std::move(helperPath))
    , _fixedArguments(std::move(fixedArguments))
{
    _timeout.setSingleShot(true);
    connect(&_timeout, &QTimer::timeout, this, [this] {
        requestStop(CommandExecutionOutcome::TimedOut);
    });
    connect(&_process, &QProcess::started, this, [this] {
        _executionMayHaveStarted = true;
    });
    connect(&_process, &QProcess::readyReadStandardOutput,
            this, &LocalSessionCommandExecutor::drainStandardOutput);
    connect(&_process, &QProcess::readyReadStandardError,
            this, &LocalSessionCommandExecutor::drainStandardError);
    connect(&_process, &QProcess::errorOccurred, this,
            [this](QProcess::ProcessError error) {
        if (!_request || error != QProcess::FailedToStart)
            return;
        appendOutput(_process.errorString().toUtf8(), _standardError);
        complete(CommandExecutionOutcome::Failed, true);
    });
    connect(&_process,
            qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
            [this](int exitCode, QProcess::ExitStatus status) {
        if (!_request)
            return;
        drainStandardOutput();
        drainStandardError();
        const auto outcome = _forcedOutcome.value_or(
            status == QProcess::NormalExit && exitCode == 0
                ? CommandExecutionOutcome::Completed
                : CommandExecutionOutcome::Failed);
        complete(outcome, true,
                 status == QProcess::NormalExit
                     ? std::optional<int>{exitCode} : std::nullopt);
    });
}

LocalSessionCommandExecutor::~LocalSessionCommandExecutor()
{
    _timeout.stop();
    if (_process.state() != QProcess::NotRunning) {
        _process.kill();
        static_cast<void>(_process.waitForFinished(1000));
    }
}

bool LocalSessionCommandExecutor::isAvailable() const
{
    const QFileInfo helper{_helperPath};
    return helper.isAbsolute() && helper.isFile();
}

CommandExecutorCapabilities LocalSessionCommandExecutor::capabilities() const
{
    return {CommandExecutionMode::Isolated, true, true, true};
}

CommandPlatformProfile LocalSessionCommandExecutor::profile() const
{
    return CommandPlatformProfile::forTransport(TransportKind::LocalShell);
}

QString LocalSessionCommandExecutor::targetFingerprint() const
{
    const QByteArray machineId = QSysInfo::machineUniqueId();
    const auto platformProfile = profile();
    if (machineId.isEmpty() || !platformProfile.isAvailable())
        return {};
    const QByteArray identity = machineId + '\0'
        + platformProfile.version().toUtf8();
    return QString::fromLatin1(
        QCryptographicHash::hash(identity, QCryptographicHash::Sha256).toHex());
}

bool LocalSessionCommandExecutor::execute(const CommandExecutionRequest& request)
{
    if (_request || !isAvailable() || request.requestId == 0
        || request.limits.maxOutputBytes <= 0 || request.limits.timeoutMs <= 0
        || !NovaTerm::LocalDiagnostic::isKnownCommand(request.commandId)) {
        return false;
    }

    _request = request;
    _forcedOutcome.reset();
    _standardOutput.clear();
    _standardError.clear();
    _executionMayHaveStarted = false;
    _outputTruncated = false;

    QProcessEnvironment environment;
    const auto systemEnvironment = QProcessEnvironment::systemEnvironment();
    for (const QString& name : {QStringLiteral("SystemRoot"),
                                QStringLiteral("WINDIR")}) {
        if (systemEnvironment.contains(name))
            environment.insert(name, systemEnvironment.value(name));
    }
    _process.setProcessEnvironment(environment);
    _process.setProgram(_helperPath);
    QStringList arguments = _fixedArguments;
    arguments.append(request.commandId);
    _process.setArguments(arguments);
    _process.setProcessChannelMode(QProcess::SeparateChannels);
    _process.start();
    _timeout.start(request.limits.timeoutMs);
    return true;
}

void LocalSessionCommandExecutor::cancel(quint64 requestId)
{
    if (_request && _request->requestId == requestId)
        requestStop(CommandExecutionOutcome::Cancelled);
}

void LocalSessionCommandExecutor::drainStandardOutput()
{
    appendOutput(_process.readAllStandardOutput(), _standardOutput);
}

void LocalSessionCommandExecutor::drainStandardError()
{
    appendOutput(_process.readAllStandardError(), _standardError);
}

void LocalSessionCommandExecutor::appendOutput(
    QByteArray bytes, QByteArray& destination)
{
    if (!_request || bytes.isEmpty())
        return;
    const qsizetype used = _standardOutput.size() + _standardError.size();
    const qsizetype remaining = std::max(
        qsizetype{0}, _request->limits.maxOutputBytes - used);
    if (bytes.size() > remaining) {
        destination.append(bytes.constData(), remaining);
        _outputTruncated = true;
        requestStop(CommandExecutionOutcome::OutputLimit);
        return;
    }
    destination.append(bytes);
}

void LocalSessionCommandExecutor::requestStop(CommandExecutionOutcome outcome)
{
    if (!_request)
        return;
    if (!_forcedOutcome || outcome == CommandExecutionOutcome::OutputLimit)
        _forcedOutcome = outcome;
    _timeout.stop();
    if (_process.state() == QProcess::NotRunning) {
        complete(outcome, true);
        return;
    }
    const quint64 requestId = _request->requestId;
    _process.terminate();
    QTimer::singleShot(100, this, [this, requestId] {
        if (_request && _request->requestId == requestId
            && _process.state() != QProcess::NotRunning) {
            _process.kill();
        }
    });
}

void LocalSessionCommandExecutor::complete(
    CommandExecutionOutcome outcome, bool terminationConfirmed,
    std::optional<int> exitCode)
{
    if (!_request)
        return;
    _timeout.stop();
    CommandExecutionResult result;
    result.requestId = _request->requestId;
    result.outcome = outcome;
    result.executionMayHaveStarted = _executionMayHaveStarted;
    result.terminationConfirmed = terminationConfirmed;
    result.outputTruncated = _outputTruncated;
    result.exitCode = exitCode;
    result.standardOutput = std::move(_standardOutput);
    result.standardError = std::move(_standardError);
    _request.reset();
    _forcedOutcome.reset();
    _executionMayHaveStarted = false;
    _outputTruncated = false;
    emit finished(result);
}
