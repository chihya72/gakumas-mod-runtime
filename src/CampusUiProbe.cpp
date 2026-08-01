#include "gkmm/CampusUiProbe.hpp"

#include "gkmm/ManagerLog.hpp"
#include "gkmm/RuntimeClient.hpp"

#include <Windows.h>
#include <MinHook.h>

#include "../../gakumas-mod-runtime/src/deps/UnityResolve/UnityResolve.hpp"
#include "../../gakumas-mod-runtime/src/deps/nlohmann/json.hpp"

#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_set>

namespace GakumasModManager {
    namespace {
        // Home menu entry + Mod management sheet.
        //
        // Facts established by the read-only probe stage, recorded in
        // docs/SIGNATURE_MATRIX.md:
        //
        //   * the out-game menu is Campus.OutGame.OutGameMenuPresenter, whose
        //     _commonView is a Campus.Common.MenuView;
        //   * MenuView._buttons / ._subButtons are SerializableDictionary keyed
        //     by MenuButtonSerializeType, not MenuButtonType;
        //   * dictionary insertion is not used by the current experiment.  The
        //     cloned CampusButton is recognised by identity in OnClicked;
        //   * MenuButtonType 21..43 (minus 31/36/40) never appear out-game.

        // ClearCache, always present in the out-game sub button row.
        constexpr std::int32_t kTemplateButtonType = 40;

        using AfterInitFn = void(UNITY_CALLING_CONVENTION*)(void* self, void* method);
        using SetEventFn = void(UNITY_CALLING_CONVENTION*)(void* self, void* method);
        // OnSelected takes no arguments: the clicked entry is written to the
        // presenter's SelectedButtonType property first and read back here.
        using PressFn = void(UNITY_CALLING_CONVENTION*)(void* self, void* method);
        // OpenTransitionAsync returns UniTask -- a 16-byte struct, which the x64
        // ABI returns through a hidden pointer passed as the first argument,
        // shifting everything else along.  Take four slots so the forward is
        // correct either way and identify the instance by class at runtime
        // rather than betting on the layout.
        using OpenTransitionFn = void* (UNITY_CALLING_CONVENTION*)(void* a, void* b, void* c, void* d);

        using ObjectGetClassFn = void* (*)(void* obj);
        using ClassGetNameFn = const char* (*)(const void* klass);
        using ClassGetParentFn = void* (*)(void* klass);
        using ClassGetMethodsFn = const void* (*)(void* klass, void** iter);
        using MethodGetNameFn = const char* (*)(const void* method);
        using MethodGetParamCountFn = std::uint32_t(*)(const void* method);
        using MethodIsGenericFn = bool (*)(const void* method);
        using MethodFromNameFn = const void* (*)(void* klass, const char* name, int argc);
        using ClassInitFn = void (*)(void* klass);
        using RuntimeInvokeFn = void* (*)(const void* method, void* obj, void** params, void** exc);
        using ClassGetFieldsFn = void* (*)(void* klass, void** iter);
        using FieldGetNameFn = const char* (*)(void* field);
        using FieldGetTypeFn = const void* (*)(void* field);
        using FieldGetOffsetFn = std::size_t(*)(void* field);
        using MethodGetReturnTypeFn = const void* (*)(const void* method);
        using MethodGetParamFn = const void* (*)(const void* method, std::uint32_t index);
        using MethodGetFlagsFn = std::uint32_t(*)(const void* method, std::uint32_t* iflags);
        using TypeGetNameFn = char* (*)(const void* type);
        using Il2CppFreeFn = void (*)(void* ptr);

        std::atomic<bool> g_started{false};

        AfterInitFn g_afterInitOriginal{};
        SetEventFn g_setEventOriginal{};
        PressFn g_pressOriginal{};
        // The Button component on our injected entry, matched by identity.
        void* g_modButton{};
        OpenTransitionFn g_openTransitionOriginal{};

        ObjectGetClassFn g_objectGetClass{};
        ClassGetNameFn g_classGetName{};
        ClassGetParentFn g_classGetParent{};
        ClassGetMethodsFn g_classGetMethods{};
        MethodGetNameFn g_methodGetName{};
        MethodGetParamCountFn g_methodGetParamCount{};
        MethodIsGenericFn g_methodIsGeneric{};
        MethodFromNameFn g_methodFromName{};
        ClassInitFn g_classInit{};
        RuntimeInvokeFn g_runtimeInvoke{};

