/// @file version_tracker.cpp
/// @brief 内存态文件版本追踪器 —— hash 对比 + 版本计数

#include "version_tracker.h"

namespace fssentry {

/// @brief 获取或创建路径对应的追踪状态
/// @param path 规范化后的文件路径
/// @return 版本状态引用
VersionState& VersionTracker::GetOrCreate(const std::wstring& path)
{
    auto it = files_.find(path);
    if (it != files_.end()) return it->second;
    auto newIt = files_.emplace(path, VersionState{}).first;
    return newIt->second;
}

bool VersionTracker::ShouldSnapshot(const std::wstring& path,
                                    const std::string& newHash)
{
    auto& st = GetOrCreate(path);
    if (st.versionCount == 0) return true;
    if (st.lastHash != newHash) return true;
    return false;
}

void VersionTracker::RecordSnapshot(const std::wstring& path,
                                    const std::string& hash)
{
    auto& st = GetOrCreate(path);
    st.lastHash = hash;
    ++st.versionCount;
}

int VersionTracker::GetVersionCount(const std::wstring& path) const
{
    auto it = files_.find(path);
    return it != files_.end() ? it->second.versionCount : 0;
}

bool VersionTracker::HasBaseFile(const std::wstring& path) const
{
    auto it = files_.find(path);
    return it != files_.end() && it->second.versionCount > 0;
}

void VersionTracker::ResetAfterRebase(const std::wstring& path, int newCount)
{
    auto it = files_.find(path);
    if (it != files_.end()) it->second.versionCount = newCount;
}

void VersionTracker::SetState(const std::wstring& path, int vc, const std::string& hash)
{
    auto& st = GetOrCreate(path);
    st.versionCount = vc;
    st.lastHash     = hash;
}

} // namespace fssentry
