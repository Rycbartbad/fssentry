/// @file cli.cpp
/// @brief CLI 命令多态实现 — Command 模式的具体命令类
#include "cli.h"
#include "etw_capture.h"
#include "snapshot_store.h"
#include "version_tracker.h"
#include "hash.h"

#include <algorithm>
#include <conio.h>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <map>
#include <set>
#include <string>
#include <vector>

#include <shellapi.h>

namespace fssentry {
namespace {

/// @brief 解析参数中的路径：支持相对/绝对/带冒号
std::wstring ResolvePath(const std::wstring& part,
                          const std::wstring& virtDir)
{
    if (part.size() >= 2 && part[1] == L':')
        return part;
    if (virtDir.empty())
        return FromVirt(part);
    return virtDir + L"\\" + part;
}

} // anonymous namespace

std::wstring ToVirt(const std::wstring& path) {
    std::wstring v; v.reserve(path.size());
    for (size_t i = 0; i < path.size(); ++i) {
        if (i == 1 && path[i] == L':') continue;
        v.push_back(path[i]);
    }
    return v;
}

std::wstring FromVirt(const std::wstring& path) {
    if (path.empty()) return path;
    if (path.size() >= 2 && path[1] == L':') return path;
    std::wstring a; a.push_back(path[0]); a.push_back(L':');
    a.append(path.begin() + 1, path.end());
    return a;
}

std::string narrow(const std::wstring& ws) {
    if (ws.empty()) return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string s(len, '\0');
    WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), -1, &s[0], len, nullptr, nullptr);
    while (!s.empty() && s.back() == '\0') s.pop_back();
    return s;
}

const char* OpName(uint8_t op) {
    switch (op) {
        case 0:  return "Nm";   case 32: return "FC";   case 35: return "FD";
        case 64: return "Crt";  case 65: return "Cln";  case 66: return "Cls";
        case 67: return "Rd";   case 68: return "Wr";   case 69: return "Set";
        case 70: return "Del";  case 71: return "Ren";  case 72: return "DE";
        case 73: return "Fl";   case 74: return "QI";   case 75: return "FSC";
        case 76: return "Op";   case 77: return "DN";
        default: return "?";
    }
}

std::string FormatEvent(const EventInfo& e) {
    const char* opName = e.opName.empty() ? OpName(e.opcode) : e.opName.c_str();
    std::string proc  = narrow(e.processName);
    std::string fpath = narrow(e.path);

    FILETIME ft;
    ft.dwLowDateTime  = static_cast<DWORD>(e.timestamp & 0xFFFFFFFF);
    ft.dwHighDateTime = static_cast<DWORD>(e.timestamp >> 32);
    SYSTEMTIME st;
    FileTimeToSystemTime(&ft, &st);

    char buf[1024];
    snprintf(buf, sizeof(buf), "%02d:%02d:%02d  %-26s  %-16s  %5u  %s",
             st.wHour, st.wMinute, st.wSecond,
             opName, proc.c_str(), e.pid, fpath.c_str());
    return buf;
}

void PrintPrompt(const std::wstring& virtDir) {
    std::cout << std::endl
              << (virtDir.empty() ? ">" : narrow(FromVirt(virtDir)) + ">")
              << " " << std::flush;
}

void DrainEvents(CliContext& ctx) {
    AcquireSRWLockExclusive(&ctx.queueLock);
    while (ctx.monitor && !ctx.eventQueue.empty()) {
        EventInfo e = std::move(ctx.eventQueue.front());
        ctx.eventQueue.pop();
        ReleaseSRWLockExclusive(&ctx.queueLock);
        std::cout << FormatEvent(e) << std::endl;
        AcquireSRWLockExclusive(&ctx.queueLock);
    }
    if (!ctx.monitor) ctx.eventQueue = {};
    ReleaseSRWLockExclusive(&ctx.queueLock);
}

std::vector<std::wstring> ParseCommandLine(const std::wstring& cmd) {
    const wchar_t* ws = L" \t";
    std::vector<std::wstring> parts;
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
    return parts;
}

void CommandRegistry::Register(Command* cmd) {
    if (cmd) cmds_[cmd->Name()] = cmd;
}

void CommandRegistry::RegisterAlias(const std::wstring& name, Command* cmd) {
    if (cmd) cmds_[name] = cmd;
}

