#include "pch.h"
#include "cMain.h"
#include <wx/dcbuffer.h>
#include <wx/graphics.h>
#include <wx/fontutil.h>
#include <objidl.h>
#include <gdiplus.h>
#include <wx/mstream.h>
#include <wx/filename.h>
#include <wx/stdpaths.h>
#include <wx/file.h>
#include <wx/base64.h>
#include <wx/taskbarbutton.h>
#include <wx/bmpbndl.h>
#include <memory>
#include <functional>
#include <dwmapi.h>
#include <shellapi.h>
#include <cmath>
#include <chrono>
#include <winhttp.h>
#include <regex>
#include <optional>
#include <fstream>
#include "client_download.h"
#include "dev_panel.h"

wxDEFINE_EVENT(EVT_RUSH_DOWNLOAD_PROGRESS, wxThreadEvent);
wxDEFINE_EVENT(EVT_RUSH_DOWNLOAD_FINISHED, wxThreadEvent);
wxDEFINE_EVENT(EVT_RUSH_UPDATE_CHECKED, wxThreadEvent);
wxDEFINE_EVENT(EVT_RUSH_CLIENT_CHECKED, wxThreadEvent);
wxDEFINE_EVENT(EVT_RUSH_DOWNLOAD_DIAGNOSTIC, wxThreadEvent);

