#include "gkmm/CampusUiProbe.hpp"

#include "gkmm/ManagerLog.hpp"
#include "gkmm/ModPresentationModel.hpp"
#include "gkmm/RuntimeClient.hpp"
#include "gkmm/RuntimeModSnapshot.hpp"

#include <Windows.h>
#include <MinHook.h>

#include "../../src/deps/UnityResolve/UnityResolve.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

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

        // ClearCache, always present in the out-game sub button row.  Only its
        // slot and wiring are wanted; the trash-can icon it ships with is
        // replaced below by the Costume icon, which is what this entry manages.
        constexpr std::int32_t kTemplateButtonType = 40;
        constexpr std::int32_t kEntryIconButtonType = 9;  // MenuButtonType.Costume

        using AfterInitFn = void(UNITY_CALLING_CONVENTION*)(void* self, void* method);
        using SetEventFn = void(UNITY_CALLING_CONVENTION*)(void* self, void* method);
        using SettingSetEventFn = void(UNITY_CALLING_CONVENTION*)(void* self, void* method);
        using EventSystemUpdateFn = void(UNITY_CALLING_CONVENTION*)(void* self, void* method);
        // OnSelected takes no arguments: the clicked entry is written to the
        // presenter's SelectedButtonType property first and read back here.
        using PressFn = void(UNITY_CALLING_CONVENTION*)(void* self, void* method);
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
        using ClassGetTypeFn = const void* (*)(void* klass);
        using TypeGetObjectFn = void* (*)(const void* type);
        using ObjectUnboxFn = void* (*)(void* object);

        std::atomic<bool> g_started{false};

        AfterInitFn g_afterInitOriginal{};
        SetEventFn g_setEventOriginal{};
        SettingSetEventFn g_settingSetEventOriginal{};
        EventSystemUpdateFn g_eventSystemUpdateOriginal{};
        PressFn g_pressOriginal{};
        // The Button component on our injected entry, matched by identity.
        void* g_modButton{};
        // The current out-game menu presenter and its real Setting button.  The
        // button is retained as a route-presence check; navigation itself is
        // dispatched through SelectedButtonType + OutGameMenuPresenter.OnSelected.
        void* g_menuPresenterInstance{};
        void* g_menuViewInstance{};
        void* g_settingButton{};
        void* g_menuCloseButton{};
        // Captured from OutGameFooterPresenter.SetEvent: the footer owns the
        // global menu layer and is the only object that can close it.
        void* g_footerPresenter{};
        const void* g_closeGlobalMenu{};
        SetEventFn g_footerSetEventOriginal{};

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
        MethodGetParamFn g_methodGetParam{};
        TypeGetNameFn g_typeGetName{};
        Il2CppFreeFn g_il2cppFree{};
        ClassGetTypeFn g_classGetType{};
        TypeGetObjectFn g_typeGetObject{};
        ObjectUnboxFn g_objectUnbox{};

        UnityResolve::Class* g_menuPresenter{};
        UnityResolve::Field* g_commonViewField{};
        UnityResolve::Field* g_buttonField{};
        UnityResolve::Field* g_subButtonsField{};
        UnityResolve::Field* g_textField{};
        UnityResolve::Field* g_menuCloseButtonField{};
        UnityResolve::Field* g_settingTabField{};
        UnityResolve::Field* g_preferencePageViewField{};
        UnityResolve::Field* g_vsyncToggleField{};
        UnityResolve::Field* g_switchInnerButtonField{};
        UnityResolve::Field* g_overlayTitleViewField{};
        UnityResolve::Field* g_menuButtonIconField{};
        UnityResolve::Field* g_menuButtonIconSettingField{};

        // MethodInfo* resolved by name + parameter count.  Deliberately not
        // UnityResolve::Method: its overload matcher falls back to "first method
        // with this name" when the argument type names do not match, which
        // silently hands out a method of the wrong arity.  That is what made the
        // first build call Instantiate(original, position, rotation) with two
        // arguments and take the game down.
        const void* g_getSubButton{};
        const void* g_getButton{};
        const void* g_setCustomText{};
        const void* g_menuIconSettingGetSprite{};
        const void* g_imageSetSprite{};
        const void* g_setSelectedButtonType{};
        const void* g_outGameOnSelected{};
        const void* g_reloadSettingScreen{};
        const void* g_cloneWithParent{};
        const void* g_getTransform{};
        const void* g_gameObjectGetTransform{};
        const void* g_gameObjectGetActiveInHierarchy{};
        const void* g_getParent{};
        const void* g_getGameObject{};
        const void* g_setActive{};
        const void* g_setAsLastSibling{};
        const void* g_getComponentByType{};
        const void* g_getComponentsInChildrenByType{};
        const void* g_findObjectsOfTypeByType{};
        const void* g_getChildCount{};
        const void* g_getChild{};
        const void* g_scrollGetContent{};
        const void* g_tabGetPage{};
        const void* g_tabGetButton{};
        const void* g_tabSetCanFlick{};
        const void* g_tabButtonSetText{};
        const void* g_tabButtonSetForceWidth{};
        void* g_tabButtonGroupType{};
        const void* g_tabGroupSetSelectIndex{};
        const void* g_tabGroupGetSelectedBarRect{};
        const void* g_getUnityObjectName{};
        void* g_barRefreshTransform{};
        std::atomic<int> g_barRefreshFrames{0};
        bool g_loggedTabBarChildren{false};
        const void* g_switchSetIsOn{};
        const void* g_switchSetDisabled{};
        const void* g_campusTextSetText{};
        const void* g_campusTextSetLocalizeKey{};
        const void* g_overlayTitleSetTitle{};
        const void* g_forceRebuildLayout{};
        const void* g_resourcesFindObjectsOfTypeAll{};
        const void* g_rectGetAnchoredPosition{};
        const void* g_rectSetAnchoredPosition{};
        const void* g_rectGetSizeDelta{};
        const void* g_rectSetSizeDelta{};
        const void* g_rectGetPivot{};
        const void* g_rectSetPivot{};
        const void* g_rectSetAnchorMin{};
        const void* g_rectSetAnchorMax{};
        const void* g_masterManagerGetCostumeMaster{};
        const void* g_masterManagerGetCostumeHeadMaster{};
        const void* g_costumeMasterGetAll{};
        const void* g_costumeHeadMasterGetAll{};
        const void* g_costumeGetId{};
        const void* g_costumeGetName{};
        const void* g_costumeGetCharacter{};
        const void* g_costumeGetHeadId{};
        const void* g_costumeGetDefaultHeadId{};
        const void* g_costumeHeadGetId{};
        const void* g_costumeHeadGetName{};
        const void* g_costumeHeadGetCharacter{};
        const void* g_costumeHeadGetHairAssetId{};
        const void* g_characterGetName{};
        const void* g_commonCostumeThumbnailSet{};
        const void* g_costumeThumbnailSetShowDetail{};
        const void* g_thumbnailSetGestureEnabled{};

        void* g_settingScreenViewType{};
        void* g_settingScreenPresenterType{};
        void* g_scrollRectType{};
        void* g_campusTextType{};
        void* g_switchButtonType{};
        void* g_campusSimpleTabType{};
        void* g_commonCostumeThumbnailType{};

        std::atomic<bool> g_modScreenPending{false};
        std::atomic<std::uint32_t> g_pendingPollFrames{0};
        // Identity of the screen already composed as Mod management.  Keeping
        // this separate from the transient menu prevents a second menu click
        // from treating the old page as the newly routed Setting page.
        void* g_activeModTab{};
        // The home-menu entry has a much smaller dependency surface than the
        // repurposed Setting screen.  Keep the entry alive even if a game update
        // moves one of the full-screen controls to another assembly.
        std::atomic<bool> g_fullScreenReady{false};

        struct ManagedArray {
            void* klass;
            void* monitor;
            void* bounds;
            std::uintptr_t length;
            void* items[1];
        };

        struct Vector2 {
            float x{};
            float y{};
        };

        struct GameTargetPresentation {
            void* costume{};
            void* costumeHead{};
            std::string displayText{"游戏内目标暂未解析"};
        };

        struct MasterCatalog {
            std::vector<void*> costumes;
            std::vector<void*> costumeHeads;
        };

        struct ModToggleBinding {
            void* campusButton{};
            void* switchButton{};
            void* statusText{};
            std::string modId;
            std::string detailPrefix;
            bool enabled{};
            bool displayedState{};
            bool visualRefreshPending{};
        };

        std::vector<ModToggleBinding> g_toggleBindings;
        // Resources.FindObjectsOfTypeAll also returns the thumbnail components
        // cloned into previous Mod screens. Reusing one of those clones as the
        // next template compounds its RectTransform state and loses the
        // original long-press interaction. Keep process-lifetime ownership so
        // only genuine game-owned thumbnail components can become templates.
        std::unordered_set<void*> g_ownedThumbnailComponents;

        std::atomic<bool> g_injectionFaulted{false};
        // Which EnsureEntry step was in flight, so a fault names its own cause
        // instead of leaving the whole function as the suspect.
        std::atomic<int> g_injectStep{0};
        // MenuView pointers whose current lifecycle already owns an injected
        // entry. SettingTopScreen.Reload can retain the MenuView address while
        // destroying its cloned children, so that route explicitly invalidates
        // the current entry before the next MenuPresenter.SetEvent.
        std::unordered_set<void*> g_injectedViews;

        void LogF(const char* format, ...) {
            char message[1024]{};
            va_list args;
            va_start(args, format);
            std::vsnprintf(message, sizeof(message), format, args);
            va_end(args);
            Log(message);
        }

        void LogErrorF(const char* format, ...) {
            char message[1024]{};
            va_list args;
            va_start(args, format);
            std::vsnprintf(message, sizeof(message), format, args);
            va_end(args);
            LogError(message);
        }

        UnityResolve::Class* FindClassAcrossAssemblies(
            const char* className,
            const char* namespaze,
            std::initializer_list<UnityResolve::Assembly*> preferred) {
            for (const auto candidate : preferred) {
                if (!candidate) continue;
                if (const auto found = candidate->Get(className, namespaze)) {
                    LogF("Mod menu: resolved %s.%s in %s.",
                         namespaze, className, candidate->name.c_str());
                    return found;
                }
            }
            for (const auto candidate : UnityResolve::assembly) {
                if (!candidate) continue;
                if (std::find(preferred.begin(), preferred.end(), candidate) != preferred.end()) {
                    continue;
                }
                if (const auto found = candidate->Get(className, namespaze)) {
                    LogF("Mod menu: resolved %s.%s in %s (fallback scan).",
                         namespaze, className, candidate->name.c_str());
                    return found;
                }
            }
            LogF("Mod menu: class %s.%s was not found in any loaded assembly.",
                 namespaze, className);
            return nullptr;
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

        void* Call(
            const void* method,
            void* instance,
            std::initializer_list<void*> params,
            const char* label,
            bool* succeeded = nullptr) {
            if (succeeded) *succeeded = false;
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
            if (succeeded) *succeeded = true;
            return result;
        }

        template <typename TValue>
        bool CallValue(
            const void* method,
            void* instance,
            std::initializer_list<void*> params,
            const char* label,
            TValue& output) {
            const auto boxed = Call(method, instance, params, label);
            if (!boxed) return false;
            const auto value = g_objectUnbox
                ? g_objectUnbox(boxed)
                : static_cast<void*>(static_cast<char*>(boxed) + sizeof(void*) * 2);
            if (!value) return false;
            std::memcpy(&output, value, sizeof(output));
            return true;
        }

        void* ManagedTypeOf(UnityResolve::Class* klass) {
            if (!klass || !g_classGetType || !g_typeGetObject) return nullptr;
            const auto type = g_classGetType(klass->address);
            return type ? g_typeGetObject(type) : nullptr;
        }

        std::vector<void*> ComponentsInChildren(void* component, void* managedType) {
            std::vector<void*> components;
            if (!component || !managedType || !g_getComponentsInChildrenByType) return components;
            bool includeInactive = true;
            const auto array = static_cast<ManagedArray*>(Call(
                g_getComponentsInChildrenByType,
                component,
                {managedType, &includeInactive},
                "Component.GetComponentsInChildren(Type, bool)"));
            if (!array || array->length > 4096) return components;
            components.reserve(static_cast<std::size_t>(array->length));
            for (std::uintptr_t index = 0; index < array->length; ++index) {
                if (array->items[index]) components.push_back(array->items[index]);
            }
            return components;
        }

        std::vector<void*> FindActiveObjectsOfType(void* managedType) {
            std::vector<void*> objects;
            if (!managedType || !g_findObjectsOfTypeByType) return objects;
            const auto array = static_cast<ManagedArray*>(Call(
                g_findObjectsOfTypeByType,
                nullptr,
                {managedType},
                "Object.FindObjectsOfType(Type)"));
            if (!array || array->length > 4096) return objects;
            objects.reserve(static_cast<std::size_t>(array->length));
            for (std::uintptr_t index = 0; index < array->length; ++index) {
                if (array->items[index]) objects.push_back(array->items[index]);
            }
            return objects;
        }

        void* FindActiveSettingTab(void* excludedTab = nullptr) {
            std::int32_t firstIndex = 0;
            for (const auto tab : FindActiveObjectsOfType(g_campusSimpleTabType)) {
                if (tab == excludedTab) continue;
                const auto firstPage = Call(
                    g_tabGetPage, tab, {&firstIndex}, "poll setting tab GetPage(0)");
                if (firstPage && std::strcmp(ClassNameOf(firstPage), "PreferenceTabPage") == 0) {
                    return tab;
                }
            }
            return nullptr;
        }

        bool IsActiveModTabPresent() {
            if (!g_activeModTab) return false;
            const auto tabs = FindActiveObjectsOfType(g_campusSimpleTabType);
            if (std::find(tabs.begin(), tabs.end(), g_activeModTab) != tabs.end()) {
                return true;
            }
            g_activeModTab = nullptr;
            g_toggleBindings.clear();
            return false;
        }

        bool IsDescendantOf(void* childTransform, void* ancestorTransform) {
            for (int depth = 0; childTransform && depth < 64; ++depth) {
                if (childTransform == ancestorTransform) return true;
                childTransform = Call(g_getParent, childTransform, {}, "Transform.get_parent");
            }
            return false;
        }

        void SetCampusText(void* campusText, const std::string& value, const char* label) {
            if (!campusText) return;
            Call(g_campusTextSetLocalizeKey,
                 campusText,
                 {UnityResolve::UnityType::String::New("")},
                 "clear CampusText localization key");
            Call(g_campusTextSetText,
                 campusText,
                 {UnityResolve::UnityType::String::New(value)},
                 label);
        }

        std::string ManagedStringValue(
            void* instance,
            const void* getter,
            const char* label) {
            const auto managed = static_cast<UnityResolve::UnityType::String*>(
                Call(getter, instance, {}, label));
            if (!managed || managed->length <= 0) return {};
            const auto required = WideCharToMultiByte(
                CP_UTF8,
                WC_ERR_INVALID_CHARS,
                managed->start_char,
                managed->length,
                nullptr,
                0,
                nullptr,
                nullptr);
            if (required <= 0) return {};
            std::string result(static_cast<std::size_t>(required), '\0');
            const auto written = WideCharToMultiByte(
                CP_UTF8,
                WC_ERR_INVALID_CHARS,
                managed->start_char,
                managed->length,
                result.data(),
                required,
                nullptr,
                nullptr);
            if (written != required) return {};
            return result;
        }

        std::vector<void*> ReadManagedList(void* list, const char* label) {
            std::vector<void*> result;
            if (!list) return result;
            const auto getCount = FindMethodOnObject(list, "get_Count", 0);
            const auto getItem = FindMethodOnObject(list, "get_Item", 1);
            std::int32_t count{};
            if (!getCount || !getItem
                || !CallValue(getCount, list, {}, label, count)
                || count < 0 || count > 4096) {
                LogF("Mod menu: %s is not a readable managed list.", label);
                return result;
            }
            result.reserve(static_cast<std::size_t>(count));
            for (std::int32_t index = 0; index < count; ++index) {
                if (const auto item = Call(getItem, list, {&index}, label)) {
                    result.push_back(item);
                }
            }
            return result;
        }

        MasterCatalog LoadMasterCatalog() {
            MasterCatalog catalog;
            std::int32_t ascending = 0;
            const auto costumeMaster = Call(
                g_masterManagerGetCostumeMaster,
                nullptr,
                {},
                "MasterManager.get_CostumeMaster");
            const auto costumeHeadMaster = Call(
                g_masterManagerGetCostumeHeadMaster,
                nullptr,
                {},
                "MasterManager.get_CostumeHeadMaster");
            const auto costumes = Call(
                g_costumeMasterGetAll,
                costumeMaster,
                {&ascending},
                "CostumeMaster.GetAllWithSortByKey");
            const auto costumeHeads = Call(
                g_costumeHeadMasterGetAll,
                costumeHeadMaster,
                {&ascending},
                "CostumeHeadMaster.GetAllWithSortByKey");
            catalog.costumes = ReadManagedList(costumes, "Costume Master list");
            catalog.costumeHeads = ReadManagedList(costumeHeads, "CostumeHead Master list");
            LogF("Mod menu: game target catalog costumes=%zu heads=%zu.",
                 catalog.costumes.size(), catalog.costumeHeads.size());
            return catalog;
        }

        std::string CharacterNameOf(void* masterRecord, const void* getCharacter) {
            const auto character = Call(
                getCharacter, masterRecord, {}, "target Master.GetCharacter");
            return ManagedStringValue(
                character, g_characterGetName, "Character.get_Name");
        }

        std::string JoinGameTargetName(
            std::string characterName,
            std::string targetName,
            const ModCategory category) {
            if (characterName.empty()) characterName = "未知角色";
            if (targetName.empty()) {
                targetName = category == ModCategory::Costume
                    ? "未命名服装"
                    : "未命名发型";
            }
            return characterName + " · " + targetName;
        }

        GameTargetPresentation ResolveGameTarget(
            const ModPresentationItem& item,
            const MasterCatalog& catalog) {
            GameTargetPresentation result;
            if (item.targetMasterKey.empty() && item.targetSource.empty()) return result;

            if (item.category == ModCategory::Costume) {
                if (item.targetMasterKey.empty()) return result;
                for (const auto costume : catalog.costumes) {
                    if (ManagedStringValue(costume, g_costumeGetId, "Costume.get_Id")
                        != item.targetMasterKey) {
                        continue;
                    }
                    result.costume = costume;
                    result.displayText = JoinGameTargetName(
                        CharacterNameOf(costume, g_costumeGetCharacter),
                        ManagedStringValue(costume, g_costumeGetName, "Costume.get_Name"),
                        item.category);
                    return result;
                }
                return result;
            }

            void* matchedHead{};
            const auto targetKey = NormalizeHairAssetKey(
                item.targetMasterKey.empty() ? item.targetSource : item.targetMasterKey);
            std::string nearbyCandidates;
            std::size_t nearbyCandidateCount{};
            const auto characterSeparator = targetKey.find("-hair-");
            const auto characterKey = characterSeparator == std::string::npos
                ? std::string{}
                : targetKey.substr(0, characterSeparator);
            for (const auto head : catalog.costumeHeads) {
                const auto id = ManagedStringValue(
                    head, g_costumeHeadGetId, "CostumeHead.get_Id");
                const auto hairAssetId = ManagedStringValue(
                    head, g_costumeHeadGetHairAssetId, "CostumeHead.get_HairAssetId");
                const auto normalizedId = NormalizeHairAssetKey(id);
                const auto normalizedHairAssetId = NormalizeHairAssetKey(hairAssetId);
                if ((!targetKey.empty() && normalizedId == targetKey)
                    || (!targetKey.empty() && normalizedHairAssetId == targetKey)) {
                    matchedHead = head;
                    LogF(
                        "Mod menu: hair target resolved target=%s head=%s hairAsset=%s.",
                        targetKey.c_str(),
                        id.c_str(),
                        hairAssetId.c_str());
                    break;
                }
                if (!characterKey.empty() && nearbyCandidateCount < 8
                    && (normalizedId.rfind(characterKey, 0) == 0
                        || normalizedHairAssetId.rfind(characterKey, 0) == 0)) {
                    if (!nearbyCandidates.empty()) nearbyCandidates += ", ";
                    nearbyCandidates += id + "|" + hairAssetId;
                    ++nearbyCandidateCount;
                }
            }
            if (!matchedHead) {
                LogF(
                    "Mod menu: hair target unresolved target=%s candidates=%s.",
                    targetKey.c_str(),
                    nearbyCandidates.empty() ? "<none>" : nearbyCandidates.c_str());
                return result;
            }

            const auto headId = ManagedStringValue(
                matchedHead, g_costumeHeadGetId, "CostumeHead.get_Id");
            result.costumeHead = matchedHead;
            result.displayText = JoinGameTargetName(
                CharacterNameOf(matchedHead, g_costumeHeadGetCharacter),
                ManagedStringValue(
                    matchedHead, g_costumeHeadGetName, "CostumeHead.get_Name"),
                item.category);
            for (const auto costume : catalog.costumes) {
                const auto head = ManagedStringValue(
                    costume, g_costumeGetHeadId, "Costume.get_CostumeHeadId");
                const auto defaultHead = ManagedStringValue(
                    costume,
                    g_costumeGetDefaultHeadId,
                    "Costume.get_DefaultCostumeHeadId");
                if (head == headId || defaultHead == headId) {
                    result.costume = costume;
                    break;
                }
            }
            return result;
        }

        std::string TargetDetailPrefix(
            const ModCategory category,
            const std::string& targetDisplayText) {
            const auto prompt = category == ModCategory::Costume
                ? "在换装中选择："
                : "在发型中选择：";
            return std::string(prompt) + "\n" + targetDisplayText;
        }

        bool IsSettledStatus(const std::string& status) {
            return status == "启用中" || status == "已关闭";
        }

        std::string RowDetailText(
            const std::string& detailPrefix,
            const std::string& status) {
            return IsSettledStatus(status)
                ? detailPrefix
                : detailPrefix + "\n" + status;
        }

        void ConfigureTitleAutoSize(void* campusText) {
            if (!campusText) return;
            bool enabled = true;
            float minimum = 24.0f;
            float maximum = 38.0f;
            Call(FindMethodOnObject(campusText, "set_enableAutoSizing", 1),
                 campusText,
                 {&enabled},
                 "Mod title enableAutoSizing");
            Call(FindMethodOnObject(campusText, "set_fontSizeMin", 1),
                 campusText,
                 {&minimum},
                 "Mod title fontSizeMin");
            Call(FindMethodOnObject(campusText, "set_fontSizeMax", 1),
                 campusText,
                 {&maximum},
                 "Mod title fontSizeMax");
        }

        bool GetRectValues(
            void* rect,
            Vector2& position,
            Vector2& size,
            Vector2& pivot) {
            return rect
                && CallValue(
                    g_rectGetAnchoredPosition,
                    rect,
                    {},
                    "RectTransform.get_anchoredPosition",
                    position)
                && CallValue(
                    g_rectGetSizeDelta,
                    rect,
                    {},
                    "RectTransform.get_sizeDelta",
                    size)
                && CallValue(
                    g_rectGetPivot,
                    rect,
                    {},
                    "RectTransform.get_pivot",
                    pivot);
        }

        void InsetTextForThumbnail(void* campusText, const float inset) {
            const auto rect = Call(
                g_getTransform, campusText, {}, "row text get_transform");
            Vector2 position;
            Vector2 size;
            Vector2 pivot;
            if (!GetRectValues(rect, position, size, pivot)) return;
            position.x += inset * (1.0f - pivot.x);
            size.x -= inset;
            Call(g_rectSetAnchoredPosition,
                 rect,
                 {&position},
                 "inset Mod row text position");
            Call(g_rectSetSizeDelta,
                 rect,
                 {&size},
                 "inset Mod row text width");
        }

        bool AttachCategoryPlaceholder(
            void* rowTransform,
            void* titleText,
            const ModCategory category) {
            const auto sourceObject = Call(
                g_getGameObject, titleText, {}, "placeholder source get_gameObject");
            if (!sourceObject) return false;
            bool worldPositionStays = false;
            const auto cloneObject = Call(
                g_cloneWithParent,
                nullptr,
                {sourceObject, rowTransform, &worldPositionStays},
                "clone game-styled category placeholder");
            const auto marker = cloneObject
                ? Call(
                    g_getComponentByType,
                    cloneObject,
                    {g_campusTextType},
                    "placeholder GetComponent(CampusText)")
                : nullptr;
            const auto rect = cloneObject
                ? Call(
                    g_gameObjectGetTransform,
                    cloneObject,
                    {},
                    "placeholder get_transform")
                : nullptr;
            if (!marker || !rect) return false;
            Vector2 anchor{0.0f, 0.5f};
            Vector2 pivot{0.0f, 0.5f};
            Vector2 position{48.0f, 0.0f};
            Vector2 size{112.0f, 112.0f};
            Call(g_rectSetAnchorMin,
                 rect,
                 {&anchor},
                 "placeholder set_anchorMin");
            Call(g_rectSetAnchorMax,
                 rect,
                 {&anchor},
                 "placeholder set_anchorMax");
            Call(g_rectSetPivot, rect, {&pivot}, "placeholder set_pivot");
            Call(g_rectSetAnchoredPosition,
                 rect,
                 {&position},
                 "place category placeholder");
            Call(g_rectSetSizeDelta,
                 rect,
                 {&size},
                 "size category placeholder");
            SetCampusText(
                marker,
                category == ModCategory::Costume ? "服" : "发",
                "set category placeholder text");
            return true;
        }

        void ConfigureThumbnailRect(void* rect) {
            if (!rect) return;
            Vector2 anchor{0.0f, 0.5f};
            Vector2 pivot{0.0f, 0.5f};
            Vector2 position{48.0f, 0.0f};
            Vector2 size{112.0f, 112.0f};
            Call(g_rectSetAnchorMin, rect, {&anchor}, "thumbnail set_anchorMin");
            Call(g_rectSetAnchorMax, rect, {&anchor}, "thumbnail set_anchorMax");
            Call(g_rectSetPivot, rect, {&pivot}, "thumbnail set_pivot");
            Call(g_rectSetAnchoredPosition,
                 rect,
                 {&position},
                 "thumbnail set_anchoredPosition");
            Call(g_rectSetSizeDelta, rect, {&size}, "thumbnail set_sizeDelta");
        }

        struct ThumbnailTemplateCandidate {
            void* component{};
            void* gameObject{};
            bool activeInHierarchy{true};
            Vector2 size{};
            float area{};
        };

        std::vector<ThumbnailTemplateCandidate> CollectThumbnailTemplateCandidates(
            void* managedType,
            const char* label,
            std::size_t& ownedCandidatesSkipped) {
            std::vector<ThumbnailTemplateCandidate> result;
            ownedCandidatesSkipped = 0;
            if (!managedType || !g_resourcesFindObjectsOfTypeAll) return result;

            const auto candidates = static_cast<ManagedArray*>(Call(
                g_resourcesFindObjectsOfTypeAll,
                nullptr,
                {managedType},
                label));
            if (!candidates || candidates->length == 0 || candidates->length > 4096) {
                return result;
            }

            for (std::uintptr_t index = 0; index < candidates->length; ++index) {
                const auto source = candidates->items[index];
                if (!source) continue;
                if (g_ownedThumbnailComponents.contains(source)) {
                    ++ownedCandidatesSkipped;
                    continue;
                }
                const auto sourceObject = Call(
                    g_getGameObject,
                    source,
                    {},
                    "thumbnail candidate get_gameObject");
                if (!sourceObject) continue;

                ThumbnailTemplateCandidate candidate;
                candidate.component = source;
                candidate.gameObject = sourceObject;
                if (g_gameObjectGetActiveInHierarchy) {
                    CallValue(
                        g_gameObjectGetActiveInHierarchy,
                        sourceObject,
                        {},
                        "thumbnail candidate get_activeInHierarchy",
                        candidate.activeInHierarchy);
                }
                const auto rect = Call(
                    g_getTransform,
                    source,
                    {},
                    "thumbnail candidate get_transform");
                if (rect) {
                    CallValue(
                        g_rectGetSizeDelta,
                        rect,
                        {},
                        "thumbnail candidate get_sizeDelta",
                        candidate.size);
                }
                const auto width = candidate.size.x < 0.0f
                    ? -candidate.size.x : candidate.size.x;
                const auto height = candidate.size.y < 0.0f
                    ? -candidate.size.y : candidate.size.y;
                candidate.area = width * height;
                result.push_back(candidate);
            }

            // Outfit screens load many live list cells. Their layout and button
            // state belong to the screen that is about to close, so they are a
            // poor cloning source. Prefer inactive game-owned templates, then
            // the largest original RectTransform for deterministic fallback.
            std::sort(result.begin(), result.end(), [](const auto& left, const auto& right) {
                if (left.activeInHierarchy != right.activeInHierarchy) {
                    return !left.activeInHierarchy;
                }
                if (left.area != right.area) return left.area > right.area;
                return left.component < right.component;
            });
            return result;
        }

        void RestoreThumbnailInteraction(void* thumbnail) {
            if (!thumbnail) return;
            const auto getButton = FindMethodOnObject(thumbnail, "get_Button", 0);
            const auto button = getButton
                ? Call(getButton, thumbnail, {}, "thumbnail get_Button")
                : nullptr;
            const auto setInteractable = FindMethodOnObject(
                button, "set_interactable", 1);
            if (!button || !setInteractable) return;
            bool interactable = true;
            Call(
                setInteractable,
                button,
                {&interactable},
                "restore thumbnail button interaction");
        }

        // Only Campus.Common.UI.CostumeThumbnailView is used as the template.
        // The outfit screen's CostumeListCellThumbnailView is a list-cell
        // wrapper: its RectTransform is driven by the grid layout and its Set
        // only forwards to the inner thumbnail, so cloning it produced a
        // shrunken icon whose long-press detail was never wired.  Set() on this
        // class registers the detail callback itself.
        //
        // Hair rows come through here too: Set takes ICostume, and master
        // CostumeHead implements ICostume just like Costume does.  The earlier
        // head-only path called the ThumbnailViewBase.Set(assetName) base
        // method instead, which swapped the sprite but left the derived view
        // uninitialised -- _emptyRoot stayed visible ("未设置" on top of the
        // correct icon) and no long-press detail callback was ever registered.
        bool AttachOfficialGameThumbnail(
            void* rowTransform,
            void* costume) {
            if (!rowTransform || !costume || !g_commonCostumeThumbnailType
                || !g_commonCostumeThumbnailSet || !g_resourcesFindObjectsOfTypeAll) {
                return false;
            }
            std::size_t ownedCandidatesSkipped = 0;
            const auto candidates = CollectThumbnailTemplateCandidates(
                g_commonCostumeThumbnailType,
                "Resources.FindObjectsOfTypeAll(CostumeThumbnailView)",
                ownedCandidatesSkipped);
            for (const auto& candidate : candidates) {
                const auto source = candidate.component;
                const auto sourceObject = candidate.gameObject;

                bool worldPositionStays = false;
                const auto cloneObject = Call(
                    g_cloneWithParent,
                    nullptr,
                    {sourceObject, rowTransform, &worldPositionStays},
                    "clone official costume thumbnail");
                const auto clone = cloneObject
                    ? Call(
                        g_getComponentByType,
                        cloneObject,
                        {g_commonCostumeThumbnailType},
                        "thumbnail clone GetComponent")
                    : nullptr;
                const auto rect = cloneObject
                    ? Call(
                        g_gameObjectGetTransform,
                        cloneObject,
                        {},
                        "thumbnail clone get_transform")
                    : nullptr;
                if (!clone || !rect) continue;
                g_ownedThumbnailComponents.emplace(clone);
                bool active = true;
                Call(g_setActive,
                     cloneObject,
                     {&active},
                     "activate official thumbnail clone");

                // Set() only registers the long-press detail callback when
                // IsShowDetailEnabled is on, and the outfit-list prefabs ship it
                // off.  Force both gates before Set so the clone owns the
                // interaction instead of inheriting the source screen's state.
                bool on = true;
                Call(g_costumeThumbnailSetShowDetail,
                     clone,
                     {&on},
                     "thumbnail set_IsShowDetailEnabled(true)");
                Call(g_thumbnailSetGestureEnabled,
                     clone,
                     {&on},
                     "thumbnail SetGestureEnabled(true)");

                bool succeeded = false;
                Call(g_commonCostumeThumbnailSet,
                     clone,
                     {costume},
                     "CostumeThumbnailView.Set(costume)",
                     &succeeded);
                if (!succeeded) {
                    active = false;
                    Call(g_setActive,
                         cloneObject,
                         {&active},
                         "hide failed thumbnail clone");
                    continue;
                }

                ConfigureThumbnailRect(rect);
                RestoreThumbnailInteraction(clone);
                LogF(
                    "Mod menu: official costume thumbnail attached source=%s object=%p active=%d sourceSize=%.1fx%.1f skippedOwned=%zu.",
                    ClassNameOf(source),
                    source,
                    candidate.activeInHierarchy ? 1 : 0,
                    candidate.size.x,
                    candidate.size.y,
                    ownedCandidatesSkipped);
                return true;
            }
            if (ownedCandidatesSkipped > 0) {
                LogF(
                    "Mod menu: skipped %zu manager-owned costume thumbnail template candidates.",
                    ownedCandidatesSkipped);
            }
            return false;
        }

        bool LoadPresentationModel(ModPresentationModel& model) {
            RuntimeClient runtime;
            std::string jsonText;
            if (!runtime.Connect() || !runtime.GetModsJson(jsonText)) {
                Log("Mod menu: Runtime snapshot refresh failed.");
                return false;
            }

            RuntimeModSnapshot snapshot;
            std::string parseError;
            if (!ParseRuntimeModSnapshot(jsonText, snapshot, parseError)) {
                LogF("Mod menu: Runtime snapshot parse failed: %s", parseError.c_str());
                return false;
            }

            model = BuildModPresentationModel(snapshot);
            LogF("Mod menu: presentation refreshed mods=%zu costume=%zu hair=%zu invalid=%zu.",
                 model.totalModCount,
                 model.costumeMods.size(),
                 model.hairMods.size(),
                 model.invalidModCount);
            return true;
        }

        void* ReadReferenceField(void* object, UnityResolve::Field* field) {
            return object && field && field->offset >= 0
                ? *reinterpret_cast<void**>(static_cast<char*>(object) + field->offset)
                : nullptr;
        }

        void* FindActiveSettingPresenter(void* tab) {
            if (!tab || !g_settingScreenPresenterType) return nullptr;
            for (const auto presenter : FindActiveObjectsOfType(g_settingScreenPresenterType)) {
                if (ReadReferenceField(presenter, g_settingTabField) == tab) {
                    return presenter;
                }
            }
            return nullptr;
        }

        // The global menu is owned by OutGameFooterPresenter, and only
        // CloseGlobalMenu() actually tears the layer down.
        //
        // The previous implementation invoked CampusButtonBase.OnClicked() on
        // MenuView._closeButton.  That is the same "fake click" shape already
        // disproved at 18:06 for the Setting button: the body returns without
        // running the behaviour the real input path would trigger, so the call
        // reported success while the menu stayed on screen.  See the screenshot
        // where the composed Mod page is visible *behind* an open menu.
        bool CloseCurrentMenu() {
            if (g_footerPresenter && g_closeGlobalMenu) {
                Call(g_closeGlobalMenu, g_footerPresenter, {},
                     "OutGameFooterPresenter.CloseGlobalMenu");
                return true;
            }
            LogF("Mod menu: cannot close the menu footer=%d closeGlobalMenu=%d.",
                 g_footerPresenter != nullptr, g_closeGlobalMenu != nullptr);
            return false;
        }

        bool GuardedCloseCurrentMenu(void*) {
            __try {
                return CloseCurrentMenu();
            }
            __except (EXCEPTION_EXECUTE_HANDLER) {
                LogError("Mod menu: closing the global menu faulted.");
                return false;
            }
        }

        bool ReloadActiveModScreenAsSettings() {
            const auto tab = g_activeModTab;
            const auto presenter = FindActiveSettingPresenter(tab);
            if (!presenter || !g_reloadSettingScreen) {
                LogF("Mod menu: cannot reload active Mod page presenter=%d reload=%d.",
                     presenter != nullptr,
                     g_reloadSettingScreen != nullptr);
                return false;
            }

            bool reloadStarted = false;
            Call(g_reloadSettingScreen,
                 presenter,
                 {},
                 "SettingTopScreenPresenter.Reload",
                 &reloadStarted);
            if (!reloadStarted) return false;

            // Reload recreates the SettingTopScreen through the game's own
            // navigator.  Clear ownership before SetEvent runs so the fresh
            // page remains the unmodified system settings page.
            g_modScreenPending.store(false);
            g_pendingPollFrames.store(0);
            g_activeModTab = nullptr;
            g_toggleBindings.clear();
            if (g_menuViewInstance) {
                g_injectedViews.erase(g_menuViewInstance);
            }
            g_modButton = nullptr;
            Log("Mod menu: current menu entry cache invalidated for Setting reload.");
            return true;
        }

        void DeactivateGameObject(void* gameObject, const char* label) {
            if (!gameObject) return;
            bool active = false;
            Call(g_setActive, gameObject, {&active}, label);
        }

        void* FindScrollContent(void* page, void* requiredDescendant) {
            void* fallback{};
            for (const auto scrollRect : ComponentsInChildren(page, g_scrollRectType)) {
                const auto content = Call(
                    g_scrollGetContent, scrollRect, {}, "ScrollRect.get_content");
                if (!content) continue;
                if (!fallback) fallback = content;
                if (requiredDescendant && IsDescendantOf(requiredDescendant, content)) {
                    return content;
                }
            }
            return requiredDescendant ? nullptr : fallback;
        }

        std::vector<void*> RowTextsOutsideSwitch(void* rowTransform, void* switchTransform) {
            std::vector<void*> result;
            for (const auto text : ComponentsInChildren(rowTransform, g_campusTextType)) {
                const auto textTransform = Call(
                    g_getTransform, text, {}, "row text get_transform");
                if (textTransform && switchTransform
                    && IsDescendantOf(textTransform, switchTransform)) {
                    continue;
                }
                result.push_back(text);
            }
            return result;
        }

        void* FindSwitchRowTemplate(void* switchButton, void* content) {
            const auto switchTransform = Call(
                g_getTransform, switchButton, {}, "template switch get_transform");
            auto candidate = switchTransform
                ? Call(g_getParent, switchTransform, {}, "template switch parent")
                : nullptr;
            for (int depth = 0; candidate && candidate != content && depth < 8; ++depth) {
                const auto texts = RowTextsOutsideSwitch(candidate, switchTransform);
                if (!texts.empty() && texts.size() <= 4) return candidate;
                candidate = Call(g_getParent, candidate, {}, "template row parent");
            }
            return nullptr;
        }

        const void* FindNonGenericMethodInClass(
            void* klass,
            const char* name,
            const std::uint32_t argc) {
            if (!klass || !g_classGetMethods) return nullptr;
            for (; klass; klass = g_classGetParent ? g_classGetParent(klass) : nullptr) {
                if (g_classInit) g_classInit(klass);
                void* iterator{};
                while (const auto method = g_classGetMethods(klass, &iterator)) {
                    if (g_methodIsGeneric && g_methodIsGeneric(method)) continue;
                    const auto methodName = g_methodGetName(method);
                    const auto methodNameLength = methodName ? std::strlen(methodName) : 0;
                    const auto requestedLength = std::strlen(name);
                    // Explicit interface implementations are named like
                    // "Campus.Common.UI.ICampusSimpleTab.GetPage" in IL2CPP.
                    // Treat the final dotted segment as the callable name.
                    const bool nameMatches = methodName
                        && (std::strcmp(methodName, name) == 0
                            || (methodNameLength > requestedLength
                                && methodName[methodNameLength - requestedLength - 1] == '.'
                                && std::strcmp(methodName + methodNameLength - requestedLength,
                                               name) == 0));
                    if (nameMatches && g_methodGetParamCount(method) == argc) {
                        return method;
                    }
                }
            }
            return nullptr;
        }

        void DeactivateContentChildren(void* content) {
            std::int32_t childCount{};
            if (!content || !CallValue(
                    g_getChildCount,
                    content,
                    {},
                    "Transform.get_childCount",
                    childCount)
                || childCount < 0 || childCount > 4096) {
                return;
            }
            for (std::int32_t index = 0; index < childCount; ++index) {
                const auto child = Call(g_getChild, content, {&index}, "Transform.GetChild");
                const auto childObject = child
                    ? Call(g_getGameObject, child, {}, "content child get_gameObject")
                    : nullptr;
                DeactivateGameObject(childObject, "hide original setting content");
            }
        }

        bool CreateModRow(
            void* rowTemplate,
            void* content,
            const MasterCatalog& masterCatalog,
            const ModPresentationItem* item,
            const char* emptyTitle,
            const char* emptyDescription) {
            if (!rowTemplate || !content) return false;
            const auto templateObject = Call(
                g_getGameObject, rowTemplate, {}, "row template get_gameObject");
            if (!templateObject) return false;

            bool worldPositionStays = false;
            const auto cloneObject = Call(
                g_cloneWithParent,
                nullptr,
                {templateObject, content, &worldPositionStays},
                "clone setting switch row");
            const auto cloneTransform = cloneObject
                ? Call(g_gameObjectGetTransform, cloneObject, {}, "row clone get_transform")
                : nullptr;
            if (!cloneObject || !cloneTransform) return false;

            bool active = true;
            Call(g_setActive, cloneObject, {&active}, "activate Mod row");
            Call(g_setAsLastSibling, cloneTransform, {}, "Mod row SetAsLastSibling");

            const auto switches = ComponentsInChildren(cloneTransform, g_switchButtonType);
            if (switches.empty()) {
                Log("Mod menu: cloned setting row has no SwitchButton.");
                return false;
            }
            const auto switchButton = switches.front();
            const auto switchTransform = Call(
                g_getTransform, switchButton, {}, "row switch get_transform");
            const auto texts = RowTextsOutsideSwitch(cloneTransform, switchTransform);
            const std::string title = item ? item->title : emptyTitle;
            GameTargetPresentation target;
            std::string detailPrefix;
            std::string description = emptyDescription;
            if (item) {
                target = ResolveGameTarget(*item, masterCatalog);
                detailPrefix = TargetDetailPrefix(item->category, target.displayText);
                description = RowDetailText(detailPrefix, item->statusText);
            }
            if (!texts.empty()) SetCampusText(texts[0], title, "set Mod row title");
            if (texts.size() > 1) SetCampusText(texts[1], description, "set Mod row status");
            for (std::size_t index = 2; index < texts.size(); ++index) {
                SetCampusText(texts[index], "", "clear Mod row extra text");
            }
            if (item && !texts.empty()) {
                ConfigureTitleAutoSize(texts[0]);
                const bool hasOfficialThumbnail = AttachOfficialGameThumbnail(
                    cloneTransform,
                    item->category == ModCategory::Hair
                        ? target.costumeHead
                        : target.costume);
                if (!hasOfficialThumbnail) {
                    AttachCategoryPlaceholder(
                        cloneTransform, texts[0], item->category);
                }
                for (const auto text : texts) {
                    InsetTextForThumbnail(text, 136.0f);
                }
            }

            bool animate = false;
            bool isOn = item && item->configuredEnabled;
            Call(g_switchSetIsOn,
                 switchButton,
                 {&isOn, &animate},
                 "SwitchButton.SetIsOn");

            bool disabled = !item || item->state == ModDisplayState::ConfigurationError;
            Call(g_switchSetDisabled,
                 switchButton,
                 {&disabled},
                 "SwitchButton.SetDisabled");
            if (!item) {
                const auto switchObject = switchTransform
                    ? Call(g_getGameObject, switchTransform, {}, "empty switch get_gameObject")
                    : nullptr;
                DeactivateGameObject(switchObject, "hide empty-state switch");
                return true;
            }

            const auto campusButton = ReadReferenceField(switchButton, g_switchInnerButtonField);
            if (!disabled && campusButton) {
                g_toggleBindings.push_back({
                    campusButton,
                    switchButton,
                    texts.size() > 1 ? texts[1] : nullptr,
                    item->modId,
                    detailPrefix,
                    item->configuredEnabled,
                    item->configuredEnabled,
                    false,
                });
            }
            return true;
        }

        void PopulateModPage(
            void* content,
            void* rowTemplate,
            const MasterCatalog& masterCatalog,
            const std::vector<ModPresentationItem>& items,
            const char* emptyTitle) {
            if (!content) return;
            DeactivateContentChildren(content);
            if (items.empty()) {
                CreateModRow(
                    rowTemplate,
                    content,
                    masterCatalog,
                    nullptr,
                    emptyTitle,
                    "请将符合规范的 Mod 放入 Mods 目录。");
            } else {
                for (const auto& item : items) {
                    CreateModRow(
                        rowTemplate, content, masterCatalog, &item, "", "");
                }
            }
            Call(g_forceRebuildLayout,
                 nullptr,
                 {content},
                 "LayoutRebuilder.ForceRebuildLayoutImmediate");
        }

        void SetComponentActive(void* component, const bool active, const char* label) {
            const auto gameObject = component
                ? Call(g_getGameObject, component, {}, label)
                : nullptr;
            if (!gameObject) return;
            auto value = active;
            Call(g_setActive, gameObject, {&value}, label);
        }

        // The orange selection bar is initialized from GetBarSize(), which still
        // sees the serialized three-tab list after the spare tab is hidden.  The
        // native SetSelectIndex path only moves the selected button; it does not
        // change SelectedBarRect.sizeDelta.  Set the rect directly after layout.
        std::string UnityObjectName(void* unityObject) {
            return g_getUnityObjectName
                ? ManagedStringValue(unityObject, g_getUnityObjectName, "Object.get_name")
                : std::string{};
        }

        void RefreshTabSelectionBar(void* barTransform) {
            if (!barTransform || !g_tabButtonGroupType || !g_tabGroupGetSelectedBarRect
                || !g_rectGetSizeDelta || !g_rectSetSizeDelta) {
                LogF("Mod menu: selection bar refresh unavailable type=%d selectedBar=%d getSize=%d setSize=%d.",
                     g_tabButtonGroupType != nullptr,
                     g_tabGroupGetSelectedBarRect != nullptr,
                     g_rectGetSizeDelta != nullptr,
                     g_rectSetSizeDelta != nullptr);
                return;
            }
            Call(g_forceRebuildLayout, nullptr, {barTransform},
                 "rebuild tab bar layout");
            for (const auto group : FindActiveObjectsOfType(g_tabButtonGroupType)) {
                const auto groupTransform = Call(g_getTransform, group, {},
                                                 "tab group get_transform");
                if (groupTransform != barTransform) continue;
                // Initialize() positioned the indicator while the serialized
                // three-tab layout was still active.  Force a 1 -> 0 native
                // position pass so an already-selected index cannot early-out;
                // SetSelectIndex moves the rect but does not change its size,
                // so the explicit size write below is still required.
                if (g_tabGroupSetSelectIndex) {
                    std::int32_t secondIndex = 1;
                    Call(g_tabGroupSetSelectIndex,
                         group,
                         {&secondIndex},
                         "CampusSimpleTabButtonGroup.SetSelectIndex(1)");
                    std::int32_t firstIndex = 0;
                    Call(g_tabGroupSetSelectIndex,
                         group,
                         {&firstIndex},
                         "CampusSimpleTabButtonGroup.SetSelectIndex(0)");
                }
                const auto selectedBar = Call(
                    g_tabGroupGetSelectedBarRect,
                    group,
                    {},
                    "CampusSimpleTabButtonGroup.get_SelectedBarRect");
                Vector2 size;
                if (!selectedBar || !CallValue(
                        g_rectGetSizeDelta,
                        selectedBar,
                        {},
                        "SelectedBarRect.get_sizeDelta",
                        size)) {
                    Log("Mod menu: SelectedBarRect was unavailable after layout rebuild.");
                    return;
                }
                // The two visible tabs are forced to 492 units each.  IDA shows
                // GetBarSize() subtracting _barMargin on both sides; the
                // serialized CampusSimpleTabButtonGroup margin is 56 units.
                constexpr float twoTabButtonWidth = 492.0f;
                constexpr float selectedBarMargin = 56.0f;
                const auto twoTabBarWidth =
                    twoTabButtonWidth - (2.0f * selectedBarMargin);
                size.x = twoTabBarWidth > 0.0f ? twoTabBarWidth : 0.0f;
                Call(g_rectSetSizeDelta,
                     selectedBar,
                     {&size},
                     "SelectedBarRect.set_sizeDelta(two tabs)");
                LogF("Mod menu: selected bar width forced to %.1f (height %.1f).",
                     size.x,
                     size.y);
                return;
            }
            Log("Mod menu: no tab button group matched the tab bar; selection bar left as is.");
        }

        // Calling the direct size fix inside composition is too early: the widths
        // set through SetForceWidth reach the layout on a later frame.  Re-run it
        // for a few frames from the EventSystem hook instead.
        void PumpDeferredSelectionBarRefresh() {
            if (!g_barRefreshTransform) return;
            const auto remaining = g_barRefreshFrames.fetch_sub(1);
            if (remaining <= 0) {
                g_barRefreshTransform = nullptr;
                g_barRefreshFrames.store(0);
                Log("Mod menu: tab selection bar refresh finished.");
                return;
            }
            RefreshTabSelectionBar(g_barRefreshTransform);
        }

        // One-shot dump of the tab bar's children.  The first attempt hid the
        // sibling immediately before the third button and the extra star stayed
        // on screen, so the real child order is not button/separator/button.
        void LogTabBarChildren(void* barTransform) {
            if (!barTransform || g_loggedTabBarChildren) return;
            std::int32_t childCount{};
            if (!CallValue(g_getChildCount, barTransform, {},
                           "tab bar get_childCount", childCount)
                || childCount <= 0 || childCount > 256) {
                return;
            }
            g_loggedTabBarChildren = true;
            // The stars are in none of the tab bar's own nodes: the bar holds
            // Background (no children), four buttons (TouchArea/ScaleRoot/Light)
            // and Overlay (SelectImage, which is the orange indicator).  So they
            // belong to the surrounding frame -- dump the parent's children too.
            if (const auto parent = Call(g_getParent, barTransform, {}, "tab bar parent")) {
                std::int32_t siblingCount{};
                if (CallValue(g_getChildCount, parent, {},
                              "tab bar parent get_childCount", siblingCount)
                    && siblingCount > 0 && siblingCount <= 64) {
                    const auto parentObject = Call(g_getGameObject, parent, {}, "tab bar parent object");
                    LogF("Mod menu: tab bar parent=%p name=\"%s\" children=%d",
                         parent,
                         parentObject ? UnityObjectName(parentObject).c_str() : "<null>",
                         siblingCount);
                    for (std::int32_t sibling = 0; sibling < siblingCount; ++sibling) {
                        const auto node = Call(g_getChild, parent, {&sibling}, "tab bar sibling");
                        const auto nodeObject = node
                            ? Call(g_getGameObject, node, {}, "tab bar sibling object")
                            : nullptr;
                        LogF("Mod menu:   sibling[%d] transform=%p name=\"%s\"",
                             sibling,
                             node,
                             nodeObject ? UnityObjectName(nodeObject).c_str() : "<null>");
                    }
                }
            }
            for (std::int32_t index = 0; index < childCount; ++index) {
                const auto child = Call(g_getChild, barTransform, {&index}, "tab bar GetChild");
                const auto object = child
                    ? Call(g_getGameObject, child, {}, "tab bar child get_gameObject")
                    : nullptr;
                const auto name = object ? UnityObjectName(object) : std::string{"<null>"};
                LogF("Mod menu: tab bar child[%d] transform=%p name=\"%s\"",
                     index, child, name.c_str());
                // The stars are not siblings of the buttons, and they sit at the
                // three-tab boundaries, so they belong to the decorative nodes.
                if (name != "Background" && name != "Overlay"
                    && name.rfind("CampusSimpleTabButton", 0) != 0) {
                    continue;
                }
                std::int32_t innerCount{};
                if (!CallValue(g_getChildCount, child, {},
                               "decor get_childCount", innerCount)
                    || innerCount <= 0 || innerCount > 64) {
                    continue;
                }
                for (std::int32_t inner = 0; inner < innerCount; ++inner) {
                    const auto grandChild = Call(g_getChild, child, {&inner}, "decor GetChild");
                    const auto grandObject = grandChild
                        ? Call(g_getGameObject, grandChild, {}, "decor child get_gameObject")
                        : nullptr;
                    LogF("Mod menu:   %s child[%d] transform=%p name=\"%s\"",
                         name.c_str(),
                         inner,
                         grandChild,
                         grandObject ? UnityObjectName(grandObject).c_str() : "<null>");
                }
            }
        }

        void HideTabButtonLight(void* button) {
            if (!button) return;
            const auto buttonTransform = Call(
                g_getTransform,
                button,
                {},
                "tab button get_transform for Light");
            std::int32_t childCount{};
            if (!buttonTransform || !CallValue(
                    g_getChildCount,
                    buttonTransform,
                    {},
                    "tab button get_childCount for Light",
                    childCount)
                || childCount <= 0 || childCount > 64) {
                return;
            }
            for (std::int32_t index = 0; index < childCount; ++index) {
                const auto child = Call(
                    g_getChild,
                    buttonTransform,
                    {&index},
                    "tab button GetChild for Light");
                const auto object = child
                    ? Call(g_getGameObject, child, {}, "tab Light get_gameObject")
                    : nullptr;
                if (!object || UnityObjectName(object) != "Light") continue;
                SetComponentActive(child, false, "hide hair tab Light");
                LogF("Mod menu: hidden tab button Light child=%p.", child);
                return;
            }
            Log("Mod menu: hair tab Light child was not found.");
        }

        // The tab bar itself has no separator nodes.  The decorative star is a
        // Light child on each CampusSimpleTabButton, so hiding the spare button
        // is not enough; hide the hair tab's Light explicitly.
        void HideSeparatorBeforeTab(void* hiddenButton, void* lastVisibleButton) {
            const auto hiddenTransform = hiddenButton
                ? Call(g_getTransform, hiddenButton, {}, "hidden tab get_transform")
                : nullptr;
            const auto barTransform = hiddenTransform
                ? Call(g_getParent, hiddenTransform, {}, "tab bar get_parent")
                : nullptr;
            std::int32_t childCount{};
            if (!barTransform
                || !CallValue(g_getChildCount, barTransform, {},
                              "tab bar get_childCount", childCount)
                || childCount <= 0 || childCount > 256) {
                Log("Mod menu: tab separator parent is unavailable; separator left visible.");
                return;
            }
            const auto keepTransform = lastVisibleButton
                ? Call(g_getTransform, lastVisibleButton, {}, "last tab get_transform")
                : nullptr;

            // There is no sibling separator to hide.  Keep the diagnostic dump,
            // then schedule the direct SelectedBarRect correction.
            (void)childCount;
            (void)hiddenTransform;
            (void)keepTransform;
            LogTabBarChildren(barTransform);
            // Schedule the indicator recompute for the following frames.
            g_barRefreshTransform = barTransform;
            g_barRefreshFrames.store(8);
        }

        bool ComposeModScreen(void* tab) {
            ModPresentationModel model;
            if (!LoadPresentationModel(model)) {
                Log("Mod menu: full-screen composition stopped because the Runtime is unavailable.");
                return false;
            }

            if (!tab) {
                Log("Mod menu: active Setting CampusSimpleTab is null.");
                return false;
            }

            std::int32_t firstIndex = 0;
            std::int32_t secondIndex = 1;
            std::int32_t thirdIndex = 2;
            const auto costumePage = Call(g_tabGetPage, tab, {&firstIndex}, "tab GetPage(0)");
            const auto hairPage = Call(g_tabGetPage, tab, {&secondIndex}, "tab GetPage(1)");
            const auto unusedPage = Call(g_tabGetPage, tab, {&thirdIndex}, "tab GetPage(2)");
            const auto costumeButton = Call(g_tabGetButton, tab, {&firstIndex}, "tab GetButton(0)");
            const auto hairButton = Call(g_tabGetButton, tab, {&secondIndex}, "tab GetButton(1)");
            const auto unusedButton = Call(g_tabGetButton, tab, {&thirdIndex}, "tab GetButton(2)");
            if (!costumePage || !hairPage || !costumeButton || !hairButton) {
                Log("Mod menu: Setting tabs or pages are incomplete.");
                return false;
            }

            Call(g_tabButtonSetText,
                 costumeButton,
                 {UnityResolve::UnityType::String::New("服装")},
                 "set costume tab text");
            Call(g_tabButtonSetText,
                 hairButton,
                 {UnityResolve::UnityType::String::New("发型")},
                 "set hair tab text");
            float halfWidth = 492.0f;
            Call(g_tabButtonSetForceWidth,
                 costumeButton,
                 {&halfWidth},
                 "set costume tab width");
            Call(g_tabButtonSetForceWidth,
                 hairButton,
                 {&halfWidth},
                 "set hair tab width");
            SetComponentActive(unusedPage, false, "hide unused setting page");
            SetComponentActive(unusedButton, false, "hide unused setting tab");
            HideTabButtonLight(hairButton);
            HideSeparatorBeforeTab(unusedButton, hairButton);
            bool canFlick = false;
            Call(g_tabSetCanFlick, tab, {&canFlick}, "disable setting tab flick");

            const auto settingViews = FindActiveObjectsOfType(g_settingScreenViewType);
            const auto settingView = settingViews.empty() ? nullptr : settingViews.front();
            const auto overlayTitle = ReadReferenceField(settingView, g_overlayTitleViewField);
            if (overlayTitle) {
                Call(g_overlayTitleSetTitle,
                     overlayTitle,
                     {UnityResolve::UnityType::String::New("MOD 管理")},
                     "OverlayTitleView.SetTitle");
            }

            const auto preferenceView = ReadReferenceField(
                costumePage, g_preferencePageViewField);
            const auto templateSwitch = ReadReferenceField(
                preferenceView, g_vsyncToggleField);
            const auto templateSwitchTransform = templateSwitch
                ? Call(g_getTransform,
                       templateSwitch,
                       {},
                       "VSync SwitchButton get_transform")
                : nullptr;
            const auto costumeContent = FindScrollContent(
                costumePage, templateSwitchTransform);
            const auto hairContent = FindScrollContent(hairPage, nullptr);
            const auto rowTemplate = FindSwitchRowTemplate(
                templateSwitch, costumeContent);
            if (!preferenceView || !templateSwitch || !costumeContent
                || !hairContent || !rowTemplate) {
                LogF("Mod menu: setting template incomplete preference=%d switch=%d "
                     "contents=%d/%d row=%d.",
                     preferenceView != nullptr,
                     templateSwitch != nullptr,
                     costumeContent != nullptr,
                     hairContent != nullptr,
                     rowTemplate != nullptr);
                return false;
            }

            g_toggleBindings.clear();
            const auto masterCatalog = LoadMasterCatalog();
            PopulateModPage(
                costumeContent,
                rowTemplate,
                masterCatalog,
                model.costumeMods,
                "暂无服装 Mod");
            PopulateModPage(
                hairContent,
                rowTemplate,
                masterCatalog,
                model.hairMods,
                "暂无发型 Mod");
            g_activeModTab = tab;
            LogF("Mod menu: full-screen Setting template composed rows=%zu tabs=2.",
                 g_toggleBindings.size());
            return true;
        }

        bool GuardedComposeModScreen(void* tab) {
            __try {
                return ComposeModScreen(tab);
            }
            __except (EXCEPTION_EXECUTE_HANDLER) {
                g_toggleBindings.clear();
                LogError("Mod menu: full-screen Setting composition faulted; original page retained.");
                return false;
            }
        }

        // Clone one existing sub button, retag it and register it under our own
        // key.  Runs before the original SetEvent so the game wires the click.
        void* EnsureEntry(void* presenter) {
            g_injectStep.store(1);
            g_menuPresenterInstance = presenter;
            if (!presenter || !g_commonViewField || g_commonViewField->offset < 0
                || !g_subButtonsField || g_subButtonsField->offset < 0
                || !g_buttonField || g_buttonField->offset < 0) {
                LogError("Mod menu: required presenter/view fields are unavailable; entry not injected.");
                return nullptr;
            }
            const auto view = *reinterpret_cast<void**>(
                static_cast<char*>(presenter) + g_commonViewField->offset);
            if (!view) return nullptr;
            g_menuViewInstance = view;
            g_menuCloseButton = ReadReferenceField(view, g_menuCloseButtonField);
            if (g_injectedViews.contains(view)) return nullptr;

            g_injectStep.store(2);
            const auto dictionary = *reinterpret_cast<void**>(
                static_cast<char*>(view) + g_subButtonsField->offset);
            if (!dictionary) {
                LogError("Mod menu: MenuView._subButtons is null; entry not injected.");
                return nullptr;
            }
            LogF("Mod menu: step 2 view=%s dictionary=%s", ClassNameOf(view), ClassNameOf(dictionary));

            g_injectStep.store(3);
            std::int32_t templateType = kTemplateButtonType;
            const auto templateButton = Call(g_getSubButton, view, {&templateType},
                                             "MenuView.GetSubButton");
            if (!templateButton) {
                LogError("Mod menu: no template sub button; entry not injected.");
                return nullptr;
            }
            LogF("Mod menu: step 3 template=%s", ClassNameOf(templateButton));
            const auto templateCampusButton = ReadReferenceField(templateButton, g_buttonField);
            if (!templateCampusButton) {
                LogError("Mod menu: template has no CampusButton; entry not injected.");
                return nullptr;
            }

            std::int32_t settingType = 16;
            const auto settingView = Call(
                g_getButton, view, {&settingType}, "MenuView.GetButton(Setting)");
            g_settingButton = ReadReferenceField(settingView, g_buttonField);
            if (!settingView || !g_settingButton) {
                LogError("Mod menu: the real Setting button is unavailable; entry not injected.");
                return nullptr;
            }
            LogF("Mod menu: native Setting route button=%s.", ClassNameOf(g_settingButton));

            g_injectStep.store(4);
            const auto templateTransform = Call(g_getTransform, templateButton, {},
                                                "Component.get_transform");
            g_injectStep.store(5);
            const auto parent = templateTransform
                ? Call(g_getParent, templateTransform, {}, "Transform.get_parent")
                : nullptr;
            if (!parent) {
                LogError("Mod menu: sub button parent not found; entry not injected.");
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
                LogError("Mod menu: clone returned null; entry not injected.");
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
            g_modButton = ReadReferenceField(clone, g_buttonField);
            LogF("Mod menu: step 7 button=%s", g_modButton ? ClassNameOf(g_modButton) : "<null>");
            if (!g_modButton) {
                if (const auto cloneObject = Call(g_getGameObject, clone, {}, "clone get_gameObject")) {
                    bool active = false;
                    Call(g_setActive, cloneObject, {&active}, "disable inert clone");
                }
                LogError("Mod menu: cloned entry has no CampusButton; injection disabled for this session.");
                g_injectionFaulted.store(true);
                return nullptr;
            }

            g_injectedViews.insert(view);
            LogF("Mod menu: entry injected into %s.", ClassNameOf(view));
            return clone;
        }

        // MenuButtonViewBase.SetIcon is private and reads _buttonType, which
        // still says ClearCache on the clone; ask the shared icon ScriptableObject
        // the clone already carries for another type's sprite instead.
        void ApplyIcon(void* button) {
            const auto setting = ReadReferenceField(button, g_menuButtonIconSettingField);
            const auto icon = ReadReferenceField(button, g_menuButtonIconField);
            if (!setting || !icon || !g_menuIconSettingGetSprite || !g_imageSetSprite) return;
            std::int32_t iconType = kEntryIconButtonType;
            const auto sprite = Call(g_menuIconSettingGetSprite,
                                     setting,
                                     {&iconType},
                                     "MenuButtonIconSetting.GetSprite");
            if (!sprite) {
                Log("Mod menu: no entry icon sprite; the template icon is kept.");
                return;
            }
            Call(g_imageSetSprite, icon, {sprite}, "entry Image.set_sprite");
        }

        void ApplyLabel(void* button) {
            if (!button) return;
            Call(g_setCustomText, button,
                 {UnityResolve::UnityType::String::New("MOD 管理")},
                 "MenuButtonViewBase.SetCustomText");
            ApplyIcon(button);
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
                LogErrorF("Mod menu: entry injection faulted at step %d; disabled for this session.",
                     g_injectStep.load());
                return nullptr;
            }
        }

        void GuardedApplyLabel(void* button) {
            __try {
                ApplyLabel(button);
            }
            __except (EXCEPTION_EXECUTE_HANDLER) {
                LogError("Mod menu: applying the entry label faulted.");
            }
        }

        const ModPresentationItem* FindPresentedMod(
            const ModPresentationModel& model,
            const std::string& modId) {
            for (const auto& item : model.costumeMods) {
                if (item.modId == modId) return &item;
            }
            for (const auto& item : model.hairMods) {
                if (item.modId == modId) return &item;
            }
            return nullptr;
        }

        void ApplyPresentationModelToToggleBindings(
            const ModPresentationModel& model) {
            for (auto& binding : g_toggleBindings) {
                const auto item = FindPresentedMod(model, binding.modId);
                if (!item) continue;
                binding.enabled = item->configuredEnabled;
                binding.displayedState = item->configuredEnabled;
                binding.visualRefreshPending = true;
                SetCampusText(
                    binding.statusText,
                    RowDetailText(binding.detailPrefix, item->statusText),
                    "refresh Mod row after toggle");
            }
        }

        bool HandleModToggle(void* campusButton) {
            const auto binding = std::find_if(
                g_toggleBindings.begin(),
                g_toggleBindings.end(),
                [campusButton](const auto& candidate) {
                    return candidate.campusButton == campusButton;
                });
            if (binding == g_toggleBindings.end()) return false;

            const bool desired = !binding->enabled;
            RuntimeClient runtime;
            const auto result = runtime.Connect()
                ? runtime.SetModEnabled(binding->modId, desired)
                : static_cast<GmrResult>(GMR_E_NOT_INITIALIZED);
            if (result != GMR_OK) {
                if (result == GMR_E_TARGET_CONFLICT) {
                    ModPresentationModel refreshed;
                    if (LoadPresentationModel(refreshed)) {
                        ApplyPresentationModelToToggleBindings(refreshed);
                        const auto item = FindPresentedMod(refreshed, binding->modId);
                        LogF(
                            "Mod menu: conflicting enable rejected mod=%s status=%s.",
                            binding->modId.c_str(),
                            item ? item->statusText.c_str() : "conflict");
                        return true;
                    }
                }
                binding->displayedState = binding->enabled;
                binding->visualRefreshPending = true;
                SetCampusText(
                    binding->statusText,
                    RowDetailText(
                        binding->detailPrefix,
                        "写入失败（错误 " + std::to_string(result) + "）"),
                    "set Mod write error");
                LogF("Mod menu: setModEnabled failed mod=%s enabled=%d result=%u.",
                     binding->modId.c_str(), desired, result);
                return true;
            }

            binding->enabled = desired;
            auto displayed = desired;
            std::string status = desired ? "启用中" : "已关闭";
            ModPresentationModel refreshed;
            if (LoadPresentationModel(refreshed)) {
                ApplyPresentationModelToToggleBindings(refreshed);
                if (const auto item = FindPresentedMod(refreshed, binding->modId)) {
                    displayed = item->configuredEnabled;
                    status = item->statusText;
                }
            }
            else {
                binding->displayedState = displayed;
                binding->visualRefreshPending = true;
                SetCampusText(
                    binding->statusText,
                    RowDetailText(binding->detailPrefix, status),
                    "set Mod hot-toggle status");
            }
            LogF("Mod menu: setModEnabled succeeded mod=%s enabled=%d status=%s.",
                 binding->modId.c_str(), displayed, status.c_str());
            return true;
        }

        void ReconcileToggleVisuals() {
            bool animate = false;
            for (auto& binding : g_toggleBindings) {
                if (!binding.visualRefreshPending) continue;
                auto displayed = binding.displayedState;
                Call(g_switchSetIsOn,
                     binding.switchButton,
                     {&displayed, &animate},
                     "reconcile SwitchButton state after input");
                binding.visualRefreshPending = false;
                LogF("Mod menu: switch visual reconciled mod=%s enabled=%d.",
                     binding.modId.c_str(),
                     displayed);
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

        void UNITY_CALLING_CONVENTION SettingSetEventHook(void* self, void* method) {
            if (g_settingSetEventOriginal) g_settingSetEventOriginal(self, method);
            if (!g_modScreenPending.load()) return;
            const auto tab = ReadReferenceField(self, g_settingTabField);
            Log("Mod menu: SettingTopScreen.SetEvent reached for Mod management.");
            if (GuardedComposeModScreen(tab)) {
                g_modScreenPending.store(false);
                Log("Mod menu: full-screen composition completed from SetEvent hook.");
            }
        }

        void UNITY_CALLING_CONVENTION EventSystemUpdateHook(void* self, void* method) {
            if (g_eventSystemUpdateOriginal) g_eventSystemUpdateOriginal(self, method);
            ReconcileToggleVisuals();
            PumpDeferredSelectionBarRefresh();
            if (!g_modScreenPending.load()) return;

            const auto frame = g_pendingPollFrames.fetch_add(1) + 1;
            const auto tab = FindActiveSettingTab(g_activeModTab);
            if (tab && GuardedComposeModScreen(tab)) {
                g_modScreenPending.store(false);
                LogF("Mod menu: full-screen composition completed from EventSystem poll after %u frames.",
                     frame);
                return;
            }
            if (frame == 300) {
                Log("Mod menu: waiting for initialized Setting tab after 300 EventSystem frames.");
            } else if (frame >= 1200) {
                g_modScreenPending.store(false);
                Log("Mod menu: Setting tab was not ready after 1200 frames; pending state cleared.");
            }
        }

        // Campus does not use UnityEngine.UI.Button: menu entries hold a
        // Campus.Common.CampusButton, and every click funnels through
        // CampusButtonBase.OnClicked, which CampusButton does not override.
        // Recognising ours by identity there needs no listener, no delegate and
        // no cooperation from the menu.
        void UNITY_CALLING_CONVENTION PressHook(void* self, void* method) {
            if (self && HandleModToggle(self)) {
                // Let the native button body run as well: the click sound and
                // press animation live there, and the settings page plays them
                // for its own switches.  The switch's own ON/OFF flip is undone
                // at frame end by ReconcileToggleVisuals(), which already runs
                // after the game's input processing.
                if (g_pressOriginal) g_pressOriginal(self, method);
                return;
            }
            if (self && self == g_settingButton && IsActiveModTabPresent()) {
                Log("Mod menu: system Setting requested from the active Mod page.");
                if (ReloadActiveModScreenAsSettings()) {
                    const auto menuClosed = GuardedCloseCurrentMenu(method);
                    LogF("Mod menu: active Mod page reloaded as system Setting; menuClosed=%d.",
                         menuClosed);
                    return;
                }
                // Keep the native Setting action as a compatibility fallback
                // if a future build strips or renames ICampusScreen.Reload.
                Log("Mod menu: in-place Setting reload unavailable; falling back to the native route.");
            }
            if (self && self == g_modButton) {
                Log("Mod menu: entry pressed.");
                if (IsActiveModTabPresent()) {
                    const auto menuClosed = GuardedCloseCurrentMenu(method);
                    LogF("Mod menu: current Mod management screen is already active; "
                         "duplicate navigation consumed and menuClosed=%d.",
                         menuClosed);
                } else if (g_modScreenPending.load()) {
                    Log("Mod menu: Mod management navigation is already pending; duplicate click consumed.");
                } else if (!g_fullScreenReady.load()) {
                    Log("Mod menu: full-screen controls are unavailable; entry retained but navigation was not attempted.");
                } else if (const auto currentSettingTab = FindActiveSettingTab()) {
                    // When the global menu is opened on top of system Setting,
                    // reuse that initialized screen instead of pushing a second
                    // SettingTopScreen onto the navigation stack.
                    if (GuardedComposeModScreen(currentSettingTab)) {
                        g_modScreenPending.store(false);
                        g_pendingPollFrames.store(0);
                        const auto menuClosed = GuardedCloseCurrentMenu(method);
                        LogF("Mod menu: current system Setting page reused in place; menuClosed=%d.",
                             menuClosed);
                    } else {
                        Log("Mod menu: current system Setting page could not be composed; navigation consumed to avoid stacking duplicate Setting pages.");
                    }
                } else if (g_menuPresenterInstance && g_setSelectedButtonType
                           && g_outGameOnSelected) {
                    std::int32_t settingType = 16;
                    bool selectionWritten = false;
                    Call(g_setSelectedButtonType,
                         g_menuPresenterInstance,
                         {&settingType},
                         "MenuPresenter.set_SelectedButtonType(Setting)",
                         &selectionWritten);
                    if (!selectionWritten) {
                        Log("Mod menu: Setting route selection could not be written.");
                        return;
                    }
                    g_modScreenPending.store(true);
                    g_pendingPollFrames.store(0);
                    bool routeDispatched = false;
                    Call(g_outGameOnSelected,
                         g_menuPresenterInstance,
                         {},
                         "OutGameMenuPresenter.OnSelected(Setting)",
                         &routeDispatched);
                    if (!routeDispatched) {
                        g_modScreenPending.store(false);
                        Log("Mod menu: Setting route dispatch failed; pending state cleared.");
                    } else {
                        Log("Mod menu: Setting route dispatched through OutGameMenuPresenter.");
                    }
                } else {
                    Log("Mod menu: presenter Setting route is unavailable; full-screen page was not opened.");
                }
                // The entry is cloned from ClearCache, so its template action is
                // always consumed.
                return;
            }
            if (g_pressOriginal) g_pressOriginal(self, method);
        }

        void UNITY_CALLING_CONVENTION FooterSetEventHook(void* self, void* method) {
            if (g_footerSetEventOriginal) g_footerSetEventOriginal(self, method);
            if (self && self != g_footerPresenter) {
                g_footerPresenter = self;
                LogF("Mod menu: out-game footer captured (%s).", ClassNameOf(self));
            }
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
            g_methodGetParam = reinterpret_cast<MethodGetParamFn>(import("il2cpp_method_get_param"));
            g_typeGetName = reinterpret_cast<TypeGetNameFn>(import("il2cpp_type_get_name"));
            g_il2cppFree = reinterpret_cast<Il2CppFreeFn>(import("il2cpp_free"));
            g_classGetType = reinterpret_cast<ClassGetTypeFn>(import("il2cpp_class_get_type"));
            g_typeGetObject = reinterpret_cast<TypeGetObjectFn>(import("il2cpp_type_get_object"));
            g_objectUnbox = reinterpret_cast<ObjectUnboxFn>(import("il2cpp_object_unbox"));
            if (!g_objectGetClass || !g_classGetMethods || !g_methodGetName
                || !g_methodGetParamCount || !g_runtimeInvoke || !g_classGetType
                || !g_typeGetObject || !g_objectUnbox) {
                LogError("Mod menu: required il2cpp exports are missing; entry disabled.");
                return;
            }

            UnityResolve::Init(gameAssembly, UnityResolve::Mode::Il2Cpp, false);
            UnityResolve::ThreadAttach();

            const auto assembly = UnityResolve::Get("Assembly-CSharp.dll");
            const auto core = UnityResolve::Get("UnityEngine.CoreModule.dll");
            const auto ui = UnityResolve::Get("UnityEngine.UI.dll");
            const auto submodule = UnityResolve::Get("campus-submodule.Runtime.dll");
            g_menuPresenter = assembly ? assembly->Get("MenuPresenter", "Campus.Common") : nullptr;
            const auto outGameMenuPresenter = assembly
                ? assembly->Get("OutGameMenuPresenter", "Campus.OutGame") : nullptr;
            const auto menuView = assembly ? assembly->Get("MenuView", "Campus.Common") : nullptr;
            const auto buttonViewBase = assembly ? assembly->Get("MenuButtonViewBase", "Campus.Common") : nullptr;
            const auto buttonBase = submodule
                ? submodule->Get("CampusButtonBase", "Campus.Common")
                : nullptr;
            if (!g_menuPresenter || !menuView || !buttonViewBase || !buttonBase) {
                LogErrorF("Mod menu: entry classes missing presenter=%d view=%d buttonView=%d "
                     "buttonBase=%d; entry disabled.",
                     g_menuPresenter != nullptr, menuView != nullptr,
                     buttonViewBase != nullptr, buttonBase != nullptr);
                return;
            }

            const auto settingTopPresenter = assembly
                ? assembly->Get("SettingTopScreenPresenter", "Campus.OutGame") : nullptr;
            const auto settingTopView = assembly
                ? assembly->Get("SettingTopScreenView", "Campus.OutGame") : nullptr;
            const auto preferencePage = assembly
                ? assembly->Get("PreferenceTabPage", "Campus.OutGame") : nullptr;
            const auto preferenceView = assembly
                ? assembly->Get("PreferenceTabView", "Campus.OutGame") : nullptr;
            const auto outGameScreenView = assembly
                ? assembly->Get("OutGameScreenViewBase", "Campus.OutGame") : nullptr;
            // These shared UI controls are not guaranteed to remain in
            // campus-submodule.Runtime.dll.  The current PC build places some
            // of them in Assembly-CSharp.dll, so resolve by full managed name
            // across the loaded images with the likely images tried first.
            const auto campusSimpleTab = FindClassAcrossAssemblies(
                "CampusSimpleTab", "Campus.Common.UI", {assembly, submodule});
            const auto campusSimpleTabButton = FindClassAcrossAssemblies(
                "CampusSimpleTabButton", "Campus.Common.UI", {assembly, submodule});
            const auto switchButton = FindClassAcrossAssemblies(
                "SwitchButton", "Campus.Common", {assembly, submodule});
            const auto campusText = FindClassAcrossAssemblies(
                "CampusText", "Campus.Common", {submodule, assembly});
            const auto overlayTitle = FindClassAcrossAssemblies(
                "OverlayTitleView", "Campus.Common", {assembly, submodule});
            const auto menuButtonIconSetting = assembly
                ? assembly->Get("MenuButtonIconSetting", "Campus.Common") : nullptr;
            const auto image = ui ? ui->Get("Image", "UnityEngine.UI") : nullptr;
            const auto scrollRect = ui ? ui->Get("ScrollRect", "UnityEngine.UI") : nullptr;
            const auto layoutRebuilder = ui
                ? ui->Get("LayoutRebuilder", "UnityEngine.UI") : nullptr;
            const auto eventSystem = ui
                ? ui->Get("EventSystem", "UnityEngine.EventSystems") : nullptr;
            const auto rectTransform = core
                ? core->Get("RectTransform", "UnityEngine") : nullptr;
            const auto resources = core
                ? core->Get("Resources", "UnityEngine") : nullptr;
            const auto masterManager = assembly
                ? assembly->Get("MasterManager", "Campus.Common.Master") : nullptr;
            const auto costumeMaster = assembly
                ? assembly->Get("CostumeMaster", "Campus.Common.Master") : nullptr;
            const auto costumeHeadMaster = assembly
                ? assembly->Get("CostumeHeadMaster", "Campus.Common.Master") : nullptr;
            const auto costume = assembly
                ? assembly->Get("Costume", "Campus.Common.Proto.Client.Master") : nullptr;
            const auto costumeHead = assembly
                ? assembly->Get("CostumeHead", "Campus.Common.Proto.Client.Master") : nullptr;
            const auto character = assembly
                ? assembly->Get("Character", "Campus.Common.Proto.Client.Master") : nullptr;
            const auto commonCostumeThumbnail = assembly
                ? assembly->Get("CostumeThumbnailView", "Campus.Common.UI")
                : nullptr;
            g_commonViewField = g_menuPresenter->Get<UnityResolve::Field>("_commonView");
            g_subButtonsField = menuView->Get<UnityResolve::Field>("_subButtons");
            g_buttonField = buttonViewBase->Get<UnityResolve::Field>("_button");
            g_textField = buttonViewBase->Get<UnityResolve::Field>("_text");
            g_menuButtonIconField = buttonViewBase->Get<UnityResolve::Field>("_icon");
            g_menuButtonIconSettingField =
                buttonViewBase->Get<UnityResolve::Field>("_setting");
            g_menuCloseButtonField = menuView->Get<UnityResolve::Field>("_closeButton");
            g_settingTabField = settingTopPresenter
                ? settingTopPresenter->Get<UnityResolve::Field>("_tab") : nullptr;
            g_preferencePageViewField = preferencePage
                ? preferencePage->Get<UnityResolve::Field>("_view") : nullptr;
            g_vsyncToggleField = preferenceView
                ? preferenceView->Get<UnityResolve::Field>("_vsyncToggleButton") : nullptr;
            g_switchInnerButtonField = switchButton
                ? switchButton->Get<UnityResolve::Field>("_button") : nullptr;
            g_overlayTitleViewField = outGameScreenView
                ? outGameScreenView->Get<UnityResolve::Field>("_overlayTitleView") : nullptr;
            g_getSubButton = FindMethodInClass(menuView->address, "GetSubButton", 1);
            g_getButton = FindMethodInClass(menuView->address, "GetButton", 1);
            g_setCustomText = FindMethodInClass(buttonViewBase->address, "SetCustomText", 1);
            g_menuIconSettingGetSprite = menuButtonIconSetting
                ? FindMethodInClass(menuButtonIconSetting->address, "GetSprite", 1)
                : nullptr;
            g_imageSetSprite = image
                ? FindMethodInClass(image->address, "set_sprite", 1) : nullptr;
            g_setSelectedButtonType = FindMethodInClass(
                g_menuPresenter->address, "set_SelectedButtonType", 1);
            g_outGameOnSelected = outGameMenuPresenter
                ? FindMethodInClass(outGameMenuPresenter->address, "OnSelected", 0) : nullptr;
            g_reloadSettingScreen = settingTopPresenter
                ? FindMethodInClass(
                    settingTopPresenter->address, "Campus.ICampusScreen.Reload", 0)
                : nullptr;
            if (!g_reloadSettingScreen && settingTopPresenter) {
                g_reloadSettingScreen = FindMethodInClass(
                    settingTopPresenter->address, "Reload", 0);
            }
            g_scrollGetContent = scrollRect
                ? FindMethodInClass(scrollRect->address, "get_content", 0) : nullptr;
            g_tabGetPage = campusSimpleTab
                ? FindMethodInClass(campusSimpleTab->address, "GetPage", 1) : nullptr;
            g_tabGetButton = campusSimpleTab
                ? FindMethodInClass(campusSimpleTab->address, "GetButton", 1) : nullptr;
            g_tabSetCanFlick = campusSimpleTab
                ? FindMethodInClass(campusSimpleTab->address, "set_CanFlick", 1) : nullptr;
            g_tabButtonSetText = campusSimpleTabButton
                ? FindMethodInClass(campusSimpleTabButton->address, "SetText", 1) : nullptr;
            const auto campusSimpleTabButtonGroup =
                assembly->Get("CampusSimpleTabButtonGroup", "Campus.Common.UI");
            g_tabButtonGroupType = ManagedTypeOf(campusSimpleTabButtonGroup);
            g_tabGroupSetSelectIndex = campusSimpleTabButtonGroup
                ? FindMethodInClass(
                    campusSimpleTabButtonGroup->address,
                    "SetSelectIndex",
                    1)
                : nullptr;
            g_tabGroupGetSelectedBarRect = campusSimpleTabButtonGroup
                ? FindMethodInClass(
                    campusSimpleTabButtonGroup->address,
                    "get_SelectedBarRect",
                    0)
                : nullptr;
            g_tabButtonSetForceWidth = campusSimpleTabButton
                ? FindMethodInClass(campusSimpleTabButton->address, "SetForceWidth", 1) : nullptr;
            g_switchSetIsOn = switchButton
                ? FindMethodInClass(switchButton->address, "SetIsOn", 2) : nullptr;
            g_switchSetDisabled = switchButton
                ? FindMethodInClass(switchButton->address, "SetDisabled", 1) : nullptr;
            g_campusTextSetText = campusText
                ? FindMethodInClass(campusText->address, "set_text", 1) : nullptr;
            g_campusTextSetLocalizeKey = campusText
                ? FindMethodInClass(campusText->address, "set_LocalizeKey", 1) : nullptr;
            g_overlayTitleSetTitle = overlayTitle
                ? FindMethodInClass(overlayTitle->address, "SetTitle", 1) : nullptr;
            g_forceRebuildLayout = layoutRebuilder
                ? FindMethodInClass(layoutRebuilder->address, "ForceRebuildLayoutImmediate", 1)
                : nullptr;
            g_resourcesFindObjectsOfTypeAll = resources
                ? FindNonGenericMethodInClass(
                    resources->address, "FindObjectsOfTypeAll", 1)
                : nullptr;
            g_rectGetAnchoredPosition = rectTransform
                ? FindMethodInClass(rectTransform->address, "get_anchoredPosition", 0)
                : nullptr;
            g_rectSetAnchoredPosition = rectTransform
                ? FindMethodInClass(rectTransform->address, "set_anchoredPosition", 1)
                : nullptr;
            g_rectGetSizeDelta = rectTransform
                ? FindMethodInClass(rectTransform->address, "get_sizeDelta", 0)
                : nullptr;
            g_rectSetSizeDelta = rectTransform
                ? FindMethodInClass(rectTransform->address, "set_sizeDelta", 1)
                : nullptr;
            g_rectGetPivot = rectTransform
                ? FindMethodInClass(rectTransform->address, "get_pivot", 0)
                : nullptr;
            g_rectSetPivot = rectTransform
                ? FindMethodInClass(rectTransform->address, "set_pivot", 1)
                : nullptr;
            g_rectSetAnchorMin = rectTransform
                ? FindMethodInClass(rectTransform->address, "set_anchorMin", 1)
                : nullptr;
            g_rectSetAnchorMax = rectTransform
                ? FindMethodInClass(rectTransform->address, "set_anchorMax", 1)
                : nullptr;
            g_masterManagerGetCostumeMaster = masterManager
                ? FindMethodInClass(masterManager->address, "get_CostumeMaster", 0)
                : nullptr;
            g_masterManagerGetCostumeHeadMaster = masterManager
                ? FindMethodInClass(masterManager->address, "get_CostumeHeadMaster", 0)
                : nullptr;
            g_costumeMasterGetAll = costumeMaster
                ? FindMethodInClass(
                    costumeMaster->address, "GetAllWithSortByKey", 1)
                : nullptr;
            g_costumeHeadMasterGetAll = costumeHeadMaster
                ? FindMethodInClass(
                    costumeHeadMaster->address, "GetAllWithSortByKey", 1)
                : nullptr;
            g_costumeGetId = costume
                ? FindMethodInClass(costume->address, "get_Id", 0) : nullptr;
            g_costumeGetName = costume
                ? FindMethodInClass(costume->address, "get_Name", 0) : nullptr;
            g_costumeGetCharacter = costume
                ? FindMethodInClass(costume->address, "GetCharacter", 0) : nullptr;
            g_costumeGetHeadId = costume
                ? FindMethodInClass(costume->address, "get_CostumeHeadId", 0) : nullptr;
            g_costumeGetDefaultHeadId = costume
                ? FindMethodInClass(
                    costume->address, "get_DefaultCostumeHeadId", 0)
                : nullptr;
            g_costumeHeadGetId = costumeHead
                ? FindMethodInClass(costumeHead->address, "get_Id", 0) : nullptr;
            g_costumeHeadGetName = costumeHead
                ? FindMethodInClass(costumeHead->address, "get_Name", 0) : nullptr;
            g_costumeHeadGetCharacter = costumeHead
                ? FindMethodInClass(costumeHead->address, "GetCharacter", 0) : nullptr;
            g_costumeHeadGetHairAssetId = costumeHead
                ? FindMethodInClass(costumeHead->address, "get_HairAssetId", 0) : nullptr;
            g_characterGetName = character
                ? FindMethodInClass(character->address, "get_Name", 0) : nullptr;
            g_commonCostumeThumbnailSet = commonCostumeThumbnail
                ? FindMethodInClass(commonCostumeThumbnail->address, "Set", 1)
                : nullptr;
            g_costumeThumbnailSetShowDetail = commonCostumeThumbnail
                ? FindMethodInClass(
                    commonCostumeThumbnail->address, "set_IsShowDetailEnabled", 1)
                : nullptr;
            g_thumbnailSetGestureEnabled = commonCostumeThumbnail
                ? FindMethodInClass(
                    commonCostumeThumbnail->address, "SetGestureEnabled", 1)
                : nullptr;
            g_settingScreenViewType = ManagedTypeOf(settingTopView);
            g_settingScreenPresenterType = ManagedTypeOf(settingTopPresenter);
            g_scrollRectType = ManagedTypeOf(scrollRect);
            g_campusTextType = ManagedTypeOf(campusText);
            g_switchButtonType = ManagedTypeOf(switchButton);
            g_campusSimpleTabType = ManagedTypeOf(campusSimpleTab);
            g_commonCostumeThumbnailType = ManagedTypeOf(commonCostumeThumbnail);
            if (core) {
                const auto objectClass = core->Get("Object");
                const auto componentClass = core->Get("Component");
                const auto transformClass = core->Get("Transform");
                const auto gameObjectClass = core->Get("GameObject");
                g_cloneWithParent = objectClass
                    ? FindMethodInClass(objectClass->address, "Internal_CloneSingleWithParent", 3) : nullptr;
                g_findObjectsOfTypeByType = objectClass
                    ? FindNonGenericMethodInClass(objectClass->address, "FindObjectsOfType", 1)
                    : nullptr;
                g_getTransform = componentClass
                    ? FindMethodInClass(componentClass->address, "get_transform", 0) : nullptr;
                g_getGameObject = componentClass
                    ? FindMethodInClass(componentClass->address, "get_gameObject", 0) : nullptr;
                g_getUnityObjectName = objectClass
                    ? FindMethodInClass(objectClass->address, "get_name", 0) : nullptr;
                g_gameObjectGetTransform = gameObjectClass
                    ? FindMethodInClass(gameObjectClass->address, "get_transform", 0) : nullptr;
                g_gameObjectGetActiveInHierarchy = gameObjectClass
                    ? FindMethodInClass(
                        gameObjectClass->address, "get_activeInHierarchy", 0)
                    : nullptr;
                g_getComponentByType = gameObjectClass
                    ? FindNonGenericMethodInClass(gameObjectClass->address, "GetComponent", 1)
                    : nullptr;
                g_getComponentsInChildrenByType = componentClass
                    ? FindNonGenericMethodInClass(
                          componentClass->address, "GetComponentsInChildren", 2)
                    : nullptr;
                g_getParent = transformClass
                    ? FindMethodInClass(transformClass->address, "get_parent", 0) : nullptr;
                g_getChildCount = transformClass
                    ? FindMethodInClass(transformClass->address, "get_childCount", 0) : nullptr;
                g_getChild = transformClass
                    ? FindMethodInClass(transformClass->address, "GetChild", 1) : nullptr;
                g_setAsLastSibling = transformClass
                    ? FindMethodInClass(transformClass->address, "SetAsLastSibling", 0) : nullptr;
                g_setActive = gameObjectClass
                    ? FindMethodInClass(gameObjectClass->address, "SetActive", 1) : nullptr;
            }
            LogF("Mod menu: resolved commonView=%d subButtons=%d button=%d text=%d "
                  "getButton=%d getSubButton=%d customText=%d clone=%d transform=%d "
                  "gameObject=%d setActive=%d sibling=%d",
                  g_commonViewField != nullptr, g_subButtonsField != nullptr,
                  g_buttonField != nullptr, g_textField != nullptr,
                  g_getButton != nullptr,
                  g_getSubButton != nullptr,
                  g_setCustomText != nullptr,
                  g_cloneWithParent != nullptr,
                  g_getTransform != nullptr,
                  g_getGameObject != nullptr,
                  g_setActive != nullptr,
                  g_setAsLastSibling != nullptr);
            LogF("Mod menu: field offsets commonView=0x%X subButtons=0x%X button=0x%X text=0x%X",
                  static_cast<unsigned>(g_commonViewField ? g_commonViewField->offset : -1),
                  static_cast<unsigned>(g_subButtonsField ? g_subButtonsField->offset : -1),
                  static_cast<unsigned>(g_buttonField ? g_buttonField->offset : -1),
                  static_cast<unsigned>(g_textField ? g_textField->offset : -1));
            LogF("Mod menu: entry icon symbols icon=0x%X setting=0x%X getSprite=%d setSprite=%d.",
                  static_cast<unsigned>(
                      g_menuButtonIconField ? g_menuButtonIconField->offset : -1),
                  static_cast<unsigned>(
                      g_menuButtonIconSettingField
                          ? g_menuButtonIconSettingField->offset : -1),
                  g_menuIconSettingGetSprite != nullptr,
                  g_imageSetSprite != nullptr);
            LogF("Mod menu: native Setting route presenter=%d setSelected=%d onSelected=%d "
                 "closeButton=%d reload=%d presenterType=%d.",
                  outGameMenuPresenter != nullptr,
                  g_setSelectedButtonType != nullptr,
                  g_outGameOnSelected != nullptr,
                  g_menuCloseButtonField != nullptr,
                  g_reloadSettingScreen != nullptr,
                  g_settingScreenPresenterType != nullptr);
            LogF("Mod menu: game target symbols masters=%d/%d lists=%d/%d "
                 "records=%d/%d/%d thumbnails=%d/%d/%d resources=%d.",
                 // thumbnails: Set+IsShowDetailEnabled / SetGestureEnabled /
                 // CostumeThumbnailView type -- costume and hair share all three.
                 g_masterManagerGetCostumeMaster != nullptr,
                 g_masterManagerGetCostumeHeadMaster != nullptr,
                 g_costumeMasterGetAll != nullptr,
                 g_costumeHeadMasterGetAll != nullptr,
                 g_costumeGetName != nullptr,
                 g_costumeHeadGetName != nullptr,
                 g_characterGetName != nullptr,
                 g_commonCostumeThumbnailSet != nullptr
                     && g_costumeThumbnailSetShowDetail != nullptr,
                 g_thumbnailSetGestureEnabled != nullptr,
                 g_commonCostumeThumbnailType != nullptr,
                 g_resourcesFindObjectsOfTypeAll != nullptr);
            LogF("Mod menu: full-screen symbols fields=%d/%d/%d/%d/%d "
                 "types=%d/%d/%d/%d/%d methods=%d/%d/%d/%d/%d/%d/%d/%d/%d/%d/%d/%d.",
                 g_settingTabField != nullptr,
                 g_preferencePageViewField != nullptr,
                 g_vsyncToggleField != nullptr,
                 g_switchInnerButtonField != nullptr,
                 g_overlayTitleViewField != nullptr,
                 g_settingScreenViewType != nullptr,
                 g_scrollRectType != nullptr,
                 g_campusTextType != nullptr,
                 g_switchButtonType != nullptr,
                 g_campusSimpleTabType != nullptr,
                 g_getComponentByType != nullptr,
                 g_getComponentsInChildrenByType != nullptr,
                 g_findObjectsOfTypeByType != nullptr,
                 g_getChildCount != nullptr,
                 g_getChild != nullptr,
                 g_scrollGetContent != nullptr,
                 g_tabGetPage != nullptr,
                 g_tabGetButton != nullptr,
                 g_tabSetCanFlick != nullptr,
                 g_tabButtonSetText != nullptr,
                 g_switchSetIsOn != nullptr,
                 g_campusTextSetText != nullptr);

            const bool entrySymbolsReady = g_commonViewField && g_commonViewField->offset >= 0
                && g_subButtonsField && g_subButtonsField->offset >= 0
                && g_buttonField && g_buttonField->offset >= 0
                && g_getButton && g_getSubButton && g_setCustomText && g_cloneWithParent
                && g_getTransform && g_getParent && g_getGameObject && g_setActive;
            if (!entrySymbolsReady) {
                Log("Mod menu: required entry symbols are incomplete; entry hooks not installed.");
                return;
            }

            const bool fullScreenSymbolsReady = settingTopPresenter && settingTopView
                && preferencePage && preferenceView && outGameScreenView
                && outGameMenuPresenter && g_setSelectedButtonType && g_outGameOnSelected
                && campusSimpleTab && campusSimpleTabButton && switchButton
                && campusText && overlayTitle && scrollRect && layoutRebuilder && eventSystem
                && g_settingTabField && g_settingTabField->offset >= 0
                && g_preferencePageViewField && g_preferencePageViewField->offset >= 0
                && g_vsyncToggleField && g_vsyncToggleField->offset >= 0
                && g_switchInnerButtonField && g_switchInnerButtonField->offset >= 0
                && g_overlayTitleViewField && g_overlayTitleViewField->offset >= 0
                && g_gameObjectGetTransform && g_setAsLastSibling
                && g_getComponentsInChildrenByType && g_findObjectsOfTypeByType
                && g_getChildCount && g_getChild && g_scrollGetContent
                && g_tabGetPage && g_tabGetButton && g_tabSetCanFlick
                && g_tabButtonSetText && g_tabButtonSetForceWidth
                && g_switchSetIsOn && g_switchSetDisabled && g_campusTextSetText
                && g_campusTextSetLocalizeKey
                && g_overlayTitleSetTitle && g_forceRebuildLayout
                && g_rectGetAnchoredPosition && g_rectSetAnchoredPosition
                && g_rectGetSizeDelta && g_rectSetSizeDelta
                && g_rectGetPivot && g_rectSetPivot
                && g_rectSetAnchorMin && g_rectSetAnchorMax
                && g_settingScreenViewType && g_scrollRectType
                && g_campusTextType && g_switchButtonType && g_campusSimpleTabType;

            const auto initStatus = MH_Initialize();
            if (initStatus != MH_OK && initStatus != MH_ERROR_ALREADY_INITIALIZED) {
                Log("Mod menu: MinHook initialization failed.");
                return;
            }

            // Install the click route before publishing the cloned entry.  The
            // full-screen hook is optional; failure there must not remove the
            // entry from the home menu again.
            const bool pressInstalled = Install(
                buttonBase->Get<UnityResolve::Method>("OnClicked"),
                reinterpret_cast<void*>(&PressHook),
                reinterpret_cast<void**>(&g_pressOriginal),
                "CampusButtonBase.OnClicked");
            const auto footerPresenterClass =
                assembly->Get("OutGameFooterPresenter", "Campus.OutGame");
            g_closeGlobalMenu = footerPresenterClass
                ? FindMethodInClass(footerPresenterClass->address, "CloseGlobalMenu", 0)
                : nullptr;
            const bool footerHookInstalled = Install(
                footerPresenterClass
                    ? footerPresenterClass->Get<UnityResolve::Method>("SetEvent")
                    : nullptr,
                reinterpret_cast<void*>(&FooterSetEventHook),
                reinterpret_cast<void**>(&g_footerSetEventOriginal),
                "OutGameFooterPresenter.SetEvent");
            LogF("Mod menu: footer close route hook=%d closeGlobalMenu=%d.",
                 footerHookInstalled, g_closeGlobalMenu != nullptr);

            bool settingHookInstalled = false;
            bool eventPollHookInstalled = false;
            if (fullScreenSymbolsReady) {
                settingHookInstalled = Install(
                    settingTopPresenter->Get<UnityResolve::Method>("SetEvent"),
                    reinterpret_cast<void*>(&SettingSetEventHook),
                    reinterpret_cast<void**>(&g_settingSetEventOriginal),
                    "SettingTopScreenPresenter.SetEvent");
                eventPollHookInstalled = Install(
                    eventSystem->Get<UnityResolve::Method>("Update"),
                    reinterpret_cast<void*>(&EventSystemUpdateHook),
                    reinterpret_cast<void**>(&g_eventSystemUpdateOriginal),
                    "EventSystem.Update");
            } else {
                Log("Mod menu: full-screen symbols are incomplete; entry will remain visible in diagnostic fallback mode.");
            }
            // SetEvent is retained as a fast path, but the current PC build can
            // inline it completely.  Only advertise a working full-screen route
            // when the guaranteed main-thread EventSystem poll hook is present.
            g_fullScreenReady.store(eventPollHookInstalled);

            const bool entryInstalled = Install(
                g_menuPresenter->Get<UnityResolve::Method>("SetEvent"),
                reinterpret_cast<void*>(&SetEventHook),
                reinterpret_cast<void**>(&g_setEventOriginal),
                "MenuPresenter.SetEvent");
            Install(g_menuPresenter->Get<UnityResolve::Method>("OnAfterInitialize"),
                    reinterpret_cast<void*>(&OnAfterInitializeHook),
                    reinterpret_cast<void**>(&g_afterInitOriginal),
                    "MenuPresenter.OnAfterInitialize");
            LogF("Mod menu: entry hooks installed=%d/%d fullScreenReady=%d "
                 "compositionHooks=%d/%d; open the home menu.",
                 entryInstalled, pressInstalled, g_fullScreenReady.load(),
                 settingHookInstalled, eventPollHookInstalled);
        }
    }

    void StartCampusUiProbe() {
        if (g_started.exchange(true)) return;
        std::thread(ProbeThread).detach();
    }
}
