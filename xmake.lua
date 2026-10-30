-- Qt-free xmake build for the musescore-mcp host (MuseScore 5.0 engine).
--
-- The engine's build belongs to the engine.  The fork (github.com/Locietta/
-- engrave, consumed as the pinned vendor/musescore submodule) carries its own
-- xmake build: project settings, the pinned package recipes, the engine module
-- targets, the engrave adapter library and their tests, all in the engine
-- checkout's xmake/ directory.  This file includes that build - from
-- vendor/musescore by default, or from the checkout named by
-- --musescore_source - and keeps what is the host's:
--
--   host_generated                   MCP tool schema (tool_schema.inc)
--   music_core                       typed document facade and composition
--   musescore_mcp                    stdio JSON-RPC host (musescore-mcp binary)
--   musescore_mcp_test, music_core_test   host regression binaries
--
-- Engine targets (muse_global, muse_draw, muse_mpe, engraving, iex_musicxml,
-- iex_guitarpro, iex_midi, native_fluidsynth, muse_audio_engine,
-- muse_generated), the engrave library and its tests and probes are defined by
-- the included engine build and are not repeated here.
--
-- Host code still includes engine headers directly, through engrave's internal
-- headers (score/score_edit.h, score/score_query.h, audio/engine_audio.h,
-- runtime/noqt_runtime.h), so the engine's include directories and its feature
-- defines stay project-wide for now: they change class layouts and interfaces
-- that the host translation units see.  S2 makes both private to the library.

set_project("musescore-mcp")
set_xmakever("3.1.1")
set_version("0.1.0")
set_languages("cxx23")
set_targetdir("$(builddir)")
add_rules("mode.debug", "mode.release", "mode.releasedbg")
-- Packages install into the project (.xmake, gitignored) so a checkout is
-- self-contained, and everything is built from source: a precompiled artifact
-- could carry a different CRT or feature set than the pinned recipe asks for.
set_policy("package.install_locally", true)
set_policy("package.precompiled", false)

-- Host-only third-party dependency.  The engine's own pinned packages
-- (freetype, harfbuzz, flac, lame, opus, opusenc, fdk-aac, pugixml, utfcpp,
-- picojson) are declared by the engine build that is included below.
includes("xmake/packages/asio.lua")
add_requires("asio 1.36.0", {system = false})

option("musescore_source")
    set_default("")
    set_showmenu(true)
    set_description("MuseScore source tree (default: the vendor/musescore submodule)")
option_end()

local project_root = os.projectdir()
local source_root = get_config("musescore_source")
if not source_root or source_root == "" then
    source_root = path.join(project_root, "vendor", "musescore")
end
source_root = path.absolute(source_root, project_root)

-------------------------------------------------------------------------------
-- The engine build (includes the pinned engine modules)
-------------------------------------------------------------------------------

-- A project-scope xmake script has no raise()/error(); a fatal input problem is
-- reported by defining a target whose on_load raises at configure time and
-- returning from this file.
local function configure_failure(message)
    target("host_build_check")
        set_kind("phony")
        on_load(function ()
            raise("%s", message)
        end)
    target_end()
end

local engine_build = path.join(source_root, "xmake", "engrave.lua")
if not os.isfile(engine_build) then
    configure_failure("the engine checkout has no xmake build: " .. engine_build
        .. "\nPoint --musescore_source at an engrave checkout that carries xmake/engrave.lua"
        .. "\n(the pinned vendor/musescore submodule gets it with the next pin bump).")
    return
end

ENGRAVE_SOURCE_ROOT = source_root
includes(engine_build)

if not ENGRAVE_DEFINES or not ENGRAVE_INCLUDES or not ENGRAVE_INTERNAL_INCLUDES then
    configure_failure("the engine build did not export ENGRAVE_DEFINES, ENGRAVE_INCLUDES, or ENGRAVE_INTERNAL_INCLUDES")
    return
end
if not ENGRAVE_SOURCE_ROOT
    or path.translate(ENGRAVE_SOURCE_ROOT) ~= path.translate(source_root) then
    configure_failure("the engine build was pointed at an unexpected source root: "
        .. tostring(ENGRAVE_SOURCE_ROOT) .. " (expected " .. source_root .. ")")
    return
end