namespace {
wxString FailureLogPath() {
    return wxStandardPaths::Get().GetUserLocalDataDir()+"\\failure.log";
}
bool LogFailure(const wxString& stage, DWORD error, DWORD http = 0, const std::wstring& requestId = {}, const std::wstring& category = {}) {
    const wxString path=FailureLogPath();
    wxFileName::Mkdir(wxFileName(path).GetPath(),wxS_DIR_DEFAULT,wxPATH_MKDIR_FULL);
    if (wxFileName::FileExists(path) && wxFileName(path).GetSize()>1024*1024)
        if (!wxRenameFile(path,path+".previous",true)) return false;
    std::ofstream file(std::wstring(path.wc_str()),std::ios::binary|std::ios::app);
    const wxString line=wxDateTime::Now().FormatISOCombined(' ')+" | injector " RUSH_INJECTOR_VERSION " | "+stage+
        wxString::Format(" | Windows=%lu | HTTP=%lu",error,http)+" | request="+wxString(requestId)+" | category="+wxString(category)+"\r\n";
    const auto utf8=line.ToUTF8(); file.write(utf8.data(),utf8.length());
    return file.good();
}
wxIcon UpdateBadgeIcon() {
    static const char* const pixels[] = {
        "16 16 3 1", "  c None", ". c #ED304F", "+ c #FFFFFF",
        "     ......     ",
        "   ..........   ",
        "  ............  ",
        " .............. ",
        " ......++...... ",
        " .....+++...... ",
        " ....++++...... ",
        " ......++...... ",
        " ......++...... ",
        " ......++...... ",
        " ......++...... ",
        " .....++++..... ",
        " .............. ",
        "  ............  ",
        "   ..........   ",
        "     ......     "
    };
    return wxIcon(pixels);
}
std::optional<bool> CheckInjectorUpdate() {
    struct InternetCloser { void operator()(void* handle) const { if (handle) WinHttpCloseHandle(handle); } };
    using Internet = std::unique_ptr<void,InternetCloser>;
    Internet session(WinHttpOpen(L"Rush-Injector-Update",WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,nullptr,nullptr,0));
    if (!session) return {};
    WinHttpSetTimeouts(session.get(),3000,3000,3000,3000);
    Internet connection(WinHttpConnect(session.get(),L"api.github.com",443,0));
    if (!connection) return {};
    Internet request(WinHttpOpenRequest(connection.get(),L"GET",L"/repos/Juice-loot-is-pog/Rush-Injector/releases/latest",
        nullptr,nullptr,WINHTTP_DEFAULT_ACCEPT_TYPES,WINHTTP_FLAG_SECURE));
    if (!request) return {};
    DWORD redirects = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
    if (!WinHttpSetOption(request.get(),WINHTTP_OPTION_REDIRECT_POLICY,&redirects,sizeof(redirects))) return {};
    if (!WinHttpSendRequest(request.get(),nullptr,0,nullptr,0,0,0) || !WinHttpReceiveResponse(request.get(),nullptr)) return {};
    DWORD status = 0, size = sizeof(status);
    if (!WinHttpQueryHeaders(request.get(),WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,nullptr,&status,&size,nullptr) || status != 200) return {};
    std::string body; char buffer[4096]; DWORD count;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    for (;;) {
        if (std::chrono::steady_clock::now() > deadline || !WinHttpReadData(request.get(),buffer,sizeof(buffer),&count)) return {};
        if (!count) break;
        if (body.size() + count > 65536) return {};
        body.append(buffer,count);
    }
    std::smatch match;
    if (!std::regex_search(body,match,std::regex("\\\"tag_name\\\"\\s*:\\s*\\\"v?([0-9]{1,6})\\.([0-9]{1,6})\\.([0-9]{1,6})\\\""))) return {};
    const std::array<int,3> latest{std::stoi(match[1]),std::stoi(match[2]),std::stoi(match[3])};
    const std::string version = RUSH_INJECTOR_VERSION;
    if (!std::regex_match(version,match,std::regex("([0-9]+)\\.([0-9]+)\\.([0-9]+)"))) return {};
    return latest > std::array<int,3>{std::stoi(match[1]),std::stoi(match[2]),std::stoi(match[3])};
}
const wxColour BG(11, 12, 16), CARD(19, 20, 27), LINE(43, 44, 53);
const wxColour FOREGROUND(235, 235, 242), MUTED(136, 140, 157);
wxColour RED(237, 48, 79);
const wxColour GOLD(224,175,65);
wxColour themeColour() {
    if (Globals::COLOR_PRESET == L"Blue") return wxColour(57,112,224);
    if (Globals::COLOR_PRESET == L"Purple") return wxColour(132,75,204);
    if (Globals::COLOR_PRESET == L"Mono") return wxColour(103,105,119);
    return wxColour(237,48,79);
}
wxFont font(int size, bool bold = false) {
    wxFont result(wxFontInfo(size).FaceName(Globals::FONT_FAMILY).Weight(bold ? wxFONTWEIGHT_BOLD : wxFONTWEIGHT_NORMAL));
    wxNativeFontInfo native(*result.GetNativeFontInfo());
    native.lf.lfQuality = CLEARTYPE_QUALITY;
    result.SetNativeFontInfo(native);
    return result;
}
void smoothText(wxGraphicsContext& graphics) {
    if (graphics.GetRenderer() == wxGraphicsRenderer::GetGDIPlusRenderer()) {
        // Grayscale coverage also works on transparent glyph images and textured backgrounds.
        static_cast<Gdiplus::Graphics*>(graphics.GetNativeContext())->SetTextRenderingHint(
            Gdiplus::TextRenderingHintAntiAliasGridFit);
    }
}
wxStaticText* label(wxWindow* p, const wxString& text, int size = 10, wxColour colour = FOREGROUND, bool bold = false) {
    auto* t = new wxStaticText(p, wxID_ANY, text);
    t->SetFont(font(size, bold)); t->SetForegroundColour(colour); return t;
}
class RushCheckBox : public wxCheckBox {
protected:
    wxSize DoGetBestSize() const override {
        const wxSize text = GetTextExtent(GetLabel());
        return wxSize(FromDIP(17)+text.x,std::max(FromDIP(13),text.y));
    }
public:
    RushCheckBox(wxWindow* parent, int id, const wxString& text)
        : wxCheckBox(parent,id,text) {
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        Bind(wxEVT_PAINT,[this](wxPaintEvent&) {
            wxAutoBufferedPaintDC dc(this);
            dc.SetBackground(wxBrush(GetParent()->GetBackgroundColour())); dc.Clear();
            const int box = FromDIP(13), gap = FromDIP(4);
            const int top = (GetClientSize().y-box)/2;
            dc.SetPen(wxPen(GetValue() ? RED : MUTED));
            dc.SetBrush(wxBrush(GetValue() ? RED : wxColour(25,25,34)));
            dc.DrawRoundedRectangle(0,top,box,box,FromDIP(2));
            dc.SetFont(GetFont()); dc.SetTextForeground(GetForegroundColour());
            const auto extent = dc.GetTextExtent(GetLabel());
            dc.DrawText(GetLabel(),box+gap,(GetClientSize().y-extent.y)/2);
        });
        Bind(wxEVT_CHECKBOX,[this](wxCommandEvent& event) { Refresh(false); event.Skip(); });
        Bind(wxEVT_SET_FOCUS,[this](wxFocusEvent& event) { Refresh(false); event.Skip(); });
        Bind(wxEVT_KILL_FOCUS,[this](wxFocusEvent& event) { Refresh(false); event.Skip(); });
    }
};
class DeveloperIconButton : public wxControl {
    wxBitmap bitmap;
public:
    explicit DeveloperIconButton(wxWindow* parent)
        : wxControl(parent,wxID_ANY,wxDefaultPosition,parent->FromDIP(wxSize(30,30)),wxBORDER_NONE|wxWANTS_CHARS) {
        SetBackgroundStyle(wxBG_STYLE_PAINT);SetCursor(wxCURSOR_HAND);SetName("Developer panel");
        Bind(wxEVT_PAINT,[this](wxPaintEvent&){
            wxAutoBufferedPaintDC dc(this);dc.SetBackground(wxBrush(GetParent()->GetBackgroundColour()));dc.Clear();
            if(bitmap.IsOk()) dc.DrawBitmap(bitmap,(GetClientSize().x-bitmap.GetWidth())/2,(GetClientSize().y-bitmap.GetHeight())/2,true);
        });
        Bind(wxEVT_LEFT_DOWN,[this](wxMouseEvent&){SetFocus();CaptureMouse();});
        Bind(wxEVT_LEFT_UP,[this](wxMouseEvent& event){if(!HasCapture())return;ReleaseMouse();if(GetClientRect().Contains(event.GetPosition()))Click();});
        Bind(wxEVT_MOUSE_CAPTURE_LOST,[](wxMouseCaptureLostEvent&){});
        Bind(wxEVT_KEY_DOWN,[this](wxKeyEvent& event){if(event.GetKeyCode()==WXK_RETURN||event.GetKeyCode()==WXK_SPACE)Click();else event.Skip();});
    }
    void SetBitmap(const wxBitmap& value){bitmap=value;Refresh(false);}
    void Click(){wxCommandEvent event(wxEVT_BUTTON,GetId());event.SetEventObject(this);ProcessWindowEvent(event);}
};
class WindowButton : public wxControl {
public:
    enum Kind { Minimize, Close };
private:
    Kind kind;
    bool hover = false, pressed = false;
public:
    WindowButton(wxWindow* parent, Kind type)
        : wxControl(parent,wxID_ANY,wxDefaultPosition,parent->FromDIP(wxSize(46,32)),wxBORDER_NONE), kind(type) {
        const wxString text = kind == Close ? "Close" : "Minimize";
        SetMinSize(parent->FromDIP(wxSize(46,32))); SetBackgroundStyle(wxBG_STYLE_PAINT);
        SetToolTip(text); SetName(text);
        Bind(wxEVT_PAINT,[this](wxPaintEvent&) {
            const bool held = pressed && hover;
            wxColour fill = GetParent()->GetBackgroundColour();
            if (hover) fill = kind == Close ? (held ? wxColour(241,112,122) : wxColour(232,17,35)) :
                (held ? wxColour(35,36,44) : LINE);
            wxAutoBufferedPaintDC dc(this); dc.SetBackground(wxBrush(fill)); dc.Clear();
            std::unique_ptr<wxGraphicsContext> g(wxGraphicsContext::Create(dc));
            if (!g) return;
            const wxSize size = GetClientSize();
            const double x = size.x/2.0, y = size.y/2.0, half = FromDIP(5);
            g->SetPen(wxPen(hover && kind == Close ? wxColour(255,255,255) : FOREGROUND,std::max(1,FromDIP(1))));
            if (kind == Close) {
                g->StrokeLine(x-half,y-half,x+half,y+half);
                g->StrokeLine(x-half,y+half,x+half,y-half);
            } else g->StrokeLine(x-half,y,x+half,y);
        });
        Bind(wxEVT_ENTER_WINDOW,[this](wxMouseEvent&) { hover = true; Refresh(false); });
        Bind(wxEVT_LEAVE_WINDOW,[this](wxMouseEvent&) { hover = false; Refresh(false); });
        Bind(wxEVT_LEFT_DOWN,[this](wxMouseEvent&) { pressed = true; CaptureMouse(); Refresh(false); });
        Bind(wxEVT_LEFT_UP,[this](wxMouseEvent& event) {
            if (!HasCapture()) return;
            ReleaseMouse(); pressed = false;
            const bool clicked = GetClientRect().Contains(event.GetPosition());
            // Minimizing moves the window away without a leave event.
            hover = false; Refresh(false);
            if (clicked) { wxCommandEvent click(wxEVT_BUTTON,GetId()); click.SetEventObject(this); ProcessWindowEvent(click); }
        });
        Bind(wxEVT_MOUSE_CAPTURE_LOST,[this](wxMouseCaptureLostEvent&) { pressed = false; Refresh(false); });
    }
    bool AcceptsFocus() const override { return false; }
};
class RushButton : public wxControl {
    bool hover = false, primary;
    wxColour swatch;
    wxTimer animationTimer;
    double hoverProgress = 0.0;
    double hoverVelocity = 0.0;
    wxBitmap textBitmap;
    wxString cachedText;
    wxFont cachedFont;
    wxColour cachedColour;
    wxSize cachedDpi;
    double textWidth = 0.0, textHeight = 0.0;
    std::chrono::steady_clock::time_point lastFrame = std::chrono::steady_clock::now();
    void AdvanceAnimation() {
        const auto now = std::chrono::steady_clock::now();
        const double elapsed = std::chrono::duration<double>(now - lastFrame).count();
        lastFrame = now;
        const double target = hover ? 1.0 : 0.0;
        // Analytic critically damped motion stays consistent when timer delivery varies.
        constexpr double speed = 24.0;
        const double offset = hoverProgress - target;
        const double change = (hoverVelocity + speed * offset) * elapsed;
        const double decay = std::exp(-speed * elapsed);
        hoverProgress = std::clamp(target + (offset + change) * decay, 0.0, 1.0);
        hoverVelocity = (hoverVelocity - speed * change) * decay;
        if (std::abs(target - hoverProgress) < 0.0005 && std::abs(hoverVelocity) < 0.015) {
            hoverProgress = target; hoverVelocity = 0.0; animationTimer.Stop();
        }
    }
    void SetHovered(bool value) {
        if (hover == value) return;
        if (animationTimer.IsRunning()) AdvanceAnimation();
        hover = value; lastFrame = std::chrono::steady_clock::now();
        if (!animationTimer.IsRunning()) animationTimer.Start(8);
        Refresh(false);
    }
    void DrawAnimatedText(wxGraphicsContext& graphics, const wxSize& size, double scale) {
        const bool yellowBackground = swatch == GOLD && (primary || hoverProgress >= 0.5);
        const wxColour colour = !IsEnabled() ? MUTED : yellowBackground ? wxColour(0,0,0) : FOREGROUND;
        graphics.SetFont(GetFont(), colour);
        if (!textBitmap.IsOk() || cachedText != GetLabel() || cachedFont != GetFont() ||
            cachedColour != colour || cachedDpi != GetDPI()) {
            double width, height; graphics.GetTextExtent(GetLabel(), &width, &height);
            constexpr double sampling = 3.0, padding = 2.0;
            const int pixelsWide = std::max(1,static_cast<int>(std::ceil((width + padding * 2) * sampling)));
            const int pixelsHigh = std::max(1,static_cast<int>(std::ceil((height + padding * 2) * sampling)));
            wxImage image(pixelsWide,pixelsHigh); image.InitAlpha();
            std::fill_n(image.GetAlpha(),static_cast<size_t>(pixelsWide) * pixelsHigh,0);
            {
                std::unique_ptr<wxGraphicsContext> text(wxGraphicsContext::Create(image));
                if (!text) return;
                smoothText(*text);
                text->Scale(sampling,sampling);
                // Use the window's graphics font to preserve its DPI and private font selection.
                text->SetFont(graphics.CreateFont(GetFont(),colour));
                text->DrawText(GetLabel(),padding,padding);
            }
            textBitmap = wxBitmap(image);
            textWidth = pixelsWide / sampling; textHeight = pixelsHigh / sampling;
            cachedText = GetLabel(); cachedFont = GetFont(); cachedColour = colour; cachedDpi = GetDPI();
        }
        // Scale a stable high-resolution glyph image instead of re-hinting text each frame.
        graphics.SetInterpolationQuality(wxINTERPOLATION_BEST);
        const double width = textWidth * scale, height = textHeight * scale;
        graphics.DrawBitmap(textBitmap,(size.x-width)/2.0,(size.y-height)/2.0,width,height);
    }
public:
    RushButton(wxWindow* p, int id, const wxString& text, bool accent = false, wxSize size = wxSize(-1, 42), wxColour colour = wxNullColour)
        : wxControl(p, id, wxDefaultPosition, size, wxBORDER_NONE | wxWANTS_CHARS), primary(accent), swatch(colour), animationTimer(this) {
        SetLabel(text); SetFont(font(10, true)); SetBackgroundStyle(wxBG_STYLE_PAINT);
        SetCursor(wxCURSOR_HAND); SetMinSize(FromDIP(size));
        Bind(wxEVT_PAINT, [this](wxPaintEvent&) {
            wxAutoBufferedPaintDC dc(this); dc.SetBackground(wxBrush(GetParent()->GetBackgroundColour())); dc.Clear();
            std::unique_ptr<wxGraphicsContext> g(wxGraphicsContext::Create(dc));
            if (!g) return;
            wxColour active = swatch.IsOk() ? swatch : RED;
            const wxColour base = primary ? active : wxColour(25, 25, 34);
            auto blend = [this](int from, int to) { return static_cast<unsigned char>(from + (to - from) * hoverProgress); };
            wxColour fill(blend(base.Red(), active.Red()), blend(base.Green(), active.Green()), blend(base.Blue(), active.Blue()));
            if (!IsEnabled()) fill = wxColour(55, 55, 63);
            auto s = GetClientSize();
            const double scale = 1.0 - 0.12 * hoverProgress;
            g->PushState();
            g->Translate(s.x * (1.0 - scale) / 2.0, s.y * (1.0 - scale) / 2.0);
            g->Scale(scale, scale);
            g->SetBrush(wxBrush(fill)); g->SetPen(wxPen((primary || hoverProgress > 0) ? fill : LINE));
            g->DrawRoundedRectangle(1, 1, s.x - 2, s.y - 2, FromDIP(7));
            g->PopState();
            DrawAnimatedText(*g,s,scale);
        });
        Bind(wxEVT_TIMER, [this](wxTimerEvent&) {
            AdvanceAnimation(); Refresh(false);
        });
        Bind(wxEVT_ENTER_WINDOW, [this](wxMouseEvent&) { SetHovered(true); });
        Bind(wxEVT_LEAVE_WINDOW, [this](wxMouseEvent&) { SetHovered(false); });
        Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent&) { SetFocus(); CaptureMouse(); });
        Bind(wxEVT_LEFT_UP, [this](wxMouseEvent& e) {
            if (!HasCapture()) return; ReleaseMouse();
            if (GetClientRect().Contains(e.GetPosition())) Click();
        });
        Bind(wxEVT_MOUSE_CAPTURE_LOST, [](wxMouseCaptureLostEvent&) {});
        Bind(wxEVT_KEY_DOWN, [this](wxKeyEvent& e) { if (e.GetKeyCode() == WXK_SPACE || e.GetKeyCode() == WXK_RETURN) Click(); else e.Skip(); });
        Bind(wxEVT_SET_FOCUS, [this](wxFocusEvent& e) { Refresh(); e.Skip(); });
        Bind(wxEVT_KILL_FOCUS, [this](wxFocusEvent& e) { Refresh(); e.Skip(); });
    }
    void Click() { if (IsEnabled()) { wxCommandEvent event(wxEVT_BUTTON, GetId()); event.SetEventObject(this); ProcessWindowEvent(event); } }
    void SetPrimary(bool value) { primary = value; Refresh(); }
    void SetAccent(wxColour colour, bool filled) { swatch = colour; SetPrimary(filled); }
};
// Discord logo as vector data so it can be tinted grey (off) or with the theme colour (on).
wxBitmap DiscordIcon(const wxColour& colour, const wxSize& size) {
    const wxString svg = wxString::Format(
        "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 127.14 96.36\"><path fill=\"%s\" d=\""
        "M107.7,8.07A105.15,105.15,0,0,0,81.47,0a72.06,72.06,0,0,0-3.36,6.83A97.68,97.68,0,0,0,49,6.83,72.37,72.37,0,0,0,45.64,0,"
        "105.89,105.89,0,0,0,19.39,8.09C2.79,32.65-1.71,56.6.54,80.21h0A105.73,105.73,0,0,0,32.71,96.36,77.7,77.7,0,0,0,39.6,85.25"
        "a68.42,68.42,0,0,1-10.85-5.18c.91-.66,1.8-1.34,2.66-2a75.57,75.57,0,0,0,64.32,0c.87.71,1.76,1.39,2.66,2a68.68,68.68,0,0,1"
        "-10.87,5.19,77,77,0,0,0,6.89,11.1A105.25,105.25,0,0,0,126.6,80.22h0C129.24,52.84,122.09,29.11,107.7,8.07Z"
        "M42.45,65.69C36.18,65.69,31,60,31,53s5-12.74,11.43-12.74S54,46,53.89,53,48.84,65.69,42.45,65.69Z"
        "m42.24,0C78.41,65.69,73.25,60,73.25,53s5-12.74,11.44-12.74S96.23,46,96.12,53,91.08,65.69,84.69,65.69Z\"/></svg>",
        colour.GetAsString(wxC2S_HTML_SYNTAX));
    const wxCharBuffer data = svg.ToUTF8();
    return wxBitmapBundle::FromSVG(data.data(),size).GetBitmap(size);
}
class DiscordToggle : public wxControl {
    bool on, hover = false;
public:
    DiscordToggle(wxWindow* p, bool enabled)
        : wxControl(p, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE | wxWANTS_CHARS), on(enabled) {
        SetFont(font(10, true)); SetBackgroundStyle(wxBG_STYLE_PAINT);
        SetCursor(wxCURSOR_HAND); SetMinSize(FromDIP(wxSize(212, 42)));
        SetToolTip("Show Rush Injector as your Discord Rich Presence status");
        Bind(wxEVT_PAINT, [this](wxPaintEvent&) {
            wxAutoBufferedPaintDC dc(this); dc.SetBackground(wxBrush(GetParent()->GetBackgroundColour())); dc.Clear();
            std::unique_ptr<wxGraphicsContext> g(wxGraphicsContext::Create(dc));
            if (!g) return;
            smoothText(*g);
            const wxSize s = GetClientSize();
            const wxColour accent = on ? RED : MUTED;
            g->SetBrush(wxBrush(hover ? wxColour(25,25,34) : CARD));
            g->SetPen(wxPen(on ? RED : hover ? MUTED : LINE));
            g->DrawRoundedRectangle(1, 1, s.x - 2, s.y - 2, FromDIP(7));
            const wxSize iconSize = FromDIP(wxSize(22, 17));
            const int pad = FromDIP(14);
            g->DrawBitmap(DiscordIcon(accent, iconSize), pad, (s.y - iconSize.y) / 2.0, iconSize.x, iconSize.y);
            g->SetFont(GetFont(), on ? FOREGROUND : MUTED);
            double w, h; g->GetTextExtent("Rich Presence", &w, &h);
            g->DrawText("Rich Presence", pad + iconSize.x + FromDIP(10), (s.y - h) / 2.0);
            // Switch track and knob on the right.
            const double tw = FromDIP(34), th = FromDIP(18), tx = s.x - pad - tw, ty = (s.y - th) / 2.0;
            g->SetPen(*wxTRANSPARENT_PEN);
            g->SetBrush(wxBrush(on ? RED : LINE));
            g->DrawRoundedRectangle(tx, ty, tw, th, th / 2.0);
            const double knob = th - FromDIP(6);
            g->SetBrush(wxBrush(on ? FOREGROUND : MUTED));
            g->DrawEllipse(on ? tx + tw - FromDIP(3) - knob : tx + FromDIP(3), ty + FromDIP(3), knob, knob);
        });
        Bind(wxEVT_ENTER_WINDOW, [this](wxMouseEvent&) { hover = true; Refresh(false); });
        Bind(wxEVT_LEAVE_WINDOW, [this](wxMouseEvent&) { hover = false; Refresh(false); });
        Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent&) { SetFocus(); CaptureMouse(); });
        Bind(wxEVT_LEFT_UP, [this](wxMouseEvent& e) {
            if (!HasCapture()) return; ReleaseMouse();
            if (GetClientRect().Contains(e.GetPosition())) Toggle();
        });
        Bind(wxEVT_MOUSE_CAPTURE_LOST, [](wxMouseCaptureLostEvent&) {});
        Bind(wxEVT_KEY_DOWN, [this](wxKeyEvent& e) { if (e.GetKeyCode() == WXK_SPACE || e.GetKeyCode() == WXK_RETURN) Toggle(); else e.Skip(); });
    }
    bool IsOn() const { return on; }
    void Toggle() {
        on = !on; Refresh(false);
        wxCommandEvent event(wxEVT_CHECKBOX, GetId()); event.SetEventObject(this); event.SetInt(on);
        ProcessWindowEvent(event);
    }
};
double GlowPulse() {
    static const auto started = std::chrono::steady_clock::now();
    const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now()-started).count();
    // A visible three-second breath, fading between 20% and full intensity.
    return 0.60 + 0.40*std::cos(elapsed*2.0943951023931953);
}
void DrawPanelGlow(wxGraphicsContext& graphics, wxWindow& panel, bool rounded, wxColour accent = wxNullColour) {
    if (!accent.IsOk()) accent = RED;
    const wxSize size = panel.GetClientSize();
    const double inset = panel.FromDIP(8);
    const double width = size.x - inset * 2 - 1, height = size.y - inset * 2 - 1;
    if (width <= 0 || height <= 0) return;
    graphics.SetBrush(*wxTRANSPARENT_BRUSH);
    if (rounded) {
        // Expand rounded contours outward so the halo follows the banner corners.
        const double step = panel.FromDIP(1);
        for (int ring = 8; ring >= 0; --ring) {
            const double spread = panel.FromDIP(ring);
            const int alpha = static_cast<int>(40 * std::exp(-ring * ring / 18.0) * GlowPulse());
            graphics.SetPen(wxPen(wxColour(accent.Red(),accent.Green(),accent.Blue(),alpha),
                std::max(1,static_cast<int>(std::ceil(step)))));
            graphics.DrawRoundedRectangle(inset-spread,inset-spread,width+spread*2,height+spread*2,
                panel.FromDIP(8)+spread);
        }
        return;
    }
    // Broad translucent layers create a soft halo without a hard neon edge.
    for (int layer = 8; layer >= 1; --layer) {
        const int alpha = static_cast<int>((2 + (8 - layer) * 2) * GlowPulse());
        wxPen pen(wxColour(accent.Red(),accent.Green(),accent.Blue(),alpha),panel.FromDIP(layer * 2));
        pen.SetJoin(wxJOIN_ROUND);
        graphics.SetPen(pen);
        graphics.DrawRectangle(inset,inset,width,height);
    }
}
void AddSoftOutline(wxPanel* panel, wxWindow* content, wxWindow* footer, wxColour fixedAccent = wxNullColour, wxWindow* betaPage = nullptr) {
    struct Cache {
        wxBitmap bitmap;
        wxImage image;
        std::vector<unsigned char> alpha;
        int pulse = -1;
        wxSize size;
        wxPoint origin, contentOrigin, footerOrigin;
        wxColour accent, background;
        int right = 0;
    };
    auto cache = std::make_shared<Cache>();
    panel->SetBackgroundStyle(wxBG_STYLE_PAINT);
    panel->Bind(wxEVT_PAINT,[panel,content,footer,cache,fixedAccent,betaPage](wxPaintEvent&) {
        const wxColour accent = betaPage && betaPage->IsShown() ? GOLD : (fixedAccent.IsOk() ? fixedAccent : RED);
        wxAutoBufferedPaintDC dc(panel);
        const auto size = panel->GetClientSize();
        if (size.x <= 0 || size.y <= 0) return;
        const auto origin = panel->GetPosition(), contentOrigin = content->GetPosition(), footerOrigin = footer->GetPosition();
        const auto background = panel->GetBackgroundColour();
        const int right = panel->GetParent()->GetClientSize().x;
        if (!cache->bitmap.IsOk() || cache->size != size || cache->origin != origin ||
            cache->contentOrigin != contentOrigin || cache->footerOrigin != footerOrigin ||
            cache->accent != accent || cache->background != background || cache->right != right) {
            wxImage image(size.x,size.y);
            image.InitAlpha();
            unsigned char* pixels = image.GetData();
            unsigned char* alpha = image.GetAlpha();
            const double cornerX = contentOrigin.x + 0.5, top = contentOrigin.y + 0.5, bottom = footerOrigin.y + 0.5;
            const double sigma = panel->FromDIP(5), core = std::max(0.65,panel->FromDIP(1)*0.65);
            // Every sibling samples the same continuous path. Minimum distance blends the
            // corner once, avoiding both a seam and a brighter overlapping corner pixel.
            auto distanceSquared = [](double x, double y, double x0, double y0, double x1, double y1) {
                const double dx = std::max({x0-x,0.0,x-x1});
                const double dy = std::max({y0-y,0.0,y-y1});
                return dx*dx+dy*dy;
            };
            for (int y = 0; y < size.y; ++y) {
                for (int x = 0; x < size.x; ++x) {
                    const double px = origin.x+x+0.5, py = origin.y+y+0.5;
                    const double topDistance = distanceSquared(px,py,cornerX,top,right,top);
                    const double sideDistance = distanceSquared(px,py,cornerX,top,cornerX,bottom);
                    const double footerDistance = distanceSquared(px,py,0,bottom,cornerX,bottom);
                    const double d2 = std::min({topDistance,sideDistance,footerDistance});
                    // The header and sidebar occlude the light. Only their exposed
                    // underside lights the content and the existing lower footer halo.
                    const bool covered = py < top || (px < cornerX && py < bottom);
                    const double opacity = covered || d2 > sigma*sigma*9 ? 0 :
                        0.26*std::exp(-d2/(2*core*core)) + 0.20*std::exp(-d2/(2*sigma*sigma));
                    *pixels++ = accent.Red(); *pixels++ = accent.Green(); *pixels++ = accent.Blue();
                    *alpha++ = static_cast<unsigned char>(255*opacity);
                }
            }
            cache->image = image;
            cache->alpha.assign(image.GetAlpha(),image.GetAlpha()+static_cast<size_t>(size.x)*size.y);
            cache->pulse = -1;
            cache->size = size; cache->origin = origin;
            cache->contentOrigin = contentOrigin; cache->footerOrigin = footerOrigin;
            cache->accent = accent; cache->background = background; cache->right = right;
        }
        // Windows GDI+ ignores BeginLayer opacity; animate the bitmap alpha itself.
        const int pulse = static_cast<int>(255*GlowPulse());
        if (cache->pulse != pulse) {
            unsigned char* alpha = cache->image.GetAlpha();
            for (size_t pixel = 0; pixel < cache->alpha.size(); ++pixel)
                alpha[pixel] = static_cast<unsigned char>((cache->alpha[pixel]*pulse+127)/255);
            cache->bitmap = wxBitmap(cache->image);
            cache->pulse = pulse;
        }
        dc.SetBackground(wxBrush(background)); dc.Clear();
        std::unique_ptr<wxGraphicsContext> graphics(wxGraphicsContext::Create(dc));
        if (graphics) {
            graphics->DrawBitmap(cache->bitmap,0,0,size.x,size.y);
        }
    });
    panel->Bind(wxEVT_SIZE,[panel](wxSizeEvent& event) { panel->Refresh(false); event.Skip(); });
}
class GlowPanel : public wxPanel {
    bool rounded;
public:
    explicit GlowPanel(wxWindow* parent, bool roundCorners) : wxPanel(parent), rounded(roundCorners) {
        SetBackgroundColour(BG);
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        Bind(wxEVT_PAINT,[this](wxPaintEvent&) {
            wxAutoBufferedPaintDC dc(this);
            dc.SetBackground(wxBrush(GetBackgroundColour())); dc.Clear();
            std::unique_ptr<wxGraphicsContext> graphics(wxGraphicsContext::Create(dc));
            if (graphics) DrawPanelGlow(*graphics,*this,rounded);
        });
        Bind(wxEVT_SIZE,[this](wxSizeEvent& event) { Refresh(false); event.Skip(); });
    }
};
wxWindow* WithGlowBehind(wxWindow* panel, bool rounded = false) {
    auto* backdrop = new GlowPanel(panel->GetParent(),rounded);
    panel->Reparent(backdrop);
    auto* layout = new wxBoxSizer(wxVERTICAL);
    layout->Add(panel,1,wxEXPAND | wxALL,backdrop->FromDIP(8));
    backdrop->SetSizer(layout);
    return backdrop;
}
class Hero : public wxPanel {
    wxBitmap art;
    bool closedBeta;
public:
    Hero(wxWindow* p, bool beta = false) : wxPanel(p, wxID_ANY), closedBeta(beta) {
        if (closedBeta) {
            SetCursor(wxCURSOR_HAND);
            SetToolTip("Apply for Beta / open the Discord ticket channel");
            SetName("Apply for Beta");
            auto openTickets = [this]() {
                if (!wxLaunchDefaultBrowser("https://discord.com/channels/1559004140288614413/1564990133370097784"))
                    wxMessageBox("The Discord ticket channel could not be opened.","Rush / Apply for Beta",wxOK|wxICON_ERROR,this);
            };
            Bind(wxEVT_LEFT_UP,[openTickets](wxMouseEvent&) { openTickets(); });
            Bind(wxEVT_KEY_DOWN,[openTickets](wxKeyEvent& event) {
                if (event.GetKeyCode()==WXK_RETURN || event.GetKeyCode()==WXK_SPACE) openTickets();
                else event.Skip();
            });
        }
        SetMinSize(FromDIP(wxSize(-1, 201))); SetBackgroundStyle(wxBG_STYLE_PAINT);
        HRSRC resource = FindResourceW(nullptr, MAKEINTRESOURCEW(201), RT_RCDATA);
        if (resource) {
            HGLOBAL data = LoadResource(nullptr, resource);
            wxMemoryInputStream stream(LockResource(data), SizeofResource(nullptr, resource));
            art = wxBitmap(wxImage(stream, wxBITMAP_TYPE_PNG));
        }
        Bind(wxEVT_PAINT, [this](wxPaintEvent&) {
            wxAutoBufferedPaintDC dc(this); dc.SetBackground(wxBrush(BG)); dc.Clear();
            std::unique_ptr<wxGraphicsContext> g(wxGraphicsContext::Create(dc)); if (!g) return;
            smoothText(*g);
            const auto full = GetClientSize();
            const int inset = FromDIP(8), radius = FromDIP(8);
            const wxSize s(full.x-inset*2,full.y-inset*2);
            if (s.x <= 0 || s.y <= 0) return;
            // Paint halo and face on one surface: no opaque child corners can hide the glow.
            DrawPanelGlow(*g,*this,true,closedBeta ? GOLD : RED);
            g->PushState();
            wxRegion face(reinterpret_cast<WXHRGN>(CreateRoundRectRgn(inset,inset,
                inset+s.x,inset+s.y,radius*2,radius*2)));
            g->Clip(face);
            g->Translate(inset,inset);
            g->SetPen(*wxTRANSPARENT_PEN); g->SetBrush(wxBrush(BG));
            g->DrawRectangle(0,0,s.x,s.y);
            if (art.IsOk()) {
                const double h = static_cast<double>(s.x) * art.GetHeight()/art.GetWidth();
                g->DrawBitmap(art,0,(s.y-h)/2,s.x,h);
            }
            g->SetPen(*wxTRANSPARENT_PEN); g->SetBrush(wxBrush(wxColour(9,10,15,160))); g->DrawRectangle(0,0,s.x,s.y);
            g->SetBrush(g->CreateLinearGradientBrush(0,0,s.x,0,wxColour(12,13,19,250),wxColour(12,13,19,20)));
            g->DrawRectangle(0,0,s.x,s.y);
            g->SetFont(font(9,true), closedBeta ? GOLD : RED); g->DrawText("MINECRAFT BEDROCK / WINDOWS",FromDIP(26),FromDIP(28));
            if (closedBeta) {
                g->SetFont(font(27,true),FOREGROUND);
                g->DrawText("Apply for Beta",FromDIP(24),FromDIP(54));
                g->SetFont(font(12),FOREGROUND);
                g->DrawText("Open a ticket on Discord",FromDIP(24),FromDIP(110));
            } else {
                g->SetFont(font(27,true),RED);
                g->DrawText("Rush",FromDIP(24),FromDIP(54));
                double brandWidth = 0, brandHeight = 0;
                g->GetTextExtent("Rush ",&brandWidth,&brandHeight);
                g->SetFont(font(27,true),FOREGROUND);
                g->DrawText("Utility Mod",FromDIP(24)+brandWidth,FromDIP(54));
            }
            g->PopState();
            g->SetPen(wxPen(LINE)); g->SetBrush(*wxTRANSPARENT_BRUSH);
            g->DrawRoundedRectangle(inset,inset,s.x-1,s.y-1,radius);
        });
    }
};
void styleInput(wxTextCtrl* input) {
    input->SetBackgroundColour(wxColour(13,14,20)); input->SetForegroundColour(FOREGROUND); input->SetFont(font(10));
    input->SetMinSize(input->FromDIP(wxSize(-1, 34)));
}
void styleTargetInput(wxTextCtrl* input, bool editable) {
    input->SetEditable(editable);
    input->SetForegroundColour(editable ? wxColour(255,255,255) : wxColour(105,108,122));
    input->SetBackgroundColour(editable ? wxColour(13,14,20) : wxColour(19,20,26));
    input->GetParent()->SetBackgroundColour(input->GetBackgroundColour());
    input->GetParent()->Refresh();
    input->Refresh();
}
wxPanel* centeredInput(wxWindow* parent, const wxString& value, wxTextCtrl*& input) {
    auto* frame = new wxPanel(parent,wxID_ANY,wxDefaultPosition,wxDefaultSize,wxBORDER_SIMPLE);
    frame->SetBackgroundColour(wxColour(13,14,20));
    frame->SetMinSize(frame->FromDIP(wxSize(330,34)));
    input = new wxTextCtrl(frame,wxID_ANY,value,wxDefaultPosition,wxDefaultSize,wxBORDER_NONE | wxTE_LEFT);
    styleInput(input);
    input->SetMinSize(wxSize(-1,input->GetTextExtent("Ag").y+input->FromDIP(2)));
    auto* layout = new wxBoxSizer(wxVERTICAL);
    layout->AddStretchSpacer();
    layout->Add(input,0,wxEXPAND | wxLEFT | wxRIGHT,frame->FromDIP(4));
    layout->AddStretchSpacer();
    frame->SetSizer(layout);
    return frame;
}
}

