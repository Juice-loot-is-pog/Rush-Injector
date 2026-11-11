#include "pch.h"
#include "config.h"
#include <wx/stdpaths.h>
#include <wx/filename.h>
#include <locale>
#include <codecvt>

Config::Config() {
    wxString directory = wxStandardPaths::Get().GetUserLocalDataDir();
    wxFileName::Mkdir(directory, wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL);
    this->path = (directory + "\\config.txt").ToStdWstring();
}

bool Config::serializeConfig() {
    std::wifstream cFile(this->path);
    cFile.imbue(std::locale(std::locale::classic(), new std::codecvt_utf8<wchar_t>));

    if (cFile.is_open()) {
        std::wstring currLine;
        while (std::getline(cFile, currLine)) {

            if (currLine.empty() || currLine[0] == '#') {
                continue;
            }

            size_t delimiterPos = currLine.find('=');
            if (delimiterPos == std::wstring::npos) continue;
            this->currentKey = currLine.substr(0, delimiterPos);

            this->currentVal = currLine.substr(delimiterPos + 1);
            // std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return std::tolower(c); });
            this->analyzeState();
        }

        cFile.close();
        return true;
    }

    // can't load config file, so we generate a new one via Config::updateConfigFile with default globals
    return false;
}

bool Config::analyzeBool() {
    std::transform(this->currentVal.begin(), this->currentVal.end(), this->currentVal.begin(), [](wchar_t c) {
        return std::tolower(static_cast<int>(c));
    });
    return (this->currentVal == L"true") || (this->currentVal == L"1");
}

int Config::analyzeInt() {
    if (std::all_of(this->currentVal.begin(), this->currentVal.end(), ::isdigit)) {
        return std::stoi(this->currentVal);
    }
    return 0;
}

bool Config::updateConfigFile() const {
    std::wofstream cfgStream(this->path, std::ios::trunc);
    cfgStream.imbue(std::locale(std::locale::classic(), new std::codecvt_utf8<wchar_t>));
    if (cfgStream.is_open()) {
        cfgStream << L"# Rush Injector config\n";
        cfgStream << L"clientDigest=" + Globals::CLIENT_DIGEST + L'\n';
        cfgStream << L"clientDigestEndpoint=" + Globals::CLIENT_DIGEST_ENDPOINT + L'\n';
        cfgStream << (Globals::USE_CUSTOM_PROC_NAME ? L"useCustomProcName=true\n" : L"useCustomProcName=false\n");
        cfgStream << L"dllPath=" + Globals::DLL_PATH + '\n';
        cfgStream << L"procName=" + Globals::PROC_NAME + '\n';
        cfgStream << L"fontFamily=" + Globals::FONT_FAMILY + L'\n';
        cfgStream << L"colorPreset=" + Globals::COLOR_PRESET + L'\n';
        cfgStream << (Globals::DISCORD_PRESENCE ? L"discordPresence=true\n" : L"discordPresence=false\n");
        cfgStream << L"downloadUrl=" + Globals::DOWNLOAD_URL + L'\n';

        cfgStream.close();
        return true;
    }
    return false;
}

void Config::analyzeState() {
    if (currentKey == L"clientDigest") { Globals::CLIENT_DIGEST=currentVal; return; }
    if (currentKey == L"clientDigestEndpoint") { Globals::CLIENT_DIGEST_ENDPOINT=currentVal; return; }
    if (this->currentKey == L"useCustomProcName") {
        Globals::USE_CUSTOM_PROC_NAME = this->analyzeBool();
    }
    else if (this->currentKey == L"dllPath") {
        Globals::DLL_PATH = this->currentVal;
    }
    else if (this->currentKey == L"procName") {
        Globals::PROC_NAME = this->currentVal;
    }
    else if (this->currentKey == L"fontFamily") {
        if (std::any_of(Globals::FONT_FAMILIES.begin(), Globals::FONT_FAMILIES.end(),
            [this](const wchar_t* family) { return currentVal == family; }))
            Globals::FONT_FAMILY = currentVal;
    }
    else if (this->currentKey == L"colorPreset") {
        if (currentVal == L"Red" || currentVal == L"Blue" || currentVal == L"Purple" || currentVal == L"Mono")
            Globals::COLOR_PRESET = currentVal;
    }
    else if (this->currentKey == L"discordPresence") {
        Globals::DISCORD_PRESENCE = this->analyzeBool();
    }
    else if (this->currentKey == L"downloadUrl") {
        if (!currentVal.empty()) Globals::DOWNLOAD_URL = currentVal;
    }
    else {
        wxLogWarning("Unknown configuration entry: %s", wxString(this->currentKey));
    }
}
