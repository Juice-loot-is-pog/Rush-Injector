target_sources(${PROJECT_NAME} PRIVATE cApp.cpp        
cMain.cpp       
dev_panel.cpp
discord_presence.cpp
config.cpp      
fontloader.cpp
client_download.cpp
FixFilePerms.cpp
inject.cpp
pch.cpp
taskBarIcon.cpp
icon.rc)

target_precompile_headers(${PROJECT_NAME} PRIVATE "pch.h")
