-- Qt-free xmake build for the pinned MuseScore native engine (5.0 layout).
--
-- This is the primary build. CMake is not invoked. The checked-in manifest
-- (xmake/manifest.lua) is generated from the engine's own CMake source lists by
-- scripts/capture_xmake_manifest.py; this file supplies everything the engine
-- CMake files cannot: feature switches, the generated framework config header,
-- and the third-party packages.
--
-- Layout (engine paths follow MuseScore 5.0: the framework lives at muse/framework):
--   muse_global / muse_draw / muse_mpe / engraving / iex_musicxml
--   iex_guitarpro / iex_midi_import  importers, explicit source lists
--   native_fluidsynth                FluidSynth sources for the audio engine
--   muse_audio_engine                MuseScore audio engine and encoders
--   noqt_support                     native font/settings/MIDI/MusicXML adapters
--   score_engine                     private engraving and format adapters
--   music_core                       typed owner-thread document facade
--   musescore_mcp                    stdio JSON-RPC host (musescore-mcp binary)
--   test targets                     core and adapter regression binaries
--
-- Third-party dependencies (freetype, harfbuzz, flac, lame, opus, opusenc,
-- fdk-aac, pugixml, utfcpp, picojson) are pinned xmake packages; see
-- xmake/packages/ and the add_requires() block below.

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

includes("xmake/packages/asio.lua")
includes("xmake/packages/harfbuzz.lua")
includes("xmake/packages/flac.lua")
includes("xmake/packages/opus.lua")
includes("xmake/packages/opusenc.lua")
includes("xmake/packages/picojson.lua")
includes("xmake/packages/pugixml.lua")
includes("xmake/packages/fdk-aac.lua")

add_requires("asio 1.36.0", {system = false})

-- Third-party engine dependencies, pinned to the versions MuseScore 5.0 builds
-- against (muse/buildscripts/muse_deps.ref = 39c897ed, recipes/):
--   freetype 2.14.1, harfbuzz 12.3.0, flac 1.4.3, lame 3.100, opus 1.5.2,
--   opusenc 0.2.1, fdk-aac 2.0.3, pugixml 1.15, utfcpp 4.1.1, picojson 111c9be.
-- freetype: muse_deps builds it static with PNG/bzip2/zlib/harfbuzz disabled.
-- pugixml/opus/opusenc/picojson carry local recipes because xmake-repo's
-- versions or layouts differ (see the recipe headers).
add_requires("freetype 2.14.1", {system = false, configs = {
    zlib = false, png = false, bzip2 = false, woff2 = false, harfbuzz = false}})
add_requires("muse-harfbuzz 12.3.0", {system = false})
add_requires("muse-flac 1.4.3", {system = false})
add_requires("lame 3.100", {system = false})
add_requires("muse-opus 1.5.2", {system = false})
add_requires("muse-opusenc 0.2.1", {system = false})
add_requires("muse-fdk-aac 2.0.3", {system = false})
add_requires("muse-pugixml 1.15", {system = false})
add_requires("utfcpp v4.1.1", {system = false})
add_requires("muse-picojson 111c9be", {system = false})

option("musescore_source")
    set_default("")
    set_showmenu(true)
    set_description("MuseScore source tree (default: the vendor/musescore submodule)")

local project_root = os.projectdir()
local source_root = get_config("musescore_source")
if not source_root or source_root == "" then
    source_root = path.join(project_root, "vendor", "musescore")
end
source_root = path.absolute(source_root, project_root)
local manifest_path = path.join(project_root, "xmake", "manifest.lua")

if not os.isfile(manifest_path) then
    raise("xmake/manifest.lua is missing. Run once: "
        .. "python scripts/capture_xmake_manifest.py --source-root <engine>")
end
includes("xmake/manifest.lua")
local manifest = MUSESCORE_XMAKE_MANIFEST
if not manifest or not manifest.targets then
    raise("xmake/manifest.lua did not define MUSESCORE_XMAKE_MANIFEST")
end

-- Resolve the portable placeholders used by the captured manifest.
local function expand(item)
    return (item:gsub("%${([A-Z_]+)}", function(name)
        if name == "PROJECT_ROOT" then
            return project_root
        elseif name == "SOURCE_ROOT" then
            return source_root
        end
        raise("unknown manifest placeholder: " .. name)
    end))
