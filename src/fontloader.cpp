#include "fontloader.h"
#include <windows.h>
#include <wx/font.h>
#include <wx/filename.h>
#include <wx/ffile.h>
#include <wx/stdpaths.h>
#include <cstdint>
#include <cstring>
#include "FixFilePerms.h"

bool StageClientFonts(const wxString& dllPath, wxString& error) {
    wxFileName dll(dllPath);
    if (!dll.FileExists() || !dll.MakeAbsolute()) {
        error = "The client DLL must exist before preparing its fonts.";
        return false;
    }
    const wxString directory = dll.GetPath() + "\\fonts";
    if (!wxFileName::DirExists(directory) &&
        !wxFileName::Mkdir(directory, wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL)) {
        error = "The client fonts folder could not be created."; return false;
    }
    std::wstring folder = directory.ToStdWstring();
    DWORD access = SetAccessControl(folder, L"S-1-15-2-1");
    if (access != ERROR_SUCCESS) {
        error = wxString::Format("Client font folder permissions failed / Windows error %lu", access); return false;
    }
    static constexpr const wchar_t* names[] = {L"Comfortaa.ttf", L"Mojangles.ttf", L"Open-Sans.ttf",
        L"Product-Sans.ttf", L"Sarabun-Light.ttf", L"SF-Pro-Display.ttf"};
    for (int index = 0; index < 6; ++index) {
        HRSRC resource = FindResourceW(nullptr, MAKEINTRESOURCEW(203 + index), RT_RCDATA);
        HGLOBAL loaded = resource ? LoadResource(nullptr, resource) : nullptr;
        const void* data = loaded ? LockResource(loaded) : nullptr;
        DWORD size = resource ? SizeofResource(nullptr, resource) : 0;
        if (!data || !size) { error = "An embedded client font is missing."; return false; }
        const wxString path = directory + "\\" + names[index];
        bool matching = false;
        if (wxFileName::FileExists(path)) {
            wxFFile existing(path, "rb");
            if (existing.IsOpened() && existing.Length() == size) {
                std::vector<unsigned char> bytes(size);
                matching = existing.Read(bytes.data(), bytes.size()) == size && std::memcmp(bytes.data(), data, size) == 0;
            }
        }
        if (!matching) {
            wxString temporary = wxFileName::CreateTempFileName(directory + "\\font-");
            wxFFile file(temporary, "wb");
            bool written = file.IsOpened() && file.Write(data, size) == size;
            if (file.IsOpened()) written = file.Close() && written;
            if (!written || !wxRenameFile(temporary, path, true)) {
                if (wxFileName::FileExists(temporary)) wxRemoveFile(temporary);
                error = "A client font could not be saved."; return false;
            }
        }
        std::wstring fontPath = path.ToStdWstring();
        access = SetAccessControl(fontPath, L"S-1-15-2-1");
        if (access != ERROR_SUCCESS) {
            error = wxString::Format("Client font permissions failed / Windows error %lu", access); return false;
        }
    }
    return true;
}

bool EmbeddedFonts::Load(wxString& error) {
    const wxString directory = wxStandardPaths::Get().GetUserLocalDataDir() + "\\fonts";
    if (!wxFileName::DirExists(directory) &&
        !wxFileName::Mkdir(directory, wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL)) {
        error = "The font cache folder could not be created.";
        return false;
    }
    for (int id = 203; id <= 208; ++id) {
        HRSRC resource = FindResourceW(nullptr, MAKEINTRESOURCEW(id), RT_RCDATA);
        HGLOBAL loaded = resource ? LoadResource(nullptr, resource) : nullptr;
        const auto* data = loaded ? static_cast<const unsigned char*>(LockResource(loaded)) : nullptr;
        DWORD size = resource ? SizeofResource(nullptr, resource) : 0;
        if (!data || !size) { error = "An embedded font resource is missing."; Unload(); return false; }

        // Content-addressed paths avoid overwriting font files held by another instance.
        std::uint64_t hash = 14695981039346656037ull;
        for (DWORD i = 0; i < size; ++i) { hash ^= data[i]; hash *= 1099511628211ull; }
        wxString path = directory + wxString::Format("\\%d-%016llx.ttf", id, static_cast<unsigned long long>(hash));
        if (!wxFileName::FileExists(path)) {
            wxString temporary = wxFileName::CreateTempFileName(directory + "\\font-");
            wxFFile file(temporary, "wb");
            bool written = file.IsOpened() && file.Write(data, size) == size;
            if (file.IsOpened()) written = file.Close() && written;
            if (!written || !wxRenameFile(temporary, path, false)) {
                if (wxFileName::FileExists(temporary)) wxRemoveFile(temporary);
                // A concurrent instance may already have completed the same file.
                if (!wxFileName::FileExists(path)) { error = "An embedded font could not be cached."; Unload(); return false; }
            }
        }
        if (!wxFont::AddPrivateFont(path)) {
            error = "An embedded font could not be registered.";
            Unload(); return false;
        }
        files.push_back(path);
    }
    return true;
}

void EmbeddedFonts::Unload() {
    for (const auto& path : files) RemoveFontResourceExW(path.wc_str(), FR_PRIVATE, nullptr);
    files.clear();
}
