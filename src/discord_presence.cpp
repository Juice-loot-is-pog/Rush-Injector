#include "discord_presence.h"
#include <windows.h>
#include <chrono>
#include <ctime>
#include <cstring>
#include <string>
#include <vector>
#include <regex>
#include <fstream>
#include <filesystem>

namespace {
using Clock = std::chrono::steady_clock;
void Log(const std::string& message) {
    wchar_t local[MAX_PATH]{};
    if(!GetEnvironmentVariableW(L"LOCALAPPDATA",local,MAX_PATH)) return;
    std::error_code error;
    const auto folder=std::filesystem::path(local)/L"Rush Injector";
    std::filesystem::create_directories(folder,error);
    const auto path=folder/L"discord-presence.log";
    const auto size=std::filesystem::file_size(path,error);
    std::ofstream file(path,(!error && size>65536) ? std::ios::trunc : std::ios::app);
    file<<std::time(nullptr)<<" "<<message<<"\n";
}
struct Handle {
    HANDLE value = INVALID_HANDLE_VALUE;
    ~Handle() { if(value != INVALID_HANDLE_VALUE && value) CloseHandle(value); }
};
bool Pause(std::stop_token stop, int milliseconds) {
    for(int elapsed=0; elapsed<milliseconds && !stop.stop_requested(); elapsed+=50)
        Sleep(50);
    return !stop.stop_requested();
}
// Bounded overlapped I/O keeps shutdown responsive even when Discord stalls.
bool Transfer(HANDLE pipe, void* buffer, DWORD bytes, bool writing, std::stop_token stop) {
    auto* cursor=static_cast<unsigned char*>(buffer);
    const auto deadline=Clock::now()+std::chrono::seconds(5);
    while(bytes && !stop.stop_requested()) {
        Handle event{CreateEventW(nullptr,TRUE,FALSE,nullptr)};
        if(!event.value) return false;
        OVERLAPPED operation{};operation.hEvent=event.value;
        DWORD count=0;
        BOOL ok=writing ? WriteFile(pipe,cursor,bytes,&count,&operation)
                        : ReadFile(pipe,cursor,bytes,&count,&operation);
        if(!ok) {
            if(GetLastError()!=ERROR_IO_PENDING) return false;
            while(WaitForSingleObject(event.value,50)==WAIT_TIMEOUT) {
                if(stop.stop_requested() || Clock::now()>=deadline) {
                    CancelIoEx(pipe,&operation);
                    GetOverlappedResult(pipe,&operation,&count,TRUE);
                    return false;
                }
            }
            if(!GetOverlappedResult(pipe,&operation,&count,FALSE)) return false;
        }
        if(!count) return false;
        cursor+=count;bytes-=count;
    }
    return bytes==0;
}
bool Send(HANDLE pipe, DWORD opcode, const std::string& body, std::stop_token stop) {
    const DWORD length=static_cast<DWORD>(body.size());
    std::vector<unsigned char> frame(8+length);
    memcpy(frame.data(),&opcode,4);memcpy(frame.data()+4,&length,4);
    memcpy(frame.data()+8,body.data(),length);
    return Transfer(pipe,frame.data(),static_cast<DWORD>(frame.size()),true,stop);
}
bool Receive(HANDLE pipe, DWORD& opcode, std::string& body, std::stop_token stop) {
    DWORD header[2]{};
    if(!Transfer(pipe,header,8,false,stop) || header[1]>1024*1024) return false;
    opcode=header[0];body.resize(header[1]);
    return body.empty() || Transfer(pipe,body.data(),header[1],false,stop);
}
bool Event(const std::string& body, const char* name) {
    return std::regex_search(body,std::regex(std::string(R"("evt"\s*:\s*")")+name+R"(")"));
}
std::string Activity(std::time_t started) {
    return R"({"cmd":"SET_ACTIVITY","args":{"pid":)"+std::to_string(GetCurrentProcessId())+
        R"(,"activity":{"type":0,"details":"Minecraft Bedrock","state":"Rush Injector","timestamps":{"start":)"+
        std::to_string(started)+
        R"(},"assets":{"large_image":"rush_logo","large_text":"Rush Injector"},"buttons":[{"label":"Download","url":"https://github.com/Juice-loot-is-pog/Rush-Injector/releases/latest"},{"label":"Discord","url":"https://discord.gg/sBrCxRZWKA"}]}},"nonce":"rush-presence"})";
}
void Run(std::stop_token stop) {
    Log("Presence started");
    const auto started=std::time(nullptr);
    while(!stop.stop_requested()) {
        Handle pipe;
        for(int index=0;index<10 && !stop.stop_requested();++index) {
            const auto name=L"\\\\?\\pipe\\discord-ipc-"+std::to_wstring(index);
            pipe.value=CreateFileW(name.c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,
                OPEN_EXISTING,FILE_FLAG_OVERLAPPED,nullptr);
            if(pipe.value!=INVALID_HANDLE_VALUE) {Log("Connected to Discord pipe "+std::to_string(index));break;}
        }
        if(pipe.value!=INVALID_HANDLE_VALUE &&
            Send(pipe.value,0,R"({"v":1,"client_id":"1565548127367340172"})",stop)) {
            bool ready=false;
            const auto handshakeDeadline=Clock::now()+std::chrono::seconds(10);
            while(!stop.stop_requested()) {
                DWORD available=0;
                if(!PeekNamedPipe(pipe.value,nullptr,0,nullptr,&available,nullptr)) break;
                if(!available) {
                    if(!ready && Clock::now()>=handshakeDeadline) break;
                    if(!Pause(stop,100)) break;
                    continue;
                }
                DWORD opcode=0;std::string body;
                if(!Receive(pipe.value,opcode,body,stop) || opcode==2) break;
                if(opcode==3) { if(!Send(pipe.value,4,body,stop)) break;continue; }
                if(opcode!=1) continue;
                if(Event(body,"ERROR")) {
                    Log("Discord rejected activity: "+body);
                    OutputDebugStringW(L"Rush: Discord rejected Rich Presence. Check application settings.\n");
                    break;
                }
                if(!ready && Event(body,"READY")) {
                    Log("Discord handshake accepted");
                    if(!Send(pipe.value,1,Activity(started),stop)) break;
                    ready=true;
                } else if(ready && body.find("rush-presence")!=std::string::npos) {
                    Log("Discord acknowledged activity");
                }
            }
        }
        // Connection closure removes its activity; retry without blocking the UI.
        if(pipe.value!=INVALID_HANDLE_VALUE) {CloseHandle(pipe.value);pipe.value=INVALID_HANDLE_VALUE;}
        if(!Pause(stop,5000)) break;
    }
}
}
void DiscordPresence::Start() {
    if(worker.joinable()) return;
    worker=std::jthread([](std::stop_token stop) {
        try { Run(stop); }
        catch(const std::exception& error) {Log(std::string("Presence exception: ")+error.what());}
        catch(...) {Log("Presence stopped after an unexpected error");}
    });
}
void DiscordPresence::Stop() {
    if(worker.joinable()) {worker.request_stop();worker.join();}
}