end

local function expand_list(items)
    local result = {}
    for _, item in ipairs(items or {}) do
        table.insert(result, expand(item))
    end
    return result
end

local function add_manifest_target(name)
    local spec = manifest.targets[name]
    if not spec then
        raise("manifest has no target: " .. name)
    end
    local sources = expand_list(spec.sources)
    if #sources > 0 then
        add_files(sources)
    end
    if spec.defines and #spec.defines > 0 then
        add_defines(spec.defines)
    end
    add_includedirs(expand_list(spec.includes))
    return spec
end

-------------------------------------------------------------------------------
-- Generated inputs (framework config header and MCP tool schema)
-------------------------------------------------------------------------------

local function read_version(os, io)
    local text = io.readfile(path.join(source_root, "version.cmake")) or ""
    local function value(name)
        return text:match("set%(" .. name .. '%s+"([^"]*)"%)')
    end
    local major = value("MUSE_APP_VERSION_MAJOR") or "0"
    local minor = value("MUSE_APP_VERSION_MINOR") or "0"
    local patch = value("MUSE_APP_VERSION_PATCH") or "0"
    local revision = ""
    local function normalized_path(value)
        local result = path.normalize(tostring(value or "")):gsub("\\", "/")
        if os.host() == "windows" then
            result = result:lower()
        end
        return result
    end
    local root_args = {"-C", source_root, "rev-parse", "--show-toplevel"}
    -- Probe first: iorunv raises in the xmake script sandbox when git is absent
    -- or the source tree is not a checkout, and the revision must fall back.
    local root_probe = os.execv("git", root_args,
        {try = true, stdout = os.nuldev(), stderr = os.nuldev()})
    if root_probe ~= 0 then
        print("warning: cannot read the engine git revision from " .. source_root
            .. "; MUSE_APP_REVISION will be empty")
    else
        local top_level = tostring(os.iorunv("git", root_args) or ""):gsub("%s+", "")
        if normalized_path(top_level) ~= normalized_path(source_root) then
            -- A plain directory inside another checkout must not inherit the
            -- outer repository's SHA as the engine revision.
            print("warning: " .. source_root .. " is not a git checkout root (repository root '"
                .. top_level .. "'); MUSE_APP_REVISION will be empty")
        else
            -- Full SHA + first 7 characters: --short can grow when the prefix
            -- is ambiguous, which would make the reported revision unstable.
            local head_args = {"-C", source_root, "rev-parse", "HEAD"}
            local code = os.execv("git", head_args,
                {try = true, stdout = os.nuldev(), stderr = os.nuldev()})
            if code == 0 then
                local sha = tostring(os.iorunv("git", head_args) or ""):gsub("%s+", "")
                revision = sha:sub(1, 7)
                if not revision:match("^%x%x%x%x%x%x%x$") then
                    print("warning: unexpected engine git revision '" .. sha
                        .. "'; MUSE_APP_REVISION will be empty")
                    revision = ""
                end
            else
                print("warning: cannot read the engine git revision from " .. source_root
                    .. "; MUSE_APP_REVISION will be empty")
            end
        end
    end
    local version_label = value("MUSE_APP_VERSION_LABEL")
    local result = {
        MUSE_APP_NAME_HUMAN_READABLE = value("MUSE_APP_NAME_HUMAN_READABLE") or "MuseScore Studio",
        MUSE_APP_NAME_MACHINE_READABLE = value("MUSE_APP_NAME_MACHINE_READABLE") or "MuseScoreStudio",
        -- Upstream SetupConfigure.cmake leaves MUSE_APP_UNSTABLE undefined for
        -- MUSE_APP_BUILD_MODE=release; debug and releasedbg stay unstable.
        MUSE_APP_UNSTABLE = not is_mode("release"),
        MUSE_APP_BUILD_NUMBER = "1",
        MUSE_APP_REVISION = revision,
        MUSE_APP_VERSION = major .. "." .. minor .. "." .. patch,
        MUSE_APP_VERSION_MAJOR = major,
        MUSE_APP_VERSION_MINOR = minor,
        MUSE_APP_VERSION_PATCH = patch,
        -- Selected module graph.  The engine modules we compile need their
        -- enable macros; the audio work mode is only read by
        -- audio/common/workmode.cpp defaultMode() (no Qt path, unused by the
        -- host) and stays at the 4.7.6 value.
        MUSE_MODULE_AUDIO = true,
        MUSE_MODULE_AUDIO_API = true,
        MUSE_MODULE_AUDIO_EXPORT = true,
        MUSE_MODULE_AUDIO_WORKMODE = 1,
        MUSE_MODULE_DRAW = true,
        MUSE_MODULE_DRAW_API = true,
        MUSE_MODULE_GLOBAL = true,
        MUSE_MODULE_GLOBAL_API = true,
        MUSE_MODULE_MPE = true,
        MUSE_MODULE_MPE_API = true,
    }
    if version_label and version_label ~= "" then
        result.MUSE_APP_VERSION_LABEL = version_label
    end
    return result
