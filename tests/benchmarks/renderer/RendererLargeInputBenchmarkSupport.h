/**
 * @file RendererLargeInputBenchmarkSupport.h
 * @brief 大文本性能夹具的参数与 JSON 输出边界。
 */
#pragma once

#include <QByteArray>
#include <QFile>
#include <QJsonDocument>
#include <QStringList>

#include <optional>

namespace NovaTerm::Benchmark
{

struct Options {
    QString outputPath;
    int chunkBytes = 0;
    bool stableTimers = false;
};

/** @brief 解析程序名、输出路径、分块大小及可选计时参数。 */
[[nodiscard]] inline std::optional<Options> parseOptions(const QStringList &arguments)
{
    if (arguments.size() != 3 && arguments.size() != 4)
        return std::nullopt;
    const bool stableTimers = arguments.size() == 4;
    if (stableTimers && arguments[3] != QStringLiteral("--stable-timers"))
        return std::nullopt;
    const int chunkBytes = arguments[2].toInt();
    if (chunkBytes != 65536 && chunkBytes != 262144)
        return std::nullopt;
    return Options{arguments[1], chunkBytes, stableTimers};
}

/** @brief 向已打开的结果文件写出 JSON。 */
[[nodiscard]] inline bool writeResult(QFile &output, const QJsonDocument &document)
{
    const QByteArray serialized = document.toJson();
    return output.write(serialized) == serialized.size() && output.flush();
}

} // namespace NovaTerm::Benchmark
