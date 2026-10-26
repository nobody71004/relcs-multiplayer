-- reLCS multiplayer platform build (net tools + shared protocol)
-- Usage: ../premake5.exe vs2019   (from the net/ directory)

workspace "reLCS-net"
	configurations { "Debug", "Release" }
	architecture "x86_64"
	location "build"
	-- the machine's installed Visual Studio toolset (VS 2022 BuildTools, v143);
	-- premake's vs2019 action otherwise pins v142, which is not installed here
	toolset "v143"
	targetdir "bin/%{cfg.buildcfg}"
	defines { "NOMINMAX", "WIN32_LEAN_AND_MEAN", "ENET_STATIC", "_CRT_SECURE_NO_WARNINGS" }
	rtti "Off"
	symbols "On"
	filter "configurations:Debug"
		optimize "Off"
	filter "configurations:Release"
		optimize "On"
	filter {}

project "enet"
	kind "StaticLib"
	language "C"
	targetname "enet"
	files { "../vendor/enet/*.c", "../vendor/enet/include/**/*.h" }
	includedirs { "../vendor/enet/include" }
	defines { "ENET_STATIC" }
	removefiles { "../vendor/enet/unix.c", "../vendor/enet/win32.c.bak" }
	filter "system:windows"
		defines { "_WINSOCK_DEPRECATED_NO_WARNINGS" }
	filter {}

project "sqlite"
	kind "StaticLib"
	language "C"
	targetname "sqlite"
	files { "../vendor/sqlite/sqlite3.c", "../vendor/sqlite/sqlite3.h" }
	includedirs { "../vendor/sqlite" }

-- shared protocol is header-only; each consumer includes ../src/net
function netCommon()
	files { "../src/net/*.h" }
	includedirs { "../src/net", "../vendor/enet/include" }
	links { "enet", "ws2_32", "winmm" }
end

-- proximity voice codec (src/net/rnet_voice.h): vendor/opus, built as x64 Release
function netOpus()
	includedirs { "../vendor/opus/include" }
	libdirs { "../vendor/opus/win32/VS2015/x64/Release" }
	links { "opus" }
end

project "net-tests"
	kind "ConsoleApp"
	language "C++"
	cppdialect "C++17"
	targetname "net-tests"
	files { "tests/main.cpp", "server/profiles.cpp", "server/profiles.h", "server/resource.cpp", "server/resource.h", "server/anticheat_config.h" }
	includedirs { "server", "../vendor/sqlite" }
	links { "sqlite" }
	netCommon()
	netOpus()

project "net-server"
	kind "ConsoleApp"
	language "C++"
	cppdialect "C++17"
	targetname "reLCS-server"
	files { "server/*.cpp", "server/*.h" }
	includedirs { "../vendor/sqlite" }
	links { "sqlite", "iphlpapi" }
	-- EOS Anti-Cheat: the server binds EOSSDK at runtime (LoadLibrary), so the
	-- headers are needed at build time but nothing is linked. A host without
	-- EOSSDK-Win64-Shipping.dll still starts; it reports eac state=sdk-missing.
	includedirs { "../vendor/eos-sdk/include" }
	netCommon()

project "net-bot"
	kind "ConsoleApp"
	language "C++"
	cppdialect "C++17"
	targetname "relcs-netbot"
	files { "bot/*.cpp", "bot/*.h" }
	netCommon()
	netOpus()

project "net-master"
	kind "ConsoleApp"
	language "C++"
	cppdialect "C++17"
	targetname "reLCS-master"
	files { "master/*.cpp", "master/*.h" }
	includedirs { "../vendor/sqlite" }
	links { "sqlite" }
	netCommon()

project "net-proxy"
	kind "ConsoleApp"
	language "C++"
	cppdialect "C++17"
	targetname "net-shaper"
	files { "proxy/*.cpp" }
	netCommon()

project "net-agent"
	kind "ConsoleApp"
	language "C++"
	cppdialect "C++17"
	targetname "reLCS-agentd"
	files { "agent/*.cpp", "agent/*.h" }
	netCommon()

project "net-launcher"
	kind "WindowedApp"
	language "C++"
	cppdialect "C++17"
	targetname "reLCS-launcher"
	files { "launcher/*.cpp", "launcher/*.h" }
	netCommon()
	links { "advapi32", "comctl32", "shell32", "ole32", "iphlpapi" }
	-- Epic Online Services: the launcher owns the EOS bootstrap and shows the
	-- Connect login state; the SDK ships next to the launcher at runtime.
	includedirs { "../vendor/eos-sdk/include" }
	libdirs { "../vendor/eos-sdk/lib/Win64" }
	links { "EOSSDK-Win64-Shipping" }
	postbuildcommands {
		'{COPYFILE} "%{wks.location}/../../vendor/eos-sdk/bin/Win64/EOSSDK-Win64-Shipping.dll" "%{cfg.targetdir}/EOSSDK-Win64-Shipping.dll"'
	}
