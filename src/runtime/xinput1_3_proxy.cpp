#include <Windows.h>
#include <Xinput.h>

#include "RuntimeBootstrap.hpp"

#include <string>

namespace {
    HMODULE GetOriginalXInput() {
        static HMODULE module = [] {
            char systemDir[MAX_PATH]{};
            GetSystemDirectoryA(systemDir, MAX_PATH);
            std::string path = std::string(systemDir) + "\\xinput1_3.dll";
            return LoadLibraryA(path.c_str());
        }();
        return module;
    }

    FARPROC ResolveOriginal(const char* name) {
        const auto module = GetOriginalXInput();
        return module ? GetProcAddress(module, name) : nullptr;
    }

    FARPROC ResolveOriginalOrdinal(const WORD ordinal) {
        const auto module = GetOriginalXInput();
        return module ? GetProcAddress(module, MAKEINTRESOURCEA(ordinal)) : nullptr;
    }
}

extern "C" DWORD WINAPI XInputGetState(_In_ DWORD dwUserIndex, _Out_ XINPUT_STATE* pState) noexcept {
    GakumasMod::Bootstrap::EnsureStarted();
    using Fn = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);
    static auto fn = reinterpret_cast<Fn>(ResolveOriginal("XInputGetState"));
    if (fn) return fn(dwUserIndex, pState);
    if (pState) *pState = {};
    return ERROR_DEVICE_NOT_CONNECTED;
}

extern "C" DWORD WINAPI XInputSetState(_In_ DWORD dwUserIndex, _In_ XINPUT_VIBRATION* pVibration) noexcept {
    GakumasMod::Bootstrap::EnsureStarted();
    using Fn = DWORD(WINAPI*)(DWORD, XINPUT_VIBRATION*);
    static auto fn = reinterpret_cast<Fn>(ResolveOriginal("XInputSetState"));
    return fn ? fn(dwUserIndex, pVibration) : ERROR_DEVICE_NOT_CONNECTED;
}

extern "C" DWORD WINAPI XInputGetCapabilities(
    _In_ DWORD dwUserIndex,
    _In_ DWORD dwFlags,
    _Out_ XINPUT_CAPABILITIES* pCapabilities) noexcept {
    GakumasMod::Bootstrap::EnsureStarted();
    using Fn = DWORD(WINAPI*)(DWORD, DWORD, XINPUT_CAPABILITIES*);
    static auto fn = reinterpret_cast<Fn>(ResolveOriginal("XInputGetCapabilities"));
    if (fn) return fn(dwUserIndex, dwFlags, pCapabilities);
    if (pCapabilities) *pCapabilities = {};
    return ERROR_DEVICE_NOT_CONNECTED;
}

extern "C" void WINAPI XInputEnable(_In_ BOOL enable) noexcept {
    GakumasMod::Bootstrap::EnsureStarted();
    using Fn = void(WINAPI*)(BOOL);
    static auto fn = reinterpret_cast<Fn>(ResolveOriginal("XInputEnable"));
    if (fn) fn(enable);
}

extern "C" DWORD WINAPI XInputGetDSoundAudioDeviceGuids(
    _In_ DWORD dwUserIndex,
    _Out_ GUID* pDSoundRenderGuid,
    _Out_ GUID* pDSoundCaptureGuid) noexcept {
    GakumasMod::Bootstrap::EnsureStarted();
    using Fn = DWORD(WINAPI*)(DWORD, GUID*, GUID*);
    static auto fn = reinterpret_cast<Fn>(ResolveOriginal("XInputGetDSoundAudioDeviceGuids"));
    if (fn) return fn(dwUserIndex, pDSoundRenderGuid, pDSoundCaptureGuid);
    if (pDSoundRenderGuid) *pDSoundRenderGuid = {};
    if (pDSoundCaptureGuid) *pDSoundCaptureGuid = {};
    return ERROR_DEVICE_NOT_CONNECTED;
}

extern "C" DWORD WINAPI XInputGetBatteryInformation(
    _In_ DWORD dwUserIndex,
    _In_ BYTE devType,
    _Out_ XINPUT_BATTERY_INFORMATION* pBatteryInformation) noexcept {
    GakumasMod::Bootstrap::EnsureStarted();
    using Fn = DWORD(WINAPI*)(DWORD, BYTE, XINPUT_BATTERY_INFORMATION*);
    static auto fn = reinterpret_cast<Fn>(ResolveOriginal("XInputGetBatteryInformation"));
    if (fn) return fn(dwUserIndex, devType, pBatteryInformation);
    if (pBatteryInformation) *pBatteryInformation = {};
    return ERROR_DEVICE_NOT_CONNECTED;
}

extern "C" DWORD WINAPI XInputGetKeystroke(
    _In_ DWORD dwUserIndex,
    _Reserved_ DWORD dwReserved,
    _Out_ PXINPUT_KEYSTROKE pKeystroke) noexcept {
    GakumasMod::Bootstrap::EnsureStarted();
    using Fn = DWORD(WINAPI*)(DWORD, DWORD, PXINPUT_KEYSTROKE);
    static auto fn = reinterpret_cast<Fn>(ResolveOriginal("XInputGetKeystroke"));
    if (fn) return fn(dwUserIndex, dwReserved, pKeystroke);
    if (pKeystroke) *pKeystroke = {};
    return ERROR_EMPTY;
}

extern "C" DWORD WINAPI XInputGetStateEx(_In_ DWORD dwUserIndex, _Out_ XINPUT_STATE* pState) noexcept {
    GakumasMod::Bootstrap::EnsureStarted();
    using Fn = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);
    static auto fn = reinterpret_cast<Fn>(ResolveOriginalOrdinal(100));
    return fn ? fn(dwUserIndex, pState) : XInputGetState(dwUserIndex, pState);
}
