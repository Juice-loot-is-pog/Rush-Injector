#include "inject.h"
#include <TlHelp32.h>
#include <psapi.h>
#include <fstream>
#include <string>
#include <tuple>
#include <vector>
#include <utility>

namespace {
constexpr DWORD injectionAccess = PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
    PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ;
struct Handle {
    HANDLE value;
    explicit Handle(HANDLE h) : value(h) {}
    ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
};
std::wstring tokenDetails(HANDLE process) {
    Handle token(nullptr);
    if (!OpenProcessToken(process, TOKEN_QUERY, &token.value))
        return L"tokenError=" + std::to_wstring(GetLastError());
    TOKEN_ELEVATION elevation{};
    DWORD bytes = 0;
    std::wstring details = L"elevated=";
    if (GetTokenInformation(token.value, TokenElevation, &elevation, sizeof(elevation), &bytes))
        details += std::to_wstring(elevation.TokenIsElevated);
    else details += L"unknown(error=" + std::to_wstring(GetLastError()) + L")";
    GetTokenInformation(token.value, TokenIntegrityLevel, nullptr, 0, &bytes);
    std::vector<BYTE> integrity(bytes);
    details += L",integrityRid=";
    if (bytes && GetTokenInformation(token.value, TokenIntegrityLevel, integrity.data(), bytes, &bytes)) {
        PSID sid = reinterpret_cast<TOKEN_MANDATORY_LABEL*>(integrity.data())->Label.Sid;
        details += std::to_wstring(*GetSidSubAuthority(sid, *GetSidSubAuthorityCount(sid) - 1));
    } else details += L"unknown(error=" + std::to_wstring(GetLastError()) + L")";
    LUID debug{};
    if (!LookupPrivilegeValueW(nullptr, SE_DEBUG_NAME, &debug))
        return details + L",debug=unknown(error=" + std::to_wstring(GetLastError()) + L")";
    bytes = 0;
    GetTokenInformation(token.value, TokenPrivileges, nullptr, 0, &bytes);
    std::vector<BYTE> privileges(bytes);
    if (!bytes || !GetTokenInformation(token.value, TokenPrivileges, privileges.data(), bytes, &bytes))
        return details + L",debug=unknown(error=" + std::to_wstring(GetLastError()) + L")";
    auto* entries = reinterpret_cast<TOKEN_PRIVILEGES*>(privileges.data());
    for (DWORD i = 0; i < entries->PrivilegeCount; ++i) {
        const auto& entry = entries->Privileges[i];
        if (entry.Luid.LowPart == debug.LowPart && entry.Luid.HighPart == debug.HighPart)
            return details + ((entry.Attributes & SE_PRIVILEGE_ENABLED) ? L",debug=enabled" : L",debug=disabled");
    }
    return details + L",debug=absent";
}
HANDLE moduleSnapshot(DWORD pid) {
    DWORD error = ERROR_BAD_LENGTH;
    for (int i = 0; i < 15; ++i) {
        HANDLE h = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
        if (h != INVALID_HANDLE_VALUE) return h;
        error = GetLastError();
        // Partial copy: the target's loader data is unreadable while it starts or exits.
        if (error == ERROR_PARTIAL_COPY) Sleep(100);
        else if (error != ERROR_BAD_LENGTH) break;
    }
    SetLastError(error);
    return INVALID_HANDLE_VALUE;
}
// Only an elevated token holds this privilege; elsewhere the request has no effect.
void ensureDebugPrivilege() {
    static const bool requested = [] {
        Handle token(nullptr);
        LUID debug{};
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token.value) ||
            !LookupPrivilegeValueW(nullptr, SE_DEBUG_NAME, &debug)) return false;
        TOKEN_PRIVILEGES privileges{};
        privileges.PrivilegeCount = 1;
        privileges.Privileges[0].Luid = debug;
        privileges.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
        return AdjustTokenPrivileges(token.value, FALSE, &privileges, sizeof(privileges), nullptr, nullptr) != FALSE;
    }();
    (void)requested;
}
std::vector<ProcessCandidate> listProcesses(const wchar_t* procName) {
    std::vector<ProcessCandidate> candidates;
    Handle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
    if (snapshot.value == INVALID_HANDLE_VALUE) return candidates;
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (Process32FirstW(snapshot.value, &entry)) do {
        if (_wcsicmp(entry.szExeFile, procName) == 0) {
            ProcessCandidate candidate;
            candidate.pid = entry.th32ProcessID;
            candidate.parentPid = entry.th32ParentProcessID;
            candidate.threads = entry.cntThreads;
            candidates.push_back(candidate);
        }
    } while (Process32NextW(snapshot.value, &entry));
    return candidates;
}
void probeProcess(ProcessCandidate& candidate) {
    ensureDebugPrivilege();
    Handle process(OpenProcess(injectionAccess, FALSE, candidate.pid));
    candidate.accessError = process.value ? ERROR_SUCCESS : GetLastError();
    Handle limited(process.value ? nullptr : OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, candidate.pid));
    FILETIME created{}, exited{}, kernel{}, user{};
    if (GetProcessTimes(process.value ? process.value : limited.value, &created, &exited, &kernel, &user))
        candidate.created = (ULONGLONG(created.dwHighDateTime) << 32) | created.dwLowDateTime;
}
DWORD validateDll(const std::wstring& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return ERROR_FILE_NOT_FOUND;
    file.seekg(0, std::ios::end);
    const std::streamoff size = file.tellg();
    file.seekg(0);
    IMAGE_DOS_HEADER dos{};
    if (!file.read(reinterpret_cast<char*>(&dos), sizeof(dos)) || dos.e_magic != IMAGE_DOS_SIGNATURE ||
        dos.e_lfanew < sizeof(dos) || static_cast<std::streamoff>(dos.e_lfanew) > size - 24)
        return ERROR_BAD_EXE_FORMAT;
    file.seekg(dos.e_lfanew);
    DWORD signature = 0;
    IMAGE_FILE_HEADER header{};
    file.read(reinterpret_cast<char*>(&signature), sizeof(signature));
    file.read(reinterpret_cast<char*>(&header), sizeof(header));
    if (!file || signature != IMAGE_NT_SIGNATURE || header.Machine != IMAGE_FILE_MACHINE_AMD64 ||
        !(header.Characteristics & IMAGE_FILE_DLL)) return ERROR_BAD_EXE_FORMAT;
    return ERROR_SUCCESS;
}
bool isLoaded(HANDLE process, DWORD pid, const std::wstring& path) {
    Handle snapshot(moduleSnapshot(pid));
    if (snapshot.value == INVALID_HANDLE_VALUE) return false;
    MODULEENTRY32W module{};
    module.dwSize = sizeof(module);
    if (Module32FirstW(snapshot.value, &module)) do {
        // Toolhelp may substitute characters in non-ASCII paths. Query the
        // actual Unicode module name from the process instead.
        std::vector<wchar_t> name(32768);
        if (K32GetModuleFileNameExW(process, module.hModule, name.data(), static_cast<DWORD>(name.size())) &&
            _wcsicmp(name.data(), path.c_str()) == 0) return true;
    } while (Module32NextW(snapshot.value, &module));
    return false;
}
}
std::wstring DescribeProcessAccess(DWORD procId) {
    struct RestoreError {
        DWORD error = GetLastError();
        ~RestoreError() { SetLastError(error); }
    } restore;
    std::wstring details = L"accessDiag=v1 | self{" + tokenDetails(GetCurrentProcess()) + L"}";
    for (const auto& entry : { std::pair{L"selfSession", GetCurrentProcessId()},
                              std::pair{L"targetSession", procId} }) {
        DWORD session = 0;
        details += L" | " + std::wstring(entry.first) + L"=";
        if (ProcessIdToSessionId(entry.second, &session)) details += std::to_wstring(session);
        else details += L"unknown(error=" + std::to_wstring(GetLastError()) + L")";
    }
    Handle target(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, procId));
    if (target.value) details += L" | target{" + tokenDetails(target.value) + L"}";
    else details += L" | target{queryError=" + std::to_wstring(GetLastError()) + L"}";
    // Probe each requested permission independently; never write memory or start a thread.
    for (const auto& entry : { std::pair{L"limitedQuery", DWORD(PROCESS_QUERY_LIMITED_INFORMATION)},
                              std::pair{L"query", DWORD(PROCESS_QUERY_INFORMATION)},
                              std::pair{L"createThread", DWORD(PROCESS_CREATE_THREAD)},
                              std::pair{L"vmOperation", DWORD(PROCESS_VM_OPERATION)},
                              std::pair{L"vmWrite", DWORD(PROCESS_VM_WRITE)},
                              std::pair{L"vmRead", DWORD(PROCESS_VM_READ)},
                              std::pair{L"combined", injectionAccess} }) {
        Handle probe(OpenProcess(entry.second, FALSE, procId));
        const DWORD error = probe.value ? ERROR_SUCCESS : GetLastError();
        details += L" | " + std::wstring(entry.first) + L"=" + std::to_wstring(error);
    }
    return details;
}
std::vector<ProcessCandidate> FindProcesses(const wchar_t* procName) {
    auto candidates = listProcesses(procName);
    for (auto& candidate : candidates) probeProcess(candidate);
    return candidates;
}
DWORD SelectProcess(const std::vector<ProcessCandidate>& candidates) {
    // A stuck or leftover process can share the game's name. Prefer a live process
    // we can open, then the newest; ties keep the first listed.
    const ProcessCandidate* best = nullptr;
    auto rank = [](const ProcessCandidate& c) {
        return std::tuple(c.threads != 0, c.accessError == ERROR_SUCCESS, c.created);
    };
    for (const auto& candidate : candidates)
        if (!best || rank(candidate) > rank(*best)) best = &candidate;
    return best ? best->pid : 0;
}
DWORD GetProcId(const wchar_t* procName) {
    // This runs on every poll, so only probe when there is a choice to make.
    auto candidates = listProcesses(procName);
    if (candidates.size() < 2) return candidates.empty() ? 0 : candidates.front().pid;
    for (auto& candidate : candidates) probeProcess(candidate);
    return SelectProcess(candidates);
}
std::wstring DescribeProcessCandidates(const wchar_t* procName) {
    struct RestoreError {
        DWORD error = GetLastError();
        ~RestoreError() { SetLastError(error); }
    } restore;
    const auto candidates = FindProcesses(procName);
    FILETIME now{};
    GetSystemTimeAsFileTime(&now);
    const ULONGLONG ticks = (ULONGLONG(now.dwHighDateTime) << 32) | now.dwLowDateTime;
    std::wstring details = L"processDiag=v1 | candidates=" + std::to_wstring(candidates.size());
    for (const auto& candidate : candidates) {
        details += L" | pid=" + std::to_wstring(candidate.pid) + L",threads=" + std::to_wstring(candidate.threads) +
            L",parent=" + std::to_wstring(candidate.parentPid) + L",access=" + std::to_wstring(candidate.accessError) + L",ageSeconds=";
        details += candidate.created ? std::to_wstring((ticks - candidate.created) / 10000000) : L"unknown";
    }
    return details;
}
DWORD performInjection(DWORD procId, const wchar_t* dllPath, const wchar_t** failureStage) {
    auto stage = [failureStage](const wchar_t* name) { if (failureStage) *failureStage=name; };
    stage(L"Validate DLL path");
    if (!dllPath || !*dllPath) return ERROR_INVALID_PARAMETER;
    DWORD length = GetFullPathNameW(dllPath, 0, nullptr, nullptr);
    if (!length) return GetLastError();
    std::vector<wchar_t> buffer(length);
    if (!GetFullPathNameW(dllPath, length, buffer.data(), nullptr)) return GetLastError();
    std::wstring path(buffer.data());
    stage(L"Read and validate DLL headers");
    DWORD error = validateDll(path);
    if (error) return error;
    stage(L"OpenProcess / Minecraft process access");
    ensureDebugPrivilege();
    Handle process(OpenProcess(injectionAccess, FALSE, procId));
    if (!process.value) return GetLastError();
    USHORT machine = 0, nativeMachine = 0;
    stage(L"IsWow64Process2 / target architecture");
    if (!IsWow64Process2(process.value, &machine, &nativeMachine)) return GetLastError();
    if (machine != IMAGE_FILE_MACHINE_UNKNOWN || nativeMachine != IMAGE_FILE_MACHINE_AMD64)
        return ERROR_BAD_EXE_FORMAT;
    if (isLoaded(process.value, procId, path)) return ERROR_SUCCESS;

    // Resolve forwarded exports in their real owning module and account for ASLR.
    stage(L"GetProcAddress / resolve LoadLibraryW");
    FARPROC loader = GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW");
    if (!loader) return GetLastError();
    HMODULE owner = nullptr;
    stage(L"GetModuleHandleExW / loader owner");
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
        GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(loader), &owner))
        return GetLastError();
    wchar_t ownerPath[MAX_PATH]{};
    stage(L"GetModuleFileNameW / loader module path");
    if (!GetModuleFileNameW(owner, ownerPath, MAX_PATH)) return GetLastError();
    const wchar_t* ownerName = wcsrchr(ownerPath, L'\\');
    ownerName = ownerName ? ownerName + 1 : ownerPath;
    ULONG_PTR remoteLoader = 0;
    stage(L"CreateToolhelp32Snapshot / target modules");
    {
        Handle snapshot(moduleSnapshot(procId));
        if (snapshot.value == INVALID_HANDLE_VALUE) return GetLastError();
        MODULEENTRY32W module{};
        module.dwSize = sizeof(module);
        if (Module32FirstW(snapshot.value, &module)) do {
            if (_wcsicmp(module.szModule, ownerName) == 0) {
                remoteLoader = reinterpret_cast<ULONG_PTR>(module.modBaseAddr) +
                    (reinterpret_cast<ULONG_PTR>(loader) - reinterpret_cast<ULONG_PTR>(owner));
                break;
            }
        } while (Module32NextW(snapshot.value, &module));
    }
    stage(L"Find loader module in target");
    if (!remoteLoader) return ERROR_MOD_NOT_FOUND;
    const SIZE_T bytes = (path.size() + 1) * sizeof(wchar_t);
    stage(L"VirtualAllocEx / allocate DLL path");
    void* remotePath = VirtualAllocEx(process.value, nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remotePath) return GetLastError();
    SIZE_T written = 0;
    stage(L"WriteProcessMemory / write DLL path");
    if (!WriteProcessMemory(process.value, remotePath, path.c_str(), bytes, &written) || written != bytes) {
        error = GetLastError();
        VirtualFreeEx(process.value, remotePath, 0, MEM_RELEASE);
        return error ? error : ERROR_WRITE_FAULT;
    }
    stage(L"CreateRemoteThread / start DLL loader");
    Handle thread(CreateRemoteThread(process.value, nullptr, 0,
        reinterpret_cast<LPTHREAD_START_ROUTINE>(remoteLoader), remotePath, 0, nullptr));
    if (!thread.value) {
        error = GetLastError();
        VirtualFreeEx(process.value, remotePath, 0, MEM_RELEASE);
        return error;
    }
    stage(L"WaitForSingleObject / wait for DLL loader");
    DWORD wait = WaitForSingleObject(thread.value, 10000);
    if (wait != WAIT_OBJECT_0) {
        // A running loader still owns this buffer; preserve it until target exit.
        return wait == WAIT_TIMEOUT ? ERROR_TIMEOUT : GetLastError();
    }
    VirtualFreeEx(process.value, remotePath, 0, MEM_RELEASE);
    // HMODULE is 64-bit but thread exit codes are DWORDs. Verify via enumeration.
    stage(L"Verify DLL appears in target module list");
    return isLoaded(process.value, procId, path) ? ERROR_SUCCESS : ERROR_DLL_INIT_FAILED;
}