        UnityResolve::Class* g_menuPresenter{};
        UnityResolve::Field* g_commonViewField{};
        UnityResolve::Field* g_buttonField{};
        UnityResolve::Field* g_subButtonsField{};
        UnityResolve::Field* g_textField{};

        // MethodInfo* resolved by name + parameter count.  Deliberately not
        // UnityResolve::Method: its overload matcher falls back to "first method
        // with this name" when the argument type names do not match, which
        // silently hands out a method of the wrong arity.  That is what made the
        // first build call Instantiate(original, position, rotation) with two
        // arguments and take the game down.
        const void* g_getSubButton{};
        const void* g_setCustomText{};
        const void* g_cloneWithParent{};
        const void* g_getTransform{};
        const void* g_getParent{};
        const void* g_getCanvas{};
        const void* g_getGameObject{};
        const void* g_setActive{};
        const void* g_setSizeDelta{};
        const void* g_setAnchoredPosition{};
        const void* g_setAnchorMin{};
        const void* g_setAnchorMax{};
        const void* g_setPivot{};
        const void* g_setLocalScale{};
        const void* g_setAsLastSibling{};

        // Our own panel: a second clone of the same sub button, blown up and
        // parented to the menu canvas.  Reusing the entry's template means the
        // panel needs no new API surface -- no prefab, no async, no managed
        // delegate, and no cooperation from the game's screen stack.
        void* g_templateButton{};
        void* g_canvasTransform{};
        void* g_panelObject{};
        bool g_panelVisible{};

        struct Vector2 { float x, y; };
        struct Vector3 { float x, y, z; };

        std::atomic<bool> g_injectionFaulted{false};
        // Which EnsureEntry step was in flight, so a fault names its own cause
        // instead of leaving the whole function as the suspect.
        std::atomic<int> g_injectStep{0};
        // Set between "our entry was clicked" and "the error sheet presenter is
        // about to play its open transition", which is where the text goes in.
        std::mutex g_sheetTextMutex;
        std::string g_sheetTitle;
        std::string g_sheetBody;

        // ponytail: MenuView pointers we already injected into.  IL2CPP uses a
        // non-moving GC so the addresses are stable; if a view is destroyed and
        // its address reused we would skip one injection and the entry would be
        // missing, not corrupt.  Revisit if the entry ever fails to appear.
        std::unordered_set<void*> g_injectedViews;

        void LogF(const char* format, ...) {
            char message[1024]{};
            va_list args;
            va_start(args, format);
            std::vsnprintf(message, sizeof(message), format, args);
            va_end(args);
            Log(message);
        }

        const char* ClassNameOf(void* object) {
            if (!object || !g_objectGetClass || !g_classGetName) return "<unknown>";
            const auto klass = g_objectGetClass(object);
            const auto name = klass ? g_classGetName(klass) : nullptr;
            return name ? name : "<unknown>";
        }

        // Exact resolution: name plus parameter count, walking the base chain and
        // skipping open generic definitions (runtime_invoke on one of those
        // faults).  Where a name still has several same-arity overloads, pick a
        // uniquely named entry point instead of guessing here.
        const void* FindMethodInClass(void* klass, const char* name, std::uint32_t argc) {
            if (!klass) return nullptr;
            // On an inflated generic class, iterating the method table can hand
            // back a MethodInfo with no generic context, which faults the moment
            // it is invoked.  The runtime's own lookup sets the class up first
            // and returns the inflated method, so try that before iterating.
            if (g_classInit) g_classInit(klass);
            if (g_methodFromName) {
                if (const auto found = g_methodFromName(klass, name, static_cast<int>(argc))) {
                    return found;
                }
            }
            if (!g_classGetMethods) return nullptr;
            for (; klass; klass = g_classGetParent ? g_classGetParent(klass) : nullptr) {
                void* iterator{};
                while (const auto method = g_classGetMethods(klass, &iterator)) {
                    if (g_methodIsGeneric && g_methodIsGeneric(method)) continue;
                    if (std::strcmp(g_methodGetName(method), name) == 0
                        && g_methodGetParamCount(method) == argc) {
                        return method;
                    }
                }
            }
            return nullptr;
        }

