#include <wx/wx.h>
#include <wx/graphics.h>
#include <wx/dcmemory.h>
#include "fontloader.h"
#include "config.h"
#include <windows.h>
#include <iostream>
#include <set>
#include <memory>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <cstring>

class FontTestApp : public wxApp { public: bool OnInit() override { return true; } };
wxIMPLEMENT_APP_NO_MAIN(FontTestApp);

int wmain(int argc, wchar_t** argv) {
    if (!wxEntryStart(argc, argv)) return 2;
    wxTheApp->CallOnInit(); wxTheApp->SetAppName("Rush Injector");
    wxInitAllImageHandlers();
    if (argc == 3 && std::wstring(argv[1]) == L"--stage-client-fonts") {
        wxString error;
        bool ok = StageClientFonts(argv[2],error);
        std::cout << (ok ? "Client fonts prepared next to the DLL." : error.ToStdString()) << std::endl;
        wxTheApp->OnExit(); wxEntryCleanup(); return ok ? 0 : 1;
    }
    int failures = 0;
    {
        auto check = [&](bool ok, const char* name) {
            std::cout << (ok ? "PASS " : "FAIL ") << name << std::endl; if (!ok) ++failures;
        };
        const auto root = std::filesystem::current_path() / (L"client-font-fixture-\u03a9-" + std::to_wstring(GetCurrentProcessId()));
        std::filesystem::create_directories(root);
        const auto dll = root / L"client.dll";
        { std::ofstream file(dll); file << "fixture"; }
        wxString error;
        check(!StageClientFonts((root / L"missing.dll").wstring(),error), "font staging requires an existing DLL");
        { std::ofstream blocked(root / L"fonts"); blocked << "blocked"; }
        { wxLogNull expected; check(!StageClientFonts(dll.wstring(),error), "blocked fonts folder stops staging"); }
        std::filesystem::remove(root / L"fonts");
        check(StageClientFonts(dll.wstring(),error), "stage fonts next to DLL in a Unicode directory");
        const wchar_t* names[] = {L"Comfortaa.ttf",L"Mojangles.ttf",L"Open-Sans.ttf",L"Product-Sans.ttf",L"Sarabun-Light.ttf",L"SF-Pro-Display.ttf"};
        auto matches = [&](int i) {
            HRSRC resource = FindResourceW(nullptr,MAKEINTRESOURCEW(203+i),RT_RCDATA);
            DWORD size = SizeofResource(nullptr,resource);
            std::ifstream file(root / L"fonts" / names[i],std::ios::binary | std::ios::ate);
            if (!file || file.tellg() != static_cast<std::streamoff>(size)) return false;
            file.seekg(0); std::vector<unsigned char> data(size); file.read(reinterpret_cast<char*>(data.data()),size);
            return file && std::memcmp(data.data(),LockResource(LoadResource(nullptr,resource)),size) == 0;
        };
        bool allMatch = true; for (int i = 0; i < 6; ++i) allMatch = matches(i) && allMatch;
        check(allMatch,"all six original font filenames contain the embedded bytes");
        { std::ofstream damaged(root / L"fonts" / names[0],std::ios::trunc); damaged << "damaged"; }
        std::filesystem::remove(root / L"fonts" / names[1]);
        check(StageClientFonts(dll.wstring(),error) && matches(0) && matches(1), "repair corrupt and missing client fonts");
        for (const auto* name : names) std::filesystem::remove(root / L"fonts" / name);
        std::filesystem::remove(root / L"fonts"); std::filesystem::remove(dll); std::filesystem::remove(root);
    }
    {
        EmbeddedFonts fonts;
        std::vector<void*> legacy;
        bool oldLoader = argc > 1 && std::wstring(argv[1]) == L"--legacy";
        wxString error;
        if (oldLoader) {
            for (int id = 203; id <= 208; ++id) {
                HRSRC resource = FindResourceW(nullptr,MAKEINTRESOURCEW(id),RT_RCDATA);
                HGLOBAL data = LoadResource(nullptr,resource); DWORD count = 0;
                legacy.push_back(AddFontMemResourceEx(LockResource(data),SizeofResource(nullptr,resource),nullptr,&count));
            }
        } else if (!fonts.Load(error)) { std::cerr << error.ToStdString() << std::endl; return 3; }
        wxBitmap sheet(850, 12 * 75, 24);
        wxMemoryDC preview(sheet); preview.SetBackground(*wxWHITE_BRUSH); preview.Clear();
        for (int bold = 0; bold < 2; ++bold) {
            std::set<std::uint64_t> hashes;
            wxFont font(wxFontInfo(24).FaceName("Segoe UI").Weight(bold ? wxFONTWEIGHT_BOLD : wxFONTWEIGHT_NORMAL));
            for (size_t i = 0; i < Globals::FONT_FAMILIES.size(); ++i) {
                font.SetFaceName(Globals::FONT_FAMILIES[i]);
                wxBitmap bitmap(640, 75, 24); wxMemoryDC dc(bitmap);
                dc.SetBackground(*wxWHITE_BRUSH); dc.Clear();
                HGDIOBJ previous = SelectObject(static_cast<HDC>(dc.GetHDC()),static_cast<HFONT>(font.GetResourceHandle()));
                wchar_t actual[LF_FACESIZE]{}; GetTextFaceW(static_cast<HDC>(dc.GetHDC()),LF_FACESIZE,actual);
                bool matched = _wcsicmp(actual,Globals::FONT_FAMILIES[i]) == 0;
                SelectObject(static_cast<HDC>(dc.GetHDC()),previous);
                std::unique_ptr<wxGraphicsContext> graphics(wxGraphicsContext::Create(dc));
                if (!graphics) return 4;
                graphics->SetFont(font,*wxBLACK); graphics->DrawText("Rush Injector / Aa 0123456789",5,10); graphics.reset();
                dc.SelectObject(wxNullBitmap);
                wxImage image = bitmap.ConvertToImage();
                std::uint64_t hash = 14695981039346656037ull; size_t ink = 0;
                for (int p = 0; p < image.GetWidth() * image.GetHeight() * 3; ++p) {
                    unsigned char value = image.GetData()[p]; ink += value < 240; hash ^= value; hash *= 1099511628211ull;
                }
                bool distinct = hashes.insert(hash).second;
                bool passed = matched && ink > 100 && distinct;
                std::cout << (passed ? "PASS " : "FAIL ") << wxString(Globals::FONT_NAMES[i]).ToStdString()
                    << (bold ? " bold" : " regular") << " / native=" << wxString(actual).ToStdString()
                    << " ink=" << ink << " distinct=" << distinct << std::endl;
                if (!passed) ++failures;
                int y = (bold * 6 + static_cast<int>(i)) * 75;
                preview.SetFont(wxFont(wxFontInfo(10).FaceName("Segoe UI"))); preview.SetTextForeground(*wxBLACK);
                preview.DrawText(wxString(Globals::FONT_NAMES[i]) + (bold ? " bold" : " regular"),5,y+20);
                preview.DrawBitmap(bitmap,205,y,false);
            }
        }
        preview.SelectObject(wxNullBitmap);
        sheet.SaveFile("font-preview.png",wxBITMAP_TYPE_PNG);
        for (void* handle : legacy) if (handle) RemoveFontMemResourceEx(handle);
    }
    wxTheApp->OnExit(); wxEntryCleanup(); return failures ? 1 : 0;
}
