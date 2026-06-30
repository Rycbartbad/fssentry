/// @file etw_capture.cpp
/// @brief ETW 内核文件 I/O 监控捕获实现（简化版）
/// @note 基于已验证的 ETW 监控代码，适配 fssentry 架构。

#include "etw_capture.h"
#include "process_tracker.h"

#include <tdh.h>
#include <sal.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <memory>

static const GUID kCustomSessionGuid = {
    0xE70D9E1C, 0x2E7B, 0x4F6F, {0x8A, 0x3C, 0x9A, 0x5B, 0x6C, 0x7D, 0x8E, 0x9F}
};

#ifndef EVENT_TRACE_SYSTEM_LOGGER_MODE
#define EVENT_TRACE_SYSTEM_LOGGER_MODE 0x02000000
#endif

namespace fssentry {

EtwCapture::EtwCapture()
{
    stopEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    BuildDriveMap();
}

EtwCapture::~EtwCapture()
{
    Stop();
    if (stopEvent_) {
        CloseHandle(stopEvent_);
        stopEvent_ = nullptr;
    }
}

void EtwCapture::BuildDriveMap()
{
    drives_.clear();
    wchar_t buf[MAX_PATH];
    for (wchar_t ch = L'A'; ch <= L'Z'; ch++) {
        wchar_t root[4] = { ch, L':', L'\\', 0 };
        if (GetDriveTypeW(root) == DRIVE_NO_ROOT_DIR)
            continue;
        wchar_t name[8] = { ch, L':', 0 };
        DWORD len = QueryDosDeviceW(name, buf, MAX_PATH);
        if (!len || len > MAX_PATH)
            continue;
        DriveMapEntry e;
        e.drive[0] = ch; e.drive[1] = L':'; e.drive[2] = 0;
        e.device = buf;
        drives_.push_back(std::move(e));
    }
}

bool EtwCapture::DeviceToDos(const wchar_t* src, wchar_t* dst, int dch) const
{
    if (!src || !dst || dch <= 0)
        return false;
    for (const auto& e : drives_) {
        int dl = (int)e.device.length();
        if (dl > 0 && _wcsnicmp(src, e.device.c_str(), dl) == 0 &&
            (src[dl] == L'\\' || src[dl] == 0))
        {
            wcscpy_s(dst, dch, e.drive);
            if (src[dl] == L'\\')
                wcscat_s(dst, dch, src + dl);
            else
                wcscat_s(dst, dch, L"\\");
            return true;
        }
    }
    wcsncpy_s(dst, dch, src, _TRUNCATE);
    return false;
}

const wchar_t* EtwCapture::FileMapResolve(uint64_t fileKey, uint64_t fileObj)
{
    if (fileKey) {
        auto* v = fileMap_.Get(fileKey);
        if (v) return v->c_str();
    }
    if (fileObj) {
        auto* v = fileMap_.Get(fileObj);
        if (v) return v->c_str();
    }
    return nullptr;
}

const wchar_t* EtwCapture::GetProcName(DWORD pid)
{
    {
        AcquireSRWLockShared(&procCacheLock_);
        auto it = procCache_.find(pid);
        if (it != procCache_.end()) {
            const wchar_t* r = it->second.c_str();
            ReleaseSRWLockShared(&procCacheLock_);
            return r;
        }
        ReleaseSRWLockShared(&procCacheLock_);
    }

    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    std::wstring name;
    if (!h) {
        name = L"Unknown";
    } else {
        wchar_t buf[MAX_PATH];
        DWORD sz = MAX_PATH;
        if (QueryFullProcessImageNameW(h, 0, buf, &sz) && sz > 0) {
            wchar_t* base = wcsrchr(buf, L'\\');
            name = base ? base + 1 : buf;
        } else {
            name = L"Unknown";
        }
        CloseHandle(h);
    }

    AcquireSRWLockExclusive(&procCacheLock_);
    auto it = procCache_.find(pid);
    if (it == procCache_.end())
        it = procCache_.emplace(pid, std::move(name)).first;
    const wchar_t* r = it->second.c_str();
    ReleaseSRWLockExclusive(&procCacheLock_);
    return r;
}

static LONG (WINAPI* g_NtQSI)(ULONG, PVOID, ULONG, PULONG) = nullptr;
static LONG InitNtQSI()
{
    if (!g_NtQSI)
        g_NtQSI = (LONG(WINAPI*)(ULONG, PVOID, ULONG, PULONG))
            GetProcAddress(GetModuleHandleA("ntdll"), "NtQuerySystemInformation");
    return g_NtQSI ? 0 : -1;
}

struct HandleInfo {
    PVOID Obj; HANDLE Pid; HANDLE H;
    ULONG Ga; USHORT Cb; USHORT OT; ULONG Ha; ULONG Rs;
};

void EtwCapture::ResolveHandles(DWORD pid)
{
    if (!pid) return;

    if (InitNtQSI()) return;

    ULONG sz = 64 * 1024;
    std::vector<BYTE> buf(sz);
    if (g_NtQSI(64, buf.data(), sz, &sz)) {
        if (sz <= buf.size()) return;
        buf.resize(sz);
        if (g_NtQSI(64, buf.data(), sz, &sz)) return;
    }

    ULONG_PTR cnt = ((ULONG_PTR*)buf.data())[0];
    auto ep = (HandleInfo*)((ULONG_PTR*)buf.data() + 2);

    std::vector<std::pair<HANDLE, uint64_t>> candidates;
    const size_t kMaxCandidates = 4096;
    for (ULONG_PTR i = 0; i < cnt && candidates.size() < kMaxCandidates; i++) {
        if ((DWORD)(ULONG_PTR)ep[i].Pid == pid)
            candidates.push_back({ ep[i].H, (uint64_t)(ULONG_PTR)ep[i].Obj });
    }

    HANDLE hProc = OpenProcess(PROCESS_DUP_HANDLE | PROCESS_QUERY_INFORMATION,
                                FALSE, pid);
    if (!hProc) return;

    for (const auto& c : candidates) {
        HANDLE hDup = nullptr;
        BOOL ok = DuplicateHandle(hProc, c.first, GetCurrentProcess(),
                                   &hDup, 0, FALSE, DUPLICATE_SAME_ACCESS);
        if (!ok) {
            ok = DuplicateHandle(hProc, c.first, GetCurrentProcess(),
                                 &hDup, FILE_READ_ATTRIBUTES, FALSE, 0);
        }
        if (!ok) continue;
        wchar_t pth[MAX_PATH];
        DWORD len = GetFinalPathNameByHandleW(hDup, pth, MAX_PATH,
                                              VOLUME_NAME_DOS);
        CloseHandle(hDup);
        if (len == 0 || len >= MAX_PATH) continue;
        wchar_t* p = pth;
        if (wcsncmp(p, L"\\\\?\\", 4) == 0) p += 4;
        fileMap_.Put(c.second, std::wstring(p, (std::min)((int)wcslen(p), 259)));
    }
    CloseHandle(hProc);
}

bool EtwCapture::ResolvePathLazy(DWORD pid, uint64_t fileKey, uint64_t fileObj)
{
    uint64_t key = fileKey ? fileKey : fileObj;
    if (!key) return false;

    auto MarkFailed = [&]() {
        AcquireSRWLockExclusive(&lazyLock_);
        lazyTried_.insert(key);
        ReleaseSRWLockExclusive(&lazyLock_);
    };

    {
        AcquireSRWLockShared(&lazyLock_);
        if (lazyTried_.count(key)) {
            ReleaseSRWLockShared(&lazyLock_);
            return false;
        }
        ReleaseSRWLockShared(&lazyLock_);
    }

    if (InitNtQSI()) {
        MarkFailed(); return false;
    }

    ULONG sz = 64 * 1024;
    static std::vector<BYTE> buf;
    if (buf.empty()) buf.resize(sz);
    if (g_NtQSI(64, buf.data(), static_cast<ULONG>(buf.size()), &sz)) {
        if (sz <= buf.size()) {
            MarkFailed(); return false;
        }
        buf.resize(sz);
        if (g_NtQSI(64, buf.data(), static_cast<ULONG>(buf.size()), &sz)) {
            MarkFailed(); return false;
        }
    }

    ULONG_PTR cnt = ((ULONG_PTR*)buf.data())[0];
    auto ep = (HandleInfo*)((ULONG_PTR*)buf.data() + 2);

    HANDLE hTarget = nullptr;
    for (ULONG_PTR i = 0; i < cnt; i++) {
        if ((DWORD)(ULONG_PTR)ep[i].Pid == pid &&
            (uint64_t)(ULONG_PTR)ep[i].Obj == key) {
            hTarget = ep[i].H;
            break;
        }
    }

    if (!hTarget) {
        MarkFailed(); return false;
    }

    HANDLE hProc = OpenProcess(PROCESS_DUP_HANDLE, FALSE, pid);
    if (!hProc) {
        MarkFailed(); return false;
    }

    HANDLE hDup = nullptr;
    BOOL ok = DuplicateHandle(hProc, hTarget, GetCurrentProcess(),
                               &hDup, 0, FALSE, DUPLICATE_SAME_ACCESS);
    if (!ok) {
        ok = DuplicateHandle(hProc, hTarget, GetCurrentProcess(),
                             &hDup, FILE_READ_ATTRIBUTES, FALSE, 0);
    }
    CloseHandle(hProc);

    if (!ok || !hDup) {
        MarkFailed(); return false;
    }

    wchar_t path[MAX_PATH];
    DWORD len = GetFinalPathNameByHandleW(hDup, path, MAX_PATH, VOLUME_NAME_DOS);
    CloseHandle(hDup);

    if (len == 0 || len >= MAX_PATH) {
        MarkFailed(); return false;
    }

    wchar_t* p = path;
    if (wcsncmp(p, L"\\\\?\\", 4) == 0) p += 4;

    fileMap_.Put(key, std::wstring(p, (std::min)((int)wcslen(p), 259)));
    return true;
}

void WINAPI EtwCapture::EventRecordCallback(EVENT_RECORD* pEvent)
{
    if (!pEvent || !pEvent->UserContext) return;
    auto* self = static_cast<EtwCapture*>(pEvent->UserContext);
    self->OnEvent(pEvent);
}

ULONG WINAPI EtwCapture::BufferCallback(EVENT_TRACE_LOGFILEW* /*pLog*/)
{
    return 1;
}

void EtwCapture::OnEvent(EVENT_RECORD* pEvent)
{
    if (!pEvent) return;

    DWORD pid = pEvent->EventHeader.ProcessId;
    UCHAR op  = pEvent->EventHeader.EventDescriptor.Opcode;

    if (pid == 0) return;

    if (tracker_ && !tracker_->IsMonitored(pid)) {
        return;
    }

    int ps = (pEvent->EventHeader.Flags & EVENT_HEADER_FLAG_64_BIT_HEADER) ? 8 :
             (pEvent->EventHeader.Flags & EVENT_HEADER_FLAG_32_BIT_HEADER) ? 4 :
             sizeof(void*);
    static const int ttid_sz = 4;

    BYTE* data = (BYTE*)pEvent->UserData;
    ULONG dlen = pEvent->UserDataLength;
    uint64_t fObj = 0, fKey = 0;
    const wchar_t* resolved = nullptr;

    switch (op) {
        case 0: case 32: case 35:
            if (dlen >= (ULONG)(ps + 2)) {
                memcpy(&fObj, data, ps);
                wchar_t* nm = (wchar_t*)(data + ps);
                int mc = (dlen - (ULONG)ps) / 2; if (mc > 256) mc = 256;
                int al = 0;
                while (al < mc && nm[al]) al++;
                fileMap_.Put(fObj, std::wstring(nm, al));
                resolved = nm;
            }
            break;

        case 64: {
            unsigned fixed = (unsigned)(ps + ps + ttid_sz + 4 + 4 + 4);
            if (dlen >= fixed + 2) {
                memcpy(&fObj, data + ps, ps);
                wchar_t* pt = (wchar_t*)(data + fixed);
                int mc = (dlen - fixed) / 2; if (mc > 256) mc = 256;
                int al = 0;
                while (al < mc && pt[al]) al++;
                fileMap_.Put(fObj, std::wstring(pt, al));
                resolved = pt;
            }
            break;
        }

        case 67: case 68: {
            unsigned off_fObj = 8 + ps;            // after Offset + IrpPtr
            unsigned off_fKey = off_fObj + ps;     // after FileObject
            unsigned total = off_fKey + ps + ps + 4 + 4;
            if (dlen >= total) {
                memcpy(&fObj, data + off_fObj, ps);
                memcpy(&fKey, data + off_fKey, ps);
                resolved = FileMapResolve(fKey, fObj);
                if (!resolved) {
                    if (ResolvePathLazy(pid, fKey, fObj)) {
                        auto* v = fileMap_.Get(fKey ? fKey : fObj);
                        resolved = v ? v->c_str() : nullptr;
                    }
                }
            }
            break;
        }

        case 65: case 66: case 73: {
            unsigned total = ps + ps + ps + ttid_sz;
            if (dlen >= total) {
                memcpy(&fObj, data + ps, ps);
                memcpy(&fKey, data + ps + ps, ps);
                resolved = FileMapResolve(fKey, fObj);
                if (!resolved) {
                    if (ResolvePathLazy(pid, fKey, fObj)) {
                        auto* v = fileMap_.Get(fKey ? fKey : fObj);
                        resolved = v ? v->c_str() : nullptr;
                    }
                }
            }
            break;
        }

        case 69: case 70: case 71: case 74: case 75: {
            unsigned total = ps + ps + ps + ps + ttid_sz + 4;
            if (dlen >= total) {
                memcpy(&fObj, data + ps, ps);
                memcpy(&fKey, data + ps + ps, ps);
                resolved = FileMapResolve(fKey, fObj);
            }
            break;
        }

        case 76:
            break;
    }

    switch (op) {
        case 0: case 32: case 35: case 64: case 65: case 66:
        case 67: case 68: case 69: case 70: case 71: case 73: case 75:
            break;
        default:
            return;
    }

    if (!resolved) {
        return;
    }

    const wchar_t* pname = GetProcName(pid);
    wchar_t curPath[260]; curPath[0] = 0;
    DeviceToDos(resolved, curPath, 260);
    int plen = (int)wcslen(curPath);
    if (plen == 0) {
        return;
    }
    if (plen > 3 && curPath[plen - 1] == L'\\') curPath[plen - 1] = 0;
    if (plen == 3 && curPath[1] == L':' && curPath[2] == L'\\') {
        return;
    }

    if (tracker_ && tracker_->IsPathBlacklisted(curPath)) return;

    {
        DWORD attr = GetFileAttributesW(curPath);
        if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY))
            return;
    }

    if (eventCb_) {
        FILETIME ft;
        SYSTEMTIME st;
        GetLocalTime(&st);
        SystemTimeToFileTime(&st, &ft);
        uint64_t eventTs = (static_cast<uint64_t>(ft.dwHighDateTime) << 32)
                         | ft.dwLowDateTime;

        EventInfo info;
        info.timestamp   = eventTs;
        info.pid         = pid;
        info.opcode      = op;
        info.processName = pname;
        info.path        = curPath;
        eventCb_(info);
    }
}

