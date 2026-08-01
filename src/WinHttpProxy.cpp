#include <Windows.h>

#define WinHttpOpen GkmmWinHttpOpenImported
#define WinHttpConnect GkmmWinHttpConnectImported
#define WinHttpOpenRequest GkmmWinHttpOpenRequestImported
#define WinHttpSetTimeouts GkmmWinHttpSetTimeoutsImported
#define WinHttpSetOption GkmmWinHttpSetOptionImported
#define WinHttpQueryOption GkmmWinHttpQueryOptionImported
#define WinHttpQueryDataAvailable GkmmWinHttpQueryDataAvailableImported
#define WinHttpReadData GkmmWinHttpReadDataImported
#define WinHttpWriteData GkmmWinHttpWriteDataImported
#define WinHttpReceiveResponse GkmmWinHttpReceiveResponseImported
#define WinHttpQueryHeaders GkmmWinHttpQueryHeadersImported
#define WinHttpSendRequest GkmmWinHttpSendRequestImported
#define WinHttpSetCredentials GkmmWinHttpSetCredentialsImported
#define WinHttpQueryAuthSchemes GkmmWinHttpQueryAuthSchemesImported
#define WinHttpAddRequestHeaders GkmmWinHttpAddRequestHeadersImported
#define WinHttpSetStatusCallback GkmmWinHttpSetStatusCallbackImported
#define WinHttpGetDefaultProxyConfiguration GkmmWinHttpGetDefaultProxyConfigurationImported
#define WinHttpCrackUrl GkmmWinHttpCrackUrlImported
#define WinHttpCreateProxyResolver GkmmWinHttpCreateProxyResolverImported
#define WinHttpGetProxyForUrlEx GkmmWinHttpGetProxyForUrlExImported
#define WinHttpGetProxyResult GkmmWinHttpGetProxyResultImported
#define WinHttpFreeProxyResult GkmmWinHttpFreeProxyResultImported
#define WinHttpGetIEProxyConfigForCurrentUser GkmmWinHttpGetIEProxyConfigForCurrentUserImported
#define WinHttpGetProxyForUrl GkmmWinHttpGetProxyForUrlImported
#define WinHttpCloseHandle GkmmWinHttpCloseHandleImported
#define _WINHTTP_INTERNAL_
#include <winhttp.h>
#undef _WINHTTP_INTERNAL_
#undef WinHttpOpen
#undef WinHttpConnect
#undef WinHttpOpenRequest
#undef WinHttpSetTimeouts
#undef WinHttpSetOption
#undef WinHttpQueryOption
#undef WinHttpQueryDataAvailable
#undef WinHttpReadData
#undef WinHttpWriteData
#undef WinHttpReceiveResponse
#undef WinHttpQueryHeaders
#undef WinHttpSendRequest
#undef WinHttpSetCredentials
#undef WinHttpQueryAuthSchemes
#undef WinHttpAddRequestHeaders
#undef WinHttpSetStatusCallback
#undef WinHttpGetDefaultProxyConfiguration
#undef WinHttpCrackUrl
#undef WinHttpCreateProxyResolver
#undef WinHttpGetProxyForUrlEx
#undef WinHttpGetProxyResult
#undef WinHttpFreeProxyResult
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

extern "C" __declspec(dllexport) BOOL WINAPI WinHttpCrackUrl(
    LPCWSTR pwszUrl,
    DWORD dwUrlLength,
    DWORD dwFlags,
    LPURL_COMPONENTS lpUrlComponents) {
    using Fn = decltype(&GkmmWinHttpCrackUrlImported);
    static const auto fn = reinterpret_cast<Fn>(Resolve("WinHttpCrackUrl"));
    return fn ? fn(pwszUrl, dwUrlLength, dwFlags, lpUrlComponents) : FALSE;
}

extern "C" __declspec(dllexport) DWORD WINAPI WinHttpCreateProxyResolver(
    HINTERNET hSession,
    HINTERNET* phResolver) {
    using Fn = decltype(&GkmmWinHttpCreateProxyResolverImported);
    static const auto fn = reinterpret_cast<Fn>(Resolve("WinHttpCreateProxyResolver"));
    return fn ? fn(hSession, phResolver) : ERROR_CALL_NOT_IMPLEMENTED;
}

extern "C" __declspec(dllexport) DWORD WINAPI WinHttpGetProxyForUrlEx(
    HINTERNET hResolver,
    PCWSTR pcwszUrl,
    WINHTTP_AUTOPROXY_OPTIONS* pAutoProxyOptions,
    DWORD_PTR pContext) {
    using Fn = decltype(&GkmmWinHttpGetProxyForUrlExImported);
    static const auto fn = reinterpret_cast<Fn>(Resolve("WinHttpGetProxyForUrlEx"));
    return fn ? fn(hResolver, pcwszUrl, pAutoProxyOptions, pContext) : ERROR_CALL_NOT_IMPLEMENTED;
}

