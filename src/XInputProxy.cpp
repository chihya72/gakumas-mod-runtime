#include <Windows.h>

#define XInputGetState GkmmXInputGetStateImported
#define XInputSetState GkmmXInputSetStateImported
#define XInputGetCapabilities GkmmXInputGetCapabilitiesImported
#define XInputEnable GkmmXInputEnableImported
#define XInputGetDSoundAudioDeviceGuids GkmmXInputGetDSoundAudioDeviceGuidsImported
#define XInputGetBatteryInformation GkmmXInputGetBatteryInformationImported
#define XInputGetKeystroke GkmmXInputGetKeystrokeImported
#define XInputGetAudioDeviceIds GkmmXInputGetAudioDeviceIdsImported
#define _XINPUT_H_
#include <Xinput.h>
#undef _XINPUT_H_
#undef XInputGetState
#undef XInputSetState
#undef XInputGetCapabilities
#undef XInputEnable
#undef XInputGetDSoundAudioDeviceGuids
#undef XInputGetBatteryInformation
#undef XInputGetKeystroke
#undef XInputGetAudioDeviceIds

#include <string>

namespace {
    HMODULE OriginalXInput() {
        static HMODULE module = [] {
            wchar_t systemDirectory[MAX_PATH]{};
            const auto length = GetSystemDirectoryW(systemDirectory, MAX_PATH);
            if (length == 0 || length >= MAX_PATH) return static_cast<HMODULE>(nullptr);

            std::wstring path(systemDirectory, length);
            path += L"\\xinput1_4.dll";
            return LoadLibraryW(path.c_str());
        }();
        return module;
    }

    FARPROC Resolve(const char* name) {
        const auto module = OriginalXInput();
        return module ? GetProcAddress(module, name) : nullptr;
    }

    FARPROC ResolveOrdinal(WORD ordinal) {
        const auto module = OriginalXInput();
        return module ? GetProcAddress(module, MAKEINTRESOURCEA(ordinal)) : nullptr;
    }
}

extern "C" DWORD WINAPI XInputGetState(
    DWORD dwUserIndex,
    XINPUT_STATE* pState) noexcept {
    using Fn = decltype(&GkmmXInputGetStateImported);
    static const auto fn = reinterpret_cast<Fn>(Resolve("XInputGetState"));
    return fn ? fn(dwUserIndex, pState) : ERROR_DEVICE_NOT_CONNECTED;
}

extern "C" DWORD WINAPI XInputSetState(
    DWORD dwUserIndex,
    XINPUT_VIBRATION* pVibration) noexcept {
    using Fn = decltype(&GkmmXInputSetStateImported);
    static const auto fn = reinterpret_cast<Fn>(Resolve("XInputSetState"));
    return fn ? fn(dwUserIndex, pVibration) : ERROR_DEVICE_NOT_CONNECTED;
}

extern "C" DWORD WINAPI XInputGetCapabilities(
    DWORD dwUserIndex,
    DWORD dwFlags,
    XINPUT_CAPABILITIES* pCapabilities) noexcept {
    using Fn = decltype(&GkmmXInputGetCapabilitiesImported);
    static const auto fn = reinterpret_cast<Fn>(Resolve("XInputGetCapabilities"));
    return fn ? fn(dwUserIndex, dwFlags, pCapabilities) : ERROR_DEVICE_NOT_CONNECTED;
}

extern "C" void WINAPI XInputEnable(BOOL enable) noexcept {
    using Fn = decltype(&GkmmXInputEnableImported);
    static const auto fn = reinterpret_cast<Fn>(Resolve("XInputEnable"));
    if (fn) fn(enable);
}

extern "C" DWORD WINAPI XInputGetDSoundAudioDeviceGuids(
    DWORD dwUserIndex,
    GUID* pDSoundRenderGuid,
    GUID* pDSoundCaptureGuid) noexcept {
    using Fn = DWORD(WINAPI*)(DWORD, GUID*, GUID*);
    static const auto fn = reinterpret_cast<Fn>(Resolve("XInputGetDSoundAudioDeviceGuids"));
    return fn ? fn(dwUserIndex, pDSoundRenderGuid, pDSoundCaptureGuid) : ERROR_DEVICE_NOT_CONNECTED;
}

extern "C" DWORD WINAPI XInputGetBatteryInformation(
    DWORD dwUserIndex,
    BYTE devType,
    XINPUT_BATTERY_INFORMATION* pBatteryInformation) noexcept {
    using Fn = decltype(&GkmmXInputGetBatteryInformationImported);
    static const auto fn = reinterpret_cast<Fn>(Resolve("XInputGetBatteryInformation"));
    return fn ? fn(dwUserIndex, devType, pBatteryInformation) : ERROR_DEVICE_NOT_CONNECTED;
}

extern "C" DWORD WINAPI XInputGetKeystroke(
    DWORD dwUserIndex,
    DWORD dwReserved,
    PXINPUT_KEYSTROKE pKeystroke) noexcept {
    using Fn = decltype(&GkmmXInputGetKeystrokeImported);
    static const auto fn = reinterpret_cast<Fn>(Resolve("XInputGetKeystroke"));
    return fn ? fn(dwUserIndex, dwReserved, pKeystroke) : ERROR_EMPTY;
}

extern "C" DWORD WINAPI XInputGetAudioDeviceIds(
    DWORD dwUserIndex,
    LPWSTR pRenderDeviceId,
    UINT* pRenderCount,
    LPWSTR pCaptureDeviceId,
    UINT* pCaptureCount) noexcept {
    using Fn = decltype(&GkmmXInputGetAudioDeviceIdsImported);
    static const auto fn = reinterpret_cast<Fn>(Resolve("XInputGetAudioDeviceIds"));
    return fn ? fn(dwUserIndex, pRenderDeviceId, pRenderCount, pCaptureDeviceId, pCaptureCount)
        : ERROR_DEVICE_NOT_CONNECTED;
}

extern "C" DWORD WINAPI XInputGetStateEx(
    DWORD dwUserIndex,
    XINPUT_STATE* pState) noexcept {
    using Fn = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);
    static const auto fn = reinterpret_cast<Fn>(ResolveOrdinal(100));
    return fn ? fn(dwUserIndex, pState) : XInputGetState(dwUserIndex, pState);
}
