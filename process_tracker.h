/// @file process_tracker.h
/// @brief 进程白名单管理（PID 过滤）
#pragma once

#include <cstdint>
#include <set>
#include <string>
#include <vector>
#include <windows.h>

namespace fssentry {

class ProcessTracker {
public:
    void LoadWhitelist(const std::vector<std::wstring>& names);
    void LoadBlacklist(const std::vector<std::wstring>& patterns);

    /// @brief 扫描当前运行进程，匹配白名单并添加到监控列表，返回新发现的 PID
    std::set<DWORD> DiscoverRunningProcesses();

    bool AddProcess(DWORD pid, const std::wstring& imagePath);
    void RemoveProcess(DWORD pid);
    bool IsMonitored(DWORD pid) const;
    bool IsPathBlacklisted(const std::wstring& filePath) const;
    std::set<DWORD> GetMonitoredPids() const;

private:
    std::vector<std::wstring> whitelist_;
    std::vector<std::wstring> blacklist_;
    std::set<DWORD> monitoredPids_;
};

} // namespace fssentry