extern "C" __declspec(dllexport) DWORD WINAPI WinHttpGetProxyResult(
    HINTERNET hResolver,
    WINHTTP_PROXY_RESULT* pProxyResult) {
    using Fn = decltype(&GkmmWinHttpGetProxyResultImported);
    static const auto fn = reinterpret_cast<Fn>(Resolve("WinHttpGetProxyResult"));
    return fn ? fn(hResolver, pProxyResult) : ERROR_CALL_NOT_IMPLEMENTED;
}

extern "C" __declspec(dllexport) VOID WINAPI WinHttpFreeProxyResult(
    WINHTTP_PROXY_RESULT* pProxyResult) {
    using Fn = decltype(&GkmmWinHttpFreeProxyResultImported);
    static const auto fn = reinterpret_cast<Fn>(Resolve("WinHttpFreeProxyResult"));
    if (fn) fn(pProxyResult);
}

extern "C" __declspec(dllexport) HINTERNET WINAPI WinHttpConnect(
    HINTERNET hSession,
    LPCWSTR pswzServerName,
    INTERNET_PORT nServerPort,
    DWORD dwReserved) {
    using Fn = decltype(&GkmmWinHttpConnectImported);
    static const auto fn = reinterpret_cast<Fn>(Resolve("WinHttpConnect"));
    return fn ? fn(hSession, pswzServerName, nServerPort, dwReserved) : nullptr;
}

extern "C" __declspec(dllexport) HINTERNET WINAPI WinHttpOpenRequest(
    HINTERNET hConnect,
    LPCWSTR pwszVerb,
    LPCWSTR pwszObjectName,
    LPCWSTR pwszVersion,
    LPCWSTR pwszReferrer,
    LPCWSTR* ppwszAcceptTypes,
    DWORD dwFlags) {
    using Fn = decltype(&GkmmWinHttpOpenRequestImported);
    static const auto fn = reinterpret_cast<Fn>(Resolve("WinHttpOpenRequest"));
    return fn ? fn(hConnect, pwszVerb, pwszObjectName, pwszVersion, pwszReferrer,
        ppwszAcceptTypes, dwFlags) : nullptr;
}

extern "C" __declspec(dllexport) BOOL WINAPI WinHttpSetTimeouts(
    HINTERNET hInternet,
    int nResolveTimeout,
    int nConnectTimeout,
    int nSendTimeout,
    int nReceiveTimeout) {
    using Fn = decltype(&GkmmWinHttpSetTimeoutsImported);
    static const auto fn = reinterpret_cast<Fn>(Resolve("WinHttpSetTimeouts"));
    return fn ? fn(hInternet, nResolveTimeout, nConnectTimeout, nSendTimeout, nReceiveTimeout) : FALSE;
}

extern "C" __declspec(dllexport) BOOL WINAPI WinHttpSetOption(
    HINTERNET hInternet,
    DWORD dwOption,
    LPVOID lpBuffer,
    DWORD dwBufferLength) {
    using Fn = decltype(&GkmmWinHttpSetOptionImported);
    static const auto fn = reinterpret_cast<Fn>(Resolve("WinHttpSetOption"));
    return fn ? fn(hInternet, dwOption, lpBuffer, dwBufferLength) : FALSE;
}

extern "C" __declspec(dllexport) BOOL WINAPI WinHttpQueryOption(
    HINTERNET hInternet,
    DWORD dwOption,
    LPVOID lpBuffer,
    LPDWORD lpdwBufferLength) {
    using Fn = decltype(&GkmmWinHttpQueryOptionImported);
    static const auto fn = reinterpret_cast<Fn>(Resolve("WinHttpQueryOption"));
    return fn ? fn(hInternet, dwOption, lpBuffer, lpdwBufferLength) : FALSE;
}

extern "C" __declspec(dllexport) BOOL WINAPI WinHttpQueryDataAvailable(
    HINTERNET hRequest,
    LPDWORD lpdwNumberOfBytesAvailable) {
    using Fn = decltype(&GkmmWinHttpQueryDataAvailableImported);
    static const auto fn = reinterpret_cast<Fn>(Resolve("WinHttpQueryDataAvailable"));
    return fn ? fn(hRequest, lpdwNumberOfBytesAvailable) : FALSE;
}

extern "C" __declspec(dllexport) BOOL WINAPI WinHttpReadData(
    HINTERNET hRequest,
    LPVOID lpBuffer,
    DWORD dwNumberOfBytesToRead,
    LPDWORD lpdwNumberOfBytesRead) {
    using Fn = decltype(&GkmmWinHttpReadDataImported);
    static const auto fn = reinterpret_cast<Fn>(Resolve("WinHttpReadData"));
    return fn ? fn(hRequest, lpBuffer, dwNumberOfBytesToRead, lpdwNumberOfBytesRead) : FALSE;
}