Command* CommandRegistry::Find(const std::wstring& name) const {
    std::wstring lower = name;
    for (auto& c : lower) c = towlower(c);
    for (const auto& pair : cmds_) {
        std::wstring k = pair.first;
        for (auto& c : k) c = towlower(c);
        if (k == lower) return pair.second;
    }
    return nullptr;
}

void Command::Execute(const std::vector<std::wstring>& args,
                       CliContext& ctx)
{
    for (size_t i = 1; i < args.size(); ++i) {
        if (args[i] == L"-h" || args[i] == L"--help") {
            std::cout << narrow(Usage()) << std::endl;
            return;
        }
    }
    Run(args, ctx);
}

class LsCommand : public Command {
public:
    const wchar_t* Name() const override { return L"ls"; }
    const wchar_t* Description() const override { return L"列出当前目录"; }
    void Run(const std::vector<std::wstring>& /*args*/, CliContext& ctx) override {
        auto paths = ctx.store.ListTrackedPaths();
        auto gvCmp = ToVirt(ctx.virtDir);
        size_t gvLen = gvCmp.size();
        std::set<std::wstring> dirs, files;
        for (const auto& p : paths) {
            std::wstring vp = ToVirt(p);
            if (ctx.virtDir.empty() || (vp.size() > gvLen + 1 &&
                _wcsnicmp(vp.c_str(), gvCmp.c_str(), gvLen) == 0 &&
                vp[gvLen] == L'\\'))
            {
                size_t start = ctx.virtDir.empty() ? 0 : gvLen + 1;
                size_t end = vp.find(L'\\', start);
                std::wstring child = vp.substr(start, end - start);
                if (end != std::wstring::npos) dirs.insert(child);
                else files.insert(child);
            }
        }
        for (const auto& d : dirs)
            std::cout << "  " << narrow(d) << (d.size() == 1 ? ":/" : "/") << std::endl;
        for (const auto& f : files) {
            std::wstring full = FromVirt(ctx.virtDir.empty() ? f : ctx.virtDir + L"\\" + f);
            int vc = ctx.verTracker.GetVersionCount(full);
            std::cout << "  " << narrow(f) << "  [" << vc << "v]" << std::endl;
        }
    }
};

class CdCommand : public Command {
public:
    const wchar_t* Name() const override { return L"cd"; }
    const wchar_t* Description() const override { return L"切换目录（.. 返回上级，/ 回到根）"; }
    std::wstring Usage() const override { return L"cd <目录> — 切换目录（.. 返回上级，/ 回到根）"; }
    void Run(const std::vector<std::wstring>& args, CliContext& ctx) override {
        if (args.size() < 2) {
            std::cout << narrow(Usage()) << std::endl;
            return;
        }
        if (args[1] == L"..") {
            auto pos = ctx.virtDir.rfind(L'\\');
            ctx.virtDir = pos == std::wstring::npos ? L"" : ctx.virtDir.substr(0, pos);
        } else if (args[1] == L"/" || args[1] == L"\\") {
            ctx.virtDir = L"";
        } else {
            std::wstring dir = args[1];
            while (!dir.empty() && (dir.back() == L'/' || dir.back() == L'\\'))
                dir.pop_back();
            if (dir.empty()) return;
            std::wstring target;
            if (dir.size() >= 2 && dir[1] == L':')
                target = dir;
            else if (ctx.virtDir.empty())
                target = FromVirt(dir);
            else
                target = ctx.virtDir + L"\\" + dir;

            std::wstring tgtCmp = ToVirt(target);
            auto paths = ctx.store.ListTrackedPaths();
            std::wstring actual;
            for (const auto& p : paths) {
                std::wstring vp = ToVirt(p);
                if (vp.size() > tgtCmp.size() &&
                    _wcsnicmp(vp.c_str(), tgtCmp.c_str(), tgtCmp.size()) == 0 &&
                    vp[tgtCmp.size()] == L'\\')
                {
                    std::wstring matched = vp.substr(0, tgtCmp.size());
                    actual = FromVirt(matched);
                    break;
                }
            }
            if (!actual.empty()) {
                ctx.virtDir = actual;
            } else {
                std::cout << "  " << narrow(args[1]) << ": 目录不存在" << std::endl;
            }
        }
    }
};

