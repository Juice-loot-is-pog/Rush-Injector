#include "client_download.h"
#include <bcrypt.h>
#include <iostream>

// Provision this PC without printing or exporting its secret download key.
int wmain(int argc, wchar_t** argv) {
    std::wstring endpoint;
    if (argc == 4 && std::wstring(argv[1]) == L"--verify-download") {
        if (!NormalizeDownloadUrl(argv[2],endpoint)) return 2;
        std::wstring key;
        if (!ReadDownloadKey(endpoint,key)) return 4;
        auto result = DownloadClient(endpoint,key,argv[3],[](size_t) { return true; });
        SecureZeroMemory(key.data(),key.size()*sizeof(wchar_t));
        std::wcout << result.message << std::endl;
        return result.error ? 1 : 0;
    }
    if (argc != 2 || !NormalizeDownloadUrl(argv[1],endpoint)) return 2;
    std::wstring key;
    if (!ReadDownloadKey(endpoint,key)) {
        unsigned char random[32];
        if (BCryptGenRandom(nullptr,random,sizeof(random),BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0) return 3;
        static constexpr wchar_t digits[] = L"0123456789abcdef";
        for (unsigned char value : random) { key += digits[value >> 4]; key += digits[value & 15]; }
        SecureZeroMemory(random,sizeof(random));
        if (!SaveDownloadKey(endpoint,key)) return 4;
    }
    std::vector<unsigned char> utf8(key.begin(),key.end());
    const std::wstring digest = ClientSha256(utf8);
    SecureZeroMemory(utf8.data(),utf8.size()); SecureZeroMemory(key.data(),key.size()*sizeof(wchar_t));
    if (digest.empty()) return 5;
    // Hashes are not bearer credentials. This JSON is the Worker secret's value.
    std::wcout << L"[\"" << digest << L"\"]" << std::endl;
    return 0;
}
