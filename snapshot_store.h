/// @file snapshot_store.h
/// @brief 增量链快照存储 + 分支管理（零 meta 文件，全从目录结构推导）
///
/// 存储布局:
///   <repo>/snapshots/<rel_path>/
///     v0                ← 基准文件（全量副本）
///     v1.delta          ← v0→v1 二进制增量
///     v2.delta          ← v1→v2 二进制增量
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace fssentry {

struct SnapshotInfo {
    uint64_t    timestamp = 0;
    int         version   = -1;
    uint64_t    fileSize  = 0;
    std::string fileName;
};

class SnapshotStore {
public:
    explicit SnapshotStore(const std::wstring& repoPath);

    /// @brief 存储快照（首次为基准文件 v0，后续为增量 v{N}.delta）
    /// @param prevVersion  -1 = 首次快照（存 v0），N = 存增量 v{N+1}.delta
    SnapshotInfo StoreSnapshot(const std::wstring& fullPath,
                               uint64_t timestamp,
                               int prevVersion);

    /// @brief 重建指定版本内容到目标路径
    bool ReconstructVersion(const std::wstring& fullPath,
                            int version,
                            const std::wstring& targetPath);

    /// @brief 将工作文件恢复到指定版本
    bool RestoreToFile(const std::wstring& fullPath, int version,
                       const std::wstring& targetPath);

    /// @brief 列出仓库中所有已追踪的文件路径
    std::vector<std::wstring> ListTrackedPaths();

    /// @brief 列出指定文件的所有版本
    std::vector<SnapshotInfo> GetVersions(const std::wstring& fullPath);

    /// @brief 直接扫描指定快照目录的版本文件（用于 tree 等遍历场景）
    std::vector<SnapshotInfo> GetVersionsFromDir(const std::wstring& snapDir);

    /// @brief 列出指定分支（支持嵌套，fork_v3 或 fork_v3/my-feature）的所有版本
    std::vector<SnapshotInfo> GetBranchVersions(const std::wstring& fullPath,
                                                 const std::string& branchPath);

    /// @brief 从分支重建指定版本到目标路径
    /// @param fullPath   文件完整路径
    /// @param branchPath 分支路径（fork_v3 或 fork_v3/my-feature）
    /// @param version    版本号
    /// @param targetPath 输出目标路径
    bool ReconstructBranchVersion(const std::wstring& fullPath,
                                  const std::string& branchPath,
                                  int version,
                                  const std::wstring& targetPath);

    /// @brief 读取指定版本的文件内容到内存
    std::vector<uint8_t> GetContent(const std::wstring& fullPath, int version);

    /// @brief 读取分支中指定版本的文件内容到内存
    std::vector<uint8_t> GetBranchContent(const std::wstring& fullPath,
                                          const std::string& branchPath,
                                          int version);

    /// @brief 将指定版本范围的增量文件复制到分支目录
    /// @param fullPath   文件完整路径
    /// @param fromVer    起始版本号（含）
    /// @param branchName 目标分支名称
    /// @return 成功返回 true
    bool CopyToBranch(const std::wstring& fullPath, int fromVer,
                      const std::string& branchName);

    /// @brief 删除指定版本范围的增量文件
    /// @param fullPath 文件完整路径
    /// @param fromVer  起始版本号（含）
    void DeleteVersions(const std::wstring& fullPath, int fromVer);

    /// @brief 压缩增量链：将 v{curVer-keep+1} 重建为新 v0，保留最近 keep 个版本
    /// @param curVer 当前版本号
    /// @param keep   保留的版本数（含新 v0）
    bool RebaseChain(const std::wstring& fullPath, int curVer, int keep);

    /// @brief 探测分支的增量版本范围
    /// @param fullPath   文件完整路径
    /// @param branchName 分支名称（UTF-8）
    /// @param outMinVer  输出：分支中最小版本号
    /// @param outMaxVer  输出：分支中最大版本号
    /// @return 分支存在且有效返回 true
    bool ProbeBranchRange(const std::wstring& fullPath,
                          const std::string& branchName,
                          int& outMinVer, int& outMaxVer);

    /// @brief 将分支提升为主线
    /// @param fullPath   文件完整路径
    /// @param branchName 分支名称（UTF-8）
    /// @param forkPoint  分叉点版本号（v0共享，此版本后的增量被替换）
    /// @return 成功返回 true
    bool PromoteBranch(const std::wstring& fullPath,
                       const std::string& branchName,
                       int forkPoint);

    /// @brief 删除整个分支目录
    bool DeleteBranch(const std::wstring& fullPath,
                      const std::string& branchName);

    const std::wstring& RepoPath() const { return repoPath_; }
    const std::wstring& SnapshotsPath() const { return snapDir_; }

private:
    std::wstring repoPath_;
    std::wstring snapDir_;

    static std::wstring PathToRel(const std::wstring& fullPath);
    std::wstring RelToSnapDir(const std::wstring& relPath) const;
    std::wstring VersionPath(const std::wstring& fullPath, int version) const;
    /// @brief 获取分支存储目录路径
    std::wstring BranchDir(const std::wstring& fullPath,
                            const std::string& branchName) const;
    int CountVersions(const std::wstring& snapDir) const;
};

} // namespace fssentry