bool EtwCapture::StartKernelSession()
{
    constexpr wchar_t kSessionName[] = L"FssentryEtwSvc";
    ULONG bufSize = sizeof(EVENT_TRACE_PROPERTIES)
                    + sizeof(kSessionName) + sizeof(kSessionName) + 256;
    auto props = std::make_unique<BYTE[]>(bufSize);
    ZeroMemory(props.get(), bufSize);
    auto p = (EVENT_TRACE_PROPERTIES*)props.get();

    auto initProps = [&]() {
        p->Wnode.BufferSize     = bufSize;
        p->Wnode.Flags          = WNODE_FLAG_TRACED_GUID;
        p->Wnode.Guid           = kCustomSessionGuid;
        p->Wnode.ClientContext  = 1;
        p->BufferSize           = 64;
        p->MinimumBuffers       = 4;
        p->MaximumBuffers       = 32;
        p->LogFileMode          = EVENT_TRACE_REAL_TIME_MODE
                                | EVENT_TRACE_SYSTEM_LOGGER_MODE;
        p->EnableFlags          = 0x06000000;
        p->LoggerNameOffset     = sizeof(EVENT_TRACE_PROPERTIES);
    };
    initProps();

    ULONG status = StartTraceW(&sessionHandle_, kSessionName, p);
    if (status == ERROR_ALREADY_EXISTS) {
        ControlTraceW(0, kSessionName, p, EVENT_TRACE_CONTROL_STOP);
        ZeroMemory(props.get(), bufSize);
        initProps();
        status = StartTraceW(&sessionHandle_, kSessionName, p);
    }

    if (status != ERROR_SUCCESS) {
        lastError_ = status;
        return false;
    }

    return true;
}

