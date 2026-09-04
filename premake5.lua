-- ------------------------------------------------------------------------------------------------
-- yampnet - the optional netplay plugin for YAMP (https://github.com/biggestsonicfan/YAMP).
--
-- YAMP never links against this DLL. It LoadLibrary's it at runtime (source/net/NetPlugin.cpp on
-- the YAMP side) and disables netplay when it is absent, which is why the plugin can live in its
-- own repository and churn on its own schedule: shipping a build without yampnet.dll in it is the
-- "no netplay" switch, and nothing in YAMP has to change for that.
--
-- WHAT THIS NEEDS FROM YAMP, and why it is an include path rather than a vendored copy:
--
--   source/net/YampNet.h      the plugin ABI - the contract between the two halves.
--   source/pxd/LJ/sl.h        (+ pxd_types.h, sl_internal.h) the engine's pad struct.
--   source/m2ftg/m2ftg.h      the arcade execute_info block.
--
-- The plugin writes execute_info.pad[] itself - a deliberate scope choice - so it is coupled to
-- those layouts. Copying the headers here would let the two drift silently; pointing at a YAMP
-- checkout means a rebuild picks up any layout change immediately. The yampnet_layout handshake in
-- YampNet.h is the runtime backstop for the case where only ONE side got rebuilt: it turns a
-- stale-layout plugin into a clean refusal at load instead of memory corruption. Keep it honest.
--
-- Tell premake where that checkout is, in this order of precedence:
--
--   premake5 --yamp-dir="C:/src/YAMP/source" vs2022     explicit, wins over everything
--   set YAMP_DIR=C:/src/YAMP/source                     environment, for a fixed machine
--   ../YAMP/source                                      the default: a sibling checkout
-- ------------------------------------------------------------------------------------------------

newoption {
	trigger = "yamp-dir",
	value = "PATH",
	description = "Path to YAMP's source/ directory (default: env YAMP_DIR, else ../YAMP/source)"
}

local function resolve_yamp_dir()
	-- Precedence: the explicit option, then the environment, then a sibling checkout. The first of
	-- these that is SET is the one used, and a set-but-wrong path is a hard error rather than a
	-- fall-through to the next candidate: silently building against a different YAMP tree than the
	-- one you named is worse than not building at all.
	--
	-- Written as a plain if-chain on purpose. This started life as a { option, env, default } list
	-- walked with ipairs, which stops at the first nil - so with no --yamp-dir passed the list was
	-- empty and every fallback was unreachable. There is no list here to get that wrong again.
	local origin, dir

	if _OPTIONS["yamp-dir"] and _OPTIONS["yamp-dir"] ~= "" then
		origin, dir = "--yamp-dir", _OPTIONS["yamp-dir"]
	elseif os.getenv("YAMP_DIR") and os.getenv("YAMP_DIR") ~= "" then
		origin, dir = "$YAMP_DIR", os.getenv("YAMP_DIR")
	else
		origin, dir = "the default sibling checkout", "../YAMP/source"
	end

	-- Fail here rather than at the first #include: a premake-time error can say WHICH path was
	-- tried and WHERE it came from, which "cannot open source file YampNet.h" three minutes into
	-- a compile cannot.
	if not os.isfile(dir .. "/net/YampNet.h") then
		error("could not find YAMP's source/ directory: " .. origin .. " points at '" .. dir
			.. "', which contains no net/YampNet.h.\n"
			.. "Pass --yamp-dir=<path to YAMP/source>, set the YAMP_DIR environment variable, or "
			.. "check YAMP out next to this repository.")
	end

	return dir
end

local yampdir = resolve_yamp_dir()
print("yampnet: using YAMP headers from " .. yampdir)

workspace "YampNet"
	platforms { "Win64" }
	configurations { "Debug", "Release", "Master" }
	location "build"

	cppdialect "C++17"
	staticruntime "on"
	buildoptions { "/sdl" }
	warnings "Extra"

	-- C4324 "structure was padded due to alignment specifier": YAMP's reverse-engineered structs
	-- use alignas deliberately to match the game's own layout and are pinned by static_asserts on
	-- their size and field offsets, so every one of these is expected. Muting it keeps a real
	-- warning visible. (Same rationale, and the same line, as YAMP's own premake5.lua.)
	disablewarnings { "4324" }

filter "configurations:Debug"
	defines { "DEBUG" }
	runtime "Debug"

filter "configurations:Master"
	defines { "NDEBUG" }
	symbols "Off"

filter "configurations:not Debug"
	optimize "Speed"
	functionlevellinking "on"
	linktimeoptimization "on"

filter { "platforms:Win64" }
	system "Windows"
	architecture "x86_64"

filter {}
	defines { "WINVER=0x0601", "_WIN32_WINNT=0x0601" } -- Target Win7, as YAMP does
	buildoptions { "/permissive-" }

project "YampNet"
	kind "SharedLib"
	language "C++"
	targetname "yampnet"

	files { "source/**.h", "source/**.cpp" }

	-- NOT "source" itself: the plugin's own headers are found by the quoted-include rule
	-- (relative to the including file), and putting that directory on the search path makes
	-- source/Winsock.h shadow the Windows SDK's own winsock.h - which <windows.h> pulls in.
	includedirs { yampdir, yampdir .. "/net" }

	-- ws2_32: the UDP/TCP transport. crypt32/secur32: TLS to the RPCN server.
	links { "ws2_32", "crypt32", "secur32" }

	vpaths { ["Headers/*"] = "source/**.h",
			["Sources/*"] = "source/**.cpp" }

filter {}
