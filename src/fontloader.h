#pragma once
#include <wx/string.h>
#include <vector>

// The client resolves <DLL directory>/fonts/<original filename>.
bool StageClientFonts(const wxString& dllPath, wxString& error);

// Register before creating any graphics context: wxWidgets snapshots this list.
class EmbeddedFonts {
    std::vector<wxString> files;
public:
    bool Load(wxString& error);
    void Unload();
    ~EmbeddedFonts() { Unload(); }
};
