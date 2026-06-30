/// @file snapshot_store.cpp
/// @brief 增量链快照存储 + 分支管理（零 meta 文件，全从目录结构推导）

#include "snapshot_store.h"
#include "delta.h"

#include <windows.h>
#include <fstream>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#include <cwchar>

#undef DeleteFile

namespace fssentry {

/// @brief 递归创建目录
/// @param path 目录路径
/// @return 成功返回 true
static bool MakeDir(const std::wstring& path)
{
    if (path.empty()) return false;
    std::wstring cur;
    for (size_t i = 0; i < path.size(); ++i) {
        cur.push_back(path[i]);
        if (path[i] == L'\\' || i == path.size() - 1) {
            if (cur.size() == 2 && cur[1] == L':') continue; ///< 跳过盘符
            DWORD attr = GetFileAttributesW(cur.c_str());
            if (attr == INVALID_FILE_ATTRIBUTES)
                CreateDirectoryW(cur.c_str(), nullptr);
        }
    }
    return true;
}
/// @brief 使用 fstream 复制文件
/// @param src 源文件
/// @param dst 目标文件
/// @return 成功返回 true
static bool CopyFileStream(const std::wstring& src, const std::wstring& dst)
{
    std::ifstream in(src, std::ios::binary);
    if (!in) return false;
    std::ofstream out(dst, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out << in.rdbuf();
    return !in.bad() && !out.fail();
}

/// @brief 原子写入：直接 CreateFileW 写入（避开 ofstream 和 rename 的锁定问题）
/// @param path 目标文件路径
/// @param data 数据指针
/// @param size 数据大小
/// @return 成功返回 true
static bool AtomicWriteFile(const std::wstring& path, const uint8_t* data, size_t size)
{
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ,
        nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    BOOL ok = WriteFile(h, data, static_cast<DWORD>(size), &written, nullptr);
    CloseHandle(h);
    return ok && static_cast<size_t>(written) == size;
}

/// @brief 原子移动文件（先写 .tmp 再 rename）
/// @param src 源文件
/// @param dst 目标文件
/// @return 成功返回 true
static bool MoveFileAtomic(const std::wstring& src, const std::wstring& dst)
{
    return MoveFileW(src.c_str(), dst.c_str()) != FALSE;
}

/// @brief 带重试的文件复制（处理文件被锁的情况）
/// @param src 源文件
/// @param dst 目标文件
/// @param maxRetries 最大重试次数
/// @return 成功返回 true
static bool CopyFileRetry(const std::wstring& src, const std::wstring& dst,
                          int maxRetries = 5)
{
    for (int retry = 0; retry <= maxRetries; ++retry) {
        HANDLE hSrc = CreateFileW(src.c_str(), GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (hSrc == INVALID_HANDLE_VALUE) {
            if (retry < maxRetries) { Sleep(50 * (retry + 1)); continue; }
            return false;
        }
        HANDLE hDst = CreateFileW(dst.c_str(), GENERIC_WRITE,
            FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (hDst == INVALID_HANDLE_VALUE) {
            CloseHandle(hSrc); return false;
        }
        uint8_t buf[65536];
        DWORD bytesRead = 0, bytesWritten = 0;
        bool ok = true;
        while (ReadFile(hSrc, buf, sizeof(buf), &bytesRead, nullptr) && bytesRead > 0) {
            if (!WriteFile(hDst, buf, bytesRead, &bytesWritten, nullptr) || bytesWritten != bytesRead) {
                ok = false; break;
            }
        }
        CloseHandle(hSrc); CloseHandle(hDst);
        if (ok) return true;
        if (retry < maxRetries) Sleep(50 * (retry + 1));
    }
    return false;
}

/// @brief UTF-8 窄字符串转 UTF-16 宽字符串
static std::wstring ToWide(const std::string& s)
{
    if (s.empty()) return {};
    int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    if (len <= 0) return {};
    std::wstring w(len, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &w[0], len);
    while (!w.empty() && w.back() == L'\0') w.pop_back();
    return w;
}

/// @brief 完整路径 → 相对路径（去掉盘符冒号）
/// @param fullPath 完整路径，如 C:\\Users\\...\\foo.txt
/// @return 相对路径，如 c\\Users\\...\\foo.txt
std::wstring SnapshotStore::PathToRel(const std::wstring& fullPath)
{
    std::wstring rel;
    rel.reserve(fullPath.size());
    for (size_t i = 0; i < fullPath.size(); ++i) {
        if (i == 1 && fullPath[i] == L':') continue;  ///< 去掉冒号
        if (fullPath[i] == L'/') rel.push_back(L'\\'); ///< 统一反斜杠
        else rel.push_back(fullPath[i]);
    }
    return rel;
}

/// @brief 相对路径 → 完整路径（恢复盘符冒号）
/// @param relPath 相对路径，如 c\\Users\\...\\foo.txt
/// @return 完整路径，如 C:\\Users\\...\\foo.txt
static std::wstring RelToFull(const std::wstring& relPath)
{
    if (relPath.size() < 2) return relPath;
    std::wstring full;
    full.reserve(relPath.size() + 1);
    full.push_back(relPath[0]);
    full.push_back(L':');
    full.append(relPath.begin() + 1, relPath.end());
    return full;
}

SnapshotStore::SnapshotStore(const std::wstring& repoPath)
    : repoPath_(repoPath)
{
    snapDir_ = repoPath_ + L"\\snapshots";
    MakeDir(snapDir_);
}

std::wstring SnapshotStore::RelToSnapDir(const std::wstring& relPath) const
{
    return snapDir_ + L"\\" + relPath;
}

/// @brief 构造版本文件的完整路径
/// @param fullPath 文件完整路径
/// @param version  版本号（0 = v0, N = v{N}.delta）
/// @return 版本文件路径
std::wstring SnapshotStore::VersionPath(const std::wstring& fullPath,
                                         int version) const
{
    std::wstring dir = RelToSnapDir(PathToRel(fullPath));
    wchar_t name[64];
    if (version == 0)
        _snwprintf_s(name, 64, _TRUNCATE, L"v0");
    else
        _snwprintf_s(name, 64, _TRUNCATE, L"v%d.delta", version);
    return dir + L"\\" + name;
}

/// @brief 统计目录中 v* 文件数量
/// @param snapDir 快照目录
/// @return 版本数
int SnapshotStore::CountVersions(const std::wstring& snapDir) const
{
    int count = 0;
    WIN32_FIND_DATAW ffd;
    HANDLE hFind = FindFirstFileW((snapDir + L"\\*").c_str(), &ffd);
    if (hFind != INVALID_HANDLE_VALUE) {
        do {
            if (wcscmp(ffd.cFileName, L".") == 0 || wcscmp(ffd.cFileName, L"..") == 0)
                continue;
            if (!(ffd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
                if (ffd.cFileName[0] == L'v') ++count;
            }
        } while (FindNextFileW(hFind, &ffd));
        FindClose(hFind);
    }
    return count;
}

SnapshotInfo SnapshotStore::StoreSnapshot(const std::wstring& fullPath,
                                           uint64_t timestamp,
                                           int prevVersion)
{
    SnapshotInfo si;
    si.timestamp = timestamp;
    si.version   = -1;

    std::wstring snapDir = RelToSnapDir(PathToRel(fullPath));
    MakeDir(snapDir);

    if (prevVersion < 0) {
        ///< 首次快照：直接复制文件作为 v0
        si.version  = 0;
        si.fileName = "v0";
        std::wstring tmp  = snapDir + L"\\_v0.tmp";
        std::wstring target = snapDir + L"\\v0";

        if (GetFileAttributesW(target.c_str()) != INVALID_FILE_ATTRIBUTES) { si.version = -1; return si; } ///< 不应发生

        if (!CopyFileRetry(fullPath, tmp)) { DeleteFileW(tmp.c_str()); si.version = -1; return si; }
        if (!MoveFileAtomic(tmp, target)) { DeleteFileW(tmp.c_str()); si.version = -1; return si; }
        return si;
    }

    ///< 后续版本：计算 delta
    si.version  = prevVersion + 1;
    wchar_t buf[64];
    _snwprintf_s(buf, 64, _TRUNCATE, L"v%d.delta", si.version);
    {
        std::wstring wname(buf);
        si.fileName.resize(wname.size());
        for (size_t i = 0; i < wname.size(); ++i)
            si.fileName[i] = static_cast<char>(wname[i]);
    }

    ///< 重建上一版本到临时文件（用随机名避免锁冲突）
    wchar_t prevTmp[MAX_PATH];
    GetTempPathW(MAX_PATH, prevTmp);
    wcscat_s(prevTmp, L"fssentry_delta.tmp");
    if (!ReconstructVersion(fullPath, prevVersion, prevTmp))
        return SnapshotInfo{};

    ///< 计算增量
    std::vector<uint8_t> delta;
    if (!ComputeDelta(prevTmp, fullPath, delta)) {
        DeleteFileW(prevTmp);
        return SnapshotInfo{};
    }
    DeleteFileW(prevTmp);

    ///< 写入增量（原子写入：.tmp → rename）
    {
        std::wstring target = snapDir + L"\\" + std::wstring(buf);
        if (!AtomicWriteFile(target, delta.data(), delta.size()))
            return SnapshotInfo{};
    }

    return si;
}

bool SnapshotStore::ReconstructVersion(const std::wstring& fullPath,
                                        int version,
                                        const std::wstring& targetPath)
{
    if (version < 0) return false;

    std::wstring v0Path = VersionPath(fullPath, 0);
    if (version == 0) {
        return CopyFileStream(v0Path, targetPath);
    }

    ///< 复制 v0，再依次 apply 增量
    if (!CopyFileStream(v0Path, targetPath))
        return false;

    for (int v = 1; v <= version; ++v) {
        std::wstring deltaPath = VersionPath(fullPath, v);
        std::wstring newTmp = targetPath + L".next";

        if (!ApplyDeltaToFile(targetPath, deltaPath, newTmp)) return false;
        DeleteFileW(targetPath.c_str());
        if (MoveFileW(newTmp.c_str(), targetPath.c_str()) == 0) {
            DeleteFileW(newTmp.c_str()); return false;
        }
    }
    return true;
}

/// @brief 解析分支路径为绝对目录（fork_v3 → .branches/fork_v3，嵌套用 / 分隔）
static std::wstring ResolveBranchDir(const std::wstring& baseDir,
                                      const std::string& branchPath)
{
    std::wstring dir = baseDir + L"\\.branches";
    std::wstring wbp = ToWide(branchPath);
    std::wstring rest = wbp;
    while (!rest.empty()) {
        size_t slash = rest.find(L'/');
        std::wstring seg = rest.substr(0, slash);
        dir += L"\\" + seg;
        DWORD attr = GetFileAttributesW(dir.c_str());
        if (attr == INVALID_FILE_ATTRIBUTES || !(attr & FILE_ATTRIBUTE_DIRECTORY))
            return {};
        if (slash == std::wstring::npos) break;
        dir += L"\\.branches";
        attr = GetFileAttributesW(dir.c_str());
        if (attr == INVALID_FILE_ATTRIBUTES || !(attr & FILE_ATTRIBUTE_DIRECTORY))
            return {};
        rest = rest.substr(slash + 1);
    }
    return dir;
}

bool SnapshotStore::ReconstructBranchVersion(const std::wstring& fullPath,
                                              const std::string& branchPath,
                                              int version,
                                              const std::wstring& targetPath)
{
    std::wstring snapDir = RelToSnapDir(PathToRel(fullPath));
    std::wstring branchDir = ResolveBranchDir(snapDir, branchPath);
    if (branchDir.empty()) return false;

    // 扫描分支获取版本范围
    int minVer = 0, maxVer = 0;
    {
        WIN32_FIND_DATAW ffd;
        HANDLE hF = FindFirstFileW((branchDir + L"\\*").c_str(), &ffd);
        if (hF == INVALID_HANDLE_VALUE) return false;
        minVer = INT_MAX; maxVer = -1;
        do {
            if (wcscmp(ffd.cFileName, L".") == 0 || wcscmp(ffd.cFileName, L"..") == 0) continue;
            if (ffd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            if (ffd.cFileName[0] != L'v') continue;
            const wchar_t* p = ffd.cFileName + 1;
            wchar_t* end = nullptr;
            int ver = static_cast<int>(wcstol(p, &end, 10));
            if (end == nullptr || end == p || (*end != L'.' && *end != L'\0')) continue;
            if (ver < 0) continue;
            if (ver < minVer) minVer = ver;
            if (ver > maxVer) maxVer = ver;
        } while (FindNextFileW(hF, &ffd));
        FindClose(hF);
    }
    if (maxVer < 0 || minVer == INT_MAX) return false;
    int forkPoint = minVer - 1;
    if (version < minVer) {
        // 版本在分支之前，直接从主线读
        return ReconstructVersion(fullPath, version, targetPath);
    }

    // 从主线重建到分叉点
    if (forkPoint >= 0) {
        if (!ReconstructVersion(fullPath, forkPoint, targetPath))
            return false;
    } else {
        // 分支含 v0（自包含）
        std::wstring v0Path = branchDir + L"\\v0";
        if (!CopyFileStream(v0Path, targetPath))
            return false;
    }

    // 从 minVer 到 version 依次应用分支增量
    for (int v = minVer; v <= version; ++v) {
        wchar_t name[64];
        if (v == 0)
            _snwprintf_s(name, 64, _TRUNCATE, L"v0");
        else
            _snwprintf_s(name, 64, _TRUNCATE, L"v%d.delta", v);
        std::wstring deltaPath = branchDir + L"\\" + name;
        DWORD attr = GetFileAttributesW(deltaPath.c_str());
        if (attr == INVALID_FILE_ATTRIBUTES) {
            // 分支没有这个版本，尝试从主线找
            deltaPath = VersionPath(fullPath, v);
            attr = GetFileAttributesW(deltaPath.c_str());
            if (attr == INVALID_FILE_ATTRIBUTES) {
                DeleteFileW(targetPath.c_str());
                return false;
            }
        }
        std::wstring newTmp = targetPath + L".next";
        if (!ApplyDeltaToFile(targetPath, deltaPath, newTmp)) {
            DeleteFileW(targetPath.c_str());
            return false;
        }
        DeleteFileW(targetPath.c_str());
        if (MoveFileW(newTmp.c_str(), targetPath.c_str()) == 0) {
            DeleteFileW(newTmp.c_str());
            return false;
        }
    }
    return true;
}

/// @brief 从临时文件中读取全部内容到内存（自动删除临时文件）
static std::vector<uint8_t> ReadTempFile(const std::wstring& tmpPath)
{
    std::ifstream f(tmpPath, std::ios::binary | std::ios::ate);
    if (!f) { DeleteFileW(tmpPath.c_str()); return {}; }
    auto sz = f.tellg();
    f.seekg(0);
    std::vector<uint8_t> data(static_cast<size_t>(sz));
    f.read(reinterpret_cast<char*>(data.data()), sz);
    DeleteFileW(tmpPath.c_str());
    return data;
}

std::vector<uint8_t> SnapshotStore::GetBranchContent(
    const std::wstring& fullPath, const std::string& branchPath, int version)
{
    wchar_t tmpPath[MAX_PATH];
    GetTempPathW(MAX_PATH, tmpPath);
    wcscat_s(tmpPath, MAX_PATH, L"fssentry_content.tmp");

    if (!ReconstructBranchVersion(fullPath, branchPath, version, tmpPath)) {
        DeleteFileW(tmpPath); return {};
    }

    return ReadTempFile(tmpPath);
}

bool SnapshotStore::RestoreToFile(const std::wstring& fullPath, int version,
                                   const std::wstring& targetPath)
{
    ///< 重建到临时文件，再移动
    wchar_t tmpPath[MAX_PATH];
    GetTempPathW(MAX_PATH, tmpPath);
    wcscat_s(tmpPath, MAX_PATH, L"fssentry_restore.tmp");

    if (!ReconstructVersion(fullPath, version, tmpPath)) return false;

    DeleteFileW(targetPath.c_str());
    if (MoveFileW(tmpPath, targetPath.c_str()) == 0) {
        ///< 跨驱动器回退：复制
        if (!CopyFileStream(tmpPath, targetPath.c_str())) {
            DeleteFileW(tmpPath);
            return false;
        }
        DeleteFileW(tmpPath);
    }
    return true;
}

/// @brief 递归遍历快照目录
/// @param absDir  绝对路径
/// @param relPart 相对路径部分
/// @param paths   输出的路径列表
static void WalkSnapDir(const std::wstring& absDir,
                        const std::wstring& relPart,
                        std::vector<std::wstring>& paths)
{
    bool hasSub = false;
    WIN32_FIND_DATAW ffd;
    HANDLE hFind = FindFirstFileW((absDir + L"\\*").c_str(), &ffd);
    if (hFind != INVALID_HANDLE_VALUE) {
        do {
            if (wcscmp(ffd.cFileName, L".") == 0 || wcscmp(ffd.cFileName, L"..") == 0)
                continue;
            if (ffd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                if (wcscmp(ffd.cFileName, L".branches") == 0) continue; ///< 跳过分支元数据
                hasSub = true;
                WalkSnapDir(absDir + L"\\" + ffd.cFileName,
                            relPart + L"\\" + ffd.cFileName, paths);
            }
        } while (FindNextFileW(hFind, &ffd));
        FindClose(hFind);
    }

    ///< 叶子目录：检查是否含 v* 文件
    if (!hasSub) {
        hFind = FindFirstFileW((absDir + L"\\*").c_str(), &ffd);
        if (hFind != INVALID_HANDLE_VALUE) {
            do {
                if (wcscmp(ffd.cFileName, L".") == 0 || wcscmp(ffd.cFileName, L"..") == 0)
                    continue;
                if (ffd.cFileName[0] == L'v' && !(ffd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
                    paths.push_back(RelToFull(relPart));
                    break;
                }
            } while (FindNextFileW(hFind, &ffd));
            FindClose(hFind);
        }
    }
}

std::vector<std::wstring> SnapshotStore::ListTrackedPaths()
{
    std::vector<std::wstring> paths;
    WIN32_FIND_DATAW ffd;
    HANDLE hFind = FindFirstFileW((snapDir_ + L"\\*").c_str(), &ffd);
    if (hFind != INVALID_HANDLE_VALUE) {
        do {
            if (wcscmp(ffd.cFileName, L".") == 0 || wcscmp(ffd.cFileName, L"..") == 0)
                continue;
            if (ffd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                WalkSnapDir(snapDir_ + L"\\" + ffd.cFileName, ffd.cFileName, paths);
        } while (FindNextFileW(hFind, &ffd));
        FindClose(hFind);
    }
    return paths;
}

/// @brief 扫描给定目录中的 v* 版本文件，返回版本信息列表（共享逻辑）
static std::vector<SnapshotInfo> ScanDirForVersions(const std::wstring& dir)
{
    std::vector<SnapshotInfo> vs;
    WIN32_FIND_DATAW ffd;
    HANDLE hFind = FindFirstFileW((dir + L"\\*").c_str(), &ffd);
    if (hFind == INVALID_HANDLE_VALUE) return vs;
    do {
        if (wcscmp(ffd.cFileName, L".") == 0 || wcscmp(ffd.cFileName, L"..") == 0)
            continue;
        if (ffd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        if (ffd.cFileName[0] != L'v') continue;

        std::wstring name(ffd.cFileName);
        int ver = -1;
        if (name == L"v0") {
            ver = 0;
        } else {
            auto dotPos = name.find(L'.');
            if (dotPos != std::wstring::npos && dotPos > 1)
                ver = _wtoi(name.substr(1, dotPos - 1).c_str());
        }
        if (ver < 0) continue;

        SnapshotInfo si;
        si.fileName.resize(name.size());
        for (size_t i = 0; i < name.size(); ++i)
            si.fileName[i] = static_cast<char>(name[i]);
        si.version = ver;
        if (ver == 0) {
            si.fileSize = (static_cast<uint64_t>(ffd.nFileSizeHigh) << 32) | ffd.nFileSizeLow;
        } else {
            std::wstring verPath = dir + L"\\" + name;
            HANDLE hf = CreateFileW(verPath.c_str(), GENERIC_READ, FILE_SHARE_READ,
                nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (hf != INVALID_HANDLE_VALUE) {
                uint8_t hdr[8];
                DWORD rd = 0;
                if (ReadFile(hf, hdr, 8, &rd, nullptr) && rd == 8)
                    si.fileSize = hdr[4] | (hdr[5] << 8) | (hdr[6] << 16) | (static_cast<uint64_t>(hdr[7]) << 24);
                CloseHandle(hf);
            }
        }
        si.timestamp = (static_cast<uint64_t>(ffd.ftLastWriteTime.dwHighDateTime) << 32)
                     | ffd.ftLastWriteTime.dwLowDateTime;
        vs.push_back(si);
    } while (FindNextFileW(hFind, &ffd));
    FindClose(hFind);

    std::sort(vs.begin(), vs.end(),
              [](const SnapshotInfo& a, const SnapshotInfo& b) {
                  return a.version < b.version;
              });
    return vs;
}

std::vector<SnapshotInfo> SnapshotStore::GetVersions(const std::wstring& fullPath)
{
    return ScanDirForVersions(RelToSnapDir(PathToRel(fullPath)));
}

std::vector<SnapshotInfo> SnapshotStore::GetVersionsFromDir(const std::wstring& snapDir)
{
    return ScanDirForVersions(snapDir);
}

std::vector<SnapshotInfo> SnapshotStore::GetBranchVersions(
    const std::wstring& fullPath, const std::string& branchPath)
{
    std::wstring dir = ResolveBranchDir(RelToSnapDir(PathToRel(fullPath)), branchPath);
    if (dir.empty()) return {};
    return ScanDirForVersions(dir);
}

std::vector<uint8_t> SnapshotStore::GetContent(const std::wstring& fullPath,
                                                int version)
{
    wchar_t tmpPath[MAX_PATH];
    GetTempPathW(MAX_PATH, tmpPath);
    wcscat_s(tmpPath, MAX_PATH, L"fssentry_content.tmp");

    if (!ReconstructVersion(fullPath, version, tmpPath)) {
        DeleteFileW(tmpPath); return {};
    }

    return ReadTempFile(tmpPath);
}

bool SnapshotStore::RebaseChain(const std::wstring& fullPath, int curVer, int keep)
{
    if (curVer <= 0 || keep < 1) return true;
    if (keep > curVer) keep = curVer; ///< 保留所有版本

    std::wstring snapDir = RelToSnapDir(PathToRel(fullPath));
    int newBaseVer = curVer - keep + 1; ///< 新 v0 将保存此版本的内容
    if (newBaseVer < 1) newBaseVer = 1;

    ///< 重建新基准版本到临时文件
    std::wstring tmpPath = snapDir + L"\\_rebase.tmp";
    if (!ReconstructVersion(fullPath, newBaseVer, tmpPath)) {
        DeleteFileW(tmpPath.c_str()); return false;
    }

    ///< 删除新基准之前的旧增量
    for (int v = 1; v <= newBaseVer; ++v) {
        wchar_t name[32];
        _snwprintf_s(name, 32, _TRUNCATE, L"v%d.delta", v);
        DeleteFileW((snapDir + L"\\" + name).c_str());
    }

    ///< 重新编号剩余的增量
    for (int oldV = newBaseVer + 1; oldV <= curVer; ++oldV) {
        int newV = oldV - newBaseVer;
        wchar_t oldName[32], newName[32];
        _snwprintf_s(oldName, 32, _TRUNCATE, L"v%d.delta", oldV);
        _snwprintf_s(newName, 32, _TRUNCATE, L"v%d.delta", newV);
        MoveFileW((snapDir + L"\\" + oldName).c_str(),
                 (snapDir + L"\\" + newName).c_str());
    }

    ///< 替换旧 v0 为新基准
    DeleteFileW((snapDir + L"\\v0").c_str());
    if (MoveFileW(tmpPath.c_str(), (snapDir + L"\\v0").c_str()) == 0) {
        DeleteFileW(tmpPath.c_str()); return false;
    }

    ///< 清理分支点已被 rebase 删除的分支
    std::wstring brDir = snapDir + L"\\.branches";
    DWORD attr = GetFileAttributesW(brDir.c_str());
    if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY)) {
        WIN32_FIND_DATAW ffd;
        HANDLE hF = FindFirstFileW((brDir + L"\\*").c_str(), &ffd);
        if (hF != INVALID_HANDLE_VALUE) {
            do {
                if (wcscmp(ffd.cFileName, L".") == 0 || wcscmp(ffd.cFileName, L"..") == 0) continue;
                if (!(ffd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
                ///< 解析分支名：fork_v2_20260617T103000 → fork 版本号 2
                int fv = -1;
                if (wcsncmp(ffd.cFileName, L"fork_v", 6) == 0) {
                    const wchar_t* p = ffd.cFileName + 6;
                    wchar_t* end = nullptr;
                    fv = static_cast<int>(wcstol(p, &end, 10));
                }
                if (fv >= 0 && fv < newBaseVer) {
                    ///< 分支点版本已被删除(rebase 合并进新 v0)，删除整个分支
                    std::wstring branchPath = brDir + L"\\" + ffd.cFileName;
                    WIN32_FIND_DATAW bfd;
                    HANDLE hB = FindFirstFileW((branchPath + L"\\*").c_str(), &bfd);
                    if (hB != INVALID_HANDLE_VALUE) {
                        do {
                            if (wcscmp(bfd.cFileName, L".") == 0 || wcscmp(bfd.cFileName, L"..") == 0) continue;
                            DeleteFileW((branchPath + L"\\" + bfd.cFileName).c_str());
                        } while (FindNextFileW(hB, &bfd));
                        FindClose(hB);
                    }
                    RemoveDirectoryW(branchPath.c_str());
                }
            } while (FindNextFileW(hF, &ffd));
            FindClose(hF);
        }
    }

    return true;
}

/// @brief 获取分支存储目录路径
/// @param fullPath   文件完整路径
/// @param branchName 分支名称（UTF-8）
/// @return 分支目录完整路径
std::wstring SnapshotStore::BranchDir(const std::wstring& fullPath,
                                       const std::string& branchName) const
{
    std::wstring rel;
    for (size_t i = 0; i < fullPath.size(); ++i) {
        if (i == 1 && fullPath[i] == L':') continue;
        rel.push_back(fullPath[i]);
    }
    std::wstring dir = snapDir_ + L"\\" + rel + L"\\.branches";
    // Convert branch name to wide
    std::wstring wb = ToWide(branchName);
    return dir + L"\\" + wb;
}

/// @brief 将指定版本范围的增量文件复制到分支目录
/// @param fullPath   文件完整路径
/// @param fromVer    起始版本号（含）
/// @param branchName 目标分支名称
/// @return 成功返回 true
bool SnapshotStore::CopyToBranch(const std::wstring& fullPath,
                                  int fromVer,
                                  const std::string& branchName)
{
    std::wstring snapDir = RelToSnapDir(PathToRel(fullPath));
    int latest = CountVersions(snapDir);
    if (fromVer > latest) return false;

    std::wstring branchDir = BranchDir(fullPath, branchName);
    if (!MakeDir(branchDir)) return false;

    for (int v = fromVer; v <= latest; ++v) {
        std::wstring src = VersionPath(fullPath, v);
        wchar_t name[64];
        if (v == 0)
            _snwprintf_s(name, 64, _TRUNCATE, L"v0");
        else
            _snwprintf_s(name, 64, _TRUNCATE, L"v%d.delta", v);
        std::wstring dst = branchDir + L"\\" + name;
        if (!CopyFileW(src.c_str(), dst.c_str(), FALSE))
            return false;
    }
    return true;
}

/// @brief 递归删除目录及其所有内容
static bool RemoveDirRecursive(const std::wstring& dir)
{
    WIN32_FIND_DATAW ffd;
    HANDLE hFind = FindFirstFileW((dir + L"\\*").c_str(), &ffd);
    if (hFind == INVALID_HANDLE_VALUE) return false;
    do {
        if (wcscmp(ffd.cFileName, L".") == 0 || wcscmp(ffd.cFileName, L"..") == 0) continue;
        std::wstring child = dir + L"\\" + ffd.cFileName;
        if (ffd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            RemoveDirRecursive(child);
        else
            DeleteFileW(child.c_str());
    } while (FindNextFileW(hFind, &ffd));
    FindClose(hFind);
    return RemoveDirectoryW(dir.c_str()) != FALSE;
}

/// @brief 递归复制目录内容
static bool CopyDirRecursive(const std::wstring& src, const std::wstring& dst)
{
    MakeDir(dst);
    WIN32_FIND_DATAW ffd;
    HANDLE hFind = FindFirstFileW((src + L"\\*").c_str(), &ffd);
    if (hFind == INVALID_HANDLE_VALUE) return true;
    bool ok = true;
    do {
        if (wcscmp(ffd.cFileName, L".") == 0 || wcscmp(ffd.cFileName, L"..") == 0) continue;
        std::wstring childSrc = src + L"\\" + ffd.cFileName;
        std::wstring childDst = dst + L"\\" + ffd.cFileName;
        if (ffd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (!CopyDirRecursive(childSrc, childDst)) { ok = false; break; }
        } else {
            if (!CopyFileW(childSrc.c_str(), childDst.c_str(), FALSE)) { ok = false; break; }
        }
    } while (FindNextFileW(hFind, &ffd));
    FindClose(hFind);
    return ok;
}

bool SnapshotStore::ProbeBranchRange(const std::wstring& fullPath,
                                      const std::string& branchName,
                                      int& outMinVer, int& outMaxVer)
{
    std::wstring branchDir = BranchDir(fullPath, branchName);
    DWORD attr = GetFileAttributesW(branchDir.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES || !(attr & FILE_ATTRIBUTE_DIRECTORY))
        return false;

    auto vers = GetBranchVersions(fullPath, branchName);
    if (vers.empty()) return false;

    outMinVer = INT_MAX;
    outMaxVer = -1;
    for (const auto& v : vers) {
        outMinVer = (std::min)(outMinVer, v.version);
        outMaxVer = (std::max)(outMaxVer, v.version);
    }
    return true;
}

bool SnapshotStore::PromoteBranch(const std::wstring& fullPath,
                                   const std::string& branchName,
                                   int forkPoint)
{
    std::wstring branchDir = BranchDir(fullPath, branchName);
    DWORD attr = GetFileAttributesW(branchDir.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES || !(attr & FILE_ATTRIBUTE_DIRECTORY))
        return false;

    int minVer = 0, maxVer = 0;
    if (!ProbeBranchRange(fullPath, branchName, minVer, maxVer))
        return false;

    // 分叉点校验
    int expectedFork = minVer - 1;
    if (minVer == 0) expectedFork = -1;  // 分支自包含（含 v0）
    if (expectedFork != forkPoint && !(minVer == 0 && forkPoint == -1))
        return false;

    // 如果主线在分叉点之后有增量 → 备份为新分支→再删除
    std::wstring snapDir = RelToSnapDir(PathToRel(fullPath));
    int mainlineCount = CountVersions(snapDir);
    int firstOverwrite = (forkPoint < 0) ? 0 : forkPoint + 1;
    if (mainlineCount > firstOverwrite) {
        char buf[64];
        SYSTEMTIME st; GetLocalTime(&st);
        snprintf(buf, sizeof(buf), "auto_backup_%04d%02d%02dT%02d%02d%02d",
                 st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
        if (!CopyToBranch(fullPath, firstOverwrite, buf))
            return false;
    }

    // 删除主线被覆盖的增量
    if (firstOverwrite < mainlineCount)
        DeleteVersions(fullPath, firstOverwrite);

    // 复制分支版本文件到主线（跳过非版本文件和 .branches 目录）
    WIN32_FIND_DATAW ffd;
    HANDLE hFind = FindFirstFileW((branchDir + L"\\*").c_str(), &ffd);
    if (hFind == INVALID_HANDLE_VALUE) return false;
    do {
        if (wcscmp(ffd.cFileName, L".") == 0 || wcscmp(ffd.cFileName, L"..") == 0) continue;
        if (ffd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            // 递归处理嵌套分支：复制到主线的 .branches/
            if (wcscmp(ffd.cFileName, L".branches") == 0) {
                std::wstring nestedSrc = branchDir + L"\\.branches";
                std::wstring mainBrDir = snapDir + L"\\.branches";
                MakeDir(mainBrDir);
                WIN32_FIND_DATAW nd;
                HANDLE hNest = FindFirstFileW((nestedSrc + L"\\*").c_str(), &nd);
                if (hNest != INVALID_HANDLE_VALUE) {
                    do {
                        if (wcscmp(nd.cFileName, L".") == 0 || wcscmp(nd.cFileName, L"..") == 0) continue;
                        if (nd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                            std::wstring ns = nestedSrc + L"\\" + nd.cFileName;
                            std::wstring ndst = mainBrDir + L"\\" + nd.cFileName;
                            CopyDirRecursive(ns, ndst);
                        }
                    } while (FindNextFileW(hNest, &nd));
                    FindClose(hNest);
                }
            }
            continue;
        }
        // 版本文件（v*）
        if (ffd.cFileName[0] != L'v') continue;
        std::wstring src = branchDir + L"\\" + ffd.cFileName;
        std::wstring dst = snapDir + L"\\" + ffd.cFileName;
        if (!CopyFileW(src.c_str(), dst.c_str(), FALSE))
            { FindClose(hFind); return false; }
    } while (FindNextFileW(hFind, &ffd));
    FindClose(hFind);

    // 删除分支目录
    RemoveDirRecursive(branchDir);
    return true;
}

bool SnapshotStore::DeleteBranch(const std::wstring& fullPath,
                                  const std::string& branchName)
{
    std::wstring branchDir = BranchDir(fullPath, branchName);
    DWORD attr = GetFileAttributesW(branchDir.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES || !(attr & FILE_ATTRIBUTE_DIRECTORY))
        return false;
    return RemoveDirRecursive(branchDir);
}

/// @brief 删除指定版本范围的增量文件
/// @param fullPath 文件完整路径
/// @param fromVer  起始版本号（含）
void SnapshotStore::DeleteVersions(const std::wstring& fullPath,
                                    int fromVer)
{
    std::wstring snapDir = RelToSnapDir(PathToRel(fullPath));
    int latest = CountVersions(snapDir);
    for (int v = fromVer; v <= latest; ++v)
        DeleteFileW(VersionPath(fullPath, v).c_str());
}

} // namespace fssentry
