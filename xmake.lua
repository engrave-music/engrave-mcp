-- Qt-free xmake build for the musescore-mcp host (MuseScore 5.0 engine).
--
-- The engine's build belongs to the engine.  The fork (github.com/Locietta/
-- engrave, consumed as the pinned vendor/musescore submodule) carries its own
-- xmake build: project settings, the pinned package recipes, the checked-in
-- source manifest (xmake/manifest.lua), the manifest generator and the engine
-- module targets, all in the engine checkout's xmake/ directory.  This file
-- includes that build - from vendor/musescore by default, or from the checkout
-- named by --musescore_source - and keeps what is the host's:
--
--   host_generated                   MCP tool schema (tool_schema.inc)
--   noqt_support                     native font/settings/MIDI/MusicXML adapters
--   score_engine                     private engraving and format adapters
--   engine_audio / music_core        audio driver and typed document facade
--   musescore_mcp                    stdio JSON-RPC host (musescore-mcp binary)
--   test targets                     core and adapter regression binaries
--
-- Engine targets (muse_global, muse_draw, muse_mpe, engraving, iex_musicxml,
-- iex_guitarpro, iex_midi_import, native_fluidsynth, muse_audio_engine,
-- muse_generated) are defined by the included engine build and are not repeated
-- here.
--
-- Host code still includes engine headers directly (native/score_edit.h,
-- native/score_query.h, native/engine_audio.h, native/noqt_runtime.h), so the
-- engine's include directories and its feature defines stay project-wide for
-- now: they change class layouts and interfaces that the host translation units
-- see.  S1 moves those adapters into the engine and S2 makes both private to
-- the library.

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

-- The engine build reads the engine sources from this checkout and the host
-- supplies the Qt-free test runtime the upstream gtest suites need (it still
-- lives in native/, see the note in the engine's xmake/tests.lua).
ENGRAVE_SOURCE_ROOT = source_root
ENGRAVE_TEST_SUPPORT = {
    runtime_target = "score_engine",
    harness_dir = path.join(project_root, "native", "tests"),
    utf8_manifest = path.join(project_root, "native", "utf8.manifest"),
}
includes(engine_build)

local engine_manifest = ENGRAVE_MANIFEST
if not engine_manifest or not engine_manifest.targets then
    configure_failure("the engine build did not export ENGRAVE_MANIFEST")
    return
end
if not ENGRAVE_SOURCE_ROOT
    or path.translate(ENGRAVE_SOURCE_ROOT) ~= path.translate(source_root) then
    configure_failure("the engine build was pointed at an unexpected source root: "
        .. tostring(ENGRAVE_SOURCE_ROOT) .. " (expected " .. source_root .. ")")
    return
end

-- Resolve the manifest's portable placeholders (the manifest is engine-only, so
-- every placeholder is the engine checkout).
local function expand(item)
    return (item:gsub("%${([A-Z_]+)}", function(name)
        if name == "PROJECT_ROOT" or name == "SOURCE_ROOT" then
            return source_root
        end
        print("error: unknown manifest placeholder: " .. name)
        return item
    end))
end

local function expand_list(items)
    local result = {}
    for _, item in ipairs(items or {}) do
        table.insert(result, expand(item))
    end
    return result
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

-- The engine's include roots.  Kept project-wide: the host adapters include
-- engine headers (see the file header).
add_includedirs(ENGRAVE_MANIFEST_INCLUDES)

-- The Qt-free feature selection comes from the engine manifest generator (its
-- PROJECT_DEFINES list), so there is one place to change what the engine
-- compiles as.  It must be project-wide: these macros change class layouts and
-- interfaces that native/ host code includes as well.
add_defines(table.unpack(engine_manifest.defines))

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
-- engine_audio.cpp reaches into FluidSynth (soundfont/synth settings) and the
-- audio facade includes the engine's audio headers, so the audio targets get
-- the FluidSynth and audio include roots from the engine manifest.
local audio_includes = expand_list(engine_manifest.targets.native_fluidsynth.includes)
table.insert(audio_includes, path.join(source_root, "muse", "framework", "audio"))

-- Every host target gets the engine's global defines (upstream's
-- add_compile_definitions, e.g. KORS_PROFILER_ENABLED) because it includes
-- engine headers; the project-wide list above covers the rest.
local function add_engine_flags(include_audio)
    add_defines(table.unpack(engine_manifest.global_defines))
    add_includedirs(host_includes)
    add_includedirs("$(builddir)/xmake-generated")
    if include_audio then
        add_includedirs(audio_includes)
    end
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
-- Host adapters
-------------------------------------------------------------------------------

