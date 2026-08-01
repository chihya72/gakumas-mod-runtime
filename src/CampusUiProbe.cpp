#include "gkmm/CampusUiProbe.hpp"

#include "gkmm/ManagerLog.hpp"

#include <Windows.h>
#include <MinHook.h>

#include "../../gakumas-mod-runtime/src/deps/UnityResolve/UnityResolve.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>

namespace GakumasModManager {
    namespace {
        using DeltaTimeFn = float(UNITY_CALLING_CONVENTION*)();

        std::atomic<bool> g_started{false};
        std::atomic<bool> g_probeFinished{false};
        std::atomic<std::uint32_t> g_frameCounter{0};
        std::atomic<bool> g_loggedPresenterReady{false};
        UnityResolve::Method* g_deltaTimeMethod{};
        DeltaTimeFn g_deltaTimeOriginal{};

        void DisableProbeHook() {
            if (g_deltaTimeMethod && g_deltaTimeMethod->function) {
                MH_DisableHook(g_deltaTimeMethod->function);
            }
        }

        bool TryOpenNoticeSheet() {
            const auto assembly = UnityResolve::Get("Assembly-CSharp.dll");
            if (!assembly) {
                Log("Campus UI probe: Assembly-CSharp.dll is not ready.");
                return false;
            }

            const auto presenterClass = assembly->Get("HomeTopScreenPresenter", "Campus.OutGame");
            if (!presenterClass) {
                Log("Campus UI probe: Campus.OutGame.HomeTopScreenPresenter not found.");
                return false;
            }

            const auto openNotice = presenterClass->Get<UnityResolve::Method>("OpenNoticeSheetAsync");
            if (!openNotice) {
                Log("Campus UI probe: HomeTopScreenPresenter.OpenNoticeSheetAsync not found.");
                g_probeFinished.store(true);
                DisableProbeHook();
                return false;
            }

            if (!g_loggedPresenterReady.exchange(true)) {
                Log("Campus UI probe: HomeTopScreenPresenter and OpenNoticeSheetAsync resolved; waiting for an active instance.");
            }

            const auto presenters = presenterClass->FindObjectsByType<void*>();
            if (presenters.empty()) {
                const auto frame = g_frameCounter.load();
                if ((frame % 300) == 0) {
                    Log("Campus UI probe: no active HomeTopScreenPresenter instance yet.");
                }
                return false;
            }

            for (const auto presenter : presenters) {
                if (!presenter) continue;
                // The async method returns a UniTask value. RuntimeInvoke<void>
                // lets IL2CPP handle that return value without guessing its ABI.
                openNotice->RuntimeInvoke<void>(presenter);
                Log("Campus UI probe: OpenNoticeSheetAsync invoked successfully.");
                g_probeFinished.store(true);
                DisableProbeHook();
                return true;
            }
            return false;
        }

        float UNITY_CALLING_CONVENTION DeltaTimeHook() {
            const auto result = g_deltaTimeOriginal ? g_deltaTimeOriginal() : 0.0f;
            if (g_probeFinished.load()) return result;

            const auto frame = g_frameCounter.fetch_add(1) + 1;
            if ((frame % 30) == 0) TryOpenNoticeSheet();
            // A cold login can take longer than a few seconds. At 60 FPS this
            // allows roughly ten minutes while still preventing a permanent hook.
            if (frame >= 36000 && !g_probeFinished.exchange(true)) {
                Log("Campus UI probe: home presenter was not found within the probe timeout.");
                DisableProbeHook();
            }
            return result;
        }

        void ProbeThread() {
            HMODULE gameAssembly{};
            for (int attempt = 0; attempt < 600 && !gameAssembly; ++attempt) {
                gameAssembly = GetModuleHandleW(L"GameAssembly.dll");
                if (!gameAssembly) std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            if (!gameAssembly) {
                Log("Campus UI probe: GameAssembly.dll was not found.");
                g_probeFinished.store(true);
                return;
            }

            UnityResolve::Init(gameAssembly, UnityResolve::Mode::Il2Cpp, false);
            UnityResolve::ThreadAttach();
            const auto coreAssembly = UnityResolve::Get("UnityEngine.CoreModule.dll");
            const auto timeClass = coreAssembly ? coreAssembly->Get("Time", "UnityEngine") : nullptr;
            g_deltaTimeMethod = timeClass
                ? timeClass->Get<UnityResolve::Method>("get_deltaTime")
                : nullptr;
            if (!g_deltaTimeMethod || !g_deltaTimeMethod->function) {
                Log("Campus UI probe: UnityEngine.Time.get_deltaTime was not found.");
                g_probeFinished.store(true);
                return;
            }

            const auto initStatus = MH_Initialize();
            if (initStatus != MH_OK && initStatus != MH_ERROR_ALREADY_INITIALIZED) {
                Log("Campus UI probe: MinHook initialization failed.");
                g_probeFinished.store(true);
                return;
            }
            if (MH_CreateHook(g_deltaTimeMethod->function,
                              reinterpret_cast<void*>(&DeltaTimeHook),
                              reinterpret_cast<void**>(&g_deltaTimeOriginal)) != MH_OK
                || MH_EnableHook(g_deltaTimeMethod->function) != MH_OK) {
                Log("Campus UI probe: failed to install Unity main-thread probe hook.");
                g_probeFinished.store(true);
                return;
            }
            Log("Campus UI probe: Unity main-thread hook installed; waiting for HomeTopScreenPresenter.");
        }
    }

    void StartCampusUiProbe() {
        if (g_started.exchange(true)) return;
        std::thread(ProbeThread).detach();
    }
}