-------------------------------------------------------------------------------
-- Shared include/link environment
-------------------------------------------------------------------------------

local native_env = path.join(project_root, ".pixi", "envs", "native")
local native_root = path.join(native_env, "Library")
if not os.isdir(native_root) then
    native_root = native_env
end
local native_include = path.join(native_root, "include")
local native_lib = path.join(native_root, "lib")

add_includedirs("$(builddir)/xmake-generated")
add_includedirs(path.join(project_root, "native"))
if os.isdir(native_include) then
    add_sysincludedirs(native_include)
    local cairo_include = path.join(native_include, "cairo")
    if os.isdir(cairo_include) then
        add_sysincludedirs(cairo_include)
    end
end
if os.isdir(native_lib) then
    add_linkdirs(native_lib)
end
-- zlib comes from the native environment for every binary, the engine's tests
-- included: the engine's zip container links it but leaves the choice to the
-- including project.  cairo is linked by engrave itself.
add_links("z")

-- The engine's include roots.  Kept project-wide: the host adapters include
-- engine headers (see the file header).
add_includedirs(ENGRAVE_INCLUDES)

-- The Qt-free feature selection comes from the engine manifest generator (its
-- PROJECT_DEFINES list), so there is one place to change what the engine
-- compiles as.  It must be project-wide: these macros change class layouts and
-- interfaces that native/ host code includes as well.
add_defines(table.unpack(ENGRAVE_DEFINES))

-- Qt platform macros normally come from <QtGlobal>; the pinned engine and
-- native/ adapters branch on them under NO_QT_SUPPORT. Define them once,
-- project-wide, for every target compiled from the engine and native/.
if is_plat("windows") then
    set_runtimes("MD")
    set_exceptions("cxx")
    add_defines("WIN32", "_WINDOWS", "NOMINMAX", "_USE_MATH_DEFINES", "UNICODE", "_UNICODE",
        "_CRT_SECURE_NO_WARNINGS", "_CRT_NONSTDC_NO_WARNINGS",
        "_SILENCE_STDEXT_ARR_ITERS_DEPRECATION_WARNING")
    add_defines("Q_OS_WIN", "Q_OS_WIN32", "Q_OS_WINDOWS")
    if is_arch("x64", "x86_64") then
        add_defines("Q_OS_WIN64")
    end
    add_cxflags("/utf-8", "/bigobj", "/Zc:__cplusplus", {tools = {"cl", "clang_cl"}})
    add_cflags("/utf-8", {tools = {"cl", "clang_cl"}})
elseif is_plat("macosx") then
    add_defines("Q_OS_MAC", "Q_OS_MACOS", "Q_OS_DARWIN", "Q_OS_UNIX")
    add_cxflags("-fPIC", "-pthread")
    add_ldflags("-pthread")
elseif is_plat("linux") then
    add_defines("Q_OS_LINUX", "Q_OS_UNIX")
    add_cxflags("-fPIC", "-pthread")
    add_ldflags("-pthread")
else
    add_cxflags("-fPIC", "-pthread")
    add_ldflags("-pthread")
end

-- Host include directories.  These were generated with the engine manifest
-- (HOST_TARGETS) until S0; from now on the host owns its own source lists and
-- include roots, and the engine owns its manifest.
local host_includes = {
    project_root,
    path.join(source_root, "src", "engraving", "thirdparty", "dtl"),
    source_root,
    path.join(source_root, "src"),
    path.join(source_root, "muse", "framework"),
    path.join(source_root, "muse", "framework", "global"),
    path.join(source_root, "src", "engraving"),
    path.join(source_root, "muse", "framework", "draw"),
}
-- The host includes engrave's audio headers, which include the engine's, so
-- host targets get the FluidSynth and audio include roots from the engine export.
local audio_includes = ENGRAVE_AUDIO_INCLUDES

-- Every host target gets the engine's global defines (upstream's
-- add_compile_definitions, e.g. KORS_PROFILER_ENABLED) because it includes
-- engine headers; the project-wide list above covers the rest.
local function add_engine_flags()
    add_defines(table.unpack(ENGRAVE_GLOBAL_DEFINES))
    add_includedirs(host_includes)
    add_includedirs("$(builddir)/xmake-generated")
    add_includedirs(audio_includes)
    add_includedirs(ENGRAVE_INTERNAL_INCLUDES)
