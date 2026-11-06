local project_root = os.projectdir()
local native_env = path.join(project_root, ".pixi", "envs", "native")
local source_root = ENGRAVE_SOURCE_ROOT

local host_dir = path.join(project_root, "engrave_mcp")

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
    add_files(path.join(host_dir, "registry.cpp"), path.join(host_dir, "schema_defs.cpp"))
target_end()

-- The document store uses the public engine API.
target("v2_store")
    set_kind("static")
    set_default(false)
    add_deps("engrave", "v2_registry")
    add_includedirs(path.join(source_root, "engrave", "include"))
    add_files(path.join(host_dir, "api_json.cpp"), path.join(host_dir, "api_edit_json.cpp"),
        path.join(host_dir, "host_api.cpp"))
target_end()

target("registry_dump")
    set_kind("binary")
    set_default(false)
    add_deps("v2_registry")
    add_files(path.join(host_dir, "tools", "registry_dump.cpp"))
target_end()

target("registry_test")
    set_kind("binary")
    set_default(false)
    add_deps("v2_registry")
    add_files(path.join(host_dir, "tests", "registry_test.cpp"))
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
        os.execv(python, {path.join(project_root, "tests", "integration", "test_tool_schema.py"),
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
        add_files(path.join(host_dir, "api_output.cpp"))
        add_files(path.join(host_dir, "async_session.cpp"))
        add_files(path.join(host_dir, "main.cpp"))
        add_files(path.join(host_dir, "mcp_transport.cpp"))
        add_files(path.join(host_dir, "publication.cpp"))
        add_packages("asio")
        if is_plat("windows") then
            add_defines("_WIN32_WINNT=0x0A00")
            add_syslinks("ws2_32", "mswsock")
            add_files(path.join(host_dir, "utf8.manifest"))
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
    add_files(path.join(host_dir, "tests", "host_api_test.cpp"))
    if is_plat("windows") then
        add_files(path.join(host_dir, "utf8.manifest"))
    end
target_end()


-- M1 direct public API oracle; no worker or snapshot in either comparison.
target("live_export_test")
    set_kind("binary")
    set_default(false)
    add_deps("muse_generated", "v2_store")
    add_files(path.join(host_dir, "tests", "live_export_test.cpp"), path.join(host_dir, "api_output.cpp"),
        path.join(host_dir, "publication.cpp"))
    if is_plat("windows") then
        add_files(path.join(host_dir, "utf8.manifest"))
    end
target_end()

-- Linux fsync ordering/failure injection, independent of the engine.
if is_plat("linux") then
    target("publication_test")
        set_kind("binary")
        set_default(false)
        add_files(path.join(host_dir, "tests", "publication_test.cpp"), path.join(host_dir, "publication.cpp"))
        add_ldflags("-Wl,--wrap=fsync", {force = true})
    target_end()
end

-- Private staging permissions/ACL, independent of the engine.
target("staging_test")
    set_kind("binary")
    set_default(false)
    add_files(path.join(host_dir, "tests", "staging_test.cpp"), path.join(host_dir, "publication.cpp"))
    if is_plat("windows") then
        add_files(path.join(host_dir, "utf8.manifest"))
        add_syslinks("advapi32")
    end
target_end()
