# fssentry

**File System Sentry** — Real-time snapshot versioning tool powered by Windows ETW (Event Tracing for Windows) kernel-level file I/O monitoring.

fssentry uses ETW to capture file operations of target processes (e.g., AI coding assistants), automatically creates versioned snapshots with binary delta compression, and provides an interactive CLI for file browsing, version restoration, and branch management.

---

## Features

- **Kernel-level file I/O monitoring** — Captures file create, read, write, delete, rename and other operations in real-time via Windows ETW
- **Process whitelist** — Monitors only specified processes; automatically discovers newly launched matching processes
- **Path blacklist** — Flexible wildcard-based path filtering to exclude system and temporary files
- **Incremental version snapshots** — Automatically creates snapshots on file change; subsequent versions store only binary deltas (rsync-style rolling hash compression) to save disk space
- **Version chain rebase** — Automatically compresses the delta chain when version count exceeds the threshold, retaining only the most recent N versions
- **Branch management** — Create branches from any version (`restore`), switch between branches (`switch`), and promote branches to mainline (automatic on `switch`)
- **Interactive CLI** — Unix-like command-line interface with command history and file content inspection

---

## Requirements

| Condition | Detail |
|-----------|--------|
| OS | Windows 10+ (x64 only) |
| Compiler | MSVC (Visual Studio 2019+) |
| CMake | 3.28+ |
| Privilege | **Administrator** (required by ETW kernel tracing — `SeSystemProfilePrivilege`) |

### Dependencies

- `bcrypt` — SHA-256 hashing (system library)
- `advapi32` — ETW session management (system library)
- `tdh` — ETW event decoding (system library)

### Build

```powershell
# Build Release with CMake
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release

# Run (requires administrator)
# The program will attempt to launch in Windows Terminal; pass --no-wt to disable
.\build\Release\fssentry.exe
```

---

## Usage

### Quick Start

```powershell
# Run as administrator
fssentry.exe
```

On startup, the program will:
1. Enable required ETW privileges
2. Scan and monitor `opencode.exe` and `claude.exe` processes
3. Create the snapshot repository at `%ProgramData%\fssentry\repo`
4. Enter the interactive CLI

### Commands

| Command | Description |
|---------|-------------|
| `ls` | List files and directories in the current directory, showing version count per file |
| `cd <dir>` | Change directory (`..` for parent, `/` for root) |
| `tree [path]` | Display version history and branch structure as a tree |
| `restore <file> <version>` | Restore a file to a specified version (auto-creates a branch to preserve replaced versions) |
| `show <file> <version>` | View or export file content at a specified version (`-o` for output path, `-b` for branch) |
| `switch <file> <branch>` | Switch a file to a different branch |
| `monitor` | Enable real-time event display (press any key to stop) |
| `delete <file/dir>` | Delete all version records for a file or directory |
| `help` | Show help information |
| `quit` / `exit` | Exit |

### Example Session

```
> ls
  d:/projects/
  d:/work/
> cd d:/projects/myapp/src
> tree
  main.cpp  [12v]
  utils/
  ├── helper.cpp  [5v]
  └── config.json  [3v]
> tree main.cpp
d:\projects\myapp\src\main.cpp
├─ v0  2026/06/17 10:00  2048 bytes
├─ v1  2026/06/17 10:05  2100 bytes
├─ v2  2026/06/17 10:12  2156 bytes
└─ v3  2026/06/17 10:30  2200 bytes
> show main.cpp v2
// ... displays v2 content ...
> restore main.cpp v2
Restored to v2 -> d:\projects\myapp\src\main.cpp
> tree main.cpp
d:\projects\myapp\src\main.cpp
├─ v0  2026/06/17 10:00  2048 bytes
├─ v1  2026/06/17 10:05  2100 bytes
├─ v2  2026/06/17 10:12  2156 bytes
├─ v3  2026/06/17 10:30  2200 bytes
└─ .branches
   └─ fork_v2_20260617T104500
      └─ v3
```

### Modules

| Module | Files | Responsibility |
|--------|-------|---------------|
| `EtwCapture` | `etw_capture.h/.cpp` | ETW kernel session management, event capture & parsing, file path resolution (device path → DOS path), LRU cache |
| `ProcessTracker` | `process_tracker.h/.cpp` | Process whitelist/path blacklist management, Toolhelp32 snapshot scanning |
| `SnapshotStore` | `snapshot_store.h/.cpp` | Snapshot repository directory structure, incremental version storage/reconstruction, branch operations (create/switch/promote/delete), delta chain rebase |
| `Delta` | `delta.h/.cpp` | Custom binary delta encode/decode (rolling hash sliding window match, copy/insert instructions) |
| `VersionTracker` | `version_tracker.h/.cpp` | In-memory version counting + SHA-256 hash deduplication |
| `Hash` | `hash.h` | SHA-256 file hashing (inline, BCrypt API) |
| `CLI` | `cli.h/.cpp` | Interactive command line (Command pattern, Unix-like virtual file system, history) |

### Storage Layout

```
%ProgramData%\fssentry\repo\
└── snapshots\
    └── <drive><rel_path>\
        ├── v0                ← base file (full copy)
        ├── v1.delta          ← v0 → v1 binary delta
        ├── v2.delta          ← v1 → v2 binary delta
        └── .branches\
            └── fork_v2_20260617T104500\
                ├── v3.delta  ← delta forked from v2
                └── ...
```

> No metadata files — all state is derived entirely from the directory structure.

---

## License

MIT
