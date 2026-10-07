/**
 * @file RendererLargeInputBenchmarkTests.cpp
 * @brief 大文本夹具参数和输出故障的无 GPU 回归测试。
 */
#include "RendererLargeInputBenchmarkSupport.h"

#include <QJsonObject>
#include <QTemporaryDir>
#include <QTest>

// 使用 QFile 的真实写入入口注入设备错误，避免制造磁盘满或修改系统卷。
class FailingFile final : public QFile
{
    Q_OBJECT
public:
    enum class Mode
    {
        Error,
        ShortWrite
    };
    FailingFile(const QString &path, Mode mode) : QFile(path), _mode(mode) {}

protected:
    qint64 writeData(const char *, qint64 bytes) override
    {
        if (_mode == Mode::Error) {
            setErrorString(QStringLiteral("注入设备写入失败"));
            return -1;
        }
        return bytes - 1;
    }

private:
    Mode _mode;
};

class RendererLargeInputBenchmarkTests : public QObject
{
    Q_OBJECT
private slots:
    void parsesSupportedArguments();
    void rejectsUnknownOption();
    void outputNameDoesNotEnableTimers();
    void rejectsInvalidArguments();
    void rejectsFailedWrite();
    void rejectsShortWrite();
    void writesCompleteJson();
};

void RendererLargeInputBenchmarkTests::parsesSupportedArguments()
{
    const auto plain = NovaTerm::Benchmark::parseOptions({"bench", "result.json", "65536"});
    QVERIFY(plain.has_value());
    QCOMPARE(plain->chunkBytes, 65536);
    QVERIFY(!plain->stableTimers);
    const auto stable =
        NovaTerm::Benchmark::parseOptions({"bench", "result.json", "262144", "--stable-timers"});
    QVERIFY(stable.has_value());
    QCOMPARE(stable->outputPath, QStringLiteral("result.json"));
    QCOMPARE(stable->chunkBytes, 262144);
    QVERIFY(stable->stableTimers);
}

void RendererLargeInputBenchmarkTests::rejectsUnknownOption()
{
    QVERIFY(
        !NovaTerm::Benchmark::parseOptions({"bench", "result.json", "65536", "--stable-timer"}));
}

void RendererLargeInputBenchmarkTests::outputNameDoesNotEnableTimers()
{
    const auto options = NovaTerm::Benchmark::parseOptions({"bench", "--stable-timers", "65536"});
    QVERIFY(options.has_value());
    QVERIFY(!options->stableTimers);
}

void RendererLargeInputBenchmarkTests::rejectsInvalidArguments()
{
    QVERIFY(!NovaTerm::Benchmark::parseOptions({"bench"}));
    QVERIFY(!NovaTerm::Benchmark::parseOptions({"bench", "result.json", "-1"}));
    QVERIFY(!NovaTerm::Benchmark::parseOptions({"bench", "result.json", "not-a-number"}));
    QVERIFY(!NovaTerm::Benchmark::parseOptions(
        {"bench", "result.json", "65536", "--stable-timers", "extra"}));
}

void RendererLargeInputBenchmarkTests::rejectsFailedWrite()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    FailingFile file(directory.filePath(QStringLiteral("error.json")), FailingFile::Mode::Error);
    QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Unbuffered));
    QVERIFY(!NovaTerm::Benchmark::writeResult(file, QJsonDocument(QJsonObject{{"ready", true}})));
}

void RendererLargeInputBenchmarkTests::rejectsShortWrite()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    FailingFile file(directory.filePath(QStringLiteral("short.json")),
                     FailingFile::Mode::ShortWrite);
    QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Unbuffered));
    QVERIFY(!NovaTerm::Benchmark::writeResult(file, QJsonDocument(QJsonObject{{"ready", true}})));
}

void RendererLargeInputBenchmarkTests::writesCompleteJson()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("result.json"));
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    QVERIFY(NovaTerm::Benchmark::writeResult(file, QJsonDocument(QJsonObject{{"ready", true}})));
    file.close();
    QVERIFY(file.open(QIODevice::ReadOnly));
    const auto actual = QJsonDocument::fromJson(file.readAll());
    QVERIFY(actual.isObject());
    QVERIFY(actual.object().value(QStringLiteral("ready")).toBool());
}

QTEST_GUILESS_MAIN(RendererLargeInputBenchmarkTests)
#include "RendererLargeInputBenchmarkTests.moc"
