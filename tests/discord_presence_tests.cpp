// Exercise the real transport against an isolated pipe, never the user's Discord.
#include "../src/discord_presence.cpp"
#include <iostream>

int main() {
    const std::wstring name=L"\\\\.\\pipe\\rush-presence-test-"+std::to_wstring(GetCurrentProcessId());
    Handle server{CreateNamedPipeW(name.c_str(),PIPE_ACCESS_DUPLEX|FILE_FLAG_OVERLAPPED,
        PIPE_TYPE_BYTE|PIPE_READMODE_BYTE|PIPE_WAIT,1,4096,4096,0,nullptr)};
    Handle client{CreateFileW(name.c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,
        OPEN_EXISTING,FILE_FLAG_OVERLAPPED,nullptr)};
    auto require=[](bool ok) {if(!ok) {std::cerr<<"Presence transport check failed\n";exit(1);}};
    require(server.value!=INVALID_HANDLE_VALUE && client.value!=INVALID_HANDLE_VALUE);
    std::stop_source source;
    DWORD opcode=0;std::string body;
    require(Send(client.value,3,"ping",source.get_token()));
    require(Receive(server.value,opcode,body,source.get_token()) && opcode==3 && body=="ping");
    require(Send(server.value,4,body,source.get_token()));
    require(Receive(client.value,opcode,body,source.get_token()) && opcode==4 && body=="ping");
    require(Event(R"({"evt": "READY"})","READY"));
    require(!Event(R"({"evt":"ERROR"})","READY"));
    const auto activity=Activity(1234);
    require(activity.find("\"start\":1234")!=std::string::npos);
    require(activity.find("1565548127367340172")==std::string::npos);
    DWORD invalid[2]{1,1024*1024+1};
    require(Transfer(server.value,invalid,sizeof(invalid),true,source.get_token()));
    require(!Receive(client.value,opcode,body,source.get_token()));
    const auto start=Clock::now();
    std::jthread pending([&](std::stop_token stop) {
        DWORD unused=0; require(!Transfer(client.value,&unused,4,false,stop));
    });
    Sleep(100);pending.request_stop();pending.join();
    require(Clock::now()-start<std::chrono::seconds(2));
    std::cout<<"Discord framing, ping/pong, event recognition, payload bounds and cancellation passed.\n";
}
