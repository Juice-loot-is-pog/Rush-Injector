#pragma once
#include <cstdint>
#include <string>

// A short startup delay, then one attempt for each process/DLL pair.
class AutoInjectState {
    bool enabled = false, attempted = false;
    std::uint32_t observedPid = 0;
    std::wstring observedDll;
    unsigned stablePolls = 0;
    void Reset() { observedPid = 0; observedDll.clear(); stablePolls = 0; attempted = false; }
public:
    bool Enabled() const { return enabled; }
    void SetEnabled(bool value) { enabled = value; Reset(); }
    bool Observe(std::uint32_t pid, const std::wstring& dll) {
        if (!enabled || !pid || dll.empty()) { Reset(); return false; }
        if (pid != observedPid || dll != observedDll) {
            observedPid = pid; observedDll = dll; stablePolls = 0; attempted = false;
        }
        if (attempted) return false;
        if (++stablePolls < 3) return false;
        attempted = true; return true;
    }
    void RecordAttempt(std::uint32_t pid, const std::wstring& dll) {
        observedPid = pid; observedDll = dll; stablePolls = 3; attempted = true;
    }
};
