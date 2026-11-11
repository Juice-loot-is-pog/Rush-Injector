#include "auto_inject.h"
#include <iostream>

int main() {
    AutoInjectState state;
    int failures = 0;
    auto check = [&](bool ok, const char* name) { std::cout << (ok ? "PASS " : "FAIL ") << name << '\n'; if (!ok) ++failures; };
    check(!state.Observe(10,L"client.dll"), "disabled state skips injection");
    state.SetEnabled(true);
    check(!state.Observe(0,L"client.dll"), "wait for game");
    check(!state.Observe(10,L""), "wait for DLL");
    check(!state.Observe(10,L"client.dll") && !state.Observe(10,L"client.dll") && state.Observe(10,L"client.dll"), "startup delay then one attempt");
    check(!state.Observe(10,L"client.dll") && !state.Observe(10,L"client.dll"), "success or failure never causes repeated attempts");
    state.Observe(0,L"client.dll");
    check(!state.Observe(11,L"client.dll") && !state.Observe(11,L"client.dll") && state.Observe(11,L"client.dll"), "new process receives an attempt");
    check(!state.Observe(11,L"other.dll") && !state.Observe(11,L"other.dll") && state.Observe(11,L"other.dll"), "changed DLL receives an attempt");
    state.SetEnabled(false); state.SetEnabled(true);
    check(!state.Observe(11,L"other.dll") && !state.Observe(11,L"other.dll") && state.Observe(11,L"other.dll"), "toggle permits retry");
    state.SetEnabled(false); state.SetEnabled(true); state.RecordAttempt(11,L"other.dll");
    check(!state.Observe(11,L"other.dll"), "manual attempt prevents automatic duplicate");
    return failures ? 1 : 0;
}
