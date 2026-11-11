#include "inject.h"
#include <iostream>
#include <string>
#include <filesystem>
#include <fstream>
#include <TlHelp32.h>

int wmain(int argc, wchar_t** argv) {
    if (argc == 2 && std::wstring(argv[1]) == L"--host") {
        std::wstring name = L"Local\\RushReady-" + std::to_wstring(GetCurrentProcessId());
        HANDLE ready = CreateEventW(nullptr,TRUE,TRUE,name.c_str());
        Sleep(25000); CloseHandle(ready); return 0;
    }
    if (argc != 3) return 2;
    int failures = 0;
    auto check = [&](bool ok, const char* name) {
        std::cout << (ok ? "PASS " : "FAIL ") << name << std::endl;
        if (!ok) ++failures;
    };
    auto candidate = [](DWORD pid, DWORD threads, DWORD accessError, ULONGLONG created) {
        ProcessCandidate c; c.pid = pid; c.threads = threads; c.accessError = accessError; c.created = created; return c;
    };
    check(SelectProcess({}) == 0,"no candidates selects nothing");
    check(SelectProcess({candidate(1,5,ERROR_ACCESS_DENIED,0),candidate(2,5,ERROR_SUCCESS,10)}) == 2,"accessible process beats a denied leftover");
    check(SelectProcess({candidate(1,5,ERROR_SUCCESS,10),candidate(2,5,ERROR_SUCCESS,20)}) == 2,"newest accessible process selected");
    check(SelectProcess({candidate(1,0,ERROR_SUCCESS,30),candidate(2,5,ERROR_ACCESS_DENIED,0)}) == 2,"process with threads beats an empty one");
    check(SelectProcess({candidate(1,5,ERROR_ACCESS_DENIED,0),candidate(2,5,ERROR_ACCESS_DENIED,0)}) == 1,"tie keeps first listed");
    check(performInjection(0,L"") == ERROR_INVALID_PARAMETER,"empty DLL path");
    check(performInjection(0,L"missing-rush-client.dll") == ERROR_FILE_NOT_FOUND,"missing DLL");
    std::wstring command = L"\"" + std::wstring(argv[0]) + L"\" --host";
    STARTUPINFOW startup{}; startup.cb = sizeof(startup);
    PROCESS_INFORMATION child{};
    if (!CreateProcessW(nullptr,command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,nullptr,&startup,&child)) return 3;
    std::wstring readyName = L"Local\\RushReady-" + std::to_wstring(child.dwProcessId);
    HANDLE ready = nullptr;
    for (int i = 0; i < 100 && !ready; ++i) { ready = OpenEventW(SYNCHRONIZE,FALSE,readyName.c_str()); if (!ready) Sleep(50); }
    check(ready != nullptr,"test host ready");
    if (ready) CloseHandle(ready);
    SetLastError(ERROR_CANCELLED);
    const auto access = DescribeProcessAccess(child.dwProcessId);
    check(GetLastError() == ERROR_CANCELLED,"access diagnostics preserve caller error");
    check(access.find(L"accessDiag=v1") != std::wstring::npos &&
        access.find(L"self{elevated=") != std::wstring::npos &&
        access.find(L"target{elevated=") != std::wstring::npos &&
        access.find(L" | combined=0") != std::wstring::npos,"access diagnostics inspect accessible host");
    const auto inaccessible = DescribeProcessAccess(0);
    check(inaccessible.find(L"target{queryError=") != std::wstring::npos &&
        inaccessible.find(L" | combined=87") != std::wstring::npos,"access diagnostics retain failed query and access errors");
    const std::wstring selfName = std::filesystem::path(argv[0]).filename().wstring();
    check(GetProcId(selfName.c_str()) == child.dwProcessId,"newest of two same-named processes selected");
    check(GetProcId(L"missing-rush-process.exe") == 0,"missing process not found");
    SetLastError(ERROR_CANCELLED);
    const auto processes = DescribeProcessCandidates(selfName.c_str());
    check(GetLastError() == ERROR_CANCELLED,"process diagnostics preserve caller error");
    check(processes.find(L"processDiag=v1 | candidates=2") != std::wstring::npos &&
        processes.find(L"pid=" + std::to_wstring(child.dwProcessId) + L",threads=") != std::wstring::npos &&
        processes.find(L",access=0,ageSeconds=") != std::wstring::npos,"process diagnostics list every candidate");
    std::wstring eventName = L"Local\\RushLoaded-" + std::to_wstring(child.dwProcessId);
    HANDLE loaded = CreateEventW(nullptr,TRUE,FALSE,eventName.c_str());
    // A Unicode path beyond the original MAX_PATH-byte allocation exercises the bug.
    auto folder = std::filesystem::path(argv[1]).parent_path() / L"smoke Unicode \u03a9 folder with spaces and a deliberately extended directory name";
    std::filesystem::create_directories(folder);
    auto dll = folder / L"client \u03a9.dll";
    std::filesystem::copy_file(argv[1],dll,std::filesystem::copy_options::overwrite_existing);
    DWORD result = performInjection(child.dwProcessId,dll.c_str());
    if (result) {
        std::cout << "Windows error " << result << std::endl;
        auto printPath = [](const std::wstring& p) {
            int bytes = WideCharToMultiByte(CP_UTF8,0,p.c_str(),-1,nullptr,0,nullptr,nullptr);
            std::string text(bytes, '\0');
            WideCharToMultiByte(CP_UTF8,0,p.c_str(),-1,text.data(),bytes,nullptr,nullptr);
            std::cout << text.c_str() << std::endl;
        };
        std::cout << "Requested: "; printPath(dll.wstring());
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE,child.dwProcessId);
        MODULEENTRY32W entry{}; entry.dwSize = sizeof(entry);
        if (Module32FirstW(snap,&entry)) do { std::cout << "Module: "; printPath(entry.szExePath); } while (Module32NextW(snap,&entry));
        CloseHandle(snap);
    }
    check(result == ERROR_SUCCESS,"load DLL using long Unicode path");
    check(WaitForSingleObject(loaded,1000) == WAIT_OBJECT_0,"DLL_PROCESS_ATTACH executed in host");
    check(performInjection(child.dwProcessId,dll.c_str()) == ERROR_SUCCESS,"already loaded DLL");
    check(performInjection(child.dwProcessId,argv[2]) == ERROR_DLL_INIT_FAILED,"DLL initialization failure reported");
    auto invalid = folder / L"invalid.dll";
    { std::ofstream file(invalid); file << "not a PE image"; }
    check(performInjection(child.dwProcessId,invalid.c_str()) == ERROR_BAD_EXE_FORMAT,"invalid PE rejected");
    auto wrongArch = folder / L"wrong-arch.dll";
    std::filesystem::copy_file(dll,wrongArch,std::filesystem::copy_options::overwrite_existing);
    { std::fstream file(wrongArch,std::ios::binary | std::ios::in | std::ios::out);
      IMAGE_DOS_HEADER dos{}; file.read(reinterpret_cast<char*>(&dos),sizeof(dos));
      file.seekp(dos.e_lfanew + sizeof(DWORD)); WORD machine = IMAGE_FILE_MACHINE_I386;
      file.write(reinterpret_cast<char*>(&machine),sizeof(machine)); }
    check(performInjection(child.dwProcessId,wrongArch.c_str()) == ERROR_BAD_EXE_FORMAT,"x86 DLL rejected");
    const wchar_t* failureStage=nullptr;
    check(performInjection(0,dll.c_str(),&failureStage) != ERROR_SUCCESS,"invalid process rejected");
    check(failureStage && std::wstring(failureStage)==L"OpenProcess / Minecraft process access","process access failure identifies exact stage");
    failureStage=nullptr;
    check(performInjection(child.dwProcessId,invalid.c_str(),&failureStage)==ERROR_BAD_EXE_FORMAT &&
        failureStage && std::wstring(failureStage)==L"Read and validate DLL headers","invalid DLL identifies validation stage");
    // A process suspended at creation has no readable loader data, like a game stuck starting.
    PROCESS_INFORMATION suspended{};
    if (CreateProcessW(nullptr,command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW | CREATE_SUSPENDED,nullptr,nullptr,&startup,&suspended)) {
        failureStage=nullptr;
        const ULONGLONG started = GetTickCount64();
        check(performInjection(suspended.dwProcessId,dll.c_str(),&failureStage)==ERROR_PARTIAL_COPY &&
            failureStage && std::wstring(failureStage)==L"CreateToolhelp32Snapshot / target modules","unreadable loader data identifies snapshot stage");
        check(GetTickCount64()-started >= 1000,"unreadable loader data is retried");
        TerminateProcess(suspended.hProcess,0); WaitForSingleObject(suspended.hProcess,5000);
        CloseHandle(suspended.hThread); CloseHandle(suspended.hProcess);
    } else check(false,"suspended host created");
    TerminateProcess(child.hProcess,0); WaitForSingleObject(child.hProcess,5000);
    CloseHandle(child.hThread); CloseHandle(child.hProcess); CloseHandle(loaded);
    std::filesystem::remove_all(folder);
    return failures ? 1 : 0;
}
