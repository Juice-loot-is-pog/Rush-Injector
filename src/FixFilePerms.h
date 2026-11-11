#pragma once

#include <string>
#include <windows.h>

DWORD SetAccessControl(std::wstring& ExecutableName, const wchar_t* AccessString, const wchar_t** failureStage = nullptr);