end

local function render_framework_config(template, variables)
    local output = {}
    for line in (template .. "\n"):gmatch("([^\r\n]*)\r?\n") do
        local name, rest = line:match("^#cmakedefine%s+([%w_]+)(.*)$")
        if name then
            local value = variables[name]
            if value == nil or value == false then
                table.insert(output, "/* #undef " .. name .. " */")
            else
                local fmt = (rest or ""):gsub("%${([%w_]+)}", function(key)
                    local item = variables[key]
                    if item == nil or item == false then
                        return ""
                    end
                    return tostring(item)
                end)
                if fmt:gsub("%s", "") == "" then
                    table.insert(output, "#define " .. name)
                else
                    table.insert(output, "#define " .. name .. fmt)
                end
            end
        else
            line = line:gsub("%${([%w_]+)}", function(key)
                local item = variables[key]
                if item == nil or item == false then
                    return ""
                end
                return tostring(item)
            end)
            table.insert(output, line)
        end
    end
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


local function generate_inputs(target, os, io, json)
    local output_dir = target:targetdir()
    if not output_dir or output_dir == "" then
        raise("generated target requires a build directory")
    end
    os.mkdir(output_dir)

    -- The engine's framework config header. 5.0 moved the template into the
    -- absorbed framework tree (muse/framework/cmake); the #cmakedefine shape is
    -- unchanged, but the variables it substitutes changed
    -- (MUSE_APP_INSTALL_* replaced MUSE_APP_INSTALL_NAME, AUDIO_WORKER_MODE
    -- became AUDIO_HYBRID_MODE, MUSE_MODULE_*_COMPAT/MULTICONTEXT_WIP are gone).
    local template_path = path.join(source_root, "muse", "framework", "cmake",
        "muse_framework_config.h.in")
    local template = io.readfile(template_path)
    if not template then
        raise("cannot read pinned config template: " .. template_path)
    end
    write_if_changed(os, io, path.join(output_dir, "muse_framework_config.h"),
        render_framework_config(template, read_version(os, io)))

    local tools_path = path.join(project_root, "native", "tools.json")
    local tools = io.readfile(tools_path)
    if not tools then
        raise("cannot read MCP tool schema: " .. tools_path)
    end
    local decoded = json.decode(tools)
    if type(decoded) ~= "table" or #decoded == 0 then
        raise("tool schema must be a nonempty JSON array")
    end
    write_if_changed(os, io, path.join(output_dir, "tool_schema.inc"),
        render_tool_schema(tools))
end

target("muse_generated")
    set_kind("phony")
    set_default(false)
    set_targetdir("$(builddir)/xmake-generated")
    on_config(function (target)
        import("core.base.json")
        generate_inputs(target, os, io, json)
    end)
    on_build(function (target)
        import("core.base.json")
        generate_inputs(target, os, io, json)
    end)
target_end()

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

local manifest_includes = expand_list(manifest.includes)
add_includedirs(manifest_includes)

-- The Qt-free feature selection comes from the manifest generator (its
-- PROJECT_DEFINES list), so there is one place to change what the engine
-- compiles as.  It must be project-wide: these macros change class layouts and
-- interfaces that native/ host code includes as well.
add_defines(table.unpack(manifest.defines))

