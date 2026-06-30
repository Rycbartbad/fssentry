/// @file process_tracker.cpp
/// @brief 进程白名单管理 —— Toolhelp32 快照扫描 + PID 过滤

#include "process_tracker.h"

#include <cwctype>
#include <string>
#include <tlhelp32.h>

namespace fssentry {

/// @brief 通配符匹配
/// @param pattern 含通配符的模式
/// @param str     要匹配的字符串
/// @return 是否匹配
static bool MatchWildcard(const std::wstring& pattern, const std::wstring& str)
{
    size_t pi = 0, si = 0;
    size_t starPi = std::wstring::npos, starSi = 0;
    while (si < str.size()) {
        if (pi < pattern.size() &&
            (pattern[pi] == L'?' || towlower(pattern[pi]) == towlower(str[si]))) {
            pi++; si++;
        } else if (pi < pattern.size() && pattern[pi] == L'*') {
            starPi = pi; starSi = si; pi++;
        } else if (starPi != std::wstring::npos) {
            pi = starPi + 1; si = ++starSi;
        } else {
            return false;
        }
    }
    while (pi < pattern.size() && pattern[pi] == L'*') pi++;
    return pi == pattern.size();
}

void ProcessTracker::LoadWhitelist(const std::vector<std::wstring>& names)
{
    whitelist_ = names;
}

void ProcessTracker::LoadBlacklist(const std::vector<std::wstring>& patterns)
{
    blacklist_.clear();
    for (auto& p : patterns) {
        std::wstring pat = p;
        for (auto& ch : pat) {
            if (ch == L'/') ch = L'\\';
        }
        blacklist_.push_back(std::move(pat));
    }
}

bool ProcessTracker::IsPathBlacklisted(const std::wstring& filePath) const
{
    for (const auto& bp : blacklist_) {
        if (MatchWildcard(bp, filePath)) return true;
    }
    return false;
}

std::set<DWORD> ProcessTracker::DiscoverRunningProcesses()
{
    std::set<DWORD> newPids;
    HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnap == INVALID_HANDLE_VALUE) return newPids;

    PROCESSENTRY32W pe = { sizeof(pe) };
    if (Process32FirstW(hSnap, &pe)) {
        do {
            for (const auto& wl : whitelist_) {
                if (_wcsicmp(pe.szExeFile, wl.c_str()) == 0) {
                    if (monitoredPids_.find(pe.th32ProcessID) == monitoredPids_.end()) {
                        monitoredPids_.insert(pe.th32ProcessID);
                        newPids.insert(pe.th32ProcessID);
                    }
                    break;
                }
            }
        } while (Process32NextW(hSnap, &pe));
    }
    CloseHandle(hSnap);
    return newPids;
}

bool ProcessTracker::AddProcess(DWORD pid, const std::wstring& /*imagePath*/)
{
    if (pid == 0) return false;
    monitoredPids_.insert(pid);
    return true;
}

void ProcessTracker::RemoveProcess(DWORD pid)
{
    monitoredPids_.erase(pid);
}

bool ProcessTracker::IsMonitored(DWORD pid) const
{
    return monitoredPids_.find(pid) != monitoredPids_.end();
}

std::set<DWORD> ProcessTracker::GetMonitoredPids() const
{
    return monitoredPids_;
}

} // namespace fssentry
