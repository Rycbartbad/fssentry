/// @file version_tracker.h
/// @brief 内存态文件版本追踪器（hash 对比 + 版本计数）
#pragma once

#include <string>
#include <unordered_map>

namespace fssentry {

struct VersionState {
    std::string lastHash;
    int versionCount = 0;
};

class VersionTracker {
public:
    /// @brief hash 变化或尚无基准文件时返回 true，表示需要快照
    bool ShouldSnapshot(const std::wstring& path, const std::string& newHash);

    /// @brief 记录已存储的快照（更新 hash + 递增版本号）
    void RecordSnapshot(const std::wstring& path,
                        const std::string& hash);

    int GetVersionCount(const std::wstring& path) const;
    bool HasBaseFile(const std::wstring& path) const;

    /// @brief Rebase 后重置版本计数
    /// @param path     文件路径
    /// @param newCount 新的版本号（keep 值）
    void ResetAfterRebase(const std::wstring& path, int newCount);

    /// @brief 设置文件的状态（用于从磁盘恢复）
    /// @param path      文件路径
    /// @param vc        版本数
    /// @param hash      最新快照的 hash
    void SetState(const std::wstring& path, int vc, const std::string& hash);

private:
    VersionState& GetOrCreate(const std::wstring& path);
    std::unordered_map<std::wstring, VersionState> files_;
};

} // namespace fssentry