void styleDarkCaption(HWND window, const wxColour& colour) {
    constexpr DWORD darkMode = 20, captionColour = 35;
    const BOOL dark = TRUE;
    const COLORREF caption = RGB(colour.Red(),colour.Green(),colour.Blue());
    DwmSetWindowAttribute(window,darkMode,&dark,sizeof(dark));
    DwmSetWindowAttribute(window,captionColour,&caption,sizeof(caption));
}

wxCheckBox* CreateRushCheckBox(wxWindow* parent, const wxString& text) {
    auto* checkbox = new RushCheckBox(parent, wxID_ANY, text);
    checkbox->SetForegroundColour(MUTED);
    return checkbox;
}

wxWindow* CreateRushActionButton(wxWindow* parent,const wxString& text,const wxSize& size,bool primary,wxColour accent) {
    return new RushButton(parent,wxID_ANY,text,primary,size,accent);
}
wxBEGIN_EVENT_TABLE(cMain, wxFrame)
EVT_BUTTON(101, OnInjectButton)
EVT_BUTTON(102, OnSelectButton)
EVT_CHECKBOX(201, OnCustomCheckBox)
wxEND_EVENT_TABLE()

cMain::cMain() : wxFrame(nullptr, wxID_ANY, "Rush Injector", wxDefaultPosition, wxDefaultSize,
    wxDEFAULT_FRAME_STYLE & ~(wxRESIZE_BORDER | wxMAXIMIZE_BOX)), clientUpdateTimer(this), processTimer(this), glowTimer(this) {
    const HWND window = static_cast<HWND>(GetHandle());
    styleDarkCaption(window,BG);
    constexpr DWORD cornerPreference = 33, borderColour = 34;
    const int roundCorners = 2;
    const COLORREF border = RGB(LINE.Red(),LINE.Green(),LINE.Blue());
    DwmSetWindowAttribute(window,cornerPreference,&roundCorners,sizeof(roundCorners));
    DwmSetWindowAttribute(window,borderColour,&border,sizeof(border));
    // Recalculate the frame now that MSWWindowProc can remove it; size the client afterwards.
    SetWindowPos(window,nullptr,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOZORDER|SWP_NOACTIVATE|SWP_FRAMECHANGED);
    SetClientSize(FromDIP(wxSize(1040, 690)));
    SetMinSize(GetSize()); SetMaxSize(GetSize());
    SetBackgroundColour(BG); SetFont(font(10));
    if (!cfg.serializeConfig()) cfg.updateConfigFile();
    RED = themeColour();
    SetFont(font(10));
    wxIcon icon;
    icon.CreateFromHICON(CopyIcon(LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(101))));
    SetIcon(icon);
    mainPanel = new wxPanel(this); mainPanel->SetBackgroundColour(BG);
    auto* root = new wxBoxSizer(wxVERTICAL);
    auto* top = new wxPanel(mainPanel); top->SetBackgroundColour(wxColour(14,14,20));
    auto* topRow = new wxBoxSizer(wxHORIZONTAL);
    topRow->Add(label(top,"Rush",24,RED,true),0,wxALIGN_CENTER_VERTICAL | wxLEFT,FromDIP(25));
    topRow->Add(label(top," Injector",24,FOREGROUND,true),0,wxALIGN_CENTER_VERTICAL);
    topRow->AddStretchSpacer();
    auto* developer = new DeveloperIconButton(top);
    HRSRC developerResource=FindResourceW(nullptr,MAKEINTRESOURCEW(210),RT_RCDATA);
    if(developerResource) {
        HGLOBAL data=LoadResource(nullptr,developerResource);
        wxMemoryInputStream stream(LockResource(data),SizeofResource(nullptr,developerResource));
        wxImage image(stream,wxBITMAP_TYPE_PNG);
        const int iconSize=FromDIP(26);
        if(image.IsOk()) {
            // Keep the PNG's alpha coverage while tinting the glyph for the dark header.
            for(int y=0;y<image.GetHeight();++y) for(int x=0;x<image.GetWidth();++x) {
                image.SetRGB(x,y,FOREGROUND.Red(),FOREGROUND.Green(),FOREGROUND.Blue());
            }
            developer->SetBitmap(wxBitmap(image.Scale(iconSize,iconSize,wxIMAGE_QUALITY_HIGH)));
        }
    }
    developer->SetToolTip("Developer panel");
    auto* edition = new wxBoxSizer(wxHORIZONTAL);
    edition->Add(developer,0,wxALIGN_CENTER_VERTICAL|wxRIGHT,FromDIP(10));
    edition->Add(label(top,"BEDROCK EDITION",9,MUTED,true),0,wxALIGN_CENTER_VERTICAL);
    auto* minimize = new WindowButton(top,WindowButton::Minimize);
    auto* close = new WindowButton(top,WindowButton::Close);
    minimize->Bind(wxEVT_BUTTON,[this](wxCommandEvent&) { Iconize(true); });
    close->Bind(wxEVT_BUTTON,[this](wxCommandEvent&) { Close(); });
    auto* windowButtons = new wxBoxSizer(wxHORIZONTAL);
    windowButtons->Add(minimize); windowButtons->Add(close);
    // Window buttons sit in the corner; the edition line lines up with the hero's right edge.
    auto* headerRight = new wxBoxSizer(wxVERTICAL);
    headerRight->Add(windowButtons,0,wxALIGN_RIGHT);
    headerRight->Add(edition,0,wxALIGN_RIGHT | wxRIGHT,FromDIP(24));
    topRow->Add(headerRight,0,wxEXPAND);
    top->SetSizer(topRow); root->Add(top,0,wxEXPAND); top->SetMinSize(FromDIP(wxSize(-1,72)));
    // Child windows swallow hit tests, so hand header clicks to the native move loop.
    auto dragWindow = [window](wxMouseEvent&) {
        const wxPoint mouse = wxGetMousePosition();
        ReleaseCapture();
        SendMessageW(window,WM_NCLBUTTONDOWN,HTCAPTION,MAKELPARAM(mouse.x,mouse.y));
    };
    auto showSystemMenu = [window](wxMouseEvent&) {
        const wxPoint mouse = wxGetMousePosition();
        const HMENU menu = GetSystemMenu(window,FALSE);
        for (const UINT item : {SC_RESTORE,SC_SIZE,SC_MAXIMIZE}) EnableMenuItem(menu,item,MF_BYCOMMAND|MF_GRAYED);
        const int command = TrackPopupMenu(menu,TPM_RETURNCMD|TPM_RIGHTBUTTON,mouse.x,mouse.y,0,window,nullptr);
        if (command) PostMessageW(window,WM_SYSCOMMAND,command,0);
    };
    std::vector<wxWindow*> dragAreas{top};
    for (wxWindow* child : top->GetChildren()) if (dynamic_cast<wxStaticText*>(child)) dragAreas.push_back(child);
    for (wxWindow* area : dragAreas) {
        area->Bind(wxEVT_LEFT_DOWN,dragWindow); area->Bind(wxEVT_LEFT_DCLICK,dragWindow);
        area->Bind(wxEVT_RIGHT_UP,showSystemMenu);
    }
    auto* body = new wxBoxSizer(wxHORIZONTAL);
    auto* sidebar = new wxPanel(mainPanel); sidebar->SetBackgroundColour(wxColour(14,14,20)); sidebar->SetMinSize(FromDIP(wxSize(180,-1)));
    auto* nav = new wxBoxSizer(wxVERTICAL);
    nav->AddSpacer(FromDIP(20));
    auto* current = new RushButton(sidebar,wxID_ANY,"Injector",true); nav->Add(current,0,wxEXPAND | wxLEFT | wxRIGHT,FromDIP(14));
    auto* folder = new RushButton(sidebar,wxID_ANY,"Open DLL folder"); nav->Add(folder,0,wxEXPAND | wxALL,FromDIP(14));
    folder->Bind(wxEVT_BUTTON,[this](wxCommandEvent&) {
        wxString selected = txt_DllPath->GetValue().Strip(wxString::both);
        if (selected.StartsWith("\"") && selected.EndsWith("\"")) selected = selected.Mid(1, selected.length() - 2);
        wxFileName file(selected);
        wxString directory;
        if (!selected.empty() && file.FileExists() && file.MakeAbsolute()) {
            directory = file.GetPath();
        } else {
            directory = wxFileName(wxStandardPaths::Get().GetExecutablePath()).GetPath() + "\\DLLs";
            if (!wxDirExists(directory) && !wxFileName::Mkdir(directory, wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL)) {
                wxMessageBox("The DLLs folder could not be created:\n" + directory,
                    "Rush Injector / Folder error", wxOK | wxICON_ERROR, this);
                return;
            }
        }
        const INT_PTR result = reinterpret_cast<INT_PTR>(ShellExecuteW(
            static_cast<HWND>(GetHandle()), L"open", directory.wc_str(), nullptr, nullptr, SW_SHOWNORMAL));
        if (result <= 32) {
            SetStatusText("Opening the DLL folder failed / Windows error " + std::to_string(result));
            wxMessageBox("Windows could not open this folder:\n" + directory,
                "Rush Injector / Folder error", wxOK | wxICON_ERROR, this);
        } else {
            SetStatusText("Opened DLL folder / " + directory);
        }
    });
    auto* theme = new RushButton(sidebar,wxID_ANY,"Theme"); nav->Add(theme,0,wxEXPAND | wxLEFT | wxRIGHT,FromDIP(14));
    auto* failureLog = new RushButton(sidebar,wxID_ANY,"Failure log");
    nav->Add(failureLog,0,wxEXPAND | wxLEFT | wxRIGHT | wxTOP,FromDIP(14));
    failureLog->Bind(wxEVT_BUTTON,[this](wxCommandEvent&) {
        const wxString path=FailureLogPath();
        if (!wxFileName::FileExists(path)) { SetStatusText("No failures have been logged yet."); return; }
        if (!wxLaunchDefaultApplication(path)) SetStatusText("Could not open failure.log in your text editor.");
    });
    theme->Bind(wxEVT_BUTTON,[this](wxCommandEvent&) { OpenThemeMenu(); });
    auto* beta = new RushButton(sidebar,wxID_ANY,"Beta...",false,wxSize(-1,42),GOLD);
    nav->Add(beta,0,wxEXPAND | wxLEFT | wxRIGHT | wxTOP,FromDIP(14));
    nav->AddStretchSpacer();
    auto* update = new RushButton(sidebar,wxID_ANY,"Update",true,wxSize(-1,42),wxColour(65,65,74));
    nav->Add(update,0,wxEXPAND | wxLEFT | wxRIGHT | wxTOP,FromDIP(14));
    update->SetToolTip("Checking for injector updates...");
    Bind(EVT_RUSH_UPDATE_CHECKED,[this,update](wxThreadEvent& event) {
        const int result = event.GetInt();
        update->SetAccent(result == 1 ? RED : wxColour(65,65,74),true);
        update->SetLabel(result == 1 ? "Update (1)" : "Update");
        injectorUpdateAvailable = result == 1;
        RefreshUpdatePing();
        update->SetToolTip(result == 1 ? "A new injector update is available." :
            result == 0 ? "Rush Injector is up to date." : "Update check unavailable. Click to try again.");
    });
    updateCheckThread = std::jthread([this](std::stop_token stop) {
        const auto available = CheckInjectorUpdate();
        if (stop.stop_requested()) return;
        auto* event = new wxThreadEvent(EVT_RUSH_UPDATE_CHECKED);
        event->SetInt(available ? (*available ? 1 : 0) : -1);
        wxQueueEvent(this,event);
    });
    update->Bind(wxEVT_BUTTON,[this](wxCommandEvent&) {
        if (downloading) {
            downloadThread.request_stop();
            SetAutoInject(false);
            btn_AutoInject->Disable();
            SetStatusText("Canceling the client download. Click Update again when cancellation finishes."); return;
        }
        if (injectionBusy) {
            SetStatusText("A client load is still running. Wait for its result, then click Update again."); return;
        }
        SetAutoInject(false);
        HMODULE module = GetModuleHandleW(nullptr);
        HRSRC resource = FindResourceW(module,MAKEINTRESOURCEW(209),RT_RCDATA);
        HGLOBAL loaded = resource ? LoadResource(module,resource) : nullptr;
        const void* bytes = loaded ? LockResource(loaded) : nullptr;
        const DWORD size = resource ? SizeofResource(module,resource) : 0;
        wxString temporary = wxFileName::CreateTempFileName("RushUpdater-");
        if (temporary.empty() || !wxRenameFile(temporary,temporary + ".ps1")) {
            SetStatusText("The updater temporary file could not be created."); return;
        }
        temporary += ".ps1";
        wxFile script(temporary,wxFile::write);
        if (!bytes || !size || !script.IsOpened() || script.Write(bytes,size) != size || !script.Close()) {
            wxRemoveFile(temporary); SetStatusText("The updater could not be prepared."); return;
        }
        auto quote = [](wxString value) { value.Replace("'","''"); return "'" + value + "'"; };
        const wxString command = "& " + quote(temporary) + " -ParentId " + std::to_string(GetCurrentProcessId()) +
            " -Target " + quote(wxStandardPaths::Get().GetExecutablePath());
        // UTF-16 Base64 avoids shell interpretation of executable and temporary paths.
        const wxString encoded = wxBase64Encode(command.wc_str(),command.length()*sizeof(wchar_t));
        wchar_t systemDirectory[MAX_PATH]{};
        GetSystemDirectoryW(systemDirectory,MAX_PATH);
        const wxString powershell = wxString(systemDirectory) + "\\WindowsPowerShell\\v1.0\\powershell.exe";
        const wxString arguments = "-NoProfile -NonInteractive -ExecutionPolicy Bypass -EncodedCommand " + encoded;
        const auto result = reinterpret_cast<INT_PTR>(ShellExecuteW(static_cast<HWND>(GetHandle()),L"open",
            powershell.wc_str(),arguments.wc_str(),nullptr,SW_HIDE));
        if (result <= 32) { wxRemoveFile(temporary); SetStatusText("Windows could not start the updater."); }
        else SetStatusText("Checking Rush Injector updates / see the update dialog.");
    });
    auto* discord = new RushButton(sidebar,wxID_ANY,"Discord"); nav->Add(discord,0,wxEXPAND | wxALL,FromDIP(14));
    discord->Bind(wxEVT_BUTTON,[this](wxCommandEvent&) {
        if (!wxLaunchDefaultBrowser("https://discord.gg/sBrCxRZWKA"))
            SetStatusText("The Discord link could not be opened.");
    });
    nav->Add(label(sidebar,"Rush " RUSH_INJECTOR_VERSION,8,MUTED),0,wxLEFT | wxBOTTOM,FromDIP(22)); sidebar->SetSizer(nav);
    body->Add(sidebar,0,wxEXPAND);
    auto* content = new wxPanel(mainPanel); content->SetBackgroundColour(BG);
    auto* column = new wxBoxSizer(wxVERTICAL);
    column->Add(new Hero(content),0,wxEXPAND | wxALL,FromDIP(16));
    auto* heading = new wxBoxSizer(wxHORIZONTAL);
    heading->Add(label(content,"Inject client",17,FOREGROUND,true));
    column->Add(heading,0,wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM,FromDIP(26));
    auto* card = new wxPanel(content); card->SetBackgroundColour(CARD);
    auto* form = new wxBoxSizer(wxVERTICAL);
    auto* targetRow = new wxBoxSizer(wxHORIZONTAL);
    targetRow->Add(label(card,"TARGET PROCESS",8,MUTED,true)); targetRow->AddStretchSpacer();
    processStatus = label(card,"Checking game...",9,MUTED); targetRow->Add(processStatus);
    form->Add(targetRow,0,wxEXPAND | wxALL,FromDIP(18));
    auto* inputArea = new wxBoxSizer(wxHORIZONTAL);
    auto* inputFields = new wxBoxSizer(wxVERTICAL);
    inputFields->Add(centeredInput(card,Globals::PROC_NAME,txt_ProcName),0,wxEXPAND);
    check_CustomTarget = new RushCheckBox(card,201,"Custom target"); check_CustomTarget->SetForegroundColour(MUTED); check_CustomTarget->SetValue(Globals::USE_CUSTOM_PROC_NAME);
    styleTargetInput(txt_ProcName,Globals::USE_CUSTOM_PROC_NAME);
    inputFields->Add(label(card,"CLIENT DLL",8,MUTED,true),0,wxTOP | wxBOTTOM,FromDIP(18));
    inputFields->Add(centeredInput(card,Globals::DLL_PATH,txt_DllPath),0,wxEXPAND);
    txt_DllPath->SetHint("Choose your x64 client DLL...");
    auto* actionButtons = new wxBoxSizer(wxVERTICAL);
    auto* autoButton = new RushButton(card,wxID_ANY,"Auto Inject: OFF",false,wxSize(220,50));
    autoButton->SetFont(font(12,true));
    btn_AutoInject = autoButton;
    actionButtons->Add(autoButton,0,wxBOTTOM,FromDIP(12));
    autoButton->Bind(wxEVT_BUTTON,[this](wxCommandEvent&) { ToggleAutoInject(); });
    autoButton->SetToolTip("Download the Rush client and inject when Minecraft starts.");
    btn_Inject = new RushButton(card,101,"Inject client  >",true,wxSize(220,50));
    btn_Inject->SetFont(font(12,true)); actionButtons->Add(btn_Inject);
    auto* rightControls = new wxBoxSizer(wxVERTICAL);
    auto* customRow = new wxBoxSizer(wxHORIZONTAL);
    customRow->SetMinSize(FromDIP(wxSize(125,34)));
    customRow->AddStretchSpacer();
    customRow->Add(check_CustomTarget,0,wxALIGN_CENTER_VERTICAL);
    customRow->AddStretchSpacer();
    rightControls->Add(customRow,0,wxEXPAND); rightControls->AddStretchSpacer();
    btn_Select = new RushButton(card,102,"Choose DLL",false,wxSize(125,36)); rightControls->Add(btn_Select,0,wxALIGN_CENTER_HORIZONTAL);
    inputArea->Add(inputFields,1,wxEXPAND);
    inputArea->Add(rightControls,0,wxEXPAND | wxLEFT,FromDIP(16));
    inputArea->Add(actionButtons,0,wxALIGN_CENTER_VERTICAL | wxLEFT,FromDIP(18));
    form->Add(inputArea,0,wxEXPAND | wxLEFT | wxRIGHT,FromDIP(18));
    selectionStatus = label(card,wxEmptyString,9,MUTED);
    auto* detailsRow = new wxBoxSizer(wxHORIZONTAL);
    detailsRow->Add(selectionStatus,1,wxALIGN_CENTER_VERTICAL);
    detailsRow->Add(label(card,"WINDOWS x64  /  CLIENT DLL",8,MUTED),0,wxALIGN_CENTER_VERTICAL | wxLEFT,FromDIP(12));
    form->Add(detailsRow,0,wxEXPAND | wxALL,FromDIP(18));
    card->SetSizer(form); column->Add(WithGlowBehind(card),0,wxEXPAND | wxLEFT | wxRIGHT,FromDIP(24)); column->AddStretchSpacer();
    content->SetSizer(column); body->Add(content,1,wxEXPAND);
    auto* betaPage = new wxPanel(mainPanel); betaPage->SetBackgroundColour(BG);
    auto* betaLayout = new wxBoxSizer(wxVERTICAL);
    betaLayout->Add(new Hero(betaPage,true),0,wxEXPAND | wxALL,FromDIP(16));
    auto* betaInjectButton=new RushButton(betaPage,wxID_ANY,"Inject Beta",true,wxSize(220,44),GOLD);
    betaLayout->Add(CreateBetaLogin(betaPage,[this](){InjectBeta();},betaInjectButton),0,wxEXPAND|wxLEFT|wxRIGHT|wxBOTTOM,FromDIP(28));
    betaLayout->Add(betaInjectButton,0,wxALIGN_CENTER_HORIZONTAL|wxBOTTOM,FromDIP(24));
    betaLayout->AddStretchSpacer();
    auto* coffee = new RushButton(betaPage,wxID_ANY,"Buy the devs a coffee",false,wxSize(220,44),GOLD);
    betaLayout->Add(coffee,0,wxALIGN_RIGHT|wxRIGHT|wxBOTTOM,FromDIP(16));
    coffee->Bind(wxEVT_BUTTON,[this](wxCommandEvent&) {
        if (!wxLaunchDefaultBrowser("https://buymeacoffee.com/rushclient"))
            SetStatusText("The Buy me a coffee link could not be opened.");
    });
    betaPage->SetSizer(betaLayout); body->Add(betaPage,1,wxEXPAND); betaPage->Hide();
    auto* developerPage=CreateDeveloperPanel(mainPanel);body->Add(developerPage,1,wxEXPAND);developerPage->Hide();
    auto showPage = [this,content,betaPage,current,beta,developerPage](bool showBeta) {
        mainPanel->Freeze();
        developerPage->Hide();
        content->Show(!showBeta); betaPage->Show(showBeta);
        current->SetPrimary(!showBeta); beta->SetPrimary(showBeta);
        mainPanel->Layout(); mainPanel->Thaw();
    };
    beta->Bind(wxEVT_BUTTON,[showPage](wxCommandEvent&) { showPage(true); });
    current->Bind(wxEVT_BUTTON,[this,showPage](wxCommandEvent&) { showPage(false); txt_DllPath->SetFocus(); });
    developer->Bind(wxEVT_BUTTON,[this,content,betaPage,developerPage,current,beta](wxCommandEvent&){
        mainPanel->Freeze();content->Hide();betaPage->Hide();developerPage->Show();current->SetPrimary(false);beta->SetPrimary(false);mainPanel->Layout();mainPanel->Thaw();
    });
    root->Add(body,1,wxEXPAND); mainPanel->SetSizer(root);
    auto* footer = new wxPanel(mainPanel); footer->SetBackgroundColour(wxColour(14,14,20));
    // Paint matching slices of one divider across the header, sidebar, content and footer.
    AddSoftOutline(content,content,footer);
    AddSoftOutline(betaPage,betaPage,footer,GOLD);
    AddSoftOutline(developerPage,developerPage,footer);
    AddSoftOutline(footer,content,footer,wxNullColour,betaPage);
    auto* footerRow = new wxBoxSizer(wxHORIZONTAL);
    statusText = label(footer,wxEmptyString,9,MUTED);
    footerRow->Add(statusText,1,wxALIGN_CENTER_VERTICAL | wxLEFT | wxRIGHT,FromDIP(22));
    auto* footerLayout = new wxBoxSizer(wxVERTICAL);
    footerLayout->AddSpacer(FromDIP(18));
    footerLayout->Add(footerRow,1,wxEXPAND | wxBOTTOM,FromDIP(6));
    footer->SetSizer(footerLayout); footer->SetMinSize(FromDIP(wxSize(-1,50))); root->Add(footer,0,wxEXPAND);
    openDialog = new wxFileDialog(this,"Select client DLL",wxEmptyString,wxEmptyString,"Dynamic link library (*.dll)|*.dll",wxFD_OPEN | wxFD_FILE_MUST_EXIST);
    Bind(EVT_RUSH_DOWNLOAD_PROGRESS,[this](wxThreadEvent& event) {
        if (downloading && !downloadThread.get_stop_token().stop_requested())
            SetStatusText(wxString::Format("Downloading Rush client... %.1f MB",event.GetPayload<size_t>() / (1024.0 * 1024.0)));
    });
    Bind(EVT_RUSH_DOWNLOAD_FINISHED,[this](wxThreadEvent& event) { FinishDownload(event.GetPayload<ClientDownloadResult>()); });
    Bind(EVT_RUSH_DOWNLOAD_DIAGNOSTIC,[this](wxThreadEvent& event) {
        const auto diagnostic=event.GetPayload<std::pair<ClientDownloadResult,unsigned>>();
        LogFailure(wxString::Format("Download attempt %u",diagnostic.second),diagnostic.first.error,
            diagnostic.first.httpStatus,diagnostic.first.requestId,diagnostic.first.category);
        if (downloading && RetryableClientFailure(diagnostic.first) && diagnostic.second<3)
            SetStatusText(wxString::Format("Temporary download failure / retrying (%u/3)...",diagnostic.second+1));
    });
    txt_DllPath->Bind(wxEVT_TEXT,[this](wxCommandEvent& event) { RefreshStatus(); event.Skip(); });
    Bind(wxEVT_TIMER,[this](wxTimerEvent&) { RefreshProcess(); },processTimer.GetId());
    Bind(wxEVT_TIMER,[this,content,betaPage,developerPage,footer](wxTimerEvent&) {
        if (IsIconized() || !IsShown()) return;
        std::function<void(wxWindow*)> refresh = [&](wxWindow* window) {
            if (!window->IsShown()) return;
            if (window == content || window == betaPage || window == developerPage || window == footer ||
                dynamic_cast<Hero*>(window) || dynamic_cast<GlowPanel*>(window)) {
                // Only invalidate exposed background: controls and labels do not animate.
                wxRegion exposed(window->GetClientRect());
                for (wxWindow* child : window->GetChildren())
                    if (child->IsShown()) exposed.Subtract(child->GetRect());
                for (wxRegionIterator part(exposed); part; ++part) {
                    const wxRect rectangle = part.GetRect(); window->Refresh(false,&rectangle);
                }
            }
            for (wxWindow* child : window->GetChildren()) refresh(child);
        };
        refresh(mainPanel);
    },glowTimer.GetId());
    glowTimer.Start(33);
    Bind(EVT_RUSH_CLIENT_CHECKED,[this](wxThreadEvent& event) {
        clientCheckBusy = false;
        const auto result = event.GetPayload<std::pair<std::wstring,std::wstring>>();
        if (result.first != Globals::DOWNLOAD_URL || result.second.empty()) return;
        clientUpdateAvailable = Globals::CLIENT_DIGEST_ENDPOINT == result.first &&
            !Globals::CLIENT_DIGEST.empty() && Globals::CLIENT_DIGEST != result.second;
        RefreshUpdatePing();
    });
    Bind(wxEVT_TIMER,[this](wxTimerEvent&) { CheckClientUpdate(); },clientUpdateTimer.GetId());
    clientUpdateTimer.Start(300000);
    CheckClientUpdate();
    processTimer.Start(2000); RefreshProcess(); Centre();
}
WXLRESULT cMain::MSWWindowProc(WXUINT message, WXWPARAM wParam, WXLPARAM lParam) {
    // Keeping the captioned style preserves the shadow, animations and snapping;
    // claiming the whole window as client area is what hides the native title bar.
    if (message == WM_NCCALCSIZE && wParam) return 0;
    return wxFrame::MSWWindowProc(message,wParam,lParam);
}
cMain::~cMain() {
    clientUpdateTimer.Stop();
    if (clientCheckThread.joinable()) { clientCheckThread.request_stop(); clientCheckThread.join(); }
    glowTimer.Stop();
    processTimer.Stop();
    if (updateCheckThread.joinable()) { updateCheckThread.request_stop(); updateCheckThread.join(); }
    if (downloadThread.joinable()) { downloadThread.request_stop(); downloadThread.join(); }
    openDialog->Destroy();
}
void cMain::SetStatusText(const wxString& text, int) {
    if (statusText) { statusText->SetLabel(text); statusText->GetParent()->Layout(); }
}
void cMain::RefreshProcess() {
    DWORD pid = GetProcId(txt_ProcName->GetValue().wc_str());
    if (pid != gamePid) {
        // Footer messages describe the previous game session once the process changes.
        if (!injectionBusy) SetStatusText(wxEmptyString);
        gamePid = pid; loadedPid = 0;
    }
    RefreshStatus();
    if (!IsEnabled() || injectionBusy) return;
    const std::wstring& path = automaticDllPath;
    if (autoInject.Observe(pid, wxFileName::FileExists(path) ? path : std::wstring{})) {
        wxCommandEvent event;
        OnInjectButtonExecute(event, *this, false);
    }
}
void cMain::RefreshStatus() {
    const bool loaded = gamePid && gamePid == loadedPid;
    const wxString pid = wxString::Format("PID %lu",gamePid);
    const wxString game = loaded ? "Client loaded / " + pid : gamePid ? "Game detected / " + pid : wxString("Waiting for game");
    const wxString path = txt_DllPath->GetValue();
    wxString next;
    if (loaded) next = "Client is running in Minecraft.";
    else if (downloading) next = "Downloading client...";
    else if (autoInject.Enabled()) next = gamePid ? "Auto Inject on / preparing to load." : "Auto Inject on / waiting for Minecraft.";
    else if (!wxFileName::FileExists(path)) next = "Choose a client DLL or turn on Auto Inject.";
    else if (!gamePid) next = wxFileName(path).GetFullName() + " / Launch Minecraft to inject.";
    else next = wxFileName(path).GetFullName() + " / Ready to inject.";
    if (processStatus->GetLabel() == game && selectionStatus->GetLabel() == next) return;
    processStatus->SetLabel(game); selectionStatus->SetLabel(next);
    processStatus->SetForegroundColour(gamePid ? wxColour(104,210,156) : MUTED);
    processStatus->GetParent()->Layout();
}
void cMain::updateGlobalVars() {
    Globals::USE_CUSTOM_PROC_NAME = check_CustomTarget->GetValue();
    Globals::DLL_PATH = txt_DllPath->GetValue().ToStdWstring();
    Globals::PROC_NAME = txt_ProcName->GetValue().ToStdWstring();
}
void cMain::OnInjectButton(wxCommandEvent& evt) { OnInjectButtonExecute(evt,*this); }
void cMain::OnInjectButtonExecute(wxCommandEvent&, cMain& ref, bool interactive) {
    if (ref.injectionBusy) return;
    if (interactive && ref.autoInject.Enabled()) return;
    DWORD pid = GetProcId(ref.txt_ProcName->GetValue().wc_str());
    if (!pid) { LogFailure("Game not found",ERROR_NOT_FOUND); ref.SetStatusText("Game not found. Launch Minecraft, then try again."); return; }
    std::wstring path = interactive ? ref.txt_DllPath->GetValue().ToStdWstring() : ref.automaticDllPath;
    if (!wxFileName::FileExists(path)) { LogFailure("DLL file missing",ERROR_FILE_NOT_FOUND); ref.SetStatusText("Select an existing DLL file first."); return; }
    // Hold the verified file through permission preparation and the loader call.
    VerifiedClientFile verified(interactive ? L"" : path, interactive ? L"" : ref.automaticDllDigest);
    if (!interactive && !verified.Valid()) {
        LogFailure("Downloaded DLL integrity check",ERROR_INVALID_DATA);
        ref.SetAutoInject(false);
        ref.automaticDllPath.clear(); ref.automaticDllDigest.clear();
        ref.SetStatusText("Client was changed or could not be verified. Download it again before loading.");
        return;
    }
    wxString fontError;
    if (!StageClientFonts(path, fontError)) {
        LogFailure("Injection font preparation",ERROR_WRITE_FAULT);
        ref.SetStatusText(fontError);
        if (interactive) wxMessageBox(fontError,"Rush client fonts",wxOK | wxICON_ERROR,&ref);
        return;
    }
    ref.autoInject.RecordAttempt(pid, path);
    ref.injectionBusy = true;
    ref.btn_Inject->Disable(); ref.btn_AutoInject->Disable(); ref.SetStatusText("Loading client..."); ref.Update();
    const wchar_t* failureStage=L"DLL permission preparation";
    DWORD result = SetAccessControl(path,L"S-1-15-2-1",&failureStage);
    if (!result) { wxBusyCursor busy; result = performInjection(pid,path.c_str(),&failureStage); }
    ref.injectionBusy = false;
    ref.btn_Inject->Enable(!ref.autoInject.Enabled()); ref.btn_AutoInject->Enable();
    if (result) {
        const wxString channel=interactive ? "Manual injection" : ref.loadedBeta ? "Beta injection" : "Auto injection";
        const bool processAccessDenied = result == ERROR_ACCESS_DENIED &&
            std::wstring(failureStage) == L"OpenProcess / Minecraft process access";
        wxString context = channel+" / "+wxString(failureStage)+wxString::Format(" / PID=%lu",pid);
        context += " | " + wxString(DescribeProcessAccess(pid));
        context += " | " + wxString(DescribeProcessCandidates(ref.txt_ProcName->GetValue().wc_str()));
        LogFailure(context,result);
        ref.SetStatusText("Injection failed / Windows error " + std::to_string(result));
        if (!interactive) {
            if (processAccessDenied)
                ref.SetStatusText("Minecraft access denied / open Failure log for diagnostics.");
            else ref.SetStatusText("Auto Inject failed / Windows error " + std::to_string(result) + " / toggle Auto Inject to retry.");
            return;
        }
        wchar_t* message = nullptr;
        FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
            nullptr,result,0,reinterpret_cast<LPWSTR>(&message),0,nullptr);
        wxString details = message ? wxString(message) : wxString("Unknown Windows error.");
        if (message) LocalFree(message);
        if (result == ERROR_DLL_INIT_FAILED) details += "\nCheck DLL dependencies, folder permissions, and DllMain initialization.";
        if (result == ERROR_BAD_EXE_FORMAT) details += "\nUse an x64 DLL and an x64 Minecraft process.";
        if (result == ERROR_PARTIAL_COPY) details += "\nMinecraft may still be starting or closing. Wait for the main menu, or restart the game, then try again.";
        if (processAccessDenied) details += "\nWindows denied access to Minecraft. Process access diagnostics were saved to the failure log.";
        wxMessageBox(details,"Rush Injector / Load failed",wxOK | wxICON_ERROR,&ref); return;
    }
    ref.gamePid = ref.loadedPid = pid;
    ref.SetStatusText(wxEmptyString); ref.RefreshStatus();
    if (!interactive) {
        std::wstring reportingKey;
        if (ReadDownloadKey(ref.loadedBeta ? L"https://rush-download-service.juiceloot-github.workers.dev/beta/client" : Globals::DOWNLOAD_URL,reportingKey))
            ref.reportingThread=std::jthread([key=std::move(reportingKey)]() mutable {
                try { ReportInjectionSuccess(key); } catch (...) {}
                SecureZeroMemory(key.data(),key.size()*sizeof(wchar_t));
            });
    }
    ref.updateGlobalVars(); ref.cfg.updateConfigFile();
}
void cMain::OnSelectButton(wxCommandEvent&) {
    if (openDialog->ShowModal() == wxID_OK) {
        txt_DllPath->SetValue(openDialog->GetPath());
        updateGlobalVars(); cfg.updateConfigFile();
    }
}
void cMain::OnCustomCheckBox(wxCommandEvent&) {
    styleTargetInput(txt_ProcName,check_CustomTarget->IsChecked());
    if (!check_CustomTarget->IsChecked()) txt_ProcName->SetValue(Globals::MCBE_PROC_NAME);
    updateGlobalVars(); cfg.updateConfigFile(); RefreshProcess();
}