        const void* FindMethodOnObject(void* object, const char* name, std::uint32_t argc) {
            return object && g_objectGetClass
                ? FindMethodInClass(g_objectGetClass(object), name, argc)
                : nullptr;
        }

        // UnityResolve's RuntimeInvoke passes &arg for every argument, which is
        // only correct for value types -- reference arguments end up as a pointer
        // to the pointer and the callee reads garbage without failing.  Pack the
        // array here instead: object pointers go in directly, value types go in
        // as the address of the local holding them.
        // MethodInfo::methodPointer is the first field.  IL2CPP is AOT with
        // stripping, so a method the game never calls can have a live MethodInfo
        // and no compiled body -- invoking that faults instead of failing.
        void* CompiledBodyOf(const void* method) {
            return method ? *reinterpret_cast<void* const*>(method) : nullptr;
        }

        void* Call(const void* method, void* instance, std::initializer_list<void*> params, const char* label) {
            if (!method || !g_runtimeInvoke) {
                LogF("Mod menu: %s is not resolved.", label);
                return nullptr;
            }
            if (!CompiledBodyOf(method)) {
                LogF("Mod menu: %s has no compiled body (stripped); not called.", label);
                return nullptr;
            }
            void* argv[8]{};
            std::size_t index = 0;
            for (const auto param : params) {
                if (index >= 8) break;
                argv[index++] = param;
            }
            void* exception{};
            const auto result = g_runtimeInvoke(method, instance, argv, &exception);
            if (exception) {
                LogF("Mod menu: %s raised %s.", label, ClassNameOf(exception));
                return nullptr;
            }
            return result;
        }

        std::string BuildSheetBody() {
            RuntimeClient runtime;
            std::string snapshot;
            if (!runtime.Connect() || !runtime.IsReady() || !runtime.GetModsJson(snapshot)) {
                return "无法读取 Mod 状态，请查看日志。";
            }

            nlohmann::json parsed;
            try {
                parsed = nlohmann::json::parse(snapshot);
            }
            catch (const std::exception&) {
                return "Mod 状态数据无法解析，请查看日志。";
            }

            const auto mods = parsed.value("mods", nlohmann::json::array());
            if (mods.empty()) return "暂未发现 Mod。";

            std::string body = "共 " + std::to_string(mods.size()) + " 个 Mod\n\n";
            bool restartNeeded = false;
            for (const auto& mod : mods) {
                const auto name = mod.value("name", std::string{"未命名 Mod"});
                const auto configured = mod.value("configuredEnabled", false);
                const auto registered = mod.value("registeredThisSession", false);
                const auto manifestState = mod.value("manifestState", std::string{});
                const auto kind = mod.contains("target") && mod["target"].is_object()
                    ? mod["target"].value("kind", std::string{})
                    : std::string{};

                const char* category = kind == "costume" ? "服装" : kind == "hair" ? "发型" : "未知类型";
                const char* status{};
                if (!manifestState.empty() && manifestState != "valid") status = "配置有误";
                else if (configured && registered) status = "启用中";
                else if (configured && !registered) status = "重启后启用";
                else if (!configured && registered) status = "重启后停用";
                else status = "已关闭";
                if (configured != registered && manifestState == "valid") restartNeeded = true;

                body += "· " + name + "    " + category + "    " + status + "\n";
            }
            if (restartNeeded) body += "\n设置将在重启游戏后生效。";
            return body;
        }

