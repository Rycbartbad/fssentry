/// @file main.cpp
/// @brief fssentry — ETW 内核文件 I/O 监控 + 快照版本化

#include "etw_capture.h"
#include "process_tracker.h"
#include "snapshot_store.h"
#include "version_tracker.h"
#include "hash.h"
#include "cli.h"

#include <conio.h>
#include <cwchar>
#include <cwctype>
#include <iostream>
#include <string>
#include <vector>
#include <queue>
#include <set>
#include <map>
#include <algorithm>
#include <shellapi.h>

using namespace fssentry;

static SRWLOCK           g_queueLock  = SRWLOCK_INIT;
static std::queue<EventInfo> g_eventQueue;
static std::wstring      g_virtDir;           ///< 虚拟文件系统当前目录
static bool              g_monitor = false;   ///< 是否处于监控显示模式



/// @brief 启用 ETW 所需特权（SeSystemProfilePrivilege + SeDebugPrivilege）
static bool EnablePrivileges() {
    HANDLE hToken = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(),
            TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &hToken))
        return false;

    auto enablePriv = [&](const wchar_t* name) {
        LUID luid;
        if (LookupPrivilegeValueW(nullptr, name, &luid)) {
            TOKEN_PRIVILEGES tp = { 1, {{luid, SE_PRIVILEGE_ENABLED}} };
            AdjustTokenPrivileges(hToken, FALSE, &tp, 0, nullptr, nullptr);
        }
    };
    enablePriv(L"SeSystemProfilePrivilege");
    enablePriv(L"SeDebugPrivilege");
    CloseHandle(hToken);
    return true;
}

