#include "client_download.h"
#include <winhttp.h>
#include <wincred.h>
#include <bcrypt.h>
#include <algorithm>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <cstring>
#include <regex>

namespace {
constexpr size_t MaxBytes = 32 * 1024 * 1024;
struct HttpHandle {
    HINTERNET handle;
    explicit HttpHandle(HINTERNET value) : handle(value) {}
    ~HttpHandle() { if (handle) WinHttpCloseHandle(handle); }
};
bool Parse(const std::wstring& url, std::wstring& host, std::wstring& path, INTERNET_PORT& port) {
    if (url.empty() || url.size() > 2048 || url.find_first_of(L"\r\n\t #?") != std::wstring::npos) return false;
    URL_COMPONENTS parts{}; parts.dwStructSize = sizeof(parts);
    parts.dwHostNameLength = parts.dwUrlPathLength = parts.dwExtraInfoLength =
        parts.dwUserNameLength = parts.dwPasswordLength = static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(url.c_str(), static_cast<DWORD>(url.size()), 0, &parts) ||
        parts.nScheme != INTERNET_SCHEME_HTTPS || !parts.dwHostNameLength ||
        parts.dwUserNameLength || parts.dwPasswordLength || parts.dwExtraInfoLength) return false;
    host.assign(parts.lpszHostName, parts.dwHostNameLength);
    std::transform(host.begin(),host.end(),host.begin(),[](wchar_t c) { return std::towlower(c); });
    path.assign(parts.lpszUrlPath ? parts.lpszUrlPath : L"",parts.dwUrlPathLength);
    if (path.empty() || path == L"/") path = L"/client";
    port = parts.nPort;
    return path == L"/client" || path == L"/beta/client";
}
std::wstring Target(const std::wstring& endpoint) { return L"Rush Injector/Download/" + endpoint; }
ClientDownloadResult Fail(DWORD error, const wchar_t* message) { return {error,message,{}}; }
}