-- Qt platform macros normally come from <QtGlobal>; the pinned engine and
-- native/ adapters branch on them under NO_QT_SUPPORT. Define them once,
-- project-wide, for every target compiled from the engine and native/:
--   engraving/dom/masterscore.cpp        platform metaTag
--   framework/global/io/fileinfo.cpp     Windows drive-letter branches
--   framework/global/io/dir.cpp          Windows separator normalization
--   framework/global/runtime.cpp         POSIX thread names
--   engraving/editing/textedit.cpp and
--   engraving/editing/editdata.h         platform key handling
--   engraving/rendering/score/tdraw.cpp
-- The compiled files behind those branches include no Qt headers; the only
-- Qt-including files that test Q_OS_* (io/internal/filesystem.cpp,
-- internal/systeminfo.cpp, global/settings.cpp) are not in the manifest.
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

-------------------------------------------------------------------------------
-- Third-party dependencies
-------------------------------------------------------------------------------
-- freetype, harfbuzz, flac, lame, opus, opusenc, fdk-aac, pugixml, utfcpp and
-- picojson are xmake packages (add_requires above), not targets: MuseScore 5.0
-- builds all of them from muse_deps, so nothing third-party is compiled from
-- the engine tree any more. The only exception is FluidSynth, which upstream
-- still ships in tree (see native_fluidsynth).

-------------------------------------------------------------------------------
-- Pinned MuseScore modules
-------------------------------------------------------------------------------

target("muse_global")
    set_kind("static")
    add_deps("muse_generated")
    add_packages("muse-pugixml", "utfcpp", "muse-picojson")
    add_manifest_target("muse_global")
    if is_plat("linux") then
        add_syslinks("dl", "pthread", {public = true})
    end

target("muse_draw")
    set_kind("static")
    add_deps("muse_generated", "muse_global")
    -- public: every consumer includes draw headers that reach <ft2build.h>/<hb-ft.h>
    -- (fontsengine.h, fontfaceutils.h), and xmake only propagates package include
    -- dirs of a dependency when the package is public.
    add_packages("freetype", "muse-harfbuzz", {public = true})
    add_manifest_target("muse_draw")

target("muse_mpe")
    set_kind("static")
    add_deps("muse_generated", "muse_global")
    add_manifest_target("muse_mpe")

target("engraving")
    set_kind("static")
    add_deps("muse_generated", "muse_global", "muse_draw", "muse_mpe")
    add_manifest_target("engraving")

target("iex_musicxml")
    set_kind("static")
    add_deps("muse_generated", "muse_global", "engraving")
    add_manifest_target("iex_musicxml")

-------------------------------------------------------------------------------
-- Audio dependencies and no-Qt adapters
-------------------------------------------------------------------------------

target("native_fluidsynth")
    set_kind("static")
    if is_plat("windows") then
        set_languages("c11", "cxx23")
    else
        -- Upstream's NO_GLIB fallback uses alloca from GNU libc headers.
        set_languages("gnu17", "cxx23")
    end
    add_manifest_target("native_fluidsynth")
    set_warnings("none")

-- MuseScore 5.0's audio engine and its encoders (roadmap step 5).  The source
-- list is in the manifest (audio/common + audio/engine + fluidsynth); the
-- encoders are the pinned packages.  MUSE_THREADS_SUPPORT is deliberately left
-- undefined so the mixer stays single-threaded, and the engine's own audio
-- worker thread (platform/general/generalaudioworker.cpp) is not compiled: the
-- host drives the engine from its own thread.
target("muse_audio_engine")
    set_kind("static")
    add_deps("muse_generated", "muse_global", "native_fluidsynth")
    add_packages("muse-flac", "lame", "muse-opus", "muse-opusenc", "muse-fdk-aac")
    add_manifest_target("muse_audio_engine")

target("audio_engine_link_check")
    set_kind("binary")
    -- Dev-only closure check; run_all builds it explicitly.
    set_default(false)
    add_deps("muse_generated", "muse_audio_engine")
    add_files(path.join(project_root, "native", "audio_engine_link_check.cpp"))
    add_defines(manifest.targets.muse_audio_engine.defines)
    add_includedirs(expand_list(manifest.targets.muse_audio_engine.includes))
    add_links("z")
    if is_plat("windows") then
        add_files(path.join(project_root, "native", "utf8.manifest"))
    else
        add_links("m")
    end

target("noqt_support")
    set_kind("static")
    add_deps("muse_generated", "muse_global", "muse_draw", "muse_mpe",
        "engraving", "iex_musicxml")
    add_manifest_target("noqt_support")
    add_links("z", {public = true})

