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

    project "dwmapi_manager"
        targetname "dwmapi"
        language "C++"
        kind "SharedLib"

        files {
            "./include/**.hpp",
            "./include/**.h",
            "./src/**.cpp",
        }
        includedirs {
            "./include",
        }
