#pragma once
#include <windows.h>
#include <string>
#include <vector>

struct ProcessCandidate {
    DWORD pid = 0, parentPid = 0, threads = 0;
    DWORD accessError = ERROR_SUCCESS; // Win32 error from opening with injection access.
    ULONGLONG created = 0;             // FILETIME ticks; 0 when unreadable.
};

// ERROR_SUCCESS means the DLL was observed in the target module list.
DWORD performInjection(DWORD procId, const wchar_t* dllPath, const wchar_t** failureStage = nullptr);
DWORD GetProcId(const wchar_t* procName);
std::vector<ProcessCandidate> FindProcesses(const wchar_t* procName);
DWORD SelectProcess(const std::vector<ProcessCandidate>& candidates);
// Read-only diagnostics; numeric failures are Win32 error codes. Preserves last error.
std::wstring DescribeProcessAccess(DWORD procId);
std::wstring DescribeProcessCandidates(const wchar_t* procName);