bool NormalizeDownloadUrl(const std::wstring& input, std::wstring& output) {
    std::wstring host,path; INTERNET_PORT port;
    if (!Parse(input,host,path,port)) return false;
    // Brackets are required when reconstructing an IPv6 authority.
    if (host.find(L':') != std::wstring::npos && host.front() != L'[') host = L"[" + host + L"]";
    output = L"https://" + host + (port == 443 ? L"" : L":" + std::to_wstring(port)) + path;
    return true;
}
bool ValidDownloadKey(const std::wstring& key) {
    return key.size() >= 32 && key.size() <= 128 && std::all_of(key.begin(),key.end(),[](wchar_t c) {
        return (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') || (c >= L'0' && c <= L'9') || c == L'_' || c == L'-';
    });
}
bool PublicClientEndpoint(const std::wstring& endpoint) {
    std::wstring normalized;
    return NormalizeDownloadUrl(endpoint,normalized) &&
        normalized==L"https://rush-download-service.juiceloot-github.workers.dev/client";
}
std::wstring DeviceIdentityHash() {
    wchar_t machine[256]{};DWORD bytes=sizeof(machine);
    if(RegGetValueW(HKEY_LOCAL_MACHINE,L"SOFTWARE\\Microsoft\\Cryptography",L"MachineGuid",RRF_RT_REG_SZ|RRF_SUBKEY_WOW6464KEY,nullptr,machine,&bytes)!=ERROR_SUCCESS) return {};
    const std::wstring material=L"Rush-Beta-Device-v1:"+std::wstring(machine);
    const auto* start=reinterpret_cast<const unsigned char*>(material.data());
    auto result=ClientSha256(std::vector<unsigned char>(start,start+material.size()*sizeof(wchar_t)));
    SecureZeroMemory(machine,sizeof(machine));return result;
}
std::wstring CheckClientDigest(const std::wstring& endpoint, const std::wstring& key) {
    std::wstring host,path; INTERNET_PORT port;
    if (!Parse(endpoint,host,path,port) || (!PublicClientEndpoint(endpoint) && !ValidDownloadKey(key))) return {};
    HttpHandle session(WinHttpOpen(L"Rush-Client-Update",WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,nullptr,nullptr,0));
    if (!session.handle) return {};
    WinHttpSetTimeouts(session.handle,5000,10000,10000,40000);
    HttpHandle connection(WinHttpConnect(session.handle,host.c_str(),port,0));
    if (!connection.handle) return {};
    HttpHandle request(WinHttpOpenRequest(connection.handle,L"HEAD",path.c_str(),nullptr,nullptr,WINHTTP_DEFAULT_ACCEPT_TYPES,WINHTTP_FLAG_SECURE));
    if (!request.handle) return {};
    DWORD redirects = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
    if (!WinHttpSetOption(request.handle,WINHTTP_OPTION_REDIRECT_POLICY,&redirects,sizeof(redirects))) return {};
    const std::wstring authorization = PublicClientEndpoint(endpoint) ? L"" :
        L"Authorization: Bearer " + key + L"\r\nX-Device-Hash: " + DeviceIdentityHash() + L"\r\n";
    if (!WinHttpSendRequest(request.handle,authorization.c_str(),static_cast<DWORD>(authorization.size()),nullptr,0,0,0) ||
        !WinHttpReceiveResponse(request.handle,nullptr)) return {};
    DWORD status=0,size=sizeof(status);
    if (!WinHttpQueryHeaders(request.handle,WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,nullptr,&status,&size,nullptr) || status != 200) return {};
    wchar_t digest[65]{}; size=sizeof(digest);
    if (!WinHttpQueryHeaders(request.handle,WINHTTP_QUERY_CUSTOM,L"X-Content-SHA256",digest,&size,nullptr)) return {};
    std::wstring result(digest);
    if (result.size()!=64 || result.find_first_not_of(L"0123456789abcdef")!=std::wstring::npos) return {};
    return result;
}
bool ReadDownloadKey(const std::wstring& endpoint, std::wstring& key) {
    PCREDENTIALW credential = nullptr;
    if (!CredReadW(Target(endpoint).c_str(),CRED_TYPE_GENERIC,0,&credential)) return false;
    bool validSize = credential->CredentialBlobSize % sizeof(wchar_t) == 0 && credential->CredentialBlobSize <= 256;
    if (validSize) key.assign(reinterpret_cast<const wchar_t*>(credential->CredentialBlob),credential->CredentialBlobSize / sizeof(wchar_t));
    CredFree(credential);
    return validSize && ValidDownloadKey(key);
}
bool SaveDownloadKey(const std::wstring& endpoint, const std::wstring& key) {
    if (!ValidDownloadKey(key)) return false;
    std::wstring target = Target(endpoint);
    CREDENTIALW credential{}; credential.Type = CRED_TYPE_GENERIC; credential.TargetName = target.data();
    credential.CredentialBlobSize = static_cast<DWORD>(key.size() * sizeof(wchar_t));
    credential.CredentialBlob = reinterpret_cast<LPBYTE>(const_cast<wchar_t*>(key.data()));
    credential.Persist = CRED_PERSIST_LOCAL_MACHINE; credential.UserName = const_cast<wchar_t*>(L"Rush client");
    return CredWriteW(&credential,0) != FALSE;
}
void ForgetDownloadKey(const std::wstring& endpoint) { CredDeleteW(Target(endpoint).c_str(),CRED_TYPE_GENERIC,0); }

std::wstring ClientSha256(const std::vector<unsigned char>& bytes) {
    if (bytes.size() > MaxBytes) return {};
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    if (BCryptOpenAlgorithmProvider(&algorithm,BCRYPT_SHA256_ALGORITHM,nullptr,0) < 0) return {};
    unsigned char digest[32]{};
    NTSTATUS status = BCryptHash(algorithm,nullptr,0,const_cast<PUCHAR>(bytes.data()),static_cast<ULONG>(bytes.size()),digest,sizeof(digest));
    BCryptCloseAlgorithmProvider(algorithm,0);
    if (status < 0) return {};
    static constexpr wchar_t digits[] = L"0123456789abcdef";
    std::wstring text; text.reserve(64);
    for (unsigned char value : digest) { text += digits[value >> 4]; text += digits[value & 15]; }
    return text;
}
bool ValidateClientDownload(const std::vector<unsigned char>& bytes, const std::wstring& digest) {
    if (bytes.size() < sizeof(IMAGE_DOS_HEADER) || bytes.size() > MaxBytes || digest.size() != 64) return false;
    IMAGE_DOS_HEADER dos{}; std::memcpy(&dos,bytes.data(),sizeof(dos));
    if (dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew < sizeof(dos) ||
        static_cast<size_t>(dos.e_lfanew) > bytes.size() - sizeof(DWORD) - sizeof(IMAGE_FILE_HEADER)) return false;
    DWORD signature; IMAGE_FILE_HEADER header{};
    std::memcpy(&signature,bytes.data()+dos.e_lfanew,sizeof(signature));
    std::memcpy(&header,bytes.data()+dos.e_lfanew+sizeof(signature),sizeof(header));
    return signature == IMAGE_NT_SIGNATURE && header.Machine == IMAGE_FILE_MACHINE_AMD64 &&
        (header.Characteristics & IMAGE_FILE_DLL) && digest == ClientSha256(bytes);
}

VerifiedClientFile::VerifiedClientFile(const std::wstring& path, const std::wstring& digest) {
    if (digest.size()!=64) return;
    HANDLE file=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL|FILE_FLAG_SEQUENTIAL_SCAN,nullptr);
    if(file==INVALID_HANDLE_VALUE) return;
    LARGE_INTEGER size{};
    bool valid=GetFileSizeEx(file,&size) && size.QuadPart>=64 && size.QuadPart<=MaxBytes;
    try {
      if(valid) {
        std::vector<unsigned char> bytes(static_cast<size_t>(size.QuadPart));
        DWORD read=0;
        valid=ReadFile(file,bytes.data(),static_cast<DWORD>(bytes.size()),&read,nullptr) &&
            read==bytes.size() && ValidateClientDownload(bytes,digest);
      }
    } catch (...) { CloseHandle(file); throw; }
    if(valid) handle=file; else CloseHandle(file);
}
VerifiedClientFile::~VerifiedClientFile() {
    if(handle!=INVALID_HANDLE_VALUE) CloseHandle(handle);
}

