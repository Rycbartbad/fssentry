/// @file cli.h
/// @brief CLI 命令多态 — Command 模式
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <queue>
#include <string>
#include <vector>
#include <windows.h>

namespace fssentry {

struct EventInfo;

class SnapshotStore;
class VersionTracker;
class EtwCapture;

/// @brief CLI 共享上下文 — 所有命令通过此结构访问全局状态
struct CliContext {
    SnapshotStore& store;
    VersionTracker& verTracker;
    std::wstring& virtDir;
    bool& monitor;
    std::queue<EventInfo>& eventQueue;
    SRWLOCK& queueLock;
    EtwCapture& capture;
    bool& running;
};

/// @brief 命令抽象基类
class Command {
public:
    virtual ~Command() = default;

    /// @brief 执行命令（非虚模板方法：自动处理 -h/--help）
    /// @param args  命令行参数（parts[0] 是命令名本身）
    /// @param ctx   CLI 共享上下文
    void Execute(const std::vector<std::wstring>& args,
                 CliContext& ctx);

    /// @brief 命令名称（小写，用于匹配）
    virtual const wchar_t* Name() const = 0;

    /// @brief 简短描述
    virtual const wchar_t* Description() const = 0;

    /// @brief 用法帮助字符串（可覆写提供详细用法）
    virtual std::wstring Usage() const {
        std::wstring u = Name();
        u += L" — ";
        u += Description();
        return u;
    }

protected:
    /// @brief 子类实现具体命令逻辑（由 Execute 在检测 -h 后调用）
    virtual void Run(const std::vector<std::wstring>& args,
                     CliContext& ctx) = 0;
};

/// @brief 命令注册表
class CommandRegistry {
    std::map<std::wstring, Command*> cmds_;
public:
    /// @brief 注册命令（不取得所有权，仅存储指针）
    void Register(Command* cmd);

    /// @brief 注册别名（同一命令的另一个名字）
    void RegisterAlias(const std::wstring& name, Command* cmd);

    /// @brief 按名称查找命令（不区分大小写）
    Command* Find(const std::wstring& name) const;

    /// @brief 获取所有注册命令（用于 help）
    const std::map<std::wstring, Command*>& All() const { return cmds_; }
};

/// @brief 绝对路径 → 虚拟路径
std::wstring ToVirt(const std::wstring& path);

/// @brief 虚拟路径 → 绝对路径
std::wstring FromVirt(const std::wstring& path);

/// @brief 宽字符串 → UTF-8 窄字符串
std::string narrow(const std::wstring& ws);

/// @brief 操作码 → 缩写名
const char* OpName(uint8_t op);

/// @brief 格式化事件为表格行
std::string FormatEvent(const EventInfo& e);

/// @brief 打印提示符
void PrintPrompt(const std::wstring& virtDir);

/// @brief 排空共享事件队列并输出
void DrainEvents(CliContext& ctx);

/// @brief 解析命令行为 tokens
std::vector<std::wstring> ParseCommandLine(const std::wstring& cmd);

/// @brief 注册内置命令到注册表
void RegisterBuiltinCommands(CommandRegistry& reg);

} // namespace fssentry
