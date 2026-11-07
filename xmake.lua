-- Native engrave-mcp host; engine targets are included from vendor/engrave.

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

option("engine_source")
    set_default("")
    set_showmenu(true)
    set_description("engrave engine source tree (default: the vendor/engrave submodule)")
option_end()

local project_root = os.projectdir()
local vendor_root = path.join(project_root, "vendor", "engrave")
local source_root = get_config("engine_source")
if not source_root or source_root == "" then
    source_root = vendor_root
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
    local vendor_build = path.join(vendor_root, "xmake", "engrave.lua")
    if os.isfile(vendor_build) then
        print("warning: configured engine source has no xmake/engrave.lua: " .. source_root
            .. "; automatically falling back to vendor/engrave (" .. vendor_root .. ")")
        source_root = vendor_root
        engine_build = vendor_build
    else
        configure_failure("the engine checkout has no xmake build: " .. engine_build
            .. "\nThe vendor/engrave build is also missing: " .. vendor_build
            .. "\nRun scripts/bootstrap.py to initialize vendor/engrave,"
            .. " or point --engine_source at an engrave checkout that carries xmake/engrave.lua.")
        return
    end
end

ENGRAVE_SOURCE_ROOT = source_root
includes(engine_build)

if not ENGRAVE_INCLUDES or not ENGRAVE_INTERNAL_INCLUDES then
    configure_failure("the engine build did not export ENGRAVE_INCLUDES or ENGRAVE_INTERNAL_INCLUDES")
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
add_includedirs(path.join(project_root, "engrave_mcp"))
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

if is_plat("windows") then
    set_runtimes("MD")
    set_exceptions("cxx")
    add_defines("WIN32", "NOMINMAX", "WIN32_LEAN_AND_MEAN", "NOGDI", "_USE_MATH_DEFINES", "_UNICODE")
    add_cxflags("/utf-8", "/Zc:__cplusplus", {tools = {"cl", "clang_cl"}})
    add_cflags("/utf-8", {tools = {"cl", "clang_cl"}})
else
    add_cxflags("-pthread")
    add_ldflags("-pthread")
end

-------------------------------------------------------------------------------
-- Host targets
-------------------------------------------------------------------------------
-- The host links engrave and uses its public API; engine tests and probes are
-- defined by the included engine build.

includes("engrave_mcp/xmake.lua")