void EtwCapture::StopKernelSession()
{
    if (sessionHandle_) {
        BYTE buf[sizeof(EVENT_TRACE_PROPERTIES) + 256] = {};
        auto p = (EVENT_TRACE_PROPERTIES*)buf;
        p->Wnode.BufferSize = sizeof(buf);
        ControlTraceW(sessionHandle_, nullptr, p, EVENT_TRACE_CONTROL_STOP);
        sessionHandle_ = 0;
    }
}

void EtwCapture::CloseTraceHandle()
{
    if (traceHandle_ && traceHandle_ != INVALID_PROCESSTRACE_HANDLE) {
        ::CloseTrace(traceHandle_);
        traceHandle_ = 0;
    }
}

DWORD WINAPI EtwCapture::TraceThreadProc(LPVOID param)
{
    return ((EtwCapture*)param)->TraceThread();
}

DWORD EtwCapture::TraceThread()
{
    wchar_t loggerName[] = L"FssentryEtwSvc";

    EVENT_TRACE_LOGFILEW lf = {};
    lf.LoggerName           = loggerName;
    lf.ProcessTraceMode     = PROCESS_TRACE_MODE_REAL_TIME
                            | PROCESS_TRACE_MODE_EVENT_RECORD;
    lf.EventRecordCallback  = reinterpret_cast<PEVENT_RECORD_CALLBACK>(
                                EventRecordCallback);
    lf.BufferCallback       = reinterpret_cast<PEVENT_TRACE_BUFFER_CALLBACKW>(
                                BufferCallback);
    lf.IsKernelTrace        = TRUE;
    lf.Context              = this;

    traceHandle_ = OpenTraceW(&lf);
    if (traceHandle_ == INVALID_PROCESSTRACE_HANDLE) {
        StopKernelSession();
        return 1;
    }

    ProcessTrace(&traceHandle_, 1, nullptr, 0);

    running_ = false;
    return 0;
}