end

-------------------------------------------------------------------------------
-- Generated host inputs (MCP tool schema)
-------------------------------------------------------------------------------
-- The engine's generated framework config header is produced by the engine
-- build's muse_generated target into the same directory; only the MCP tool
-- schema is the host's.

local function render_tool_schema(tools)
    -- Keep each literal small for MSVC; byte escapes also preserve UTF-8 when
    -- a chunk boundary falls inside a multi-byte character.
    local output = {"[] { std::string value; value.reserve(" .. #tools .. ");"}
    for first = 1, #tools, 1024 do
        local chunk = tools:sub(first, first + 1023)
        local escaped = chunk:gsub(".", function(c)
            return string.format("\\x%02x", c:byte())
        end)
        table.insert(output, 'value.append("' .. escaped .. '", ' .. #chunk .. ');')
    end
    table.insert(output, "return value; }()")
    return table.concat(output, "\n") .. "\n"
end

local function write_if_changed(os, io, filename, content)
    if os.isfile(filename) then
        local previous = io.readfile(filename)
        if previous == content then
            return false
        end
    end
    io.writefile(filename, content)
    return true
end

-- The generator runs in target-callback scope, where raise() exists (a
-- project-scope function would not see it).
target("host_generated")
    set_kind("phony")
    set_default(false)
    set_targetdir("$(builddir)/xmake-generated")
    local function generate_tool_schema(target, os, io, json)
        local output_dir = target:targetdir()
        if not output_dir or output_dir == "" then
            raise("generated target requires a build directory")
        end
        os.mkdir(output_dir)
        local tools_path = path.join(project_root, "native", "tools.json")
        local tools = io.readfile(tools_path)
        if not tools then
            raise("cannot read MCP tool schema: %s", tools_path)
        end
        local decoded = json.decode(tools)
        if type(decoded) ~= "table" or #decoded == 0 then
            raise("tool schema must be a nonempty JSON array")
        end
        write_if_changed(os, io, path.join(output_dir, "tool_schema.inc"),
            render_tool_schema(tools))
    end
    on_config(function (target)
        import("core.base.json")
        generate_tool_schema(target, os, io, json)
    end)
    on_build(function (target)
        import("core.base.json")
        generate_tool_schema(target, os, io, json)
    end)
target_end()

-------------------------------------------------------------------------------
-- Host targets
-------------------------------------------------------------------------------
-- The engine adapters (runtime, io, render, audio, score) are the engine's
-- engrave library, which also defines their tests and probes; the host links
-- it and includes its internal headers ("score/score_edit.h", ...).

local native_dir = path.join(project_root, "native")

-- Typed document facade and the composition model it drives.
target("music_core")
    set_kind("static")
    add_deps("muse_generated", "host_generated", "engrave")
    add_files(path.join(native_dir, "music_core.cpp"), path.join(native_dir, "composition.cpp"))
    add_engine_flags()

-- The stdio JSON-RPC host: every native/ source except music_core's and the
-- tests.  musescore-mcp-test is the same host with the async test hooks.
local function add_host_binary(name, basename)
    target(name)
        set_kind("binary")
        set_basename(basename)
        add_deps("muse_generated", "host_generated", "music_core")
        add_files(path.join(native_dir, "*.cpp"))
        remove_files(path.join(native_dir, "music_core.cpp"), path.join(native_dir, "composition.cpp"),
            path.join(native_dir, "*_test.cpp"))
        add_engine_flags()
        add_packages("asio")
        if is_plat("windows") then
            add_defines("_WIN32_WINNT=0x0A00")
            add_syslinks("ws2_32", "mswsock")
            add_files(path.join(native_dir, "utf8.manifest"))
        end
    target_end()
end
add_host_binary("musescore_mcp", "musescore-mcp")
add_host_binary("musescore_mcp_test", "musescore-mcp-test")
target("musescore_mcp_test")
    add_defines("MUSIC_ASYNC_TEST_HOOKS")
target_end()

target("music_core_test")
    set_kind("binary")
    add_deps("muse_generated", "host_generated", "music_core")
    add_files(path.join(native_dir, "music_core_test.cpp"))
    add_engine_flags()
    if is_plat("windows") then
        add_files(path.join(native_dir, "utf8.manifest"))
    end
target_end()