target("score_engine")
    set_kind("static")
    add_deps("muse_generated", "noqt_support", "muse_draw",
        "engraving", "iex_musicxml")
    add_deps("iex_guitarpro", "iex_midi_import")
    add_manifest_target("score_engine")
    add_links("cairo", "z", {public = true})
    if not is_plat("windows") then
        add_links("m", {public = true})
    end

-- Headless audio-engine driver (roadmap step 5). It needs both the engraving
-- playback model (score_engine) and the Qt-free engine/encoders
-- (muse_audio_engine); the process-wide engine state lives in engine_audio.cpp.
target("engine_audio")
    set_kind("static")
    add_deps("muse_generated", "score_engine", "muse_audio_engine", "native_fluidsynth")
    add_manifest_target("engine_audio")
    -- engine_audio.cpp reaches into FluidSynth (soundfont/synth settings), so the
    -- FluidSynth include roots must be visible here; manifest includes are
    -- per-target and do not travel through add_deps.
    add_includedirs(expand_list(manifest.targets.native_fluidsynth.includes))

target("music_core")
    set_kind("static")
    add_deps("muse_generated", "score_engine", "engine_audio")
    add_manifest_target("music_core")

target("engine_audio_probe")
    set_kind("binary")
    -- Dev-only probe used by the engine_audio lane; built explicitly.
    set_default(false)
    add_deps("muse_generated", "engine_audio", "score_engine")
    add_files(path.join(project_root, "native", "engine_audio_probe.cpp"))
    add_defines(manifest.targets.engine_audio.defines)
    add_includedirs(expand_list(manifest.targets.engine_audio.includes))
    add_links("cairo", "z")
    if is_plat("windows") then
        -- PeakWorkingSetSize for the probe's memory report.
        add_syslinks("psapi")
        add_files(path.join(project_root, "native", "utf8.manifest"))
    else
        add_links("m")
    end

-------------------------------------------------------------------------------
-- Host and existing probes
-------------------------------------------------------------------------------

target("musescore_mcp")
    set_kind("binary")
    set_basename("musescore-mcp")
    add_deps("muse_generated", "music_core", "engine_audio")
    add_manifest_target("musescore_mcp")
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
    add_deps("muse_generated", "noqt_support")
    add_manifest_target("noqt_probe")
    add_links("z")
    if is_plat("windows") then
        add_files(path.join(project_root, "native", "utf8.manifest"))
    end

target("musescore_mcp_test")
    set_kind("binary")
    set_basename("musescore-mcp-test")
    add_deps("muse_generated", "music_core", "engine_audio")
    add_manifest_target("musescore_mcp")
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
    add_deps("muse_generated", "noqt_support")
    add_manifest_target("noqt_support_test")
    add_links("z")
    if is_plat("windows") then
        add_files(path.join(project_root, "native", "utf8.manifest"))
    end

target("score_positions_test")
    set_kind("binary")
    add_deps("muse_generated", "score_engine")
    add_files(path.join(project_root, "native", "score_positions_test.cpp"))
    add_defines(manifest.targets.music_core.defines)
    add_includedirs(expand_list(manifest.targets.music_core.includes))
    add_links("cairo", "z")
    if is_plat("windows") then
        add_files(path.join(project_root, "native", "utf8.manifest"))
    else
        add_links("m")
    end


-- Import algorithms keep their upstream module boundary; both source lists are
-- generated from the engine's importexport CMakeLists (xmake/manifest.lua), so
-- they follow upstream's file moves (for example the 5.0 guitarbendimport
-- split and the new MIDI import model, which stays out: it is Qt-only).
target("iex_guitarpro")
    set_kind("static")
    add_deps("muse_generated", "noqt_support")
    add_manifest_target("iex_guitarpro")
target_end()

target("iex_midi_import")
    set_kind("static")
    add_deps("muse_generated", "noqt_support")
    add_manifest_target("iex_midi_import")
target_end()


local function add_engine_test(name, file)
    target(name)
        set_kind("binary")
        add_deps("muse_generated", "score_engine")
        add_defines(manifest.targets.score_engine.defines)
        add_includedirs(expand_list(manifest.targets.score_engine.includes))
        add_files(path.join(project_root, "native", file))
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
-- Dev tool for the rebase-proof semantic goldens (roadmap-engrave A0).
add_engine_test("semantic_dump", "semantic_dump.cpp")
target("score_edit_test")
    add_defines("MUSIC_CORE_TESTING")
    add_files(path.join(project_root, "native", "score_edit.cpp"))
