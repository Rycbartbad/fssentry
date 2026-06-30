/// @file delta.cpp
/// @brief 二进制增量编码/解码实现

#include "delta.h"

#include <windows.h>
#include <algorithm>
#include <cstring>
#include <fstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace fssentry {

namespace {

/// @brief 读取整个文件到内存
/// @param path 文件路径
/// @return 文件内容，失败返回空
static std::vector<uint8_t> ReadEntireFile(const std::wstring& path)
{
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return {};
    LARGE_INTEGER sz;
    if (!GetFileSizeEx(h, &sz)) { CloseHandle(h); return {}; }
    if (sz.QuadPart <= 0 || sz.QuadPart > 256LL * 1024 * 1024)
        { CloseHandle(h); return {}; }
    std::vector<uint8_t> data(static_cast<size_t>(sz.QuadPart));
    DWORD bytesRead = 0;
    if (!::ReadFile(h, data.data(), static_cast<DWORD>(data.size()), &bytesRead, nullptr))
        { CloseHandle(h); return {}; }
    CloseHandle(h);
    data.resize(bytesRead);
    return data;
}

/// @brief 写入数据到文件
/// @param path 文件路径
/// @param data 数据指针
/// @param size 数据大小
/// @return 成功返回 true
static bool WriteFile(const std::wstring& path, const uint8_t* data, size_t size)
{
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size));
    return !f.fail();
}

/// @brief 写入 vector 到文件
/// @param path 文件路径
/// @param data 数据
/// @return 成功返回 true
static bool WriteFile(const std::wstring& path, const std::vector<uint8_t>& data)
{
    return WriteFile(path, data.data(), data.size());
}

static constexpr size_t kBlockSize = 16;
static constexpr size_t kMinMatch  = 8;
static constexpr uint32_t kRollBase = 131;

/// @brief 滚动哈希初始化
static uint32_t RollInit(const uint8_t* data)
{
    uint32_t h = 0;
    for (size_t i = 0; i < kBlockSize; ++i)
        h = h * kRollBase + data[i];
    return h;
}

/// @brief 滚动哈希前进一个字节
static uint32_t RollAdvance(uint32_t prev, uint8_t out_byte, uint8_t in_byte, uint32_t pow)
{
    return prev * kRollBase - static_cast<uint32_t>(out_byte) * pow + in_byte;
}

/// @brief 预计算 kRollBase^(kBlockSize-1) mod 2^32
static uint32_t RollPow()
{
    uint32_t p = 1;
    for (size_t i = 0; i < kBlockSize - 1; ++i)
        p = p * kRollBase;
    return p;
}

/// @brief 批量扩展匹配长度（优先 8 字节块 memcmp）
static uint32_t ExtendMatch(const uint8_t* a, const uint8_t* b, size_t maxLen)
{
    uint32_t len = 0;
    while (len + 8 <= maxLen) {
        if (std::memcmp(a + len, b + len, 8) != 0)
            break;
        len += 8;
    }
    while (len < maxLen && a[len] == b[len])
        ++len;
    return len;
}

/// @brief 写入 4 字节小端序 uint32
/// @param delta 输出 delta 缓冲
/// @param val   要写入的值
static void EmitU32(std::vector<uint8_t>& delta, uint32_t val)
{
    delta.push_back(static_cast<uint8_t>(val));
    delta.push_back(static_cast<uint8_t>(val >> 8));
    delta.push_back(static_cast<uint8_t>(val >> 16));
    delta.push_back(static_cast<uint8_t>(val >> 24));
}

/// @brief 写入 copy 指令（从 base 复制）
/// @param delta  输出 delta 缓冲
/// @param offset base 中的偏移
/// @param length 复制长度
static void EmitCopy(std::vector<uint8_t>& delta,
                     uint32_t offset, uint32_t length)
{
    delta.push_back(0x00);
    EmitU32(delta, offset);
    EmitU32(delta, length);
}

/// @brief 写入 insert 指令（插入新数据）
/// @param delta 输出 delta 缓冲
/// @param data  数据指针
/// @param length 插入长度
static void EmitInsert(std::vector<uint8_t>& delta,
                       const uint8_t* data, uint32_t length)
{
    delta.push_back(0x01);
    EmitU32(delta, length);
    delta.insert(delta.end(), data, data + length);
}

using BlockMap = std::unordered_map<uint32_t, std::vector<uint32_t>>;

/// @brief 构建 base 文件的 16 字节滑动窗口 hash 映射
/// @param base base 文件内容
/// @param map  输出的 hash→偏移列表 映射
static void BuildBlockMap(const std::vector<uint8_t>& base, BlockMap& map)
{
    if (base.size() < kBlockSize) return;
    size_t limit = base.size() - kBlockSize + 1;

    uint32_t pow = RollPow();
    uint32_t h   = RollInit(base.data());
    map[h].push_back(0);

    for (size_t i = 1; i < limit; ++i) {
        h = RollAdvance(h, base[i - 1], base[i - 1 + kBlockSize], pow);
        map[h].push_back(static_cast<uint32_t>(i));
    }
}

} // anonymous namespace

