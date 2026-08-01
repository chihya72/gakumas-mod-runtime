#include <Windows.h>

#define DwmDefWindowProc GkmmDwmDefWindowProcImported
#define DwmExtendFrameIntoClientArea GkmmDwmExtendFrameIntoClientAreaImported
#define DwmGetCompositionTimingInfo GkmmDwmGetCompositionTimingInfoImported
#define DwmGetWindowAttribute GkmmDwmGetWindowAttributeImported
#define DwmSetWindowAttribute GkmmDwmSetWindowAttributeImported
#define _DWMAPI_
#include <dwmapi.h>
#undef _DWMAPI_
#undef DwmDefWindowProc
#undef DwmExtendFrameIntoClientArea
#undef DwmGetCompositionTimingInfo
#undef DwmGetWindowAttribute
#undef DwmSetWindowAttribute

#include <string>

namespace {
    HMODULE OriginalDwmApi() {
        static HMODULE module = [] {
            wchar_t systemDirectory[MAX_PATH]{};
            const auto length = GetSystemDirectoryW(systemDirectory, MAX_PATH);
            if (length == 0 || length >= MAX_PATH) return static_cast<HMODULE>(nullptr);

            std::wstring path(systemDirectory, length);
            path += L"\\dwmapi.dll";
            return LoadLibraryW(path.c_str());
        }();
        return module;
    }

    FARPROC Resolve(const char* name) {
        const auto module = OriginalDwmApi();
        return module ? GetProcAddress(module, name) : nullptr;
    }
}

extern "C" __declspec(dllexport) BOOL WINAPI DwmDefWindowProc(
    HWND hWnd,
    UINT msg,
    WPARAM wParam,
    LPARAM lParam,
    LRESULT* plResult) {
    using Fn = decltype(&GkmmDwmDefWindowProcImported);
    static const auto fn = reinterpret_cast<Fn>(Resolve("DwmDefWindowProc"));
    return fn ? fn(hWnd, msg, wParam, lParam, plResult) : FALSE;
}

extern "C" __declspec(dllexport) HRESULT WINAPI DwmExtendFrameIntoClientArea(
    HWND hWnd,
    const MARGINS* pMarInset) {
    using Fn = decltype(&GkmmDwmExtendFrameIntoClientAreaImported);
    static const auto fn = reinterpret_cast<Fn>(Resolve("DwmExtendFrameIntoClientArea"));
    return fn ? fn(hWnd, pMarInset) : E_NOTIMPL;
}

extern "C" __declspec(dllexport) HRESULT WINAPI DwmGetCompositionTimingInfo(
    HWND hwnd,
    DWM_TIMING_INFO* pTimingInfo) {
    using Fn = decltype(&GkmmDwmGetCompositionTimingInfoImported);
    static const auto fn = reinterpret_cast<Fn>(Resolve("DwmGetCompositionTimingInfo"));
    return fn ? fn(hwnd, pTimingInfo) : E_NOTIMPL;
}

extern "C" __declspec(dllexport) HRESULT WINAPI DwmGetWindowAttribute(
    HWND hwnd,
    DWORD dwAttribute,
    PVOID pvAttribute,
    DWORD cbAttribute) {
    using Fn = decltype(&GkmmDwmGetWindowAttributeImported);
    static const auto fn = reinterpret_cast<Fn>(Resolve("DwmGetWindowAttribute"));
    return fn ? fn(hwnd, dwAttribute, pvAttribute, cbAttribute) : E_NOTIMPL;
}

extern "C" __declspec(dllexport) HRESULT WINAPI DwmSetWindowAttribute(
    HWND hwnd,
    DWORD dwAttribute,
    LPCVOID pvAttribute,
    DWORD cbAttribute) {
    using Fn = decltype(&GkmmDwmSetWindowAttributeImported);
    static const auto fn = reinterpret_cast<Fn>(Resolve("DwmSetWindowAttribute"));
    return fn ? fn(hwnd, dwAttribute, pvAttribute, cbAttribute) : E_NOTIMPL;
}
