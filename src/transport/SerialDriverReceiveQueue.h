/**
 * @file SerialDriverReceiveQueue.h
 * @brief 串口驱动 RX 队列初始化与实际容量判定，不把请求成功当作核验成功。
 */
#pragma once
#include <cstdint>

namespace NovaTerm {

/** @brief 驱动队列请求与查询的独立结果；容量为 0 表示无法确认。 */
struct SerialDriverReceiveQueueResult
{
    static constexpr std::uint32_t RequestedReceiveBytes = 256 * 1024;
    static constexpr std::uint32_t RequestedTransmitBytes = 4096;
    std::uint32_t requestedBytes{RequestedReceiveBytes};
    bool requestAccepted{false};
    bool capacityQueried{false};
    std::uint32_t actualBytes{0};
    std::uint32_t requestError{0};
    std::uint32_t queryError{0};

    /** @brief 只有驱动报告的实际容量达标才认为接收队列已核验。 */
    [[nodiscard]] bool verified() const noexcept
    {
        return capacityQueried && actualBytes != 0 && actualBytes >= requestedBytes;
    }
};

/**
 * @brief 通过驱动操作请求并查询 RX 队列；每次打开都应重新调用。
 * @param driver 提供 request(rx, tx, error) 与 query(actual, error) 的驱动。
 * @param requestedBytes 请求容量；0 仅查询，供压力夹具保留驱动默认值。
 * @return 请求结果、实际容量及两次调用各自的错误码。
 * @note 操作边界可替代，仅用于测试无法由默认 CTest 占用的真实设备。
 */
template<typename Driver>
[[nodiscard]] SerialDriverReceiveQueueResult initializeSerialDriverReceiveQueue(
    Driver& driver,
    std::uint32_t requestedBytes = SerialDriverReceiveQueueResult::RequestedReceiveBytes)
{
    SerialDriverReceiveQueueResult result;
    result.requestedBytes = requestedBytes;
    result.requestAccepted = requestedBytes == 0 || driver.request(
        requestedBytes,
        SerialDriverReceiveQueueResult::RequestedTransmitBytes,
        result.requestError);
    // SetupComm 是容量建议，必须独立读回；即使请求失败，已有的实际
    // 容量仍可能足够。两次错误分别保存，避免第二次调用覆盖第一条错误。
    result.capacityQueried = driver.query(result.actualBytes, result.queryError);
    return result;
}

} // namespace NovaTerm
