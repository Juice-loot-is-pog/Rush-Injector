#pragma once
#include <wx/wx.h>
#include <functional>
wxPanel* CreateDeveloperPanel(wxWindow* parent);
wxPanel* CreateBetaLogin(wxWindow* parent,std::function<void()> inject,wxWindow* injectButton);
wxWindow* CreateRushActionButton(wxWindow* parent,const wxString& text,const wxSize& size,bool primary=false,wxColour accent=wxNullColour);
void ReportInjectionSuccess(const std::wstring& key);

wxCheckBox* CreateRushCheckBox(wxWindow* parent, const wxString& text);
void styleDarkCaption(HWND window, const wxColour& colour);