bool EtwCapture::Start(ProcessTracker& tracker)
{
    if (running_) return true;

    lastError_ = ERROR_SUCCESS;
    tracker_ = &tracker;

    auto pids = tracker_->GetMonitoredPids();
    for (DWORD pid : pids) {
        ResolveHandles(pid);
    }

    if (!StartKernelSession()) {
        return false;
    }

    ResetEvent(stopEvent_);
    running_ = true;
    traceThread_ = CreateThread(nullptr, 0, TraceThreadProc, this, 0, nullptr);
    if (!traceThread_) {
        running_ = false;
        CloseTraceHandle();
        StopKernelSession();
        return false;
    }

    discoveryThread_ = CreateThread(nullptr, 0, DiscoveryThreadProc, this, 0, nullptr);

    return true;
}

void EtwCapture::Stop()
{
    if (!running_) return;

    SetEvent(stopEvent_);  // 通知 discovery 线程退出

    CloseTraceHandle();

    if (discoveryThread_) {
        WaitForSingleObject(discoveryThread_, 5000);
        CloseHandle(discoveryThread_);
        discoveryThread_ = nullptr;
    }

    if (traceThread_) {
        WaitForSingleObject(traceThread_, 5000);
        CloseHandle(traceThread_);
        traceThread_ = nullptr;
    }

    StopKernelSession();
    if (!eventCb_) {
        fwprintf(stdout, L"\n");
    }
    running_ = false;
}