        // Our own panel instead of one of the game's sheets.
        //
        // Borrowing ErrorSheetManager.OpenAsync was the expensive path: it is
        // async, it wants an Action<ErrorSheetPresenter>, and passing null froze
        // the game.  Building the panel ourselves needs nothing the entry
        // injection has not already proven -- clone the same sub button, blow it
        // up, put the text in, toggle it.
        void TogglePanel() {
            if (g_panelObject) {
                g_panelVisible = !g_panelVisible;
                Call(g_setActive, g_panelObject, {&g_panelVisible}, "GameObject.SetActive");
                LogF("Mod menu: panel %s.", g_panelVisible ? "shown" : "hidden");
                return;
            }
            if (!g_templateButton || !g_canvasTransform) {
                Log("Mod menu: no template or canvas; panel not created.");
                return;
            }

            bool worldPositionStays = false;
            const auto panel = Call(g_cloneWithParent, nullptr,
                                    {g_templateButton, g_canvasTransform, &worldPositionStays},
                                    "clone panel");
            if (!panel) return;

            std::string text;
            {
                std::lock_guard lock(g_sheetTextMutex);
                text = g_sheetTitle + "\n\n" + g_sheetBody;
            }
            Call(g_setCustomText, panel, {UnityResolve::UnityType::String::New(text)},
                 "panel SetCustomText");

            if (const auto rect = Call(g_getTransform, panel, {}, "panel get_transform")) {
                Vector2 size{900.0f, 620.0f};
                Vector2 centre{0.0f, 0.0f};
                Vector2 centreAnchor{0.5f, 0.5f};
                Vector3 unitScale{1.0f, 1.0f, 1.0f};
                Call(g_setAnchorMin, rect, {&centreAnchor}, "panel set_anchorMin");
                Call(g_setAnchorMax, rect, {&centreAnchor}, "panel set_anchorMax");
                Call(g_setPivot, rect, {&centreAnchor}, "panel set_pivot");
                Call(g_setSizeDelta, rect, {&size}, "set_sizeDelta");
                Call(g_setAnchoredPosition, rect, {&centre}, "set_anchoredPosition");
                Call(g_setLocalScale, rect, {&unitScale}, "panel set_localScale");
                Call(g_setAsLastSibling, rect, {}, "panel SetAsLastSibling");
            }

            if (g_textField && g_textField->offset >= 0) {
                const auto textView = *reinterpret_cast<void**>(
                    static_cast<char*>(panel) + g_textField->offset);
                if (const auto textRect = textView
                    ? Call(g_getTransform, textView, {}, "panel text get_transform")
                    : nullptr) {
                    Vector2 stretchMin{0.0f, 0.0f};
                    Vector2 stretchMax{1.0f, 1.0f};
                    Vector2 centre{0.0f, 0.0f};
                    Vector2 inset{-80.0f, -80.0f};
                    Vector2 centrePivot{0.5f, 0.5f};
                    Vector3 unitScale{1.0f, 1.0f, 1.0f};
                    Call(g_setAnchorMin, textRect, {&stretchMin}, "panel text set_anchorMin");
                    Call(g_setAnchorMax, textRect, {&stretchMax}, "panel text set_anchorMax");
                    Call(g_setPivot, textRect, {&centrePivot}, "panel text set_pivot");
                    Call(g_setSizeDelta, textRect, {&inset}, "panel text set_sizeDelta");
                    Call(g_setAnchoredPosition, textRect, {&centre}, "panel text set_anchoredPosition");
                    Call(g_setLocalScale, textRect, {&unitScale}, "panel text set_localScale");
                }
            }

            g_panelObject = Call(g_getGameObject, panel, {}, "panel get_gameObject");
            g_panelVisible = true;
            if (g_panelObject) {
                Call(g_setActive, g_panelObject, {&g_panelVisible}, "panel SetActive");
            }
            LogF("Mod menu: panel created (%s), object=%s.",
                 ClassNameOf(panel), g_panelObject ? ClassNameOf(g_panelObject) : "<null>");
        }

        void GuardedTogglePanel() {
            __try {
                TogglePanel();
            }
            __except (EXCEPTION_EXECUTE_HANDLER) {
                g_panelObject = nullptr;
                Log("Mod menu: panel toggle faulted.");
            }
        }