-- Qt-free adapters and the engine-side sources they compile.  The engine
-- sources (font faces, MIDI export, MusicXML configuration) come from the
-- engine manifest's noqt_support entry; the host files are this project's.
local noqt_spec = engine_manifest.targets.noqt_support

target("noqt_support")
    set_kind("static")
    add_deps("muse_generated", "host_generated", "muse_global", "muse_draw", "muse_mpe",
        "engraving", "iex_musicxml")
    add_files(expand_list(noqt_spec.sources))
    add_defines(noqt_spec.defines)
    add_includedirs(expand_list(noqt_spec.includes))
    -- native filesystem and settings adapters
    add_files(
        path.join(project_root, "native", "noqt_filesystem.cpp"),
        path.join(project_root, "native", "noqt_settings.cpp")
    )
    add_links("z", {public = true})

target("score_engine")
    set_kind("static")
    add_deps("muse_generated", "host_generated", "noqt_support", "muse_draw",
        "engraving", "iex_musicxml")
    add_deps("iex_guitarpro", "iex_midi_import")
    add_files(
        path.join(project_root, "native", "atomic_file.cpp"),
        path.join(project_root, "native", "cairo_painter.cpp"),
        path.join(project_root, "native", "composition.cpp"),
        path.join(project_root, "native", "noqt_application.cpp"),
        path.join(project_root, "native", "noqt_runtime.cpp"),
        path.join(project_root, "native", "score_metadata.cpp"),
        path.join(project_root, "native", "score_positions.cpp"),
        path.join(project_root, "native", "score_semantic.cpp"),
        path.join(project_root, "native", "score_render.cpp"),
        path.join(project_root, "native", "score_excerpts.cpp"),
        path.join(project_root, "native", "score_imports.cpp"),
        path.join(project_root, "native", "midi_import_adapter.cpp"),
        path.join(project_root, "native", "runtime_assets.cpp"),
        path.join(project_root, "native", "score_query.cpp"),
        path.join(project_root, "native", "score_edit.cpp"),
        path.join(project_root, "native", "score_audio.cpp")
    )
    add_engine_flags(true)
    add_links("cairo", "z", {public = true})
    if not is_plat("windows") then
        add_links("m", {public = true})
    end

-- Headless audio-engine driver.  It needs both the engraving playback model
-- (score_engine) and the Qt-free engine/encoders (muse_audio_engine); the
-- process-wide engine state lives in engine_audio.cpp.
target("engine_audio")
    set_kind("static")
    add_deps("muse_generated", "host_generated", "score_engine", "muse_audio_engine", "native_fluidsynth")
    add_files(path.join(project_root, "native", "engine_audio.cpp"))
    add_engine_flags(true)

target("music_core")
    set_kind("static")
    add_deps("muse_generated", "host_generated", "score_engine", "engine_audio")
    add_files(path.join(project_root, "native", "music_core.cpp"))
    add_engine_flags(true)

-- The importers keep their upstream module boundary; the engine build defines
-- them.  They were linked through noqt_support before S0; keep that edge so the
-- link order of the host binaries does not change.
target("iex_guitarpro")
    add_deps("noqt_support")
target_end()

target("iex_midi_import")
    add_deps("noqt_support")
target_end()

-------------------------------------------------------------------------------
-- Host binaries and dev probes
-------------------------------------------------------------------------------

target("musescore_mcp")
    set_kind("binary")
    set_basename("musescore-mcp")
    add_deps("muse_generated", "host_generated", "music_core", "engine_audio")
    add_files(
        path.join(project_root, "native", "main.cpp"),
        path.join(project_root, "native", "noqt_engine.cpp"),
        path.join(project_root, "native", "async_session.cpp"),
        path.join(project_root, "native", "background_job.cpp"),
        path.join(project_root, "native", "worker_protocol.cpp"),
        path.join(project_root, "native", "mcp_transport.cpp")
    )
    add_engine_flags(true)
    add_packages("asio")
    if is_plat("windows") then
        add_defines("_WIN32_WINNT=0x0A00")
        add_syslinks("ws2_32", "mswsock")
    end
    add_links("cairo", "z")
    if is_plat("windows") then
        add_files(path.join(project_root, "native", "utf8.manifest"))
    else
        add_links("m")
    end

target("noqt_probe")
    set_kind("binary")
    add_deps("muse_generated", "host_generated", "noqt_support")
    add_files(
        path.join(project_root, "native", "noqt_application.cpp"),
        path.join(project_root, "native", "noqt_probe.cpp")
    )
    add_engine_flags(false)
    add_links("z")
    if is_plat("windows") then
        add_files(path.join(project_root, "native", "utf8.manifest"))
    end