class TreeCommand : public Command {
public:
    const wchar_t* Name() const override { return L"tree"; }
    const wchar_t* Description() const override { return L"树状显示版本和分支"; }
    std::wstring Usage() const override { return L"tree [文件路径] — 树状显示版本和分支"; }

    void Run(const std::vector<std::wstring>& args, CliContext& ctx) override {
        if (args.size() >= 2) {
            std::wstring wpath = ResolvePath(args[1], ctx.virtDir);
            if (!wpath.empty()) {
                std::wstring disp = wpath;
                std::wstring vp = ToVirt(wpath);
                for (const auto& tp : ctx.store.ListTrackedPaths()) {
                    if (_wcsnicmp(ToVirt(tp).c_str(), vp.c_str(), vp.size()) == 0 &&
                        ToVirt(tp).size() == vp.size()) {
                        disp = tp; break;
                    }
                }
                std::wstring rel;
                for (size_t i = 0; i < disp.size(); ++i) {
                    if (i == 1 && disp[i] == L':') continue;
                    rel.push_back(disp[i]);
                }
                std::wstring snapDir = ctx.store.SnapshotsPath() + L"\\" + rel;
                std::cout << narrow(disp) << std::endl;
                PrintSnapTree(snapDir, "", ctx);
            }
        } else {
            PrintDirTree(ctx.virtDir, "", ctx);
        }
    }

private:
    /// @brief 递归打印快照树（版本 + 分支）
    static void PrintSnapTree(const std::wstring& snapDir,
                               const std::string& prefix,
                               CliContext& ctx)
    {
        auto vers = ctx.store.GetVersionsFromDir(snapDir);
        std::wstring brDir = snapDir + L"\\.branches";
        DWORD brAttr = GetFileAttributesW(brDir.c_str());
        bool hasBranches = (brAttr != INVALID_FILE_ATTRIBUTES &&
                            (brAttr & FILE_ATTRIBUTE_DIRECTORY));
        std::vector<std::wstring> brNames;
        if (hasBranches) {
            WIN32_FIND_DATAW ffd;
            HANDLE hF = FindFirstFileW((brDir + L"\\*").c_str(), &ffd);
            if (hF != INVALID_HANDLE_VALUE) {
                do {
                    if (wcscmp(ffd.cFileName, L".") == 0 ||
                        wcscmp(ffd.cFileName, L"..") == 0) continue;
                    if (ffd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                        brNames.push_back(ffd.cFileName);
                } while (FindNextFileW(hF, &ffd));
                FindClose(hF);
            }
        }
        for (size_t i = 0; i < vers.size(); ++i) {
            const auto& v = vers[i];
            FILETIME ft;
            ft.dwLowDateTime  = static_cast<DWORD>(v.timestamp & 0xFFFFFFFF);
            ft.dwHighDateTime = static_cast<DWORD>(v.timestamp >> 32);
            FILETIME local;
            FileTimeToLocalFileTime(&ft, &local);
            SYSTEMTIME st;
            FileTimeToSystemTime(&local, &st);
            bool lastVer = (i == vers.size() - 1) && brNames.empty();
            std::cout << prefix << (lastVer ? "└─ " : "├─ ")
                      << "v" << v.version << "  "
                      << st.wYear << "/" << st.wMonth << "/" << st.wDay
                      << " " << st.wHour << ":"
                      << (st.wMinute < 10 ? "0" : "") << st.wMinute
                      << "  " << v.fileSize << " bytes" << std::endl;
        }
        if (!brNames.empty()) {
            std::cout << prefix << "└─ .branches" << std::endl;
            std::string brPrefix = prefix + "   ";
            for (size_t i = 0; i < brNames.size(); ++i) {
                bool lastBr = (i == brNames.size() - 1);
                std::wstring bdir = brDir + L"\\" + brNames[i];
                std::cout << brPrefix
                          << (lastBr ? "└─ " : "├─ ")
                          << narrow(brNames[i]) << std::endl;
                PrintSnapTree(bdir, brPrefix + (lastBr ? "   " : "│  "), ctx);
            }
        }
    }

    /// @brief 递归打印目录树（文件 + 子目录）
    static void PrintDirTree(const std::wstring& vpath,
                              const std::string& prefix,
                              CliContext& ctx)
    {
        auto allPaths = ctx.store.ListTrackedPaths();
        std::set<std::wstring> dirs;
        std::map<std::wstring, std::wstring> fileMap;
        auto vpCmp = ToVirt(vpath);
        size_t vplen = vpCmp.size();
        for (const auto& p : allPaths) {
            std::wstring vp = ToVirt(p);
            bool under = vplen == 0 ||
                (vp.size() > vplen && vp[vplen] == L'\\' &&
                 _wcsnicmp(vp.c_str(), vpCmp.c_str(), vplen) == 0);
            if (!under) continue;
            std::wstring rest = vplen == 0 ? vp : vp.substr(vplen + 1);
            size_t slash = rest.find(L'\\');
            if (slash != std::wstring::npos)
                dirs.insert(rest.substr(0, slash));
            else
                fileMap[rest] = p;
        }
        std::vector<std::wstring> allNames;
        for (const auto& d : dirs) allNames.push_back(d);
        for (const auto& f : fileMap) allNames.push_back(f.first);
        std::sort(allNames.begin(), allNames.end());
        bool isRoot = vpath.empty();
        for (size_t i = 0; i < allNames.size(); ++i) {
            bool last = (i == allNames.size() - 1);
            const auto& name = allNames[i];
            if (dirs.count(name)) {
                std::wstring subVpath = vpath.empty() ? name : vpath + L"\\" + name;
                if (isRoot)
                    std::cout << prefix << narrow(name) << ":/" << std::endl;
                else
                    std::cout << prefix
                              << (last ? "└─ " : "├─ ")
                              << narrow(name) << "/" << std::endl;
                PrintDirTree(subVpath, prefix + (isRoot ? "" : (last ? "   " : "│  ")), ctx);
            } else {
                int vc = ctx.verTracker.GetVersionCount(fileMap[name]);
                std::cout << prefix
                          << (last ? "└─ " : "├─ ")
                          << narrow(name) << "  [" << vc << "v]" << std::endl;
            }
        }
    }
};

class RestoreCommand : public Command {
public:
    const wchar_t* Name() const override { return L"restore"; }
    const wchar_t* Description() const override { return L"恢复文件到指定版本（自动创建分支）"; }
    std::wstring Usage() const override { return L"restore <文件路径> <版本号> — 恢复文件到指定版本"; }
    void Run(const std::vector<std::wstring>& args, CliContext& ctx) override {
        if (args.size() < 3) {
            std::cout << narrow(Usage()) << std::endl;
            return;
        }
        std::wstring wpath = ResolvePath(args[1], ctx.virtDir);
        std::wstring verStr = args[2];
        if (wpath.empty()) {
            std::cout << "未指定文件路径" << std::endl;
            return;
        }
        if (!verStr.empty() && verStr[0] == L'v') verStr.erase(0, 1);
        int version = std::stoi(verStr);
        auto vers = ctx.store.GetVersions(wpath);
        int latest = static_cast<int>(vers.size()) - 1;
        if (version < latest) {
            char buf[64];
            SYSTEMTIME st; GetLocalTime(&st);
            snprintf(buf, sizeof(buf), "fork_v%d_%04d%02d%02dT%02d%02d%02d",
                    version, st.wYear, st.wMonth, st.wDay,
                    st.wHour, st.wMinute, st.wSecond);
            ctx.store.CopyToBranch(wpath, version + 1, buf);
            ctx.verTracker.SetState(wpath, version + 1, "");
        }
        ctx.store.DeleteVersions(wpath, version + 1);
        if (ctx.store.RestoreToFile(wpath, version, wpath))
            std::cout << "已恢复到 v" << version << " -> " << narrow(wpath) << std::endl;
        else
            std::cout << "恢复失败" << std::endl;
    }
};

class ShowCommand : public Command {
public:
    const wchar_t* Name() const override { return L"show"; }
    const wchar_t* Description() const override { return L"临时查看或导出指定版本的文件内容"; }
    std::wstring Usage() const override { return L"show <文件路径> <版本号> [-b <分支>] [-o <输出路径>] — 临时查看或导出指定版本的文件内容"; }
    void Run(const std::vector<std::wstring>& args, CliContext& ctx) override {
        if (args.size() < 3) {
            std::cout << narrow(Usage()) << std::endl;
            return;
        }
        std::wstring wpath = ResolvePath(args[1], ctx.virtDir);
        std::wstring verStr = args[2];
        std::string branchPath;
        std::wstring outPath;
        for (size_t ai = 3; ai + 1 < args.size(); ++ai) {
            if (args[ai] == L"-b") branchPath = narrow(args[ai + 1]);
            else if (args[ai] == L"-o") outPath = args[ai + 1];
        }
        if (wpath.empty()) {
            std::cout << "未指定文件路径" << std::endl;
            return;
        }
        if (!verStr.empty() && verStr[0] == L'v') verStr.erase(0, 1);
        int version = std::stoi(verStr);
        std::vector<uint8_t> content;
        if (!branchPath.empty())
            content = ctx.store.GetBranchContent(wpath, branchPath, version);
        else
            content = ctx.store.GetContent(wpath, version);
        if (content.empty()) {
            std::cout << "读取 v" << version << " 失败或版本不存在" << std::endl;
        } else if (!outPath.empty()) {
            HANDLE h = CreateFileW(outPath.c_str(), GENERIC_WRITE, 0,
                nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (h == INVALID_HANDLE_VALUE)
                std::cout << "写入 " << narrow(outPath) << " 失败" << std::endl;
            else {
                DWORD written = 0;
                WriteFile(h, content.data(), static_cast<DWORD>(content.size()), &written, nullptr);
                CloseHandle(h);
                std::cout << "已输出到 " << narrow(outPath) << "（"
                          << content.size() << " 字节）" << std::endl;
            }
        } else {
            if (content.size() >= 3 && content[0] == 0xEF && content[1] == 0xBB && content[2] == 0xBF)
                content.erase(content.begin(), content.begin() + 3);
            bool isBinary = false;
            size_t checkLen = (std::min)(content.size(), size_t(4096));
            for (size_t i = 0; i < checkLen; ++i) {
                if (content[i] == 0) { isBinary = true; break; }
            }
            if (isBinary) {
                std::cout << "（二进制文件，" << content.size() << " 字节）" << std::endl;
            } else if (content.size() > 100 * 1024) {
                std::cout << "（文件过大，" << content.size() << " 字节，仅显示前 100KB）" << std::endl;
                std::cout.write(reinterpret_cast<const char*>(content.data()), 100 * 1024);
                std::cout << std::flush;
            } else {
                std::cout.write(reinterpret_cast<const char*>(content.data()), content.size());
                std::cout << std::flush;
            }
        }
    }
};

class MonitorCommand : public Command {
public:
    const wchar_t* Name() const override { return L"monitor"; }
    const wchar_t* Description() const override { return L"开启监控事件显示"; }
    void Run(const std::vector<std::wstring>& /*args*/, CliContext& ctx) override {
        ctx.monitor = true;
        std::cout << "监控显示已开启（按任意键停止）\n"
                  << "Time      Operation                   Process           PID    Path\n"
                  << "────────  ──────────────────────────  ────────────────  ─────  ─────────────────────────────────────────────────────"
                  << std::endl;
        while (ctx.monitor) {
            DrainEvents(ctx);
            if (_kbhit()) { _getwch(); ctx.monitor = false; }
            else Sleep(50);
        }
    }
};

class SwitchCommand : public Command {
public:
    const wchar_t* Name() const override { return L"switch"; }
    const wchar_t* Description() const override { return L"切换到指定分支"; }
    std::wstring Usage() const override { return L"switch <文件路径> <分支名> — 切换到指定分支"; }
    void Run(const std::vector<std::wstring>& args, CliContext& ctx) override {
        if (args.size() < 3) {
            std::cout << narrow(Usage()) << std::endl;
            return;
        }
        std::wstring wpath = ResolvePath(args[1], ctx.virtDir);
        std::string branchName = narrow(args[2]);
        if (wpath.empty() || branchName.empty()) {
            std::cout << narrow(Usage()) << std::endl;
            return;
        }

        int minVer = 0, maxVer = 0;
        if (!ctx.store.ProbeBranchRange(wpath, branchName, minVer, maxVer)) {
            std::cout << "分支 '" << branchName << "' 不存在或已损坏" << std::endl;
            return;
        }
        int forkPoint = minVer - 1;
        if (forkPoint >= 0) {
            auto vers = ctx.store.GetVersions(wpath);
            int mainCount = static_cast<int>(vers.size());
            if (mainCount <= forkPoint) {
                std::cout << "主线缺少分叉点版本 v" << forkPoint
                          << "，分支不可用" << std::endl;
                return;
            }
        }
        {
            std::wstring brDir = ctx.store.SnapshotsPath() + L"\\";
            for (size_t i = 0; i < wpath.size(); ++i) {
                if (i == 1 && wpath[i] == L':') continue;
                brDir.push_back(wpath[i]);
            }
            brDir += L"\\.branches\\";
            int wlen = MultiByteToWideChar(CP_UTF8, 0, branchName.c_str(), -1, nullptr, 0);
            std::wstring wb(wlen, L'\0');
            MultiByteToWideChar(CP_UTF8, 0, branchName.c_str(), -1, &wb[0], wlen);
            while (!wb.empty() && wb.back() == L'\0') wb.pop_back();
            WIN32_FIND_DATAW ffd;
            HANDLE hF = FindFirstFileW((brDir + wb + L"\\*").c_str(), &ffd);
            if (hF == INVALID_HANDLE_VALUE) {
                std::cout << "分支 '" << branchName << "' 已被切换或删除" << std::endl;
                return;
            }
            FindClose(hF);
        }
        if (!ctx.store.PromoteBranch(wpath, branchName, forkPoint)) {
            std::cout << "切换分支失败" << std::endl;
            return;
        }
        auto vers = ctx.store.GetVersions(wpath);
        int latest = static_cast<int>(vers.size()) - 1;
        if (latest >= 0) {
            if (ctx.store.RestoreToFile(wpath, latest, wpath)) {
                std::string hash = ComputeFileHash(wpath);
                ctx.verTracker.SetState(wpath, latest + 1, hash);
                std::cout << "已切换到分支 '" << branchName << "' 最新版本 v"
                          << latest << std::endl;
            } else {
                std::cout << "恢复工作文件失败" << std::endl;
            }
        }
    }
};

class HelpCommand : public Command {
public:
    explicit HelpCommand(const CommandRegistry* registry)
        : registry_(registry) {}