void cMain::ApplyTheme(wxWindow* extraWindow) {
    const wxColour previous = RED;
    RED = themeColour();
    std::function<void(wxWindow*)> apply = [&](wxWindow* window) {
        wxFont current = window->GetFont();
        if (current.IsOk()) { current.SetFaceName(Globals::FONT_FAMILY); window->SetFont(current); }
        if (window->GetForegroundColour() == previous) window->SetForegroundColour(RED);
        for (wxWindow* child : window->GetChildren()) if (!child->IsTopLevel()) apply(child);
        window->Layout(); window->Refresh();
    };
    apply(this);
    if (extraWindow) apply(extraWindow);
    if (extraWindow && extraWindow->GetSizer()) {
        const wxSize needed = extraWindow->GetSizer()->CalcMin();
        const wxSize current = extraWindow->GetClientSize();
        extraWindow->SetClientSize(wxSize(std::max(current.x,needed.x),std::max(current.y,needed.y)));
        extraWindow->Layout();
    }
    Layout(); mainPanel->Layout();
}

void cMain::OpenThemeMenu() {
    wxDialog dialog(this,wxID_ANY,"Rush Injector / Theme",wxDefaultPosition,FromDIP(wxSize(680,590)),wxDEFAULT_DIALOG_STYLE);
    dialog.SetBackgroundColour(BG); dialog.SetFont(font(10));
    styleDarkCaption(static_cast<HWND>(dialog.GetHandle()),BG);
    auto* root = new wxBoxSizer(wxVERTICAL);
    auto* header = new wxBoxSizer(wxHORIZONTAL);
    header->Add(label(&dialog,"Themes",24,FOREGROUND,true),0,wxALIGN_CENTER_VERTICAL);
    header->AddStretchSpacer();
    auto* discordToggle = new DiscordToggle(&dialog,Globals::DISCORD_PRESENCE);
    discordToggle->Bind(wxEVT_CHECKBOX,[this,discordToggle](wxCommandEvent&) {
        Globals::DISCORD_PRESENCE = discordToggle->IsOn();
        wxGetApp().SetDiscordPresence(Globals::DISCORD_PRESENCE);
        const wxString state = Globals::DISCORD_PRESENCE ? "ON" : "OFF";
        SetStatusText(cfg.updateConfigFile() ? "Discord Rich Presence " + state : "Discord Rich Presence " + state + " / settings save failed.");
    });
    header->Add(discordToggle,0,wxALIGN_CENTER_VERTICAL);
    root->Add(header,0,wxEXPAND | wxLEFT | wxRIGHT | wxTOP,FromDIP(28));
    auto* colorTitle = label(&dialog,"COLOR PRESET / " + wxString(Globals::COLOR_PRESET),9,MUTED,true);
    root->Add(colorTitle,0,wxALL,FromDIP(28));
    auto* colors = new wxBoxSizer(wxHORIZONTAL);
    const std::array<const wchar_t*,4> names = {L"Red",L"Blue",L"Purple",L"Mono"};
    const std::array<wxColour,4> palette = {wxColour(237,48,79),wxColour(57,112,224),wxColour(132,75,204),wxColour(103,105,119)};
    for (size_t i = 0; i < names.size(); ++i) {
        auto* button = new RushButton(&dialog,wxID_ANY,names[i],true,wxSize(132,42),palette[i]);
        colors->Add(button,1,i + 1 < names.size() ? wxRIGHT : 0,FromDIP(10));
        button->Bind(wxEVT_BUTTON,[&,i](wxCommandEvent&) {
            Globals::COLOR_PRESET = names[i]; colorTitle->SetLabel("COLOR PRESET / " + wxString(Globals::COLOR_PRESET));
            ApplyTheme(&dialog);
            SetStatusText(cfg.updateConfigFile() ? "Theme saved / " + wxString(Globals::COLOR_PRESET) : wxString("Theme applied / settings save failed."));
        });
    }
    root->Add(colors,0,wxEXPAND | wxLEFT | wxRIGHT,FromDIP(28));
    root->Add(label(&dialog,"INTERFACE FONT",9,MUTED,true),0,wxALL,FromDIP(28));
    auto* fonts = new wxGridSizer(3,2,FromDIP(10),FromDIP(12));
    std::array<RushButton*,6> fontButtons{};
    for (size_t i = 0; i < fontButtons.size(); ++i) {
        fontButtons[i] = new RushButton(&dialog,wxID_ANY,Globals::FONT_NAMES[i],Globals::FONT_FAMILY == Globals::FONT_FAMILIES[i],wxSize(240,42));
        fonts->Add(fontButtons[i],1,wxEXPAND);
        fontButtons[i]->Bind(wxEVT_BUTTON,[&,i](wxCommandEvent&) {
            Globals::FONT_FAMILY = Globals::FONT_FAMILIES[i];
            for (size_t j = 0; j < fontButtons.size(); ++j) fontButtons[j]->SetPrimary(j == i);
            ApplyTheme(&dialog);
            SetStatusText(cfg.updateConfigFile() ? "Font saved / " + wxString(Globals::FONT_NAMES[i]) : wxString("Font applied / settings save failed."));
        });
    }
    root->Add(fonts,0,wxEXPAND | wxLEFT | wxRIGHT,FromDIP(28));
    root->Add(label(&dialog,"Rush Injector",19,FOREGROUND,true),0,wxALL,FromDIP(28));
    root->AddStretchSpacer();
    auto* close = new RushButton(&dialog,wxID_CANCEL,"Done",true,wxSize(130,42));
    close->Bind(wxEVT_BUTTON,[&](wxCommandEvent&) { dialog.EndModal(wxID_OK); });
    root->Add(close,0,wxALIGN_RIGHT | wxLEFT | wxRIGHT | wxBOTTOM,FromDIP(28));
    dialog.SetSizerAndFit(root);
    const wxSize needed = root->CalcMin();
    dialog.SetClientSize(wxSize(std::max(dialog.FromDIP(680),needed.x),needed.y));
    dialog.CentreOnParent(); dialog.ShowModal();
}