target("musescore_mcp_test")
    set_kind("binary")
    set_basename("musescore-mcp-test")
    add_deps("muse_generated", "host_generated", "music_core", "engine_audio")
    add_files(
        path.join(project_root, "native", "main.cpp"),
        path.join(project_root, "native", "noqt_engine.cpp"),
        path.join(project_root, "native", "async_session.cpp"),
        path.join(project_root, "native", "background_job.cpp"),
        path.join(project_root, "native", "worker_protocol.cpp"),
        path.join(project_root, "native", "mcp_transport.cpp")
    )
    add_engine_flags(true)
    add_packages("asio")
    add_defines("MUSIC_ASYNC_TEST_HOOKS")
    add_links("cairo", "z")
    if is_plat("windows") then
        add_defines("_WIN32_WINNT=0x0A00")
        add_syslinks("ws2_32", "mswsock")
        add_files(path.join(project_root, "native", "utf8.manifest"))
    else
        add_links("m")
    end

target("noqt_support_test")
    set_kind("binary")
    -- engine_audio is linked so the IoC contract test can call a real host entry
    -- point that requires the file-system service (initialize_engine_audio, which
    -- guards on it in engine_audio.cpp); the adapter itself lives in noqt_support.
    add_deps("muse_generated", "host_generated", "noqt_support", "engine_audio")
    add_files(path.join(project_root, "native", "noqt_support_test.cpp"))
    add_files(path.join(project_root, "native", "tests", "ioc_tu2.cpp"))
    add_engine_flags(false)
    add_links("z")
    if is_plat("windows") then
        add_files(path.join(project_root, "native", "utf8.manifest"))
    end

target("score_positions_test")
    set_kind("binary")
    add_deps("muse_generated", "host_generated", "score_engine")
    add_files(path.join(project_root, "native", "score_positions_test.cpp"))
    add_engine_flags(true)
    add_links("cairo", "z")
    if is_plat("windows") then
        add_files(path.join(project_root, "native", "utf8.manifest"))
    else
        add_links("m")
    end

-- Dev-only closure check for the audio engine; run_all builds it explicitly.
target("audio_engine_link_check")
    set_kind("binary")
    set_default(false)
    add_deps("muse_generated", "host_generated", "muse_audio_engine")
    add_files(path.join(project_root, "native", "audio_engine_link_check.cpp"))
    add_defines(engine_manifest.targets.muse_audio_engine.defines)
    add_includedirs(expand_list(engine_manifest.targets.muse_audio_engine.includes))
    add_includedirs("$(builddir)/xmake-generated")
    add_links("z")
    if is_plat("windows") then
        add_files(path.join(project_root, "native", "utf8.manifest"))
    else
        add_links("m")
    end

-- Dev-only probe used by the engine_audio lane; built explicitly.
target("engine_audio_probe")
    set_kind("binary")
    set_default(false)
    add_deps("muse_generated", "host_generated", "engine_audio", "score_engine")
    add_files(path.join(project_root, "native", "engine_audio_probe.cpp"))
    add_engine_flags(true)
    add_links("cairo", "z")
    if is_plat("windows") then
        -- PeakWorkingSetSize for the probe's memory report.
        add_syslinks("psapi")
        add_files(path.join(project_root, "native", "utf8.manifest"))
    else
        add_links("m")
    end

-------------------------------------------------------------------------------
-- Adapter regression binaries
-------------------------------------------------------------------------------

local function add_engine_test(name, file)
    target(name)
        set_kind("binary")
        add_deps("muse_generated", "host_generated", "score_engine")
        add_files(path.join(project_root, "native", file))
        add_engine_flags(true)
        add_links("cairo", "z")
        if is_plat("windows") then add_files(path.join(project_root, "native", "utf8.manifest"))
        else add_links("m") end
    target_end()
end
add_engine_test("excerpts_test", "excerpts_test.cpp")
add_engine_test("assets_test", "assets_test.cpp")
add_engine_test("archive_test", "archive_test.cpp")
add_engine_test("score_audio_test", "score_audio_test.cpp")
add_engine_test("score_edit_test", "score_edit_test.cpp")
add_engine_test("semantic_dump", "semantic_dump.cpp")
target("score_edit_test")
    add_defines("MUSIC_CORE_TESTING")
    add_files(path.join(project_root, "native", "score_edit.cpp"))
target_end()
add_engine_test("music_core_test", "music_core_test.cpp")
target("music_core_test")
    add_deps("music_core")
target_end()