    const wchar_t* Name() const override { return L"help"; }
    const wchar_t* Description() const override { return L"显示此帮助"; }
    void Run(const std::vector<std::wstring>& /*args*/, CliContext& /*ctx*/) override {
        std::cout << "可用命令:" << std::endl;
        for (const auto& pair : registry_->All()) {
            const std::wstring& name = pair.first;
            Command* cmd = pair.second;
            std::cout << "  " << narrow(name);
            size_t pad = 18;
            if (name.size() < pad) {
                std::string spaces(pad - name.size(), ' ');
                std::cout << spaces;
            } else {
                std::cout << std::endl << std::string(60, ' ');
            }
            std::cout << narrow(cmd->Description()) << std::endl;
        }
        std::cout << std::flush;
    }

private:
    const CommandRegistry* registry_;
};


class QuitCommand : public Command {
public:
    const wchar_t* Name() const override { return L"quit"; }
    const wchar_t* Description() const override { return L"退出"; }
    std::wstring Usage() const override { return L"quit / exit — 退出"; }
    void Run(const std::vector<std::wstring>& /*args*/, CliContext& ctx) override {
        ctx.running = false;
    }
};

class DeleteComand : public Command {
public:
    const wchar_t* Name() const override { return L"delete"; }
    const wchar_t* Description() const override { return L"删除指定目录或文件的所有版本记录"; }

};

class DeleteCommand : public Command {
public:
    const wchar_t* Name() const override { return L"delete"; }
    const wchar_t* Description() const override { return L"删除文件或目录的所有版本记录"; }
    std::wstring Usage() const override { return L"delete <文件路径|目录> — 删除文件或目录的所有版本记录"; }

