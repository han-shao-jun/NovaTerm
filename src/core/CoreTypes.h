/**
 * @file   CoreTypes.h
 * @brief  终端核心层的基础整数别名与字节视图。
 *
 * 核心层不依赖 Qt。这里提供与 Qt 同义但不引入 Qt 的整数别名，使
 * src/core/ 下的代码可以在无 Qt 环境中编译。命名保持简短且语义明确：
 * - isize 对应 qsizetype（有符号，容器/缓冲长度与索引统一用它）
 * - u8/u32/u64 对应 quint8/quint32/quint64
 * ByteView 是对连续只读字节序列的非拥有视图，替代 QByteArrayView 在
 * 核心层内部的用途。
 */
#pragma once

#include <cstddef>
#include <cstdint>

namespace NovaTerm {

// 有符号长度/索引类型，语义等价于 qsizetype（指针宽度）。
using isize = std::ptrdiff_t;
using u8 = std::uint8_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;

// 连续只读字节序列的非拥有视图。不管理生命周期，调用方保证 data 在
// 视图使用期间有效。替代核心层内部对 QByteArrayView 的使用。
struct ByteView
{
    const char* data{nullptr};
    isize size{0};

    ByteView() = default;
    ByteView(const char* d, isize n) : data(d), size(n) {}

    bool empty() const { return size <= 0; }
};

} // namespace NovaTerm
