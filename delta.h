/// @file delta.h
/// @brief 二进制增量编码/解码（滑动窗口 hash 匹配）
///
/// 格式:
///   [4B LE] base_size  [4B LE] new_size
///   重复:
///     [1B] opcode (0x00 = copy, 0x01 = insert)
///     copy: [4B LE] offset  [4B LE] length
///     insert: [4B LE] length  [length B] data
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace fssentry {

/// @brief 计算增量：从基准文件到新文件
bool ComputeDelta(const std::wstring& basePath,
                  const std::wstring& newPath,
                  std::vector<uint8_t>& outDelta);

/// @brief 从内存中的基准数据 + 增量数据重建新文件
bool ApplyDelta(const std::vector<uint8_t>& baseData,
                const std::vector<uint8_t>& delta,
                std::vector<uint8_t>& outNew);

/// @brief 从磁盘上的基准文件 + 增量文件重建新文件
bool ApplyDeltaToFile(const std::wstring& basePath,
                      const std::wstring& deltaPath,
                      const std::wstring& outputPath);

} // namespace fssentry
