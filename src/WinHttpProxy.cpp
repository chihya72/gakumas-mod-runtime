#include <Windows.h>

#define WinHttpOpen GkmmWinHttpOpenImported
#define WinHttpGetIEProxyConfigForCurrentUser GkmmWinHttpGetIEProxyConfigForCurrentUserImported
#define WinHttpGetProxyForUrl GkmmWinHttpGetProxyForUrlImported
#define WinHttpCloseHandle GkmmWinHttpCloseHandleImported
#define _WINHTTP_INTERNAL_
#include <winhttp.h>
#undef _WINHTTP_INTERNAL_
#undef WinHttpOpen
#undef WinHttpGetIEProxyConfigForCurrentUser
#undef WinHttpGetProxyForUrl
#undef WinHttpCloseHandle

#include <string>

namespace {
    HMODULE OriginalWinHttp() {
        static HMODULE module = [] {
            wchar_t systemDirectory[MAX_PATH]{};
            const auto length = GetSystemDirectoryW(systemDirectory, MAX_PATH);
            if (length == 0 || length >= MAX_PATH) return static_cast<HMODULE>(nullptr);

            std::wstring path(systemDirectory, length);
            path += L"\\winhttp.dll";
            return LoadLibraryW(path.c_str());
        }();
        return module;
    }

    FARPROC Resolve(const char* name) {
        const auto module = OriginalWinHttp();
        return module ? GetProcAddress(module, name) : nullptr;
    }
}

extern "C" __declspec(dllexport) HINTERNET WINAPI WinHttpOpen(
    LPCWSTR pszAgentW,
    DWORD dwAccessType,
    LPCWSTR pszProxyW,
    LPCWSTR pszProxyBypassW,
    DWORD dwFlags) {
    using Fn = decltype(&GkmmWinHttpOpenImported);
    static const auto fn = reinterpret_cast<Fn>(Resolve("WinHttpOpen"));
    return fn ? fn(pszAgentW, dwAccessType, pszProxyW, pszProxyBypassW, dwFlags) : nullptr;
}

extern "C" __declspec(dllexport) BOOL WINAPI WinHttpGetIEProxyConfigForCurrentUser(
    WINHTTP_CURRENT_USER_IE_PROXY_CONFIG* pProxyConfig) {
    using Fn = decltype(&GkmmWinHttpGetIEProxyConfigForCurrentUserImported);
    static const auto fn = reinterpret_cast<Fn>(Resolve("WinHttpGetIEProxyConfigForCurrentUser"));
    return fn ? fn(pProxyConfig) : FALSE;
}

extern "C" __declspec(dllexport) BOOL WINAPI WinHttpGetProxyForUrl(
    HINTERNET hSession,
    LPCWSTR lpcwszUrl,
    WINHTTP_AUTOPROXY_OPTIONS* pAutoProxyOptions,
    WINHTTP_PROXY_INFO* pProxyInfo) {
    using Fn = decltype(&GkmmWinHttpGetProxyForUrlImported);
    static const auto fn = reinterpret_cast<Fn>(Resolve("WinHttpGetProxyForUrl"));
    return fn ? fn(hSession, lpcwszUrl, pAutoProxyOptions, pProxyInfo) : FALSE;
}

extern "C" __declspec(dllexport) BOOL WINAPI WinHttpCloseHandle(HINTERNET hInternet) {
    using Fn = decltype(&GkmmWinHttpCloseHandleImported);
    static const auto fn = reinterpret_cast<Fn>(Resolve("WinHttpCloseHandle"));
    return fn ? fn(hInternet) : FALSE;
}
