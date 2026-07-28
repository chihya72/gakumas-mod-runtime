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
		}

		includedirs {
			"./src",
			"./src/deps",
			"%{prj.location}/src",
		}

		dependencies.imports()

		linkoptions "/SAFESEH:NO"