void cMain::SetAutoInject(bool enabled) {
    autoInject.SetEnabled(enabled);
    btn_Inject->Enable(!enabled && !injectionBusy && !downloading);
    btn_Inject->Refresh();
    auto* button = static_cast<RushButton*>(btn_AutoInject);
    button->SetLabel(clientUpdateAvailable ? "Auto Inject: UPDATE (1)" : enabled ? "Auto Inject: ON" : "Auto Inject: OFF");
    button->SetPrimary(enabled);
    RefreshStatus();
}
void cMain::RefreshUpdatePing() {
    if (!downloading) {
        btn_AutoInject->SetLabel(clientUpdateAvailable ? "Auto Inject: UPDATE (1)" :
            autoInject.Enabled() ? "Auto Inject: ON" : "Auto Inject: OFF");
        static_cast<RushButton*>(btn_AutoInject)->SetPrimary(clientUpdateAvailable || autoInject.Enabled());
        btn_AutoInject->SetToolTip(clientUpdateAvailable ?
            "New Rush client available. Turn Auto Inject off and on to download. Restart Minecraft if a client is already loaded." :
            "Download the Rush client and inject when Minecraft starts.");
    }
    if (auto* taskbar = MSWGetTaskBarButton()) {
        const bool available = injectorUpdateAvailable || clientUpdateAvailable;
        const wxString message = clientUpdateAvailable ? "Rush client update available / use Auto Inject" :
            injectorUpdateAvailable ? "Rush Injector update available" : "Rush Injector";
        taskbar->SetOverlayIcon(available ? UpdateBadgeIcon() : wxNullIcon,message);
        taskbar->SetThumbnailTooltip(message);
    }
}
void cMain::CheckClientUpdate() {
    if (clientCheckBusy || downloading || Globals::DOWNLOAD_URL.empty()) return;
    std::wstring key;
    if (!PublicClientEndpoint(Globals::DOWNLOAD_URL) && !ReadDownloadKey(Globals::DOWNLOAD_URL,key)) return;
    clientCheckBusy = true;
    clientCheckThread = std::jthread([this,endpoint=Globals::DOWNLOAD_URL,key=std::move(key)](std::stop_token stop) mutable {
        std::wstring digest;
        try { digest = CheckClientDigest(endpoint,key); } catch (...) { }
        SecureZeroMemory(key.data(),key.size()*sizeof(wchar_t));
        if (stop.stop_requested()) return;
        auto* event = new wxThreadEvent(EVT_RUSH_CLIENT_CHECKED);
        event->SetPayload(std::make_pair(endpoint,digest)); wxQueueEvent(this,event);
    });
}