    void Run(const std::vector<std::wstring>& args, CliContext& ctx) override {
        if (args.size() < 2) {
            std::cout << narrow(Usage()) << std::endl;
            return;
        }
        std::wstring wpath = ResolvePath(args[1], ctx.virtDir);
        if (wpath.empty()) {
            std::cout << "未指定路径" << std::endl;
            return;
        }

        auto allPaths = ctx.store.ListTrackedPaths();
        std::wstring vpTarget = ToVirt(wpath);

        // 判断是目录模式还是文件模式：有子路径匹配前缀 → 目录
        bool isDir = false;
        for (const auto& p : allPaths) {
            std::wstring vp = ToVirt(p);
            if (vp.size() > vpTarget.size() &&
                _wcsnicmp(vp.c_str(), vpTarget.c_str(), vpTarget.size()) == 0 &&
                vp[vpTarget.size()] == L'\\') {
                isDir = true;
                break;
            }
        }

        int count = 0;
        if (isDir) {
            size_t vpLen = vpTarget.size();
            for (const auto& p : allPaths) {
                std::wstring vp = ToVirt(p);
                bool under = vpLen == 0 ||
                    (vp.size() > vpLen && vp[vpLen] == L'\\' &&
                     _wcsnicmp(vp.c_str(), vpTarget.c_str(), vpLen) == 0);
                if (under) {
                    if (DeleteSnapshots(p, ctx)) ++count;
                }
            }
        } else {
            if (DeleteSnapshots(wpath, ctx)) count = 1;
        }

        if (count > 0)
            std::cout << "已删除 " << count << " 个文件的所有版本记录" << std::endl;
        else
            std::cout << "未找到匹配的文件" << std::endl;
    }

private:
    static bool DeleteSnapshots(const std::wstring& fullPath, CliContext& ctx) {
        // 1. 删除版本文件（v0, v1.delta, ...）
        ctx.store.DeleteVersions(fullPath, 0);

        // 2. 构造快照目录路径
        std::wstring snapDir = ctx.store.SnapshotsPath() + L"\\" + ToVirt(fullPath);

        // 3. 删除 .branches 目录（递归）
        std::wstring brDir = snapDir + L"\\.branches";
        DWORD brAttr = GetFileAttributesW(brDir.c_str());
        if (brAttr != INVALID_FILE_ATTRIBUTES && (brAttr & FILE_ATTRIBUTE_DIRECTORY)) {
            RemoveDirRecursive(brDir);
        }

        // 4. 清理快照目录中可能残留的文件，然后删除目录
        CleanDir(snapDir);
        RemoveDirectoryW(snapDir.c_str());

        // 5. 重置版本追踪状态
        ctx.verTracker.SetState(fullPath, 0, "");
        return true;
    }

