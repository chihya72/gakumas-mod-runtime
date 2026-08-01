workspace "gakumas_in_game_mod_manager"
    location "build"
    objdir "%{wks.location}/obj/%{prj.name}/%{cfg.platform}/%{cfg.buildcfg}"
    targetdir "%{wks.location}/bin/%{cfg.platform}/%{cfg.buildcfg}"

    architecture "x64"
    platforms "x64"

    configurations {
        "Debug",
        "Release",
    }

    buildoptions {
        "/std:c++latest",
        "/utf-8",
    }
    systemversion "latest"
    symbols "On"
    editandcontinue "Off"
    warnings "Off"
    characterset "Unicode"
    staticruntime "Off"

    filter "configurations:Release"
        optimize "Full"
        buildoptions "/Os"

    filter "configurations:Debug"
        optimize "Debug"

    filter {}

    project "xinput9_1_0_manager"
        targetname "xinput9_1_0"
        language "C++"
        kind "SharedLib"

        files {
            "./include/gkmm/RuntimeClient.hpp",
            "./include/gkmm/gmr_runtime_api.h",
            "./src/PluginMain.cpp",
            "./src/RuntimeClient.cpp",
            "./src/XInputProxy.cpp",
            "./src/xinput9_1_0.def",
        }
        includedirs {
            "./include",
        }