static ClientDownloadResult DownloadClientOnce(const std::wstring& endpoint, const std::wstring& key,
    const std::wstring& directory, const std::function<bool(size_t)>& progress) {
    std::wstring host,path; INTERNET_PORT port;
    if (!Parse(endpoint,host,path,port) || (!PublicClientEndpoint(endpoint) && !ValidDownloadKey(key)))
        return Fail(ERROR_INVALID_PARAMETER,L"Check the service URL and Rush download key.");
    HttpHandle session(WinHttpOpen(L"Rush Injector/1.2",WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,nullptr,nullptr,0));
    if (!session.handle) return Fail(GetLastError(),L"The download connection could not be created.");
    WinHttpSetTimeouts(session.handle,5000,10000,10000,40000);
    HttpHandle connection(WinHttpConnect(session.handle,host.c_str(),port,0));
    if (!connection.handle) return Fail(GetLastError(),L"The download service could not be reached.");
    HttpHandle request(WinHttpOpenRequest(connection.handle,L"GET",path.c_str(),nullptr,nullptr,WINHTTP_DEFAULT_ACCEPT_TYPES,WINHTTP_FLAG_SECURE));
    if (!request.handle) return Fail(GetLastError(),L"The download request could not be created.");
    DWORD redirects = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
    if (!WinHttpSetOption(request.handle,WINHTTP_OPTION_REDIRECT_POLICY,&redirects,sizeof(redirects)))
        return Fail(GetLastError(),L"The download connection could not be configured.");
    std::wstring authorization = PublicClientEndpoint(endpoint) ? L"" :
        L"Authorization: Bearer " + key + L"\r\nX-Device-Hash: " + DeviceIdentityHash() + L"\r\n";
    if (!WinHttpSendRequest(request.handle,authorization.c_str(),static_cast<DWORD>(authorization.size()),nullptr,0,0,0) ||
        !WinHttpReceiveResponse(request.handle,nullptr)) return Fail(GetLastError(),L"The client download failed. Check your connection.");
    DWORD status = 0, size = sizeof(status);
    if (!WinHttpQueryHeaders(request.handle,WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,nullptr,&status,&size,nullptr))
        return Fail(GetLastError(),L"The download service returned an invalid response.");
    if (status != 200) {
        auto result = Fail(status == 401 ? ERROR_LOGON_FAILURE : ERROR_BAD_NET_RESP,
            status == 401 ? L"Your Rush download key was rejected. Click Inject Rush to enter another key." : L"The service could not supply the client. Contact Rush support.");
        result.httpStatus = status;
        result.category=L"unclassified-http";
        wchar_t category[65]{}; DWORD categorySize=sizeof(category);
        if (WinHttpQueryHeaders(request.handle,WINHTTP_QUERY_CUSTOM,L"X-Rush-Error",category,&categorySize,nullptr)) {
            const std::wstring value(category);
            const std::vector<std::wstring> known={L"configuration",L"source-unavailable",L"source-invalid",L"source-too-large",L"upstream-failure",L"access-rejected",L"request-rejected",L"beta-unavailable"};
            if (std::find(known.begin(),known.end(),value)!=known.end()) result.category=L"worker-"+value;
        } else {
            wchar_t server[65]{}; DWORD serverSize=sizeof(server);
            if (WinHttpQueryHeaders(request.handle,WINHTTP_QUERY_CUSTOM,L"Server",server,&serverSize,nullptr) &&
                _wcsicmp(server,L"cloudflare")==0) result.category=L"cloudflare-unclassified";
            // Only unlabelled responses need this: the worker's own errors say what they are.
            std::string body(4096,'\0');
            DWORD read=0, total=0;
            while (total<body.size() && WinHttpReadData(request.handle,body.data()+total,static_cast<DWORD>(body.size())-total,&read) && read) total+=read;
            body.resize(total);
            const std::wstring detail=ErrorResponseDetail(body);
            if (!detail.empty()) result.category+=L"/"+detail;
        }
        wchar_t ray[129]{}; DWORD raySize=sizeof(ray);
        if (WinHttpQueryHeaders(request.handle,WINHTTP_QUERY_CUSTOM,L"CF-Ray",ray,&raySize,nullptr)) {
            std::wstring value(ray);
            if (value.find_first_not_of(L"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-")==std::wstring::npos)
                result.requestId=value;
        }
        result.message += L" HTTP " + std::to_wstring(status) + L". See the failure log.";
        return result;
    }
    wchar_t hash[65]{}; size = sizeof(hash);
    if (!WinHttpQueryHeaders(request.handle,WINHTTP_QUERY_CUSTOM,L"X-Content-SHA256",hash,&size,nullptr))
        return Fail(ERROR_INVALID_DATA,L"The service did not include a client checksum.");
    std::vector<unsigned char> bytes; unsigned char chunk[65536]; DWORD count = 0;
    const ULONGLONG started = GetTickCount64();
    for (;;) {
        if (!progress(bytes.size())) return Fail(ERROR_CANCELLED,L"Client download canceled.");
        if (GetTickCount64() - started > 60000) return Fail(ERROR_TIMEOUT,L"Client download timed out.");
        if (!WinHttpReadData(request.handle,chunk,sizeof(chunk),&count)) return Fail(GetLastError(),L"The client download was interrupted.");
        if (!count) break;
        if (bytes.size() + count > MaxBytes) return Fail(ERROR_FILE_TOO_LARGE,L"The client download is larger than expected.");
        bytes.insert(bytes.end(),chunk,chunk+count);
    }
    if (!ValidateClientDownload(bytes,hash)) return Fail(ERROR_INVALID_DATA,L"The downloaded client failed verification.");
    std::error_code ec; std::filesystem::create_directories(directory,ec);
    if (ec) return Fail(ERROR_ACCESS_DENIED,L"The client cache folder could not be created.");
    const auto target = std::filesystem::path(directory) / (L"RushClient-" + std::wstring(hash) + L".dll");
    if (std::filesystem::exists(target,ec)) {
        std::ifstream existing(target,std::ios::binary | std::ios::ate);
        if (existing && existing.tellg() == static_cast<std::streamoff>(bytes.size())) {
            existing.seekg(0); std::vector<unsigned char> cached(bytes.size());
            existing.read(reinterpret_cast<char*>(cached.data()),static_cast<std::streamsize>(cached.size()));
            if (existing && ValidateClientDownload(cached,hash))
                return {ERROR_SUCCESS,L"Client downloaded and verified.",target.wstring(),0,{},{},hash};
        }
    }
    auto temporary = target; temporary += L".part." + std::to_wstring(GetCurrentProcessId());
    {
        std::ofstream file(temporary,std::ios::binary | std::ios::trunc);
        file.write(reinterpret_cast<const char*>(bytes.data()),static_cast<std::streamsize>(bytes.size())); file.close();
        if (!file) { std::filesystem::remove(temporary,ec); return Fail(ERROR_WRITE_FAULT,L"The client file could not be saved."); }
    }
    if (!MoveFileExW(temporary.c_str(),target.c_str(),MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DWORD error = GetLastError(); std::filesystem::remove(temporary,ec); return Fail(error,L"The client cache could not be updated.");
    }
    return {ERROR_SUCCESS,L"Client downloaded and verified.",target.wstring(),0,{},{},hash};
}
std::wstring ErrorResponseDetail(const std::string& body) {
    // Cloudflare sends "error code: 1102" as text, or "Error <span>1102</span>" in HTML.
    static const std::regex code(R"(error(?:\s+code:?|:)?\s*(?:<[^>]*>\s*)?([0-9]{3,4}))",std::regex::icase);
    std::smatch match;
    if (std::regex_search(body,match,code)) { const std::string value=match[1]; return L"code-"+std::wstring(value.begin(),value.end()); }
    // Anything else lands in a pipe-delimited log line, so keep it short and single-line.
    std::wstring text;
    for (unsigned char c : body) {
        if (text.size()==48) break;
        if (c>=0x20 && c<0x7f && c!='|') text.push_back(static_cast<wchar_t>(c));
        else if (!text.empty() && text.back()!=L' ') text.push_back(L' ');
    }
    while (!text.empty() && text.back()==L' ') text.pop_back();
    return text;
}
bool RetryableClientFailure(const ClientDownloadResult& result) {
    if (!result.error) return false;
    if (result.httpStatus) return result.httpStatus==502 || result.httpStatus==503 || result.httpStatus==504;
    return result.error==ERROR_WINHTTP_TIMEOUT || result.error==ERROR_WINHTTP_CONNECTION_ERROR ||
        result.error==ERROR_WINHTTP_CANNOT_CONNECT || result.error==ERROR_WINHTTP_NAME_NOT_RESOLVED || result.error==ERROR_TIMEOUT;
}
ClientDownloadResult DownloadClient(const std::wstring& endpoint, const std::wstring& key,
    const std::wstring& directory, const std::function<bool(size_t)>& progress,
    const std::function<void(const ClientDownloadResult&,unsigned)>& diagnostic) {
    for (unsigned attempt=1; attempt<=3; ++attempt) {
        if (!progress(0)) return Fail(ERROR_CANCELLED,L"Client download canceled.");
        auto result=DownloadClientOnce(endpoint,key,directory,progress);
        if (result.error && diagnostic) diagnostic(result,attempt);
        if (!RetryableClientFailure(result) || attempt==3) return result;
        // Bounded backoff of 5 then 15 seconds, long enough to outlast a brief service
        // failure, checking cancellation every 100 ms. Never reuse partial bytes.
        for (unsigned tick=0; tick<(attempt==1 ? 50u : 150u); ++tick) {
            if (!progress(0)) return Fail(ERROR_CANCELLED,L"Client download canceled.");
            Sleep(100);
        }
    }
    return Fail(ERROR_UNHANDLED_EXCEPTION,L"Client download failed.");
}
