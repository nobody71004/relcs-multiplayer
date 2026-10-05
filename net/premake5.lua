-- reLCS multiplayer platform build (net tools + shared protocol)
-- Usage: ../premake5.exe vs2019   (from the net/ directory)

workspace "reLCS-net"
	configurations { "Debug", "Release" }
	architecture "x86_64"
	location "build"
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

-- shared protocol is header-only; each consumer includes ../src/net
function netCommon()
	files { "../src/net/*.h" }
	includedirs { "../src/net", "../vendor/enet/include" }
	links { "enet", "ws2_32", "winmm" }
end

project "net-tests"
	kind "ConsoleApp"
	language "C++"
	cppdialect "C++17"
	targetname "net-tests"
	files { "tests/main.cpp" }
	netCommon()

project "net-server"
	kind "ConsoleApp"
	language "C++"
	cppdialect "C++17"
	targetname "reLCS-server"
	files { "server/*.cpp", "server/*.h" }
	netCommon()

project "net-bot"
	kind "ConsoleApp"
	language "C++"
	cppdialect "C++17"
	targetname "relcs-netbot"
	files { "bot/*.cpp", "bot/*.h" }
	netCommon()

project "net-master"
	kind "ConsoleApp"
	language "C++"
	cppdialect "C++17"
	targetname "reLCS-master"
	files { "master/*.cpp" }
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
	links { "comctl32", "shell32", "ole32" }
