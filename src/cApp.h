#pragma once

#include "config.h"
#include "fontloader.h"
#include "discord_presence.h"

class cMain;

class cApp : public wxApp {
    EmbeddedFonts fonts;
    DiscordPresence presence;
public:
	virtual bool OnInit() override;
    int OnExit() override;
    void SetDiscordPresence(bool enabled) { if (enabled) presence.Start(); else presence.Stop(); }
};

wxDECLARE_APP(cApp);

inline cMain* CMAIN_INSTANCE{};
