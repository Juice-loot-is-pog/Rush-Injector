#include "client_download.h"
#include <iostream>
#include <cstring>
#include <filesystem>
#include <fstream>

int main() {
    int failures = 0;
    auto check = [&](bool ok,const char* name) { std::cout << (ok ? "PASS " : "FAIL ") << name << '\n'; if (!ok) ++failures; };
    for (DWORD status : {502u,503u,504u}) {
        ClientDownloadResult result; result.error=ERROR_BAD_NET_RESP; result.httpStatus=status;
        check(RetryableClientFailure(result),"retry temporary HTTP failure");
    }
    for (DWORD status : {401u,403u,404u,500u}) {
        ClientDownloadResult result; result.error=ERROR_BAD_NET_RESP; result.httpStatus=status;
        check(!RetryableClientFailure(result),"do not retry permanent or authentication HTTP failure");
    }
    check(RetryableClientFailure({ERROR_TIMEOUT}),"retry timeout");
    check(!RetryableClientFailure({ERROR_CANCELLED}) && !RetryableClientFailure({ERROR_INVALID_DATA}) &&
        !RetryableClientFailure({}),"do not retry cancellation, invalid binary or success");
    check(ErrorResponseDetail("error code: 1102") == L"code-1102","read Cloudflare plain error code");
    check(ErrorResponseDetail("<title>Worker exceeded | x</title><span>Error <span class=\"cf-error-code\">1102</span></span>") == L"code-1102","read Cloudflare HTML error code");
    check(ErrorResponseDetail("") == L"" && ErrorResponseDetail(std::string("\r\n\0",3)) == L"","empty error body has no detail");
    check(ErrorResponseDetail("bad | gateway\r\nnext") == L"bad  gateway next","error detail stays on one log field");
    check(ErrorResponseDetail(std::string(500,'a')).size() == 48,"error detail is bounded");
    std::wstring normalized;
    check(NormalizeDownloadUrl(L"https://rush.example",normalized) && normalized == L"https://rush.example/client","normalize HTTPS service root");
    check(NormalizeDownloadUrl(L"https://rush.example/client",normalized),"accept exact endpoint");
    check(!NormalizeDownloadUrl(L"http://rush.example/client",normalized),"reject plaintext transport");
    check(!NormalizeDownloadUrl(L"https://user:secret@rush.example/client",normalized),"reject credentials in URL");
    check(!NormalizeDownloadUrl(L"https://rush.example/client?token=secret",normalized),"reject query secrets");
    check(!NormalizeDownloadUrl(L"https://rush.example/other",normalized),"reject unrelated path");
    check(!ValidDownloadKey(L"short") && !ValidDownloadKey(std::wstring(40,L'a')+L"\r\nInjected: x"),"reject invalid authorization headers");
    check(ValidDownloadKey(std::wstring(43,L'a')),"accept Rush download key");
    check(PublicClientEndpoint(L"https://rush-download-service.juiceloot-github.workers.dev/client"),"official Alpha allows keyless access");
    check(!PublicClientEndpoint(L"https://rush-download-service.juiceloot-github.workers.dev/beta/client"),"Beta never uses public download mode");
    check(!PublicClientEndpoint(L"https://other.example/client"),"custom services preserve credential requirements");
    check(ClientSha256({'a','b','c'}) == L"ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad","SHA-256 known vector");
    std::vector<unsigned char> image(128);
    IMAGE_DOS_HEADER dos{}; dos.e_magic = IMAGE_DOS_SIGNATURE; dos.e_lfanew = 64;
    DWORD signature = IMAGE_NT_SIGNATURE;
    IMAGE_FILE_HEADER header{}; header.Machine = IMAGE_FILE_MACHINE_AMD64; header.Characteristics = IMAGE_FILE_DLL;
    std::memcpy(image.data(),&dos,sizeof(dos)); std::memcpy(image.data()+64,&signature,sizeof(signature));
    std::memcpy(image.data()+68,&header,sizeof(header));
    std::wstring digest = ClientSha256(image);
    check(ValidateClientDownload(image,digest),"accept matching x64 DLL and checksum");
    const auto fixture=std::filesystem::temp_directory_path()/
        (L"RushIntegrity-"+std::to_wstring(GetCurrentProcessId())+L".dll");
    auto saveFixture=[&] {
        std::ofstream file(fixture,std::ios::binary|std::ios::trunc);
        file.write(reinterpret_cast<const char*>(image.data()),image.size());
    };
    saveFixture();
    {
        VerifiedClientFile verified(fixture.wstring(),digest);
        check(verified.Valid(),"verify cached file before loading");
        HANDLE writer=CreateFileW(fixture.c_str(),GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,
            nullptr,OPEN_EXISTING,0,nullptr);
        check(writer==INVALID_HANDLE_VALUE,"prevent modification while verified file is held");
        if(writer!=INVALID_HANDLE_VALUE) CloseHandle(writer);
        check(!DeleteFileW(fixture.c_str()),"prevent replacement while verified file is held");
    }
    image.back() ^= 1;
    saveFixture();
    check(!VerifiedClientFile(fixture.wstring(),digest).Valid(),"reject cached DLL modified after download");
    check(!VerifiedClientFile(fixture.wstring(),L"").Valid(),"reject missing trusted checksum");
    std::filesystem::remove(fixture);
    check(!VerifiedClientFile(fixture.wstring(),digest).Valid(),"reject missing cached DLL");
    check(!ValidateClientDownload(image,digest),"reject altered download");
    image[68] = 0x4c; image[69] = 0x01;
    check(!ValidateClientDownload(image,ClientSha256(image)),"reject x86 DLL");
    check(!ValidateClientDownload({'<','h','t','m','l','>'},std::wstring(64,L'0')),"reject HTML response");
    std::wstring endpoint = L"https://fixture-" + std::to_wstring(GetCurrentProcessId()) + L".example/client";
    std::wstring secret(43,L'a'), restored;
    bool stored = SaveDownloadKey(endpoint,secret);
    check(stored && ReadDownloadKey(endpoint,restored) && restored == secret,"Credential Manager round trip");
    ForgetDownloadKey(endpoint);
    check(!ReadDownloadKey(endpoint,restored),"credential deletion");
    return failures ? 1 : 0;
}
