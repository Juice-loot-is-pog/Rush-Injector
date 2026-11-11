#include "pch.h"
#include "cApp.h"

wxIMPLEMENT_APP(cApp);

bool cApp::OnInit() {

    SetAppName("Rush Injector");
    wxString error;
    if (!fonts.Load(error)) {
        wxMessageBox(error, "Rush Injector", wxOK | wxICON_ERROR);
        return false;
    }

    wxInitAllImageHandlers();

    CMAIN_INSTANCE = new cMain();
    CMAIN_INSTANCE->Show();
    if (Globals::DISCORD_PRESENCE) presence.Start();
    return true;
}

int cApp::OnExit() {
    presence.Stop();
    fonts.Unload();
    return wxApp::OnExit();
}
