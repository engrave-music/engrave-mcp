-- Qt-free xmake build for the pinned MuseScore native engine.
--
-- This is the primary build. CMake is not invoked. The checked-in manifest was
-- initially captured from the previous CMake baseline and is now maintained here.
--
-- Layout:
--   freetype / harfbuzz              bundled dependency libraries
--   muse_global / muse_draw / muse_mpe / engraving / iex_musicxml
--   native_fluidsynth / native_audio FluidSynth sessions and audio codecs
--   noqt_support                     native filesystem/settings/font/MIDI adapters
--   score_engine                     private engraving and format adapters
--   music_core                       typed owner-thread document facade
--   musescore_mcp                    stdio JSON-RPC host (musescore-mcp binary)
--   test targets                     engine, core and adapter regression binaries

set_project("musescore-mcp")
set_xmakever("3.1.1")
set_version("0.1.0")
set_languages("cxx23")
set_targetdir("$(builddir)")
add_rules("mode.debug", "mode.release", "mode.releasedbg")
set_policy("package.install_locally", true)
includes("xmake/packages/asio.lua")
add_requires("asio 1.36.0", {system = false})

option("harfbuzz_root")
    set_default("")
    set_showmenu(true)
    set_description("HarfBuzz source tree containing src/harfbuzz.cc")

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

local function find_harfbuzz()
    local configured = get_config("harfbuzz_root")
    local candidates = {}
    if configured and configured ~= "" then
        table.insert(candidates, configured)
    end
    table.insert(candidates, path.join(project_root, ".upstream", "harfbuzz"))
    table.insert(candidates, path.join(project_root, ".upstream", "harfbuzz", "harfbuzz"))
    table.insert(candidates, path.join(project_root, ".upstream", "harfbuzz", "harfbuzz-12.3.0"))
    for _, candidate in ipairs(candidates) do
        if os.isfile(path.join(candidate, "src", "harfbuzz.cc")) then
            return path.absolute(candidate)
        end
    end
    raise("HarfBuzz source not found. Expected .upstream/harfbuzz/src/harfbuzz.cc "
        .. "(or pass --harfbuzz_root=DIR) so bundled HarfBuzz stays pinned.")
end

local harfbuzz_root = find_harfbuzz()

if not os.isfile(manifest_path) then
    raise("xmake/manifest.lua is missing. Run once: "
        .. "python scripts/capture_xmake_manifest.py")
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
        elseif name == "HARFBUZZ_ROOT" then
            return harfbuzz_root
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
        MUSE_APP_NAME_HUMAN_READABLE = "MuseScore Studio",
        MUSE_APP_NAME_MACHINE_READABLE = "MuseScoreStudio",
        MUSE_APP_NAME_HUMAN_READABLE_COMPAT = "MuseScore",
        MUSE_APP_NAME_MACHINE_READABLE_COMPAT = "MuseScore",
        MUSE_APP_NAME = "MuseScore",
        MUSE_APP_TITLE = "MuseScore Studio",
        -- Upstream SetupConfigure.cmake leaves MUSE_APP_UNSTABLE undefined for
        -- MUSE_APP_BUILD_MODE=release; debug and releasedbg stay unstable.
        MUSE_APP_UNSTABLE = not is_mode("release"),
        MUSE_APP_BUILD_NUMBER = "1",
        MUSE_APP_REVISION = revision,
        MUSE_APP_VERSION = major .. "." .. minor .. "." .. patch,
        MUSE_APP_VERSION_MAJOR = major,
        MUSE_APP_VERSION_MINOR = minor,
        MUSE_APP_VERSION_PATCH = patch,
        -- Selected module graph, matching the native CMake baseline.
        MUSE_MODULE_ACTIONS = true,
        MUSE_MODULE_ACTIONS_API = true,
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

    local template_path = path.join(source_root, "src", "framework", "cmake",
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
-- Bundled dependencies
-------------------------------------------------------------------------------

target("freetype")
    set_kind("static")
    set_languages("c11")
    local freetype = add_manifest_target("freetype")
    if is_plat("windows") then
        add_files(expand_list(freetype.windows_sources))
    else
        add_files(expand_list(freetype.unix_sources))
    end
    add_defines("FT2_BUILD_LIBRARY")
    if not is_plat("windows") then
        -- FreeType's CMake build patches these into the generated unix
        -- ftconfig.h after check_include_file().
        add_defines("HAVE_UNISTD_H=1", "HAVE_FCNTL_H=1")
    end
    add_links("z", {public = true})
    set_warnings("none")

target("harfbuzz")
    set_kind("static")
    add_deps("freetype")
    add_manifest_target("harfbuzz")
    set_warnings("none")

-------------------------------------------------------------------------------
-- Pinned MuseScore modules
-------------------------------------------------------------------------------

target("muse_global")
    set_kind("static")
    add_deps("muse_generated")
    add_manifest_target("muse_global")
    if is_plat("linux") then
        add_syslinks("dl", "pthread", {public = true})
    end

target("muse_draw")
    set_kind("static")
    add_deps("muse_generated", "muse_global", "harfbuzz", "freetype")
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

target("native_audio")
    set_kind("static")
    add_deps("native_fluidsynth")
    add_manifest_target("native_audio")
    add_links("sndfile", {public = true})

target("noqt_support")
    set_kind("static")
    add_deps("muse_generated", "muse_global", "muse_draw", "muse_mpe",
        "engraving", "iex_musicxml", "harfbuzz", "freetype")
    add_manifest_target("noqt_support")
    add_links("z", {public = true})

target("score_engine")
    set_kind("static")
    add_deps("muse_generated", "noqt_support", "native_audio", "muse_draw",
        "engraving", "iex_musicxml")
    add_deps("iex_guitarpro", "iex_midi_import")
    add_manifest_target("score_engine")
    add_links("cairo", "z", {public = true})
    if not is_plat("windows") then
        add_links("m", {public = true})
    end

target("music_core")
    set_kind("static")
    add_deps("muse_generated", "score_engine")
    add_manifest_target("music_core")

-------------------------------------------------------------------------------
-- Host and existing probes
-------------------------------------------------------------------------------

target("musescore_mcp")
    set_kind("binary")
    set_basename("musescore-mcp")
    add_deps("muse_generated", "music_core")
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
    add_deps("muse_generated", "music_core")
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


-- Import algorithms retain their upstream module boundary and explicit source lists.
includes("xmake/imports.lua")
local function add_import_module(name, sources, extra_includes)
    target(name)
        set_kind("static")
        add_deps("muse_generated", "noqt_support")
        add_defines(manifest.targets.music_core.defines)
        add_includedirs(expand_list(manifest.targets.music_core.includes))
        for _, source in ipairs(sources) do add_files(path.join(source_root, source)) end
        if extra_includes then add_includedirs(path.join(source_root, extra_includes)) end
    target_end()
end
add_import_module("iex_guitarpro", IMPORT_SOURCES.guitarpro, "src/importexport/guitarpro")
local midi_sources = table.join(IMPORT_SOURCES.midi, IMPORT_SOURCES.beatroot)
add_import_module("iex_midi_import", midi_sources, "src/importexport/midi")


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
target("score_edit_test")
    add_defines("MUSIC_CORE_TESTING")
    add_files(path.join(project_root, "native", "score_edit.cpp"))
target_end()
add_engine_test("music_core_test", "music_core_test.cpp")
target("music_core_test")
    add_deps("music_core")
target_end()