bool cMain::ConfigureDownload() {
    wxDialog dialog(this,wxID_ANY,"Rush download access",wxDefaultPosition,wxDefaultSize,wxDEFAULT_DIALOG_STYLE);
    dialog.SetBackgroundColour(BG); dialog.SetFont(font(10));
    styleDarkCaption(static_cast<HWND>(dialog.GetHandle()),BG);
    auto* layout = new wxBoxSizer(wxVERTICAL);
    layout->Add(label(&dialog,"Rush download access",20,FOREGROUND,true),0,wxALL,FromDIP(24));
    layout->Add(label(&dialog,"Enter the service address and your Rush download key.",9,MUTED),0,wxLEFT | wxRIGHT | wxBOTTOM,FromDIP(24));
    layout->Add(label(&dialog,"SERVICE ADDRESS",8,MUTED,true),0,wxLEFT | wxRIGHT,FromDIP(24));
    auto* address = new wxTextCtrl(&dialog,wxID_ANY,Globals::DOWNLOAD_URL,wxDefaultPosition,FromDIP(wxSize(510,34)),wxBORDER_SIMPLE);
    styleInput(address); address->SetHint("https://your-rush-service.workers.dev/client");
    layout->Add(address,0,wxEXPAND | wxLEFT | wxRIGHT | wxTOP | wxBOTTOM,FromDIP(24));
    layout->Add(label(&dialog,"RUSH DOWNLOAD KEY",8,MUTED,true),0,wxLEFT | wxRIGHT,FromDIP(24));
    auto* key = new wxTextCtrl(&dialog,wxID_ANY,wxEmptyString,wxDefaultPosition,FromDIP(wxSize(510,34)),wxTE_PASSWORD | wxBORDER_SIMPLE);
    styleInput(key); key->SetHint("Your personal download key");
    std::wstring saved;
    if (ReadDownloadKey(Globals::DOWNLOAD_URL,saved)) key->SetValue(saved);
    if (!saved.empty()) SecureZeroMemory(saved.data(),saved.size()*sizeof(wchar_t));
    layout->Add(key,0,wxEXPAND | wxLEFT | wxRIGHT | wxTOP | wxBOTTOM,FromDIP(24));
    auto* actions = new wxBoxSizer(wxHORIZONTAL); actions->AddStretchSpacer();
    auto* cancel = new RushButton(&dialog,wxID_CANCEL,"Cancel",false,wxSize(110,40));
    auto* save = new RushButton(&dialog,wxID_OK,"Save",true,wxSize(110,40));
    actions->Add(cancel,0,wxRIGHT,FromDIP(12)); actions->Add(save);
    layout->Add(actions,0,wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM,FromDIP(24));
    cancel->Bind(wxEVT_BUTTON,[&](wxCommandEvent&) { dialog.EndModal(wxID_CANCEL); });
    save->Bind(wxEVT_BUTTON,[&](wxCommandEvent&) {
        std::wstring endpoint;
        if (!NormalizeDownloadUrl(address->GetValue().Strip(wxString::both).ToStdWstring(),endpoint)) {
            wxMessageBox("Enter an HTTPS service address ending in /client.","Check service address",wxOK | wxICON_INFORMATION,&dialog); return;
        }
        std::wstring secret = key->GetValue().ToStdWstring();
        bool valid = ValidDownloadKey(secret);
        bool stored = valid && SaveDownloadKey(endpoint,secret);
        if (!secret.empty()) SecureZeroMemory(secret.data(),secret.size()*sizeof(wchar_t));
        if (!stored) {
            wxMessageBox(valid ? "Windows could not save the download key." : "Enter a valid Rush download key.","Check download key",wxOK | wxICON_INFORMATION,&dialog); return;
        }
        Globals::DOWNLOAD_URL = endpoint;
        if (!cfg.updateConfigFile()) {
            wxMessageBox("The download address could not be saved.","Rush Injector",wxOK | wxICON_ERROR,&dialog); return;
        }
        SetAutoInject(false); dialog.EndModal(wxID_OK);
    });
    dialog.SetSizerAndFit(layout); dialog.CentreOnParent();
    return dialog.ShowModal() == wxID_OK;
}