DWORD WINAPI EtwCapture::DiscoveryThreadProc(LPVOID param)
{
    return ((EtwCapture*)param)->DiscoveryThread();
}

DWORD EtwCapture::DiscoveryThread()
{
    while (true) {
        DWORD waitResult = WaitForSingleObject(stopEvent_, 2000);
        if (waitResult == WAIT_OBJECT_0) {
            break;  // 收到停止信号
        }
        if (tracker_) {
            auto newPids = tracker_->DiscoverRunningProcesses();
            if (!newPids.empty()) {
                for (DWORD pid : newPids) {
                    {
                        AcquireSRWLockShared(&procCacheLock_);
                        bool cached = procCache_.count(pid) > 0;
                        ReleaseSRWLockShared(&procCacheLock_);
                        if (cached) continue;
                    }
                    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
                    if (h) {
                        wchar_t buf[MAX_PATH];
                        DWORD sz = MAX_PATH;
                        if (QueryFullProcessImageNameW(h, 0, buf, &sz) && sz > 0) {
                            wchar_t* base = wcsrchr(buf, L'\\');
                            AcquireSRWLockExclusive(&procCacheLock_);
                            if (procCache_.find(pid) == procCache_.end())
                                procCache_.emplace(pid, base ? base + 1 : buf);
                            ReleaseSRWLockExclusive(&procCacheLock_);
                        }
                        CloseHandle(h);
                    }
                }
            }
        }
    }

    return 0;
}

} // namespace fssentry
