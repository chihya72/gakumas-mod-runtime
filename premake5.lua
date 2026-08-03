dependencies = {
	basePath = "./deps"
}

function dependencies.imports()
	for i, proj in pairs(dependencies) do
		if type(i) == "number" then
			proj.import()
		end
	end
end

function dependencies.projects()
	for i, proj in pairs(dependencies) do
		if type(i) == "number" then
			proj.project()
		end
	end
end

include "deps/minhook.lua"

workspace "gakumas_mod_runtime"
	location "./build"
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
	characterset "ASCII"

	flags {
		"NoIncrementalLink",
		"NoMinimalRebuild",
		"MultiProcessorCompile",
	}

	staticruntime "Off"

	filter "configurations:Release"
		optimize "Full"
		buildoptions "/Os"

	filter "configurations:Debug"
		optimize "Debug"

	filter {}

	-- Stamped into the DLL so a shipped build can name itself; the release
	-- workflow sets it from the tag.  Local builds say "dev".
	defines { 'GKMS_VERSION="' .. (os.getenv("GKMS_VERSION") or "dev") .. '"' }

	dependencies.projects()

	project "gakumas_mod_runtime"
		targetname "xinput1_3"

		language "C++"
		kind "SharedLib"

		files {
			"./src/runtime/**.hpp",
			"./src/runtime/**.h",
			"./src/runtime/**.cpp",
			"./src/runtime/**.def",
			"./src/deps/UnityResolve/UnityResolve.hpp",
			"./src/deps/nlohmann/json.hpp",
			-- The in-game manager UI ships inside this DLL: one proxy slot, no
			-- cross-module handshake.  See manager/README.md.
			"./manager/include/gkmm/**.hpp",
			"./manager/src/**.cpp",
		}

		includedirs {
			"./src",
			"./src/deps",
			"./src/runtime",
			"./manager/include",
			"%{prj.location}/src",
		}

		dependencies.imports()

		linkoptions "/SAFESEH:NO"

	project "mod_presentation_tests"
		language "C++"
		kind "ConsoleApp"

		files {
			"./manager/include/gkmm/ModPresentationModel.hpp",
			"./manager/include/gkmm/RuntimeModSnapshot.hpp",
			"./manager/src/ModPresentationModel.cpp",
			"./manager/src/RuntimeModSnapshot.cpp",
			"./manager/tests/ModPresentationModelTests.cpp",
			-- Game-free by design, so the config and log-level outcomes stay
			-- testable offline.  ModLog opens its file lazily on the first
			-- message that passes the filter, so linking it writes nothing.
			"./src/runtime/ModConfig.hpp",
			"./src/runtime/ModConfig.cpp",
			"./src/runtime/ModLog.hpp",
			"./src/runtime/ModLog.cpp",
		}

		includedirs {
			"./manager/include",
			"./src/deps",
			"./src/runtime",
		}