bool ComputeDelta(const std::wstring& basePath,
                  const std::wstring& newPath,
                  std::vector<uint8_t>& outDelta)
{
    auto base = ReadEntireFile(basePath);
    if (base.empty()) return false;
    auto newData = ReadEntireFile(newPath);
    if (newData.empty()) return false;

    BlockMap blockMap;
    BuildBlockMap(base, blockMap);

    outDelta.clear();
    uint32_t baseSize = static_cast<uint32_t>(base.size());
    uint32_t newSize  = static_cast<uint32_t>(newData.size());
    EmitU32(outDelta, baseSize);
    EmitU32(outDelta, newSize);

    size_t pos = 0;
    uint32_t rollPow = RollPow();
    uint32_t curHash = 0;
    size_t lastHashPos = SIZE_MAX;

    /// 滚动哈希 getter：相邻位置 O(1) 递推，跳跃时自动重算
    auto getHash = [&](size_t p) -> uint32_t {
        if (p == lastHashPos) return curHash;
        if (lastHashPos != SIZE_MAX && p == lastHashPos + 1) {
            curHash = RollAdvance(curHash, newData[p - 1],
                                  newData[p - 1 + kBlockSize], rollPow);
        } else {
            curHash = RollInit(newData.data() + p);
        }
        lastHashPos = p;
        return curHash;
    };

    while (pos < newData.size()) {
        uint32_t bestOff = 0, bestLen = 0;
        if (pos + kBlockSize <= newData.size()) {
            uint32_t h = getHash(pos);
            auto it = blockMap.find(h);
            if (it != blockMap.end()) {
                for (uint32_t boff : it->second) {
                    size_t avail = (std::min)(base.size() - boff, newData.size() - pos);
                    uint32_t len = ExtendMatch(&base[boff], &newData[pos], avail);
                    if (len > bestLen) {
                        bestLen = len;
                        bestOff = boff;
                    }
                }
            }
        }

        if (bestLen >= kMinMatch) {
            EmitCopy(outDelta, bestOff, bestLen);
            pos += bestLen;
            lastHashPos = SIZE_MAX; // 跳跃后失效，下次重新 init
        } else {
            size_t start = pos;
            while (pos < newData.size()) {
                bool found = false;
                if (pos + kBlockSize <= newData.size()) {
                    uint32_t h = getHash(pos);
                    auto it = blockMap.find(h);
                    if (it != blockMap.end()) {
                        for (uint32_t boff : it->second) {
                            size_t avail = (std::min)(base.size() - boff, newData.size() - pos);
                            uint32_t len = ExtendMatch(&base[boff], &newData[pos], avail);
                            if (len >= kMinMatch) { found = true; break; }
                        }
                    }
                }
                if (found) break;
                ++pos;
            }
            EmitInsert(outDelta, newData.data() + start,
                       static_cast<uint32_t>(pos - start));
        }
    }

    return true;
}

bool ApplyDelta(const std::vector<uint8_t>& baseData,
                const std::vector<uint8_t>& delta,
                std::vector<uint8_t>& outNew)
{
    if (delta.size() < 8) return false;

    uint32_t baseSize = delta[0] | (delta[1] << 8) | (delta[2] << 16) | (delta[3] << 24);
    uint32_t newSize  = delta[4] | (delta[5] << 8) | (delta[6] << 16) | (delta[7] << 24);

    if (baseData.size() != baseSize) return false;

    outNew.clear();
    outNew.reserve(newSize);

    size_t pos = 8;
    while (pos + 1 <= delta.size()) {
        uint8_t op = delta[pos++];
        if (pos + 4 > delta.size()) return false;

        if (op == 0x00) {
            if (pos + 8 > delta.size()) return false;
            uint32_t off = delta[pos] | (delta[pos+1] << 8) |
                          (delta[pos+2] << 16) | (delta[pos+3] << 24);
            uint32_t len = delta[pos+4] | (delta[pos+5] << 8) |
                          (delta[pos+6] << 16) | (delta[pos+7] << 24);
            pos += 8;
            if (off + len > baseData.size()) return false;
            outNew.insert(outNew.end(), baseData.begin() + off,
                          baseData.begin() + off + len);
        } else if (op == 0x01) {
            uint32_t len = delta[pos] | (delta[pos+1] << 8) |
                          (delta[pos+2] << 16) | (delta[pos+3] << 24);
            pos += 4;
            if (pos + len > delta.size()) return false;
            outNew.insert(outNew.end(), delta.begin() + pos,
                          delta.begin() + pos + len);
            pos += len;
        } else {
            return false;
        }
    }

    return outNew.size() == newSize;
}

bool ApplyDeltaToFile(const std::wstring& basePath,
                      const std::wstring& deltaPath,
                      const std::wstring& outputPath)
{
    auto base  = ReadEntireFile(basePath);
    auto delta = ReadEntireFile(deltaPath);
    if (base.empty() || delta.empty()) return false;

    std::vector<uint8_t> result;
    if (!ApplyDelta(base, delta, result)) return false;

    return WriteFile(outputPath, result);
}

} // namespace fssentry