extern "C" __declspec(dllexport) BOOL WINAPI WinHttpWriteData(
    HINTERNET hRequest,
    LPCVOID lpBuffer,
    DWORD dwNumberOfBytesToWrite,
    LPDWORD lpdwNumberOfBytesWritten) {
    using Fn = decltype(&GkmmWinHttpWriteDataImported);
    static const auto fn = reinterpret_cast<Fn>(Resolve("WinHttpWriteData"));
    return fn ? fn(hRequest, lpBuffer, dwNumberOfBytesToWrite, lpdwNumberOfBytesWritten) : FALSE;
}

extern "C" __declspec(dllexport) BOOL WINAPI WinHttpReceiveResponse(
    HINTERNET hRequest,
    LPVOID lpReserved) {
    using Fn = decltype(&GkmmWinHttpReceiveResponseImported);
    static const auto fn = reinterpret_cast<Fn>(Resolve("WinHttpReceiveResponse"));
    return fn ? fn(hRequest, lpReserved) : FALSE;
}

extern "C" __declspec(dllexport) BOOL WINAPI WinHttpQueryHeaders(
    HINTERNET hRequest,
    DWORD dwInfoLevel,
    LPCWSTR pwszName,
    LPVOID lpBuffer,
    LPDWORD lpdwBufferLength,
    LPDWORD lpdwIndex) {
    using Fn = decltype(&GkmmWinHttpQueryHeadersImported);
    static const auto fn = reinterpret_cast<Fn>(Resolve("WinHttpQueryHeaders"));
    return fn ? fn(hRequest, dwInfoLevel, pwszName, lpBuffer, lpdwBufferLength, lpdwIndex) : FALSE;
}

extern "C" __declspec(dllexport) BOOL WINAPI WinHttpSendRequest(
    HINTERNET hRequest,
    LPCWSTR lpszHeaders,
    DWORD dwHeadersLength,
    LPVOID lpOptional,
    DWORD dwOptionalLength,
    DWORD dwTotalLength,
    DWORD_PTR dwContext) {
    using Fn = decltype(&GkmmWinHttpSendRequestImported);
    static const auto fn = reinterpret_cast<Fn>(Resolve("WinHttpSendRequest"));
    return fn ? fn(hRequest, lpszHeaders, dwHeadersLength, lpOptional, dwOptionalLength,
        dwTotalLength, dwContext) : FALSE;
}

extern "C" __declspec(dllexport) BOOL WINAPI WinHttpSetCredentials(
    HINTERNET hRequest,
    DWORD AuthTargets,
    DWORD AuthScheme,
    LPCWSTR pwszUserName,
    LPCWSTR pwszPassword,
    LPVOID pAuthParams) {
    using Fn = decltype(&GkmmWinHttpSetCredentialsImported);
    static const auto fn = reinterpret_cast<Fn>(Resolve("WinHttpSetCredentials"));
    return fn ? fn(hRequest, AuthTargets, AuthScheme, pwszUserName, pwszPassword, pAuthParams) : FALSE;
}

extern "C" __declspec(dllexport) BOOL WINAPI WinHttpQueryAuthSchemes(
    HINTERNET hRequest,
    LPDWORD lpdwSupportedSchemes,
    LPDWORD lpdwFirstScheme,
    LPDWORD pdwAuthTarget) {
    using Fn = decltype(&GkmmWinHttpQueryAuthSchemesImported);
    static const auto fn = reinterpret_cast<Fn>(Resolve("WinHttpQueryAuthSchemes"));
    return fn ? fn(hRequest, lpdwSupportedSchemes, lpdwFirstScheme, pdwAuthTarget) : FALSE;
}

extern "C" __declspec(dllexport) BOOL WINAPI WinHttpAddRequestHeaders(
    HINTERNET hRequest,
    LPCWSTR lpszHeaders,
    DWORD dwHeadersLength,
    DWORD dwModifiers) {
    using Fn = decltype(&GkmmWinHttpAddRequestHeadersImported);
    static const auto fn = reinterpret_cast<Fn>(Resolve("WinHttpAddRequestHeaders"));
    return fn ? fn(hRequest, lpszHeaders, dwHeadersLength, dwModifiers) : FALSE;
}

extern "C" __declspec(dllexport) WINHTTP_STATUS_CALLBACK WINAPI WinHttpSetStatusCallback(
    HINTERNET hInternet,
    WINHTTP_STATUS_CALLBACK lpfnInternetCallback,
    DWORD dwNotificationFlags,
    DWORD_PTR dwReserved) {
    using Fn = decltype(&GkmmWinHttpSetStatusCallbackImported);
    static const auto fn = reinterpret_cast<Fn>(Resolve("WinHttpSetStatusCallback"));
    return fn ? fn(hInternet, lpfnInternetCallback, dwNotificationFlags, dwReserved) : nullptr;
}

extern "C" __declspec(dllexport) BOOL WINAPI WinHttpGetDefaultProxyConfiguration(
    WINHTTP_PROXY_INFO* pProxyInfo) {
    using Fn = decltype(&GkmmWinHttpGetDefaultProxyConfigurationImported);
    static const auto fn = reinterpret_cast<Fn>(Resolve("WinHttpGetDefaultProxyConfiguration"));
    return fn ? fn(pProxyInfo) : FALSE;
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