int main()
{
    bool noWtRelaunch = false;
    {
        int argc = 0;
        LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
        for (int i = 1; i < argc; ++i) {
            if (wcscmp(argv[i], L"--no-wt") == 0) {
                noWtRelaunch = true;
                break;
            }
        }
        LocalFree(argv);
    }

    if (!noWtRelaunch && GetEnvironmentVariableW(L"WT_SESSION", nullptr, 0) == 0) {
        wchar_t exePath[MAX_PATH], cwd[MAX_PATH];
        GetModuleFileNameW(nullptr, exePath, MAX_PATH);
        GetCurrentDirectoryW(MAX_PATH, cwd);

        wchar_t args[MAX_PATH * 2 + 80];
        swprintf_s(args, L"-w 0 -d \"%s\" -- \"%s\" --no-wt", cwd, exePath);

        FreeConsole();
        ShellExecuteW(nullptr, L"open", L"wt.exe", args, nullptr, SW_SHOW);
        return 0;
    }

    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);

    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    if (hOut != INVALID_HANDLE_VALUE && hOut != nullptr) {
        DWORD mode = 0;
        if (GetConsoleMode(hOut, &mode))
            SetConsoleMode(hOut, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
    }

    constexpr const wchar_t* kProcesses[] = { L"opencode.exe", L"claude.exe" };
    constexpr const wchar_t* kBlacklist[] = {
        // opencode黑名单
        L"c:\\$extend\\$deleted\\*", L"%USERPROFILE%\\.opencode\\*", 
        L"*\\git.*", L"%USERPROFILE%\\.local*",
        L"%USERPROFILE%\\appdata\\local\\temp*", L"*\\cmd*",
        L"%ProgramData%\\fssentry\\repo", L"*\\powershell.*",
        L"%USERPROFILE%\\.config\\opencode*", L"*\\grep.*",
        L"%ProgramFiles%\\windowsapps*", L"%USERPROFILE%\\.cache*",
        L"*\\.git\\*", L"*\\.sisyphus*", L"%USERPROFILE%\\.bun*",
        L"%USERPROFILE%\\.npmrc*", L"*\\opencode.exe", L"%SystemRoot%*",
        L"*\\.agents*", L"%USERPROFILE%\\appdata\\local\\*",
        L"%USERPROFILE%\\package.json", L"%ProgramFiles(x86)%\\windows kits*",

        // claude黑名单
        L"*\\.claude\\*", L"%USERPROFILE%\\appdata\\roaming\\npm\\node_modules\\@anthropic-ai\\claude-code\\*",
        L"%ProgramFiles%\\claudecode\\*", L"*\\bash.exe", L"*\\etc\\ssl*", 
        L"%USERPROFILE%\\appdata\\roaming\\anthropic\\*", L"*\\.mcp.json", 
        L"%USERPROFILE%\\appdata\\roaming\\opera software\\opera stable*",
        L"*\\claude.md", L"*\\uvx.*", L"*\\.claude.json"
    };

    // 展开环境变量
    std::vector<std::wstring> blacklist;
    blacklist.reserve(std::size(kBlacklist));
    for (const auto* p : kBlacklist) {
        wchar_t buf[MAX_PATH];
        ExpandEnvironmentStringsW(p, buf, MAX_PATH);
        blacklist.push_back(buf);
    }
    constexpr int kMaxVersions = 50;
    constexpr int kRebaseKeep  = 10;

    EnablePrivileges();

    ProcessTracker tracker;
    tracker.LoadWhitelist({kProcesses, kProcesses + sizeof(kProcesses) / sizeof(kProcesses[0])});
    tracker.LoadBlacklist(blacklist);
    tracker.DiscoverRunningProcesses();

    wchar_t repoBuf[MAX_PATH] = {};
    ExpandEnvironmentStringsW(L"%ProgramData%\\fssentry\\repo", repoBuf, MAX_PATH);
    SnapshotStore  store(repoBuf);
    VersionTracker verTracker;

    // 从已有快照恢复版本计数和 lastHash
    auto tracked = store.ListTrackedPaths();
    for (const auto& p : tracked) {
        auto versions = store.GetVersions(p);
        int vc = static_cast<int>(versions.size());
        std::string hash;
        if (vc > 0) {
            wchar_t tmpPath[MAX_PATH];
            GetTempPathW(MAX_PATH, tmpPath);
            wcscat_s(tmpPath, MAX_PATH, L"fssentry_recover.tmp");
            if (store.ReconstructVersion(p, vc - 1, tmpPath)) {
                hash = ComputeFileHash(tmpPath);
                DeleteFileW(tmpPath);
            }
        }
        verTracker.SetState(p, vc, hash.empty() ? "" : hash);
    }

    std::cout << "\n仓库路径: " << narrow(repoBuf) << "\n" << std::endl;

    EtwCapture capture;

    capture.SetEventCallback([&](const EventInfo& info) {
        // 跳过 Name 事件
        if (info.opcode == 0 || info.opcode == 32 || info.opcode == 35) return;

        // 跳过非文件路径
        DWORD attr = GetFileAttributesW(info.path.c_str());
        if (attr == INVALID_FILE_ATTRIBUTES || (attr & FILE_ATTRIBUTE_DIRECTORY))
            return;

        // 快照处理（Write/Close 触发）
        if (info.opcode == 66 || info.opcode == 68 || info.opcode == 69) {
            std::string hash = ComputeFileHash(info.path);
            if (!hash.empty() && verTracker.ShouldSnapshot(info.path, hash)) {
                int prevVer = verTracker.HasBaseFile(info.path)
                    ? verTracker.GetVersionCount(info.path) - 1
                    : -1;
                auto si = store.StoreSnapshot(info.path, info.timestamp, prevVer);
                if (si.version >= 0) {
                    verTracker.RecordSnapshot(info.path, hash);
                    int vc = verTracker.GetVersionCount(info.path);
                    if (kMaxVersions > 0 && vc >= kMaxVersions) {
                        if (store.RebaseChain(info.path, vc - 1, kRebaseKeep))
                            verTracker.ResetAfterRebase(info.path, kRebaseKeep);
                    }
                }
            }
        }

        // 事件合并：同进程+同路径+同秒内的事件合并去重
        static std::wstring s_key;
        static int s_sec = -1;
        static uint64_t s_opBits[2];
        static std::string s_ops;
        static EventInfo s_last = {};

        FILETIME ft;
        ft.dwLowDateTime  = static_cast<DWORD>(info.timestamp & 0xFFFFFFFF);
        ft.dwHighDateTime = static_cast<DWORD>(info.timestamp >> 32);
        SYSTEMTIME st;
        FileTimeToSystemTime(&ft, &st);
        int curSec = st.wHour * 3600 + st.wMinute * 60 + st.wSecond;

        wchar_t key[1024];
        swprintf_s(key, L"%s|%s", info.processName.c_str(), info.path.c_str());
        std::wstring curKey(key);

        auto flush = [&]() {
            if (s_ops.empty()) return;
            s_last.opName = s_ops;
            AcquireSRWLockExclusive(&g_queueLock);
            g_eventQueue.push(s_last);
            ReleaseSRWLockExclusive(&g_queueLock);
        };

        int idx = (info.opcode < 64) ? 0 : 1;
        uint64_t bit = static_cast<uint64_t>(1) << (info.opcode & 63);
        if (curKey == s_key && curSec == s_sec) {
            if (!(s_opBits[idx] & bit)) {
                s_opBits[idx] |= bit;
                s_ops += ", ";
                s_ops += OpName(info.opcode);
            }
            return;
        }
        flush();
        s_key = curKey;
        s_sec = curSec;
        s_opBits[0] = s_opBits[1] = 0;
        s_opBits[idx] = bit;
        s_ops = OpName(info.opcode);
        s_last = info;
    });

    if (!capture.Start(tracker)) {
        std::cout << "启动 ETW 监控失败 (错误码: " << capture.LastError() << ")" << std::endl;
        return 1;
    }

    std::cout << "开始监控... 输入 'help' 查看可用命令\n" << std::endl;

    std::wstring cmdBuf;
    bool running = true;
    std::cout << "> " << std::flush;

    std::vector<std::wstring> history;
    size_t histIdx = 0, cursorPos = 0;
    HANDLE hCon = GetStdHandle(STD_OUTPUT_HANDLE);
    auto fullRedraw = [&]() {
        std::wstring prompt = g_virtDir.empty() ? L">" : FromVirt(g_virtDir) + L">";
        std::wstring fullLine = prompt + L" " + cmdBuf;
        CONSOLE_SCREEN_BUFFER_INFO csbi;
        GetConsoleScreenBufferInfo(hCon, &csbi);
        SHORT y = csbi.dwCursorPosition.Y;
        DWORD written;
        WriteConsoleOutputCharacterW(hCon, fullLine.c_str(),
            static_cast<DWORD>(fullLine.size()), {0, y}, &written);
        if (fullLine.size() < csbi.dwSize.X) {
            FillConsoleOutputCharacterW(hCon, L' ',
                csbi.dwSize.X - static_cast<DWORD>(fullLine.size()),
                {static_cast<SHORT>(fullLine.size()), y}, &written);
        }
        SetConsoleCursorPosition(hCon, {static_cast<SHORT>(prompt.size() + 1 + cursorPos), y});
    };
    auto cursorTo = [&]() {
        std::wstring prompt = g_virtDir.empty() ? L">" : FromVirt(g_virtDir) + L">";
        CONSOLE_SCREEN_BUFFER_INFO csbi;
        GetConsoleScreenBufferInfo(hCon, &csbi);
        SetConsoleCursorPosition(hCon, {static_cast<SHORT>(prompt.size() + 1 + cursorPos), csbi.dwCursorPosition.Y});
    };

    while (running) {
        if (_kbhit()) {
            wint_t wc = _getwch();
            if (wc == 0xE0 || wc == 0) {
                wint_t ext = _getwch();
                if (ext == 75) { // ←
                    if (cursorPos > 0) { --cursorPos; cursorTo(); }
                } else if (ext == 77) { // →
                    if (cursorPos < cmdBuf.size()) { ++cursorPos; cursorTo(); }
                } else if (ext == 72) { // ↑
                    if (histIdx < history.size()) {
                        ++histIdx;
                        cmdBuf = history[history.size() - histIdx];
                        cursorPos = cmdBuf.size();
                        fullRedraw();
                    }
                } else if (ext == 80) { // ↓
                    if (histIdx > 0) {
                        --histIdx;
                        cmdBuf = histIdx == 0 ? L"" : history[history.size() - histIdx];
                        cursorPos = cmdBuf.size();
                        fullRedraw();
                    }
                } else if (ext == 83) { // Delete
                    if (cursorPos < cmdBuf.size()) {
                        cmdBuf.erase(cursorPos, 1);
                        fullRedraw();
                    }
                }
            } else if (wc == L'\r' || wc == L'\n') {
                _putwch(L'\n');
                if (!cmdBuf.empty() && (history.empty() || cmdBuf != history.back()))
                    history.push_back(cmdBuf);
                histIdx = 0;
                std::wstring cmd = std::move(cmdBuf);
                cmdBuf.clear();
                cursorPos = 0;

                const wchar_t* ws = L" \t";
                size_t first = cmd.find_first_not_of(ws);
                size_t last  = cmd.find_last_not_of(ws);
                if (first == std::wstring::npos) {
                    PrintPrompt(g_virtDir);
                    continue;
                }
                cmd = cmd.substr(first, last - first + 1);

                std::vector<std::wstring> parts;
                {
                    size_t s = 0;
                    while (s < cmd.size()) {
                        size_t sp = cmd.find(L' ', s);
                        std::wstring part = cmd.substr(s, sp - s);
                        size_t pf = part.find_first_not_of(ws);
                        if (pf != std::wstring::npos) {
                            part = part.substr(pf);
                            size_t pl = part.find_last_not_of(ws);
                            if (pl != std::wstring::npos)
                                part.resize(pl + 1);
                            parts.push_back(part);
                        }
                        if (sp == std::wstring::npos) break;
                        s = sp + 1;
                    }
                }
                if (parts.empty()) {
                    PrintPrompt(g_virtDir);
                continue;
            }

                const std::wstring& cmdName = parts[0];

                // 命令注册表（首次使用时初始化）
                static CommandRegistry s_reg;
                static bool s_init = false;
                if (!s_init) {
                    RegisterBuiltinCommands(s_reg);
                    s_init = true;
                }

                CliContext ctx = {
                    store, verTracker, g_virtDir, g_monitor,
                    g_eventQueue, g_queueLock, capture, running
                };

                Command* pCmd = s_reg.Find(cmdName);
                if (pCmd) {
                    pCmd->Execute(parts, ctx);
                } else {
                    std::cout << "未知命令: " << narrow(cmdName)
                              << " (输入 'help' 查看可用命令)" << std::endl;
                }
                PrintPrompt(g_virtDir);
            } else if (wc == L'\b' || wc == 127) {
                if (cursorPos > 0) {
                    cmdBuf.erase(cursorPos - 1, 1);
                    --cursorPos;
                    fullRedraw();
                }
            } else if (wc >= 32 && wc != 127) {
                cmdBuf.insert(cursorPos, 1, static_cast<wchar_t>(wc));
                ++cursorPos;
                fullRedraw();
            }
        } else {
            if (!g_monitor && !g_eventQueue.empty()) {
                AcquireSRWLockExclusive(&g_queueLock);
                g_eventQueue = {};
                ReleaseSRWLockExclusive(&g_queueLock);
            }
            if (!capture.IsRunning())
                running = false;
            else
                Sleep(50);
        }
    }

    capture.Stop();

    std::cout << "已停止" << std::endl;
    return 0;
}
