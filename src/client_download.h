#pragma once
#include <windows.h>
#include <functional>
#include <string>
#include <vector>

bool NormalizeDownloadUrl(const std::wstring& input, std::wstring& output);
bool ValidDownloadKey(const std::wstring& key);
bool PublicClientEndpoint(const std::wstring& endpoint);
std::wstring DeviceIdentityHash();
bool ReadDownloadKey(const std::wstring& endpoint, std::wstring& key);
bool SaveDownloadKey(const std::wstring& endpoint, const std::wstring& key);
std::wstring CheckClientDigest(const std::wstring& endpoint, const std::wstring& key);
void ForgetDownloadKey(const std::wstring& endpoint);
std::wstring ClientSha256(const std::vector<unsigned char>& bytes);
bool ValidateClientDownload(const std::vector<unsigned char>& bytes, const std::wstring& digest);
// Keeps the verified file open without write/delete sharing until loading completes.
class VerifiedClientFile {
    HANDLE handle = INVALID_HANDLE_VALUE;
public:
    VerifiedClientFile(const std::wstring& path, const std::wstring& digest);
    ~VerifiedClientFile();
    VerifiedClientFile(const VerifiedClientFile&) = delete;
    VerifiedClientFile& operator=(const VerifiedClientFile&) = delete;
    bool Valid() const { return handle != INVALID_HANDLE_VALUE; }
};
struct ClientDownloadResult {
    DWORD error = ERROR_SUCCESS;
    std::wstring message, path;
    DWORD httpStatus = 0;
    std::wstring requestId;
    std::wstring category;
    std::wstring digest;
};
bool RetryableClientFailure(const ClientDownloadResult& result);
// A short log-safe summary of an unlabelled error response; empty if it has no text.
std::wstring ErrorResponseDetail(const std::string& body);
ClientDownloadResult DownloadClient(const std::wstring& endpoint, const std::wstring& key,
    const std::wstring& directory, const std::function<bool(size_t)>& progress,
    const std::function<void(const ClientDownloadResult&,unsigned)>& diagnostic = {});
