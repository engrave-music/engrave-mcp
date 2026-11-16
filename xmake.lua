-- Native engrave-mcp host; engine targets are included from vendor/engrave.

set_project("engrave-mcp")
set_xmakever("3.1.1")
set_version("0.1.0")
set_languages("cxx23")
set_targetdir("$(builddir)")
includes("xmake/rules/*.lua")
ENGRAVE_BUILD_RULES_LOADED = true
add_rules("mode.debug", "mode.release", "mode.releasedbg", "mode.asan",
    "build.ccache", "toolchain.lld", "ci.test-output")
-- The profile runner shares build/.packages; xmake hashes package configurations
-- so CRT and sanitizer variants remain separate. Always use the pinned sources.
set_policy("package.install_locally", true)
set_policy("package.precompiled", false)

-- Host-only third-party dependency, from the host's pinned package repository
-- (xmake/packages/<letter>/<name>/xmake.lua).  The engine's own pinned
-- packages are required by the engine build that is included below.
add_repositories("engrave-mcp-pinned xmake", {rootdir = os.scriptdir()})
add_requires("asio 1.36.0", {system = false})

-- JSON: host targets add_packages("glaze") explicitly. The engine build pins
-- and requires the same glaze release; declaring it here keeps the host's
-- dependency visible in the host project.
add_requires("glaze 9.1.0", {system = false})

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
            .. "\nRun `git submodule update --init vendor/engrave`,"
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

-- The engine build requires cairo and zlib from this project's Pixi
-- environment (pixi::cairo, pixi::zlib); the host's build callbacks run its
-- Python interpreter.
includes("xmake/pixi.lua")
if not os.isfile(ENGRAVE_PIXI_PYTHON) then
    configure_failure("Pixi environment interpreter is missing: " .. ENGRAVE_PIXI_PYTHON .. "; run `pixi install`")
    return
end

add_includedirs("$(builddir)/xmake-generated")
add_includedirs(path.join(project_root, "engrave_mcp"))

-- Engine targets and tests use these shared roots; host code is public-only.
add_includedirs(ENGRAVE_INCLUDES)

if is_plat("windows") then
    set_runtimes("MD")
    set_exceptions("cxx")
    add_defines("WIN32", "NOMINMAX", "WIN32_LEAN_AND_MEAN", "NOGDI", "_USE_MATH_DEFINES", "_UNICODE", "UNICODE")
    add_cxflags("/utf-8", "/Zc:__cplusplus", {tools = {"cl", "clang_cl"}})
    add_cflags("/utf-8", {tools = {"cl", "clang_cl"}})
else
    add_cxflags("-fPIC", "-pthread")
    add_ldflags("-pthread")
end

-------------------------------------------------------------------------------
-- Host targets
-------------------------------------------------------------------------------
-- The host links engrave and uses its public API; engine tests and probes are
-- defined by the included engine build.

includes("engrave_mcp/xmake.lua")