    static void RemoveDirRecursive(const std::wstring& dir) {
        WIN32_FIND_DATAW ffd;
        HANDLE hFind = FindFirstFileW((dir + L"\\*").c_str(), &ffd);
        if (hFind == INVALID_HANDLE_VALUE) return;
        do {
            if (wcscmp(ffd.cFileName, L".") == 0 || wcscmp(ffd.cFileName, L"..") == 0) continue;
            std::wstring child = dir + L"\\" + ffd.cFileName;
            if (ffd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                RemoveDirRecursive(child);
                RemoveDirectoryW(child.c_str());
            } else {
                DeleteFileW(child.c_str());
            }
        } while (FindNextFileW(hFind, &ffd));
        FindClose(hFind);
    }

    static void CleanDir(const std::wstring& dir) {
        WIN32_FIND_DATAW ffd;
        HANDLE hFind = FindFirstFileW((dir + L"\\*").c_str(), &ffd);
        if (hFind == INVALID_HANDLE_VALUE) return;
        do {
            if (wcscmp(ffd.cFileName, L".") == 0 || wcscmp(ffd.cFileName, L"..") == 0) continue;
            std::wstring child = dir + L"\\" + ffd.cFileName;
            if (ffd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                RemoveDirRecursive(child);
                RemoveDirectoryW(child.c_str());
            } else {
                DeleteFileW(child.c_str());
            }
        } while (FindNextFileW(hFind, &ffd));
        FindClose(hFind);
    }
};

void RegisterBuiltinCommands(CommandRegistry& reg) {
    static LsCommand s_ls;
    static CdCommand s_cd;
    static TreeCommand s_tree;
    static RestoreCommand s_restore;
    static ShowCommand s_show;
    static MonitorCommand s_monitor;
    static SwitchCommand s_switch;
    static QuitCommand    s_quit;
    static DeleteCommand  s_delete;

    reg.Register(&s_ls);
    reg.Register(&s_cd);
    reg.Register(&s_tree);
    reg.Register(&s_restore);
    reg.Register(&s_show);
    reg.Register(&s_monitor);
    reg.Register(&s_switch);
    reg.Register(&s_quit);
    reg.RegisterAlias(L"exit", &s_quit);
    reg.Register(&s_delete);

    static HelpCommand s_help(&reg);
    reg.Register(&s_help);
}

} // namespace fssentry
