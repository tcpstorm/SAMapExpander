local sdkdir = os.getenv("PLUGIN_SDK_DIR")
if not sdkdir or sdkdir == "" then
    error("PLUGIN_SDK_DIR environment variable is not set")
end
sdkdir = path.translate(sdkdir, "/")

-- The one place the version is written down. Anything that needs it - the build, a release, the
-- line the plugin logs on start-up - reads this file rather than carrying its own copy.
local function readVersion()
    local f = io.open("VERSION", "r")
    if not f then return "0.0.0" end
    local v = f:read("*l") or "0.0.0"
    f:close()
    return (v:gsub("%s+$", ""))
end

workspace "MapExpander"
    configurations { "Release", "Debug" }
    location "build"

project "MapExpander"
    kind "SharedLib"
    language "C++"
    architecture "x86"
    targetextension ".asi"
    targetprefix ""
    targetname "MapExpander.SA"
    characterset "MBCS"
    staticruntime "On"

    filter "action:gmake or action:gmake2"
        toolset "clang"
        buildoptions {
            "-fpermissive",
            "-fcommon",
            "-fms-extensions",
            "-Wno-invalid-offsetof",
            "-Wno-microsoft-include",
            "-Wno-builtin-macro-redefined",
            "-static",
            "-D__cpp_concepts=202202L",
            "--target=i686-w64-mingw32",
        }
        linkoptions {
            "--target=i686-w64-mingw32",
            "-static",
            "-static-libgcc",
            "-static-libstdc++",
        }
    filter {}

    filter "files:**.cpp"
        buildoptions { "-std=c++2b" }
    filter {}

    includedirs {
        "src",
        "src/core",
        "src/region",
        "src/limits",
        "src/weapons",
        "src/peds",
        "src/patches",
        "src/debug",
        sdkdir .. "/shared",
        sdkdir .. "/shared/game",
        sdkdir .. "/shared/dxsdk",
        sdkdir .. "/safetyhook",
        sdkdir .. "/plugin_sa",
        sdkdir .. "/plugin_sa/game_sa",
        sdkdir .. "/plugin_sa/game_sa/enums",
        sdkdir .. "/plugin_sa/game_sa/rw",
    }

    libdirs { sdkdir .. "/output/lib" }

    files {
        "src/**.h",
        "src/**.cpp",
    }

    defines {
        "GTASA",
        "PLUGIN_SGV_10US",
        "RW",
        "TARGET_NAME=\"MapExpander\"",
        "MAPEXPANDER_VERSION=\"" .. (os.getenv("MAPEXPANDER_VERSION") or readVersion()) .. "\"",
    }

    filter "configurations:Release"
        defines { "NDEBUG" }
        optimize "On"
        symbols "Off"
        buildoptions { "-ffunction-sections", "-fdata-sections" }
        linkoptions { "-s", "-Wl,--gc-sections" }
        links { "Plugin", "ntdll" }
        targetdir "output/Release"

    filter "configurations:Debug"
        defines { "DEBUG", "_DEBUG" }
        symbols "On"
        links { "Plugin_d", "ntdll" }
        targetdir "output/Debug"
    filter {}