void cMain::ToggleAutoInject() {
    if (downloading) {
        downloadThread.request_stop(); btn_AutoInject->Disable();
        SetStatusText("Canceling client download..."); return;
    }
    if (injectionBusy) return;
    if (autoInject.Enabled()) { SetAutoInject(false); SetStatusText("Auto Inject disabled."); return; }
    std::wstring key;
    const bool hasSavedKey = !Globals::DOWNLOAD_URL.empty() && ReadDownloadKey(Globals::DOWNLOAD_URL,key);
    const bool publicAlpha=PublicClientEndpoint(Globals::DOWNLOAD_URL);
    if (Globals::DOWNLOAD_URL.empty() || (!publicAlpha && !hasSavedKey)) {
        if (!ConfigureDownload() || !ReadDownloadKey(Globals::DOWNLOAD_URL,key)) return;
    }
    if(publicAlpha) {SecureZeroMemory(key.data(),key.size()*sizeof(wchar_t));key.clear();}
    BeginClientDownload(Globals::DOWNLOAD_URL,std::move(key),false);
}
void cMain::InjectBeta() {
    if(injectionBusy || downloading) {SetStatusText("A client operation is already running.");return;}
    if(!GetProcId(txt_ProcName->GetValue().wc_str())) {SetStatusText("Launch Minecraft before injecting Beta.");return;}
    const std::wstring endpoint=L"https://rush-download-service.juiceloot-github.workers.dev/beta/client";
    std::wstring key;
    if(!ReadDownloadKey(endpoint,key)) {SetStatusText("Sign in to Beta first.");return;}
    SetAutoInject(false);
    BeginClientDownload(endpoint,std::move(key),true);
}
void cMain::BeginClientDownload(const std::wstring& downloadEndpoint,std::wstring key,bool beta) {
    downloadingBeta=beta;
    injectionBusy = true; downloading = true; btn_Inject->Disable(); btn_Select->Disable();
    btn_AutoInject->SetLabel("Cancel Download"); btn_AutoInject->Refresh(false);
    btn_AutoInject->SetToolTip("Cancel the current client download.");
    SetStatusText(beta ? "Downloading Rush Beta..." : "Downloading Rush client...");
    RefreshStatus();
    const std::wstring endpoint = downloadEndpoint;
    const std::wstring directory = (wxStandardPaths::Get().GetUserLocalDataDir()+(beta ? "\\beta-client" : "\\client")).ToStdWstring();
    downloadThread = std::jthread([this,endpoint,directory,key=std::move(key)](std::stop_token stop) mutable {
        ClientDownloadResult result;
        auto lastProgress = std::chrono::steady_clock::now() - std::chrono::milliseconds(100);
        try {
            result = DownloadClient(endpoint,key,directory,[&](size_t bytes) {
                if (stop.stop_requested()) return false;
                const auto now = std::chrono::steady_clock::now();
                if (now-lastProgress >= std::chrono::milliseconds(50)) {
                    auto* event = new wxThreadEvent(EVT_RUSH_DOWNLOAD_PROGRESS);
                    event->SetPayload(bytes); wxQueueEvent(this,event); lastProgress = now;
                }
                return true;
            },[this](const ClientDownloadResult& failure,unsigned attempt) {
                auto* diagnostic=new wxThreadEvent(EVT_RUSH_DOWNLOAD_DIAGNOSTIC);
                diagnostic->SetPayload(std::make_pair(failure,attempt)); wxQueueEvent(this,diagnostic);
            });
        } catch (...) {
            result = {ERROR_UNHANDLED_EXCEPTION,L"Client download failed.",{}};
        }
        if (!key.empty()) SecureZeroMemory(key.data(),key.size()*sizeof(wchar_t));
        if (stop.stop_requested()) result = {ERROR_CANCELLED,L"Client download canceled.",{}};
        auto* event = new wxThreadEvent(EVT_RUSH_DOWNLOAD_FINISHED);
        event->SetPayload(std::move(result)); wxQueueEvent(this,event);
    });
}

