#pragma once
#include "config.h"
#include "taskBarIcon.h"
#include <wx/timer.h>
#include "auto_inject.h"
#include <thread>

struct ClientDownloadResult;

class cMain : public wxFrame {
public:
    wxPanel* mainPanel = nullptr;
    wxWindow* btn_Inject = nullptr;
    wxWindow* btn_Select = nullptr;
    wxWindow* btn_AutoInject = nullptr;
    AutoInjectState autoInject;
    bool injectionBusy = false;
    bool downloading = false;
    bool downloadingBeta = false;
    bool loadedBeta = false;
    DWORD gamePid = 0;
    DWORD loadedPid = 0;
    std::jthread downloadThread;
    std::jthread reportingThread;
    std::jthread updateCheckThread;
    std::jthread clientCheckThread;
    bool clientCheckBusy = false;
    bool clientUpdateAvailable = false;
    bool injectorUpdateAvailable = false;
    wxTimer clientUpdateTimer;
    std::wstring automaticDllPath;
    std::wstring automaticDllDigest;
    wxTextCtrl* txt_ProcName = nullptr;
    wxTextCtrl* txt_DllPath = nullptr;
    wxCheckBox* check_CustomTarget = nullptr;
    wxFileDialog* openDialog = nullptr;
    wxStaticText* processStatus = nullptr;
    wxStaticText* selectionStatus = nullptr;
    wxStaticText* statusText = nullptr;
    wxTimer processTimer;
    wxTimer glowTimer;
    Config cfg;
    cMain();
    ~cMain() override;
    WXLRESULT MSWWindowProc(WXUINT message, WXWPARAM wParam, WXLPARAM lParam) override;
    static void OnInjectButtonExecute(wxCommandEvent& evt, cMain& ref, bool interactive = true);
    void OnInjectButton(wxCommandEvent& evt);
    void OnSelectButton(wxCommandEvent& evt);
    void OnCustomCheckBox(wxCommandEvent& evt);
    void updateGlobalVars();
    void RefreshProcess();
    void RefreshStatus();
    void SetStatusText(const wxString& text, int number = 0);
    void OpenThemeMenu();
    void ApplyTheme(wxWindow* extraWindow = nullptr);
    bool ConfigureDownload();
    void SetAutoInject(bool enabled);
    void CheckClientUpdate();
    void RefreshUpdatePing();
    void ToggleAutoInject();
    void InjectBeta();
    void BeginClientDownload(const std::wstring& endpoint,std::wstring key,bool beta);
    void FinishDownload(const ClientDownloadResult& result);
    wxDECLARE_EVENT_TABLE();
};