        // Clone one existing sub button, retag it and register it under our own
        // key.  Runs before the original SetEvent so the game wires the click.
        void* EnsureEntry(void* presenter) {
            g_injectStep.store(1);
            if (!presenter || !g_commonViewField || g_commonViewField->offset < 0
                || !g_subButtonsField || g_subButtonsField->offset < 0
                || !g_buttonField || g_buttonField->offset < 0) {
                Log("Mod menu: required presenter/view fields are unavailable; entry not injected.");
                return nullptr;
            }
            const auto view = *reinterpret_cast<void**>(
                static_cast<char*>(presenter) + g_commonViewField->offset);
            if (!view) return nullptr;
            if (g_injectedViews.contains(view)) return nullptr;

            g_injectStep.store(2);
            const auto dictionary = *reinterpret_cast<void**>(
                static_cast<char*>(view) + g_subButtonsField->offset);
            if (!dictionary) {
                Log("Mod menu: MenuView._subButtons is null; entry not injected.");
                return nullptr;
            }
            LogF("Mod menu: step 2 view=%s dictionary=%s", ClassNameOf(view), ClassNameOf(dictionary));

            g_injectStep.store(3);
            std::int32_t templateType = kTemplateButtonType;
            const auto templateButton = Call(g_getSubButton, view, {&templateType},
                                             "MenuView.GetSubButton");
            if (!templateButton) {
                Log("Mod menu: no template sub button; entry not injected.");
                return nullptr;
            }
            LogF("Mod menu: step 3 template=%s", ClassNameOf(templateButton));
            const auto templateCampusButton = *reinterpret_cast<void**>(
                static_cast<char*>(templateButton) + g_buttonField->offset);
            if (!templateCampusButton) {
                Log("Mod menu: template has no CampusButton; entry not injected.");
                return nullptr;
            }

            // A fresh MenuView means the previous panel went with it.
            g_templateButton = templateButton;
            g_panelObject = nullptr;
            g_panelVisible = false;
            const auto canvas = Call(g_getCanvas, view, {}, "MenuView.get_Canvas");
            g_canvasTransform = canvas
                ? Call(g_getTransform, canvas, {}, "canvas get_transform")
                : nullptr;
            LogF("Mod menu: canvas=%s transform=%s",
                 canvas ? ClassNameOf(canvas) : "<null>",
                 g_canvasTransform ? ClassNameOf(g_canvasTransform) : "<null>");

            g_injectStep.store(4);
            const auto templateTransform = Call(g_getTransform, templateButton, {},
                                                "Component.get_transform");
            g_injectStep.store(5);
            const auto parent = templateTransform
                ? Call(g_getParent, templateTransform, {}, "Transform.get_parent")
                : nullptr;
            if (!parent) {
                Log("Mod menu: sub button parent not found; entry not injected.");
                return nullptr;
            }
            LogF("Mod menu: step 5 parent=%s", ClassNameOf(parent));

            // Object.Instantiate has several two-argument overloads, so go
            // straight to the uniquely named internal the parented overload
            // forwards to.  worldPositionStays=false keeps the layout group in
            // charge of placement.
            g_injectStep.store(6);
            bool worldPositionStays = false;
            const auto clone = Call(g_cloneWithParent, nullptr,
                                    {templateButton, parent, &worldPositionStays},
                                    "Object.Internal_CloneSingleWithParent");
            if (!clone) {
                Log("Mod menu: clone returned null; entry not injected.");
                return nullptr;
            }
            LogF("Mod menu: step 6 clone=%s", ClassNameOf(clone));

            // Registering the clone in MenuView._subButtons so the game's own
            // SetEvent would wire it turned out to be a dead end: every insertion
            // path faults, including a direct call to Dictionary.Add's compiled
            // body.  It is not needed anyway -- Unity's EventSystem drives the
            // cloned Button regardless of whether the menu knows about it, so we
            // recognise our own Button in Button.Press and skip the menu's
            // bookkeeping entirely.
            g_injectStep.store(7);
            g_modButton = *reinterpret_cast<void**>(
                static_cast<char*>(clone) + g_buttonField->offset);
            LogF("Mod menu: step 7 button=%s", g_modButton ? ClassNameOf(g_modButton) : "<null>");
            if (!g_modButton) {
                if (const auto cloneObject = Call(g_getGameObject, clone, {}, "clone get_gameObject")) {
                    bool active = false;
                    Call(g_setActive, cloneObject, {&active}, "disable inert clone");
                }
                Log("Mod menu: cloned entry has no CampusButton; injection disabled for this session.");
                g_injectionFaulted.store(true);
                return nullptr;
            }

            g_injectedViews.insert(view);
            LogF("Mod menu: entry injected into %s.", ClassNameOf(view));
            return clone;
        }

        void ApplyLabel(void* button) {
            if (!button) return;
            Call(g_setCustomText, button,
                 {UnityResolve::UnityType::String::New("Mod 管理")},
                 "MenuButtonViewBase.SetCustomText");
        }

