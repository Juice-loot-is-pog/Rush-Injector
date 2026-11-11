#include <windows.h>
#include <string>
BOOL WINAPI DllMain(HINSTANCE, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
#ifdef REJECT_ATTACH
        return FALSE;
#endif
        std::wstring name = L"Local\\RushLoaded-" + std::to_wstring(GetCurrentProcessId());
        HANDLE event = OpenEventW(EVENT_MODIFY_STATE, FALSE, name.c_str());
        if (event) { SetEvent(event); CloseHandle(event); }
    }
    return TRUE;
}