target_end()
add_engine_test("music_core_test", "music_core_test.cpp")
target("music_core_test")
    add_deps("music_core")
target_end()

-- Upstream unit tests (dev-only; roadmap step 5 acceptance). Qt-free gtest main in native/tests.
-------------------------------------------------------------------------------
-- Upstream GoogleTest / GoogleMock (dev-only)
-------------------------------------------------------------------------------
-- MuseScore 5.0 no longer vendors googletest: muse_deps fetches it as a source
-- dependency when MUSE_ENABLE_UNIT_TESTS is on (ExtDepsManifest.cmake), pinned
-- by muse_deps.ref, and the version is not reproduced here. The dev-only test
-- lanes therefore take xmake-repo's gtest; they never run in the release build.
add_requires("gtest v1.17.0", {system = false, configs = {shared = false}})

-------------------------------------------------------------------------------
-- Upstream Engraving Playback unit tests (Qt-free)
-------------------------------------------------------------------------------

target("engraving_playback_tests")
    set_kind("binary")
    -- Dev-only upstream tests; built explicitly by the upstream_gtest lane.
    set_default(false)
    set_basename("engraving_playback_tests")
    add_deps("muse_generated", "score_engine")
    add_packages("gtest")
    add_defines(manifest.targets.score_engine.defines)
    add_defines("TEST_HAS_ENGRAVING=1")
    add_defines('ENGRAVING_TESTS_DATA_ROOT="' .. path.join(source_root, "src/engraving/tests"):gsub("\\", "/") .. '"')
    add_includedirs(expand_list(manifest.targets.score_engine.includes))
    add_includedirs(path.join(source_root, "src/engraving/tests"))
    add_includedirs(path.join(source_root, "muse/framework"))
    add_includedirs(path.join(project_root, "native/tests"))
    add_files(
        path.join(source_root, "src/engraving/tests/playback/playbackeventsrendering_tests.cpp"),
        path.join(source_root, "src/engraving/tests/playback/playbackmodel_tests.cpp"),
        path.join(source_root, "src/engraving/tests/playback/playbackcontext_tests.cpp"),
        path.join(source_root, "src/engraving/tests/playback/bendsrenderer_tests.cpp"),
        path.join(source_root, "src/engraving/tests/repeat_tests.cpp"),
        path.join(source_root, "src/engraving/tests/parts_tests.cpp"),
        path.join(source_root, "src/engraving/tests/utils/scorecomp.cpp"),
        path.join(source_root, "src/engraving/tests/utils/scorerw.cpp"),
        path.join(source_root, "src/engraving/tests/utils/testutils.cpp"),
        path.join(project_root, "native/tests/gtest_main.cpp"),
        path.join(project_root, "native/tests/test_environment.cpp")
    )
    add_links("cairo", "z")
    if is_plat("windows") then
        add_files(path.join(project_root, "native/utf8.manifest"))
    else
        add_links("m")
    end
target_end()

-------------------------------------------------------------------------------
-- Upstream MPE unit tests (Qt-free)
-------------------------------------------------------------------------------

target("mpe_tests")
    set_kind("binary")
    -- Dev-only upstream tests; built explicitly by the upstream_gtest lane.
    set_default(false)
    set_basename("mpe_tests")
    add_deps("muse_generated", "muse_mpe")
    add_packages("gtest")
    add_defines(manifest.targets.muse_mpe.defines)
    add_includedirs(expand_list(manifest.targets.muse_mpe.includes))
    add_includedirs(path.join(source_root, "muse/framework"))
    add_includedirs(path.join(project_root, "native/tests"))
    add_files(
        path.join(source_root, "muse/framework/mpe/tests/singlenotearticulationstest.cpp"),
        path.join(source_root, "muse/framework/mpe/tests/multinotearticulationstest.cpp"),
        path.join(project_root, "native/tests/gtest_main.cpp"),
        path.join(project_root, "native/tests/test_environment.cpp")
    )
    if is_plat("windows") then
        add_files(path.join(project_root, "native/utf8.manifest"))
    else
        add_links("m")
    end
target_end()
