#include "pch.h"
#include "dev_panel.h"
#include "client_download.h"
#include <winhttp.h>
#include <wincred.h>
#include <objbase.h>
#include <thread>
#include <regex>
#include <memory>
#include <wx/popupwin.h>
#include <wx/dcbuffer.h>
#include <uxtheme.h>
#include <dwmapi.h>
#pragma comment(lib,"uxtheme.lib")

namespace {
class PopOutIcon : public wxControl {
public:
    explicit PopOutIcon(wxWindow* parent):wxControl(parent,wxID_ANY,wxDefaultPosition,parent->FromDIP(wxSize(32,32)),wxBORDER_NONE|wxWANTS_CHARS) {
        SetMinSize(parent->FromDIP(wxSize(32,32)));SetBackgroundStyle(wxBG_STYLE_PAINT);SetCursor(wxCURSOR_HAND);SetToolTip("Pop out results");SetName("Pop out results");
        Bind(wxEVT_PAINT,[this](wxPaintEvent&){
            wxAutoBufferedPaintDC dc(this);dc.SetBackground(wxBrush(GetParent()->GetBackgroundColour()));dc.Clear();
            dc.SetPen(wxPen(wxColour(170,173,186),FromDIP(2)));dc.SetBrush(*wxTRANSPARENT_BRUSH);
            const auto p=[this](int x,int y){return FromDIP(wxPoint(x,y));};
            dc.DrawLine(p(7,11),p(7,25));dc.DrawLine(p(7,25),p(23,25));dc.DrawLine(p(23,25),p(23,19));dc.DrawLine(p(7,11),p(14,11));
            dc.DrawLine(p(15,17),p(25,7));dc.DrawLine(p(19,7),p(25,7));dc.DrawLine(p(25,7),p(25,13));
        });
        Bind(wxEVT_LEFT_DOWN,[this](wxMouseEvent&){SetFocus();CaptureMouse();});
        Bind(wxEVT_LEFT_UP,[this](wxMouseEvent& event){if(!HasCapture())return;ReleaseMouse();if(GetClientRect().Contains(event.GetPosition()))Click();});
        Bind(wxEVT_MOUSE_CAPTURE_LOST,[](wxMouseCaptureLostEvent&){});
        Bind(wxEVT_KEY_DOWN,[this](wxKeyEvent& event){if(event.GetKeyCode()==WXK_SPACE||event.GetKeyCode()==WXK_RETURN)Click();else event.Skip();});
    }
    void Click(){wxCommandEvent event(wxEVT_BUTTON,GetId());event.SetEventObject(this);ProcessWindowEvent(event);}
};
class DarkDurationPicker : public wxControl {
    wxArrayString choices;
    int selection=0;
    wxPopupTransientWindow* popup=nullptr;
    void Select(int index) {
        selection=index;Refresh(false);
        wxCommandEvent event(wxEVT_COMBOBOX,GetId());event.SetEventObject(this);ProcessWindowEvent(event);
    }
    void Open() {
        if(!popup) {
            popup=new wxPopupTransientWindow(this,wxBORDER_NONE);
            popup->SetBackgroundColour(wxColour(19,20,27));popup->SetFont(GetFont());
            auto* layout=new wxBoxSizer(wxVERTICAL);
            for(unsigned i=0;i<choices.size();++i) {
                auto* option=CreateRushActionButton(popup,choices[i],wxSize(180,38));
                layout->Add(option,0,wxEXPAND|wxALL,FromDIP(3));
                option->Bind(wxEVT_BUTTON,[this,i](wxCommandEvent&){Select(i);popup->Dismiss();});
            }
            popup->SetSizerAndFit(layout);
        }
        popup->Position(ClientToScreen(wxPoint(0,0)),GetSize());popup->Popup(this);
    }
public:
    DarkDurationPicker(wxWindow* parent):wxControl(parent,wxID_ANY,wxDefaultPosition,parent->FromDIP(wxSize(170,44)),wxBORDER_NONE|wxWANTS_CHARS) {
        SetMinSize(parent->FromDIP(wxSize(170,44)));SetBackgroundStyle(wxBG_STYLE_PAINT);SetCursor(wxCURSOR_HAND);
        Bind(wxEVT_PAINT,[this](wxPaintEvent&){wxAutoBufferedPaintDC dc(this);dc.SetBackground(wxBrush(wxColour(19,20,27)));dc.Clear();dc.SetFont(GetFont());dc.SetTextForeground(wxColour(235,235,242));
            if(!choices.empty()) {const wxString text=choices[selection];const auto extent=dc.GetTextExtent(text);dc.DrawText(text,FromDIP(12),(GetClientSize().y-extent.y)/2);}
            dc.SetPen(wxPen(wxColour(136,140,157),FromDIP(1)));const int x=GetClientSize().x-FromDIP(15),y=GetClientSize().y/2;dc.DrawLine(x-4,y-2,x,y+2);dc.DrawLine(x,y+2,x+4,y-2);
        });
        Bind(wxEVT_LEFT_UP,[this](wxMouseEvent&){SetFocus();Open();});
        Bind(wxEVT_KEY_DOWN,[this](wxKeyEvent& e){if(e.GetKeyCode()==WXK_SPACE||e.GetKeyCode()==WXK_RETURN)Open();else if(e.GetKeyCode()==WXK_DOWN)Select((selection+1)%choices.size());else if(e.GetKeyCode()==WXK_UP)Select((selection+choices.size()-1)%choices.size());else e.Skip();});
    }
    void Append(const wxString& text){choices.Add(text);}
    int GetSelection() const{return selection;}
    void SetSelection(int index){selection=index;Refresh(false);}
};
wxDEFINE_EVENT(DEV_RESULT,wxThreadEvent);
struct Result { DWORD status=0; wxString text; bool login=false; DWORD error=0; };
struct Http { HINTERNET h; ~Http(){if(h) WinHttpCloseHandle(h);} };
Result Call(const std::wstring& user,const std::wstring& key,const std::wstring& path,const std::string& body,bool login,const std::wstring& eventId={}) {
    Result result; result.login=login;
    Http session{WinHttpOpen(L"Rush-Developer",WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,nullptr,nullptr,0)};
    if(!session.h) {result.error=GetLastError();return result;}
    WinHttpSetTimeouts(session.h,5000,5000,10000,10000);
    Http connection{WinHttpConnect(session.h,L"rush-download-service.juiceloot-github.workers.dev",443,0)};
    if(!connection.h) {result.error=GetLastError();return result;}
    Http request{WinHttpOpenRequest(connection.h,body.empty()?L"GET":L"POST",path.c_str(),nullptr,nullptr,WINHTTP_DEFAULT_ACCEPT_TYPES,WINHTTP_FLAG_SECURE)};
    if(!request.h) {result.error=GetLastError();return result;}
    DWORD policy=WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
    WinHttpSetOption(request.h,WINHTTP_OPTION_REDIRECT_POLICY,&policy,sizeof(policy));
    std::wstring headers=L"Authorization: Bearer "+key+L"\r\nX-Admin-Username: "+user+L"\r\nContent-Type: application/json\r\n";
    headers+=L"X-Device-Hash: "+DeviceIdentityHash()+L"\r\n";
    if(path==L"/beta/login") headers+=L"X-Beta-Username: "+user+L"\r\n";
    if(!eventId.empty()) headers+=L"Idempotency-Key: "+eventId+L"\r\n";
    const bool sent=WinHttpSendRequest(request.h,headers.c_str(),static_cast<DWORD>(headers.size()),body.empty()?nullptr:const_cast<char*>(body.data()),static_cast<DWORD>(body.size()),static_cast<DWORD>(body.size()),0);
    SecureZeroMemory(headers.data(),headers.size()*sizeof(wchar_t));
    if(!sent || !WinHttpReceiveResponse(request.h,nullptr)) {result.error=GetLastError();return result;}
    DWORD size=sizeof(result.status);
    WinHttpQueryHeaders(request.h,WINHTTP_QUERY_STATUS_CODE|WINHTTP_QUERY_FLAG_NUMBER,nullptr,&result.status,&size,nullptr);
    std::string response; char buffer[4096]; DWORD count;
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(20);
    while(std::chrono::steady_clock::now()<deadline) {
        if(!WinHttpReadData(request.h,buffer,sizeof(buffer),&count)) {result.error=GetLastError();result.status=0;break;}
        if(!count) { result.text=wxString::FromUTF8(response);return result; }
        if(response.size()+count>1024*1024) break;
        response.append(buffer,count);
    }
    result.status=0; return result;
}
bool ValidName(const wxString& name) { return std::regex_match(name.ToStdString(),std::regex("[A-Za-z0-9_.-]{1,48}")); }
wxString Pretty(const wxString& text) {
    const std::string data=text.ToStdString();
    auto field=[](const std::string& object,const std::string& name) -> wxString {
        std::smatch match;
        if(std::regex_search(object,match,std::regex("\""+name+"\"\\s*:\\s*(?:\"([^\"]*)\"|([0-9]+|true|false|null))")))
            return wxString::FromUTF8(match[1].matched?match[1].str():match[2].str());
        return "";
    };
    auto expiry=[&](const std::string& object) {
        wxLongLong_t millis=0;field(object,"expires_at").ToLongLong(&millis);
        return wxDateTime(static_cast<time_t>(millis/1000)).FormatISOCombined(' ');
    };
    if(!field(data,"key").empty()) return "New beta key for "+field(data,"username")+"\n\n"+field(data,"key")+"\n\nExpires: "+expiry(data)+"\nKey ID: "+field(data,"id")+"\n\nSave the key now. It will not be shown again.";
    if(data.find("\"keys\"")!=std::string::npos) {
        wxString out="Beta accounts and keys\nTimes are shown in your local time zone.\n\n";
        const std::regex objects("\\{[^{}]*\"id\"[^{}]*\\}");
        int count=0;
        for(auto it=std::sregex_iterator(data.begin(),data.end(),objects);it!=std::sregex_iterator();++it) {
            const std::string object=it->str();++count;
            long long remaining=0;field(object,"time_left_seconds").ToLongLong(&remaining);
            out+=field(object,"username")+"  /  "+(field(object,"active")=="true"?"Active":"Inactive")+"\nAccount: "+field(object,"account_status")+"\nKey ID: "+field(object,"id")+"\nReported injections: "+field(object,"injection_count")+"\nExpires: "+expiry(object)+wxString::Format("\nTime left: %lld days, %lld hours, %lld minutes\n\n",remaining/86400,(remaining%86400)/3600,(remaining%3600)/60);
        }
        return count?out:wxString("No beta keys have been created.");
    }
    if(!field(data,"all_time").empty()) return "Inject Rush statistics\n\nPast 24 hours: "+field(data,"last_24h")+"\nAll time: "+field(data,"all_time")+"\nBeta in past 24 hours: "+field(data,"beta_last_24h")+"\nBeta all time: "+field(data,"beta_all_time")+"\n\nCounts start with this injector version. Manual injections and older versions are not counted. Reports confirm DLL loading, not menu startup.";
    if(field(data,"ok")=="true") return "Changes saved. Use View keys to refresh account and key status.";
    return "The server returned an unexpected response.";
}
class DeveloperPanel : public wxPanel {
    wxTextCtrl *username,*secret,*account,*identifier,*seconds,*output;
    DarkDurationPicker* duration;
    wxPanel* outputCard;
    wxPanel* controls;
    wxWindow* login;
    wxStaticText* status;
    wxStaticText* title;
    wxWindow* popout;
    std::wstring sessionUser,sessionKey;
    std::jthread worker;
    bool busy=false;
    void ClearSession() { if(!sessionKey.empty()) SecureZeroMemory(sessionKey.data(),sessionKey.size()*sizeof(wchar_t));sessionKey.clear();sessionUser.clear();for(wxWindow* child:GetChildren()) if(child->IsTopLevel()) child->Destroy();controls->Hide();output->Clear();outputCard->Hide();popout->Hide();title->SetLabel("Developer login");Layout(); }
    void Request(const std::wstring& path,const std::string& body={},bool signingIn=false) {
        if(busy) return;
        busy=true;login->Disable();controls->Disable();status->SetLabel("Contacting server...");
        worker=std::jthread([this,path,body,signingIn,user=sessionUser,key=sessionKey]() mutable {
            Result result;
            try {result=Call(user,key,path,body,signingIn);} catch(...) {result.login=signingIn;}
            SecureZeroMemory(key.data(),key.size()*sizeof(wchar_t));
            auto* event=new wxThreadEvent(DEV_RESULT);event->SetPayload(result);wxQueueEvent(this,event);
        });
    }
public:
    explicit DeveloperPanel(wxWindow* parent):wxPanel(parent) {
        SetBackgroundColour(wxColour(11,12,16));SetForegroundColour(wxColour(235,235,242));
        auto* root=new wxBoxSizer(wxVERTICAL);
        title=new wxStaticText(this,wxID_ANY,"Developer login");auto heading=GetFont();heading.SetPointSize(20);title->SetFont(heading);
        auto* titleRow=new wxBoxSizer(wxHORIZONTAL);titleRow->Add(title,0,wxALIGN_CENTER_VERTICAL);
        popout=new PopOutIcon(this);titleRow->Add(popout,0,wxALIGN_CENTER_VERTICAL|wxLEFT,FromDIP(12));popout->Hide();
        root->Add(titleRow,0,wxLEFT|wxRIGHT|wxTOP,FromDIP(28));
        root->AddSpacer(FromDIP(24));
        auto* credentials=new wxBoxSizer(wxHORIZONTAL);
        auto input=[this](wxWindow* parent,const wxString& hint,long flags,wxTextCtrl*& field) {
            auto* frame=new wxPanel(parent,wxID_ANY,wxDefaultPosition,wxDefaultSize,wxBORDER_SIMPLE);
            frame->SetBackgroundColour(wxColour(19,20,27));frame->SetMinSize(FromDIP(wxSize(-1,44)));
            field=new wxTextCtrl(frame,wxID_ANY,"",wxDefaultPosition,wxDefaultSize,wxBORDER_NONE|flags);
            field->SetBackgroundColour(wxColour(19,20,27));field->SetForegroundColour(wxColour(235,235,242));field->SetFont(GetFont());field->SetHint(hint);
            auto* layout=new wxBoxSizer(wxVERTICAL);layout->AddStretchSpacer();layout->Add(field,0,wxEXPAND|wxLEFT|wxRIGHT,FromDIP(12));layout->AddStretchSpacer();frame->SetSizer(layout);return frame;
        };
        credentials->Add(input(this,"Username",0,username),1,wxRIGHT,FromDIP(12));
        credentials->Add(input(this,"Developer key",wxTE_PASSWORD,secret),2,wxRIGHT,FromDIP(12));
        login=CreateRushActionButton(this,"Sign in",wxSize(120,44),true);credentials->Add(login);
        root->Add(credentials,0,wxEXPAND|wxLEFT|wxRIGHT,FromDIP(28));
        status=new wxStaticText(this,wxID_ANY,"");status->SetForegroundColour(wxColour(136,140,157));root->Add(status,0,wxLEFT|wxRIGHT|wxTOP,FromDIP(28));
        controls=new wxPanel(this);controls->SetBackgroundColour(GetBackgroundColour());auto* actions=new wxBoxSizer(wxVERTICAL);
        auto addButton=[&](wxBoxSizer* row,const wxString& label,auto action) {
            const int width=std::max(100,controls->ToDIP(controls->GetTextExtent(label).x)+26);
            auto* button=CreateRushActionButton(controls,label,wxSize(width,40));row->Add(button,0,wxRIGHT,FromDIP(8));button->Bind(wxEVT_BUTTON,action);
        };
        auto* toolbar=new wxBoxSizer(wxHORIZONTAL);
        addButton(toolbar,"View keys",[this](wxCommandEvent&){Request(L"/admin/keys");});
        addButton(toolbar,"Injection statistics",[this](wxCommandEvent&){Request(L"/admin/stats");});
        addButton(toolbar,"Publish on GitHub",[this](wxCommandEvent&){
            output->SetValue("Publish a client update\n\nUpload the new DLL to the main branch as RushClient_Alpha_v3.0.dll, replacing the existing file.\n\nGitHub verifies your publishing access separately. Wait up to 60 seconds for the download cache to refresh. Users must restart Minecraft and toggle Auto Inject to load the new client.");
            wxLaunchDefaultBrowser("https://github.com/Juice-loot-is-pog/Rush-Dll/upload/main");
        });
        addButton(toolbar,"Sign out",[this](wxCommandEvent&){ClearSession();username->Clear();secret->Clear();status->SetLabel("Signed out.");});
        actions->Add(toolbar,0,wxBOTTOM,FromDIP(20));
        auto* generate=new wxBoxSizer(wxHORIZONTAL);
        generate->Add(input(controls,"Beta username",0,account),1,wxRIGHT,FromDIP(10));
        duration=new DarkDurationPicker(controls);
        duration->SetBackgroundColour(wxColour(19,20,27));duration->SetForegroundColour(wxColour(235,235,242));
        for(auto choice:{"1 day","1 week","1 month (30 days)","Custom seconds"}) duration->Append(choice);duration->SetSelection(1);generate->Add(duration,0,wxRIGHT,FromDIP(10));
        auto* customInput=input(controls,"Seconds",0,seconds);customInput->SetMinSize(FromDIP(wxSize(90,44)));seconds->SetValue("86400");seconds->SetEditable(false);seconds->SetForegroundColour(wxColour(136,140,157));generate->Add(customInput,0,wxRIGHT,FromDIP(10));
        duration->Bind(wxEVT_COMBOBOX,[this](wxCommandEvent&){const bool custom=duration->GetSelection()==3;seconds->SetEditable(custom);seconds->SetForegroundColour(custom?wxColour(235,235,242):wxColour(136,140,157));seconds->Refresh();});
        addButton(generate,"Generate key",[this](wxCommandEvent&){
            if(!ValidName(account->GetValue())) {status->SetLabel("Use a username with letters, numbers, dots, underscores or hyphens.");return;}
            long value=0;if(duration->GetSelection()==3 && (!seconds->GetValue().ToLong(&value)||value<60||value>31536000)) {status->SetLabel("Custom duration must be 60 to 31536000 seconds.");return;}
            const char* names[]={"day","week","month","custom"};
            Request(L"/admin/keys","{\"username\":\""+account->GetValue().ToStdString()+"\",\"duration\":\""+names[duration->GetSelection()]+"\",\"seconds\":"+std::to_string(value)+"}");
        });actions->Add(generate,0,wxEXPAND|wxBOTTOM,FromDIP(16));
        auto* manage=new wxBoxSizer(wxHORIZONTAL);
        manage->Add(input(controls,"Key ID from View keys",0,identifier),1,wxRIGHT,FromDIP(10));
        addButton(manage,"Revoke key",[this](wxCommandEvent&){if(!std::regex_match(identifier->GetValue().ToStdString(),std::regex("[a-fA-F0-9-]{36}"))) {status->SetLabel("Enter the key ID, not its secret.");return;}Request(L"/admin/revoke","{\"id\":\""+identifier->GetValue().ToStdString()+"\"}");});
        for(bool suspend:{true,false}) addButton(manage,suspend?"Suspend account":"Activate account",[this,suspend](wxCommandEvent&){if(!ValidName(account->GetValue())) {status->SetLabel("Enter the beta username above.");return;}Request(L"/admin/account","{\"username\":\""+account->GetValue().ToStdString()+"\",\"status\":\""+(suspend?"suspended":"active")+"\"}");});
        actions->Add(manage,0,wxEXPAND);controls->SetSizer(actions);root->Add(controls,0,wxEXPAND|wxALL,FromDIP(28));controls->Hide();
        auto* deviceRow=new wxBoxSizer(wxHORIZONTAL);
        addButton(deviceRow,"Add time",[this](wxCommandEvent&){
            if(!std::regex_match(identifier->GetValue().ToStdString(),std::regex("[a-fA-F0-9-]{36}"))) {status->SetLabel("Enter the key ID from View keys.");return;}
            const long durations[]={86400,604800,2592000};
            const int selected=duration->GetSelection();
            long value=selected<3?durations[selected]:0;
            if(selected==3 && (!seconds->GetValue().ToLong(&value)||value<60||value>31536000)) {status->SetLabel("Added time must be 60 to 31536000 seconds.");return;}
            Request(L"/admin/extend","{\"id\":\""+identifier->GetValue().ToStdString()+"\",\"seconds\":"+std::to_string(value)+"}");
        });
        addButton(deviceRow,"Remove key",[this](wxCommandEvent&){
            if(!std::regex_match(identifier->GetValue().ToStdString(),std::regex("[a-fA-F0-9-]{36}"))) {status->SetLabel("Enter the key ID from View keys.");return;}
            if(wxMessageBox("Remove this key from the list and revoke its access? Injection history is retained.","Remove beta key",wxYES_NO|wxNO_DEFAULT|wxICON_QUESTION,this)!=wxYES)return;
            Request(L"/admin/remove","{\"id\":\""+identifier->GetValue().ToStdString()+"\"}");
        });
        addButton(deviceRow,"Reset device binding",[this](wxCommandEvent&){
            if(!std::regex_match(identifier->GetValue().ToStdString(),std::regex("[a-fA-F0-9-]{36}"))) {status->SetLabel("Enter the key ID to reset its device binding.");return;}
            if(wxMessageBox("Allow the next successful login to bind this key to a new device?","Reset device",wxYES_NO|wxNO_DEFAULT|wxICON_QUESTION,this)!=wxYES)return;
            Request(L"/admin/reset-device","{\"id\":\""+identifier->GetValue().ToStdString()+"\"}");
        });actions->Add(deviceRow,0,wxTOP,FromDIP(12));
        outputCard=new wxPanel(this);outputCard->SetBackgroundColour(wxColour(19,20,27));
        auto* outputLayout=new wxBoxSizer(wxVERTICAL);
        popout->Bind(wxEVT_BUTTON,[this](wxCommandEvent&){
            auto* window=new wxFrame(this,wxID_ANY,"Rush / Results",wxDefaultPosition,FromDIP(wxSize(800,520)),wxDEFAULT_FRAME_STYLE);
            window->SetMinSize(FromDIP(wxSize(420,280)));window->SetBackgroundColour(wxColour(19,20,27));
            styleDarkCaption(static_cast<HWND>(window->GetHandle()),wxColour(19,20,27));
            auto* text=new wxTextCtrl(window,wxID_ANY,output->GetValue(),wxDefaultPosition,wxDefaultSize,wxTE_MULTILINE|wxTE_READONLY|wxTE_WORDWRAP|wxBORDER_NONE);
            text->SetFont(output->GetFont());text->SetBackgroundColour(wxColour(19,20,27));text->SetForegroundColour(wxColour(235,235,242));
            SetWindowTheme(static_cast<HWND>(text->GetHandle()),L"DarkMode_Explorer",nullptr);
            auto* layout=new wxBoxSizer(wxVERTICAL);layout->Add(text,1,wxEXPAND|wxALL,FromDIP(20));window->SetSizer(layout);window->CentreOnParent();window->Show();
        });
        output=new wxTextCtrl(outputCard,wxID_ANY,"",wxDefaultPosition,wxDefaultSize,wxTE_MULTILINE|wxTE_READONLY|wxTE_WORDWRAP|wxBORDER_NONE);
        output->SetBackgroundColour(wxColour(19,20,27));output->SetForegroundColour(wxColour(235,235,242));
        SetWindowTheme(static_cast<HWND>(output->GetHandle()),L"DarkMode_Explorer",nullptr);
        outputLayout->Add(output,1,wxEXPAND|wxALL,FromDIP(18));outputCard->SetSizer(outputLayout);
        root->Add(outputCard,1,wxEXPAND|wxLEFT|wxRIGHT|wxBOTTOM,FromDIP(28));outputCard->Hide();SetSizer(root);
        login->Bind(wxEVT_BUTTON,[this](wxCommandEvent&){if(!ValidName(username->GetValue())||!ValidDownloadKey(secret->GetValue().ToStdWstring())) {status->SetLabel("Enter a valid developer username and key.");return;}ClearSession();sessionUser=username->GetValue().ToStdWstring();sessionKey=secret->GetValue().ToStdWstring();secret->Clear();Request(L"/admin/stats",{},true);});
        Bind(DEV_RESULT,[this](wxThreadEvent& event){busy=false;login->Enable();controls->Enable();auto result=event.GetPayload<Result>();
            if(result.status>=200 && result.status<300) {if(result.login){title->SetLabel("Developer panel");controls->Show();outputCard->Show();popout->Show();Layout();}status->SetLabel("Server request completed. Generated secrets are shown only once.");output->SetValue(Pretty(result.text));}
            else {if(result.login||result.status==401)ClearSession();status->SetLabel(result.status?wxString::Format("Server returned HTTP %lu",result.status):wxString("Server connection failed. Try again."));output->SetValue(result.text);}
        });
    }
    ~DeveloperPanel() override {if(worker.joinable())worker.join();ClearSession();}
};
constexpr wchar_t rememberedBetaTarget[] = L"Rush Injector/Beta Remember Me";
bool SaveRememberedBeta(const std::wstring& username, const std::wstring& secret) {
    CREDENTIALW credential{};
    credential.Type=CRED_TYPE_GENERIC;
    credential.TargetName=const_cast<wchar_t*>(rememberedBetaTarget);
    credential.UserName=const_cast<wchar_t*>(username.c_str());
    credential.CredentialBlob=reinterpret_cast<LPBYTE>(const_cast<wchar_t*>(secret.data()));
    credential.CredentialBlobSize=static_cast<DWORD>(secret.size()*sizeof(wchar_t));
    credential.Persist=CRED_PERSIST_LOCAL_MACHINE;
    return CredWriteW(&credential,0)!=FALSE;
}
class BetaLogin : public wxPanel {
    std::jthread worker;
    wxTextCtrl *user,*key;
    wxCheckBox* remember;
    wxStaticText* status;
    wxWindow* button;
    wxWindow* injectBeta;
    bool busy=false;
public:
    explicit BetaLogin(wxWindow* parent,std::function<void()> inject,wxWindow* injectButton):wxPanel(parent) {
        SetBackgroundColour(wxColour(11,12,16));SetForegroundColour(wxColour(235,235,242));
        auto* layout=new wxBoxSizer(wxVERTICAL);
        layout->Add(new wxStaticText(this,wxID_ANY,"Beta access"),0,wxBOTTOM,FromDIP(12));
        auto* row=new wxBoxSizer(wxHORIZONTAL);
        auto field=[this](const wxString& hint,bool password,wxTextCtrl*& value){
            auto* frame=new wxPanel(this,wxID_ANY,wxDefaultPosition,wxDefaultSize,wxBORDER_SIMPLE);frame->SetBackgroundColour(wxColour(19,20,27));frame->SetMinSize(FromDIP(wxSize(-1,44)));
            value=new wxTextCtrl(frame,wxID_ANY,"",wxDefaultPosition,wxDefaultSize,wxBORDER_NONE|(password?wxTE_PASSWORD:0));value->SetHint(hint);value->SetBackgroundColour(wxColour(19,20,27));value->SetForegroundColour(wxColour(235,235,242));
            auto* s=new wxBoxSizer(wxVERTICAL);s->AddStretchSpacer();s->Add(value,0,wxEXPAND|wxLEFT|wxRIGHT,FromDIP(12));s->AddStretchSpacer();frame->SetSizer(s);return frame;
        };
        row->Add(field("Username",false,user),1,wxRIGHT,FromDIP(10));row->Add(field("Secret beta key",true,key),2,wxRIGHT,FromDIP(10));
        button=CreateRushActionButton(this,"Sign in",wxSize(115,44),true,wxColour(224,175,65));row->Add(button);layout->Add(row,0,wxEXPAND);
        remember=CreateRushCheckBox(this,"Remember me");
        remember->SetToolTip("Remember beta sign-in on this Windows device only.");
        layout->Add(remember,0,wxTOP,FromDIP(12));
        PCREDENTIALW saved=nullptr;
        if(CredReadW(rememberedBetaTarget,CRED_TYPE_GENERIC,0,&saved)) {
            if(saved->UserName && saved->CredentialBlobSize>0 && saved->CredentialBlobSize<=256 && saved->CredentialBlobSize%sizeof(wchar_t)==0) {
                std::wstring secret(reinterpret_cast<const wchar_t*>(saved->CredentialBlob),saved->CredentialBlobSize/sizeof(wchar_t));
                if(ValidName(saved->UserName) && ValidDownloadKey(secret)) {
                    user->SetValue(saved->UserName);key->SetValue(secret);remember->SetValue(true);
                }
                SecureZeroMemory(secret.data(),secret.size()*sizeof(wchar_t));
            }
            CredFree(saved);
        }
        remember->Bind(wxEVT_CHECKBOX,[this](wxCommandEvent& event){
            if(!remember->GetValue()) CredDeleteW(rememberedBetaTarget,CRED_TYPE_GENERIC,0);
            event.Skip();
        });
        injectBeta=injectButton;injectBeta->Hide();
        injectBeta->Bind(wxEVT_BUTTON,[inject](wxCommandEvent&){inject();});
        status=new wxStaticText(this,wxID_ANY,"First login binds your key to this Windows device. Developer reset required to change devices.");status->SetForegroundColour(wxColour(136,140,157));status->Wrap(FromDIP(720));layout->Add(status,0,wxTOP,FromDIP(12));SetSizer(layout);
        button->Bind(wxEVT_BUTTON,[this](wxCommandEvent&){
            if(busy)return;
            injectBeta->Hide();Layout();GetParent()->Layout();
            if(!ValidName(user->GetValue())||!ValidDownloadKey(key->GetValue().ToStdWstring())) {status->SetLabel("Enter your username and secret beta key, not the public Key ID.");return;}
            if(DeviceIdentityHash().size()!=64) {status->SetLabel("This PC has no readable Windows device ID (MachineGuid), which beta login requires. A system-tweaking or debloat tool may have removed it.");status->Wrap(FromDIP(720));Layout();GetParent()->Layout();return;}
            busy=true;button->Disable();remember->Disable();status->SetLabel("Verifying beta access...");
            worker=std::jthread([this,username=user->GetValue().ToStdWstring(),secret=key->GetValue().ToStdWstring(),keep=remember->GetValue()]() mutable {
                Result result;try {
                    result=Call(username,secret,L"/beta/login","{}",false);
                    if(result.status==200 && !SaveDownloadKey(L"https://rush-download-service.juiceloot-github.workers.dev/beta/client",secret)){result.status=0;result.text="Could not save the beta key in Windows Credential Manager.";}
                    if(result.status==200 && keep && !SaveRememberedBeta(username,secret)) {
                        result.status=0;result.text="Could not remember beta credentials.";
                    }
                    if(!keep || result.status==401) CredDeleteW(rememberedBetaTarget,CRED_TYPE_GENERIC,0);
                }catch(...) {result.status=0;}
                SecureZeroMemory(secret.data(),secret.size()*sizeof(wchar_t));auto* event=new wxThreadEvent(DEV_RESULT);event->SetPayload(result);wxQueueEvent(this,event);
            });
        });
        Bind(DEV_RESULT,[this](wxThreadEvent& event){busy=false;button->Enable();remember->Enable();const auto result=event.GetPayload<Result>();key->Clear();
            if(result.status==200){injectBeta->Show();status->SetLabel("Beta access verified. Launch Minecraft, then click Inject Beta.");}
            else if(result.status==403)status->SetLabel("This beta key belongs to another device. Ask a developer to reset its binding.");
            else if(result.status==401)status->SetLabel("Username or key rejected, expired, revoked or suspended.");
            else if(result.status==0 && !result.text.empty())status->SetLabel(result.text);
            else if(result.status==0)status->SetLabel(wxString::Format("Beta login failed: could not reach the Rush service (Windows error %lu). Check your internet, VPN, proxy or antivirus, then try again.",static_cast<unsigned long>(result.error)));
            else {
                std::smatch match;const std::string body=result.text.ToStdString();
                const wxString reason=std::regex_search(body,match,std::regex(R"re("error"\s*:\s*"([^"]{1,160})")re"))?wxString::FromUTF8(match[1].str()):wxString("no details");
                status->SetLabel(wxString::Format("Beta login failed (HTTP %lu): ",static_cast<unsigned long>(result.status))+reason);
            }
            status->Wrap(FromDIP(720));Layout();GetParent()->Layout();
        });
    }
    ~BetaLogin() override {if(worker.joinable())worker.join();}
};
}
wxPanel* CreateDeveloperPanel(wxWindow* parent){return new DeveloperPanel(parent);}
wxPanel* CreateBetaLogin(wxWindow* parent,std::function<void()> inject,wxWindow* injectButton){return new BetaLogin(parent,std::move(inject),injectButton);}
void ReportInjectionSuccess(const std::wstring& key) {
    GUID guid{}; if(CoCreateGuid(&guid)!=S_OK) return;
    wchar_t id[40]{};StringFromGUID2(guid,id,40);
    Call(L"",key,L"/injection","{}",false,std::wstring(id+1,36));
}