        // ponytail: SEH around the two paths that drive live UI.  A wrong
        // signature here takes the whole game down, which is a terrible way to
        // find out; this turns it into a log line and disables the feature for
        // the session.  Drop the guard once the paths stop changing.
        void* GuardedEnsureEntry(void* presenter) {
            if (g_injectionFaulted.load()) return nullptr;
            __try {
                return EnsureEntry(presenter);
            }
            __except (EXCEPTION_EXECUTE_HANDLER) {
                g_injectionFaulted.store(true);
                LogF("Mod menu: entry injection faulted at step %d; disabled for this session.",
                     g_injectStep.load());
                return nullptr;
            }
        }

        void GuardedApplyLabel(void* button) {
            __try {
                ApplyLabel(button);
            }
            __except (EXCEPTION_EXECUTE_HANDLER) {
                Log("Mod menu: applying the entry label faulted.");
            }
        }

        void UNITY_CALLING_CONVENTION SetEventHook(void* self, void* method) {
            // Clone while the menu is being initialised, then re-apply the label
            // after the original call in case it restyles nearby entries.
            const auto injected = GuardedEnsureEntry(self);
            if (g_setEventOriginal) g_setEventOriginal(self, method);
            GuardedApplyLabel(injected);
        }

        void UNITY_CALLING_CONVENTION OnAfterInitializeHook(void* self, void* method) {
            if (g_afterInitOriginal) g_afterInitOriginal(self, method);
            LogF("Mod menu: OnAfterInitialize on %s.", ClassNameOf(self));
        }

        // Campus does not use UnityEngine.UI.Button: menu entries hold a
        // Campus.Common.CampusButton, and every click funnels through
        // CampusButtonBase.OnClicked, which CampusButton does not override.
        // Recognising ours by identity there needs no listener, no delegate and
        // no cooperation from the menu.
        void UNITY_CALLING_CONVENTION PressHook(void* self, void* method) {
            if (self && self == g_modButton) {
                Log("Mod menu: entry pressed.");
                GuardedTogglePanel();
                // The entry is cloned from ClearCache.  Forwarding this click to
                // the original CampusButtonBase.OnClicked would also execute the
                // template action, so our synthetic button must consume it.
                return;
            }
            if (g_pressOriginal) g_pressOriginal(self, method);
        }

        bool Install(UnityResolve::Method* target, void* detour, void** original, const char* label) {
            if (!target || !target->function) {
                LogF("Mod menu: %s was not found; that hook is skipped.", label);
                return false;
            }
            if (MH_CreateHook(target->function, detour, original) != MH_OK
                || MH_EnableHook(target->function) != MH_OK) {
                LogF("Mod menu: failed to install the %s hook.", label);
                return false;
            }
            return true;
        }

