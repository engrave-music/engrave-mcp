-- Qt-free xmake build for the engrave-mcp host (MuseScore 5.0 engine).
--
-- The engine's build belongs to the engine.  The fork (github.com/Locietta/
-- engrave, consumed as the pinned vendor/musescore submodule) carries its own
-- xmake build: project settings, the pinned package recipes, the engine module
-- targets, the engrave adapter library and their tests, all in the engine
-- checkout's xmake/ directory.  This file includes that build - from
-- vendor/musescore by default, or from the checkout named by
-- --musescore_source - and keeps what is the host's:
--
--   v2_registry, registry_dump       standalone v2 schema authority
--   registry_test, registry_check    v2 contract validation
--   v2_store                         public-API document store
--   engrave_mcp                    stdio JSON-RPC host (engrave-mcp binary)
--   engrave_mcp_test, host_api_test, export_job_test   host regression binaries
--
-- Engine targets (muse_global, muse_draw, muse_mpe, engraving, iex_musicxml,
-- iex_guitarpro, iex_midi, native_fluidsynth, muse_audio_engine,
-- muse_generated), the engrave library and its tests and probes are defined by
-- the included engine build and are not repeated here.
--
-- Host translation units include only the engine's public engrave/ headers.
-- The included engine targets and their tests still use internal include roots.

set_project("engrave-mcp")
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
-- The engine's page renderer links cairo.  A bare checkout uses the system
-- cairo; a host with no system cairo (linux-host) or a pinned conda cairo is given
-- with --cairo_prefix, the same option and layout the engine's own xmake.lua
-- accepts (<prefix>/include/cairo, <prefix>/lib).
option("cairo_prefix")
    set_default("")
    set_showmenu(true)
    set_description("cairo install prefix (default: the native environment or the system cairo)")
option_end()

local cairo_prefix = get_config("cairo_prefix")
if cairo_prefix and cairo_prefix ~= "" then
    cairo_prefix = path.absolute(cairo_prefix)
    target("engrave")
        add_sysincludedirs(path.join(cairo_prefix, "include"), path.join(cairo_prefix, "include", "cairo"))
        add_linkdirs(path.join(cairo_prefix, "lib"), {public = true})
        if not is_plat("windows") then
            add_rpathdirs(path.join(cairo_prefix, "lib"))
        end
    target_end()
end
-- zlib comes from the native environment for every binary, the engine's tests
-- included: the engine's zip container links it but leaves the choice to the
-- including project.  cairo is linked by engrave itself.
add_links("z")

-- Engine targets and tests use these shared roots; host code is public-only.
add_includedirs(ENGRAVE_INCLUDES)

-- The Qt-free feature selection comes from the engine manifest generator (its
-- PROJECT_DEFINES list), so there is one place to change what the engine
-- and its tests compile as.
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

-------------------------------------------------------------------------------
-- Host targets
-------------------------------------------------------------------------------
-- The host links engrave and uses its public API; engine tests and probes are
-- defined by the included engine build.

local native_dir = path.join(project_root, "native")

-- Check every host/test include before building any host target.
target("native_include_check")
    set_kind("phony")
    set_default(false)
    on_build(function ()
        local python = path.join(native_env, is_host("windows") and "python.exe" or "bin/python")
        if not os.isfile(python) then
            python = is_host("windows") and "python" or "python3"
        end
        os.execv(python, {path.join(project_root, "scripts", "check_native_includes.py"),
            "--engine-source", source_root})
    end)
target_end()

-- The declared contract is independent of the Engine and of tools.json.
target("v2_registry")
    set_kind("static")
    set_default(false)
    add_deps("native_include_check")
    add_includedirs(path.join(source_root, "engrave", "include"))
    add_files(path.join(native_dir, "registry.cpp"), path.join(native_dir, "schema_defs.cpp"))
target_end()

-- The document store uses the public engine API.
target("v2_store")
    set_kind("static")
    set_default(false)
    add_deps("engrave", "v2_registry")
    add_includedirs(path.join(source_root, "engrave", "include"))
    add_files(path.join(native_dir, "api_json.cpp"), path.join(native_dir, "api_edit_json.cpp"),
        path.join(native_dir, "host_api.cpp"))
target_end()

target("registry_dump")
    set_kind("binary")
    set_default(false)
    add_deps("v2_registry")
    add_files(path.join(native_dir, "registry_dump.cpp"))
target_end()

target("registry_test")
    set_kind("binary")
    set_default(false)
    add_deps("v2_registry")
    add_files(path.join(native_dir, "registry_test.cpp"))
target_end()

target("registry_check")
    set_kind("phony")
    set_default(false)
    add_deps("registry_dump", "registry_test")
    on_build(function (target)
        local python = path.join(native_env, is_host("windows") and "python.exe" or "bin/python")
        if not os.isfile(python) then
            python = is_host("windows") and "python" or "python3"
        end
        os.execv(python, {path.join(project_root, "scripts", "test_tool_schema.py"),
            "--dump", target:dep("registry_dump"):targetfile(),
            "--registry-test", target:dep("registry_test"):targetfile()})
    end)
target_end()

-- The stdio JSON-RPC host and the same host with async test hooks.
local function add_host_binary(name, basename)
    target(name)
        set_kind("binary")
        set_default(false)
        set_basename(basename)
        add_deps("muse_generated", "v2_registry", "v2_store")
        add_files(path.join(native_dir, "*.cpp"))
        remove_files(path.join(native_dir, "*_test.cpp"), path.join(native_dir, "registry.cpp"),
            path.join(native_dir, "schema_defs.cpp"), path.join(native_dir, "registry_dump.cpp"),
            path.join(native_dir, "api_json.cpp"), path.join(native_dir, "api_edit_json.cpp"),
            path.join(native_dir, "host_api.cpp"))
        add_packages("asio")
        if is_plat("windows") then
            add_defines("_WIN32_WINNT=0x0A00")
            add_syslinks("ws2_32", "mswsock")
            add_files(path.join(native_dir, "utf8.manifest"))
        end
    target_end()
end
add_host_binary("engrave_mcp", "engrave-mcp")
add_host_binary("engrave_mcp_test", "engrave-mcp-test")
target("engrave_mcp_test")
    add_defines("MUSIC_ASYNC_TEST_HOOKS")
target_end()

target("host_api_test")
    set_kind("binary")
    set_default(false)
    add_deps("muse_generated", "v2_store")
    add_files(path.join(native_dir, "host_api_test.cpp"))
    if is_plat("windows") then
        add_files(path.join(native_dir, "utf8.manifest"))
    end
target_end()

target("export_job_test")
    set_kind("binary")
    set_default(false)
    add_deps("muse_generated", "v2_store")
    add_files(path.join(native_dir, "export_job_test.cpp"), path.join(native_dir, "api_output.cpp"),
        path.join(native_dir, "worker_v2.cpp"), path.join(native_dir, "background_job.cpp"),
        path.join(native_dir, "export_job.cpp"))
    if is_plat("windows") then
        add_files(path.join(native_dir, "utf8.manifest"))
    end
target_end()
