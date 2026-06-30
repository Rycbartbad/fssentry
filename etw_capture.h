/// @file etw_capture.h
/// @brief ETW 内核文件 I/O 捕获引擎 — EventInfo 数据结构 + LRU 缓存 + EtwCapture 类
#pragma once

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <evntrace.h>
#include <evntcons.h>

namespace fssentry {

class ProcessTracker;

struct EventInfo {
    uint64_t    timestamp;
    uint32_t    pid;
    uint8_t     opcode;
    std::wstring processName;
    std::wstring path;
    std::string  opName;
};

using EventCallback = std::function<void(const EventInfo&)>;

template<typename K, typename V, size_t MaxSize = 32768>
class LRUCache {
    struct Node {
        K key;
        V value;
        Node* prev;
        Node* next;
    };

public:
    LRUCache() : size_(0) { head_.prev = head_.next = &head_; }

    ~LRUCache() { Clear(); }

    LRUCache(const LRUCache&) = delete;
    LRUCache& operator=(const LRUCache&) = delete;

    void Put(K key, V value) {
        Node* existing = Find(key);
        if (existing) {
            existing->value = value;
            Detach(existing);
            AttachFront(existing);
            return;
        }
        if (size_ >= MaxSize) {
            Node* lru = head_.prev;
            Detach(lru);
            delete lru;
            --size_;
        }
        Node* n = new Node{key, value, nullptr, nullptr};
        AttachFront(n);
        ++size_;
    }

    const V* Get(const K& key) {
        Node* n = Find(key);
        if (!n) return nullptr;
        Detach(n);
        AttachFront(n);
        return &n->value;
    }

    void Clear() {
        Node* n = head_.next;
        while (n != &head_) {
            Node* next = n->next;
            delete n;
            n = next;
        }
        head_.prev = head_.next = &head_;
        size_ = 0;
    }

private:
    Node* Find(const K& key) {
        Node* n = head_.next;
        while (n != &head_) {
            if (n->key == key) return n;
            n = n->next;
        }
        return nullptr;
    }

    void Detach(Node* n) {
        n->prev->next = n->next;
        n->next->prev = n->prev;
    }

    void AttachFront(Node* n) {
        n->next = head_.next;
        n->prev = &head_;
        head_.next->prev = n;
        head_.next = n;
    }

    Node head_;
    size_t size_;
};

class EtwCapture {
public:
    EtwCapture();
    ~EtwCapture();

    EtwCapture(const EtwCapture&) = delete;
    EtwCapture& operator=(const EtwCapture&) = delete;

    bool Start(ProcessTracker& tracker);
    void Stop();

    bool IsRunning() const { return running_; }
    ULONG LastError() const { return lastError_; }

    void SetEventCallback(EventCallback cb) { eventCb_ = std::move(cb); }

private:
    static void WINAPI EventRecordCallback(EVENT_RECORD* pEvent);
    static ULONG  WINAPI BufferCallback(EVENT_TRACE_LOGFILEW* pLog);
    void OnEvent(EVENT_RECORD* pEvent);

    static DWORD WINAPI TraceThreadProc(LPVOID param);
    DWORD TraceThread();

    static DWORD WINAPI DiscoveryThreadProc(LPVOID param);
    DWORD DiscoveryThread();

    void BuildDriveMap();
    bool DeviceToDos(const wchar_t* src, wchar_t* dst, int dch) const;
    const wchar_t* FileMapResolve(uint64_t fileKey, uint64_t fileObj);
    const wchar_t* GetProcName(DWORD pid);
    void ResolveHandles(DWORD pid);
    bool ResolvePathLazy(DWORD pid, uint64_t fileKey, uint64_t fileObj);

    bool StartKernelSession();
    void StopKernelSession();
    void CloseTraceHandle();

    struct DriveMapEntry { std::wstring device; wchar_t drive[4]; };
    std::vector<DriveMapEntry> drives_;

    LRUCache<uint64_t, std::wstring, 4096> fileMap_;

    std::unordered_map<DWORD, std::wstring> procCache_;
    SRWLOCK procCacheLock_{SRWLOCK_INIT};

    std::set<uint64_t> lazyTried_;
    SRWLOCK lazyLock_{SRWLOCK_INIT};

    TRACEHANDLE sessionHandle_{0};
    TRACEHANDLE traceHandle_{0};
    HANDLE traceThread_{nullptr};
    HANDLE discoveryThread_{nullptr};
    HANDLE stopEvent_{nullptr};

    ProcessTracker* tracker_{nullptr};
    EventCallback   eventCb_;

    ULONG lastError_{ERROR_SUCCESS};
    std::atomic<bool> running_{false};
};

} // namespace fssentry