        void ProbeThread() {
            HMODULE gameAssembly{};
            for (int attempt = 0; attempt < 600 && !gameAssembly; ++attempt) {
                gameAssembly = GetModuleHandleW(L"GameAssembly.dll");
                if (!gameAssembly) std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            if (!gameAssembly) {
                Log("Mod menu: GameAssembly.dll was not found.");
                return;
            }

            const auto import = [gameAssembly](const char* name) {
                return GetProcAddress(gameAssembly, name);
            };
            g_objectGetClass = reinterpret_cast<ObjectGetClassFn>(import("il2cpp_object_get_class"));
            g_classGetName = reinterpret_cast<ClassGetNameFn>(import("il2cpp_class_get_name"));
            g_classGetParent = reinterpret_cast<ClassGetParentFn>(import("il2cpp_class_get_parent"));
            g_classGetMethods = reinterpret_cast<ClassGetMethodsFn>(import("il2cpp_class_get_methods"));
            g_methodGetName = reinterpret_cast<MethodGetNameFn>(import("il2cpp_method_get_name"));
            g_methodGetParamCount = reinterpret_cast<MethodGetParamCountFn>(
                import("il2cpp_method_get_param_count"));
            g_methodIsGeneric = reinterpret_cast<MethodIsGenericFn>(import("il2cpp_method_is_generic"));
            g_methodFromName = reinterpret_cast<MethodFromNameFn>(import("il2cpp_class_get_method_from_name"));
            g_classInit = reinterpret_cast<ClassInitFn>(import("il2cpp_runtime_class_init"));
            g_runtimeInvoke = reinterpret_cast<RuntimeInvokeFn>(import("il2cpp_runtime_invoke"));
            if (!g_objectGetClass || !g_classGetMethods || !g_methodGetName
                || !g_methodGetParamCount || !g_runtimeInvoke) {
                Log("Mod menu: required il2cpp exports are missing; entry disabled.");
                return;
            }

            UnityResolve::Init(gameAssembly, UnityResolve::Mode::Il2Cpp, false);
            UnityResolve::ThreadAttach();

            const auto assembly = UnityResolve::Get("Assembly-CSharp.dll");
            const auto core = UnityResolve::Get("UnityEngine.CoreModule.dll");
            g_menuPresenter = assembly ? assembly->Get("MenuPresenter", "Campus.Common") : nullptr;
            const auto menuView = assembly ? assembly->Get("MenuView", "Campus.Common") : nullptr;
            const auto buttonViewBase = assembly ? assembly->Get("MenuButtonViewBase", "Campus.Common") : nullptr;
            if (!g_menuPresenter || !menuView || !buttonViewBase) {
                LogF("Mod menu: required classes missing presenter=%d view=%d buttonView=%d; entry disabled.",
                     g_menuPresenter != nullptr, menuView != nullptr, buttonViewBase != nullptr);
                return;
            }
            g_commonViewField = g_menuPresenter->Get<UnityResolve::Field>("_commonView");
            g_subButtonsField = menuView->Get<UnityResolve::Field>("_subButtons");
            g_buttonField = buttonViewBase->Get<UnityResolve::Field>("_button");
            g_textField = buttonViewBase->Get<UnityResolve::Field>("_text");
            g_getSubButton = FindMethodInClass(menuView->address, "GetSubButton", 1);
            g_getCanvas = FindMethodInClass(menuView->address, "get_Canvas", 0);
            g_setCustomText = FindMethodInClass(buttonViewBase->address, "SetCustomText", 1);
            if (core) {
                const auto objectClass = core->Get("Object");
                const auto componentClass = core->Get("Component");
                const auto transformClass = core->Get("Transform");
                const auto gameObjectClass = core->Get("GameObject");
                const auto rectTransformClass = core->Get("RectTransform");
                g_cloneWithParent = objectClass
                    ? FindMethodInClass(objectClass->address, "Internal_CloneSingleWithParent", 3) : nullptr;
                g_getTransform = componentClass
                    ? FindMethodInClass(componentClass->address, "get_transform", 0) : nullptr;
                g_getGameObject = componentClass
                    ? FindMethodInClass(componentClass->address, "get_gameObject", 0) : nullptr;
                g_getParent = transformClass
                    ? FindMethodInClass(transformClass->address, "get_parent", 0) : nullptr;
                g_setLocalScale = transformClass
                    ? FindMethodInClass(transformClass->address, "set_localScale", 1) : nullptr;
                g_setAsLastSibling = transformClass
                    ? FindMethodInClass(transformClass->address, "SetAsLastSibling", 0) : nullptr;
                g_setActive = gameObjectClass
                    ? FindMethodInClass(gameObjectClass->address, "SetActive", 1) : nullptr;
                g_setSizeDelta = rectTransformClass
                    ? FindMethodInClass(rectTransformClass->address, "set_sizeDelta", 1) : nullptr;
                g_setAnchoredPosition = rectTransformClass
                    ? FindMethodInClass(rectTransformClass->address, "set_anchoredPosition", 1) : nullptr;
                g_setAnchorMin = rectTransformClass
                    ? FindMethodInClass(rectTransformClass->address, "set_anchorMin", 1) : nullptr;
                g_setAnchorMax = rectTransformClass
                    ? FindMethodInClass(rectTransformClass->address, "set_anchorMax", 1) : nullptr;
                g_setPivot = rectTransformClass
                    ? FindMethodInClass(rectTransformClass->address, "set_pivot", 1) : nullptr;
            }
            LogF("Mod menu: resolved commonView=%d subButtons=%d button=%d text=%d "
                  "getSubButton=%d customText=%d clone=%d transform=%d parent=%d "
                  "canvas=%d gameObject=%d setActive=%d sizeDelta=%d anchoredPos=%d "
                  "anchors=%d/%d pivot=%d scale=%d sibling=%d",
                  g_commonViewField != nullptr, g_subButtonsField != nullptr,
                  g_buttonField != nullptr, g_textField != nullptr,
                  g_getSubButton != nullptr, g_setCustomText != nullptr,
                  g_cloneWithParent != nullptr, g_getTransform != nullptr, g_getParent != nullptr,
                  g_getCanvas != nullptr, g_getGameObject != nullptr,
                  g_setActive != nullptr, g_setSizeDelta != nullptr,
                  g_setAnchoredPosition != nullptr,
                  g_setAnchorMin != nullptr, g_setAnchorMax != nullptr,
                  g_setPivot != nullptr, g_setLocalScale != nullptr,
                  g_setAsLastSibling != nullptr);
            LogF("Mod menu: field offsets commonView=0x%X subButtons=0x%X button=0x%X text=0x%X",
                 static_cast<unsigned>(g_commonViewField ? g_commonViewField->offset : -1),
                 static_cast<unsigned>(g_subButtonsField ? g_subButtonsField->offset : -1),
                 static_cast<unsigned>(g_buttonField ? g_buttonField->offset : -1),
                 static_cast<unsigned>(g_textField ? g_textField->offset : -1));

            if (!g_commonViewField || g_commonViewField->offset < 0
                || !g_subButtonsField || g_subButtonsField->offset < 0
                || !g_buttonField || g_buttonField->offset < 0
                || !g_textField || g_textField->offset < 0
                || !g_getSubButton || !g_setCustomText || !g_cloneWithParent
                || !g_getTransform || !g_getParent || !g_getCanvas
                || !g_getGameObject || !g_setActive || !g_setSizeDelta
                || !g_setAnchoredPosition || !g_setAnchorMin || !g_setAnchorMax
                || !g_setPivot || !g_setLocalScale || !g_setAsLastSibling) {
                Log("Mod menu: required entry symbols are incomplete; hooks not installed.");
                return;
            }

            const auto initStatus = MH_Initialize();
            if (initStatus != MH_OK && initStatus != MH_ERROR_ALREADY_INITIALIZED) {
                Log("Mod menu: MinHook initialization failed.");
                return;
            }

            Install(g_menuPresenter->Get<UnityResolve::Method>("SetEvent"),
                    reinterpret_cast<void*>(&SetEventHook),
                    reinterpret_cast<void**>(&g_setEventOriginal),
                    "MenuPresenter.SetEvent");
            Install(g_menuPresenter->Get<UnityResolve::Method>("OnAfterInitialize"),
                    reinterpret_cast<void*>(&OnAfterInitializeHook),
                    reinterpret_cast<void**>(&g_afterInitOriginal),
                    "MenuPresenter.OnAfterInitialize");
            // CampusButton/CampusButtonBase live in campus-submodule.Runtime.dll,
            // not Assembly-CSharp.dll -- looking them up in the wrong image is a
            // silent nullptr and a hook that never fires.
            const auto submodule = UnityResolve::Get("campus-submodule.Runtime.dll");
            const auto buttonBase = submodule
                ? submodule->Get("CampusButtonBase", "Campus.Common")
                : nullptr;
            LogF("Mod menu: campus-submodule=%d CampusButtonBase=%d",
                 submodule != nullptr, buttonBase != nullptr);
            Install(buttonBase ? buttonBase->Get<UnityResolve::Method>("OnClicked") : nullptr,
                    reinterpret_cast<void*>(&PressHook),
                    reinterpret_cast<void**>(&g_pressOriginal),
                    "CampusButtonBase.OnClicked");
            // Built here, on the worker thread: reading the Runtime snapshot is a
            // cross-DLL call with its own locking and has no business running
            // inside a click handler on Unity's main thread.
            {
                std::lock_guard lock(g_sheetTextMutex);
                g_sheetTitle = "Mod 管理";
                g_sheetBody = BuildSheetBody();
                LogF("Mod menu: panel text built on the worker thread (%zu bytes).",
                     g_sheetBody.size());
            }

            Log("Mod menu: hooks installed; open the home menu.");
        }
    }

    void StartCampusUiProbe() {
        if (g_started.exchange(true)) return;
        std::thread(ProbeThread).detach();
    }
}