void cMain::FinishDownload(const ClientDownloadResult& result) {
    const bool beta=downloadingBeta;downloadingBeta=false;
    downloading = false; injectionBusy = false;
    btn_Inject->Enable(); btn_AutoInject->Enable(); btn_Select->Enable(); SetAutoInject(false);
    btn_AutoInject->SetToolTip("Download the Rush client and inject when Minecraft starts.");
    if (downloadThread.get_stop_token().stop_requested()) { SetStatusText("Client download canceled."); return; }
    if (result.error) {
        const bool logged=LogFailure(beta ? "Beta download" : "Public download",result.error,result.httpStatus,result.requestId,result.category);
        if (result.error == ERROR_LOGON_FAILURE) ForgetDownloadKey(beta ? L"https://rush-download-service.juiceloot-github.workers.dev/beta/client" : Globals::DOWNLOAD_URL);
        SetStatusText(result.message+L" / Windows error "+std::to_wstring(result.error)+
            (logged ? L" / Failure log saved." : L" / Could not write failure log."));
        return;
    }
    wxString fontError;
    if (!StageClientFonts(result.path, fontError)) {
        LogFailure(beta ? "Beta downloaded fonts" : "Public downloaded fonts",ERROR_WRITE_FAULT);
        SetStatusText(fontError); return;
    }
    automaticDllPath = result.path;
    automaticDllDigest = result.digest;
    loadedBeta=beta;
    if(beta) {
        SetStatusText("Rush Beta downloaded and verified.");
        wxCommandEvent event;OnInjectButtonExecute(event,*this,false);return;
    }
    const std::wstring name = wxFileName(result.path).GetName().ToStdWstring();
    if (name.size() == 75 && name.starts_with(L"RushClient-")) {
        Globals::CLIENT_DIGEST = name.substr(11);
        Globals::CLIENT_DIGEST_ENDPOINT = Globals::DOWNLOAD_URL;
        cfg.updateConfigFile();
    }
    clientUpdateAvailable = false;
    SetAutoInject(true); RefreshUpdatePing();
    SetStatusText("Rush client downloaded and verified.");
}
