#pragma once

#include <string>
#include <array>
#ifndef RUSH_DOWNLOAD_URL
#define RUSH_DOWNLOAD_URL L""
#endif

namespace Globals {
    inline std::wstring CLIENT_DIGEST;
    inline std::wstring CLIENT_DIGEST_ENDPOINT;

    inline char WORKING_DIR[1024]{};
    inline bool USE_CUSTOM_PROC_NAME = false;
    inline const std::wstring NO_DLL_PATH_SELECTED_MSG{ L"Click \"Select\" to select a DLL file" };
    inline std::wstring DLL_PATH{};
    inline const std::wstring MCBE_PROC_NAME{ L"Minecraft.Windows.exe" };
    inline std::wstring PROC_NAME{ Globals::MCBE_PROC_NAME };
    inline constexpr std::array<const wchar_t*, 6> FONT_NAMES = {
        L"Comfortaa", L"Mojangles", L"Open Sans", L"Product Sans", L"Sarabun Light", L"SF Pro Display"
    };
    inline constexpr std::array<const wchar_t*, 6> FONT_FAMILIES = {
        L"Comfortaa", L"Minecraft", L"Open Sans", L"Product Sans", L"Sarabun Light", L"SF Pro Display"
    };
    inline std::wstring FONT_FAMILY = L"Comfortaa";
    inline std::wstring COLOR_PRESET = L"Red";
    inline bool DISCORD_PRESENCE = true;
    inline std::wstring DOWNLOAD_URL = RUSH_DOWNLOAD_URL;

} //namespace Globals

class Config {
    std::wstring path;
    //Config name and state
    std::wstring currentKey;
    std::wstring currentVal;

    int analyzeInt();
    bool analyzeBool();
    void analyzeState();
public:
    Config();

    bool serializeConfig();
    bool updateConfigFile() const;
};
