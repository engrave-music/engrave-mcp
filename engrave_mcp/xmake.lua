local project_root = os.projectdir()
local pixi_python = ENGRAVE_PIXI_PYTHON
local source_root = ENGRAVE_SOURCE_ROOT

local host_dir = path.join(project_root, "engrave_mcp")

-- Check every host/test include before building any host target.
target("native_include_check")
    set_kind("phony")
    set_default(false)
    on_build(function ()
        local python = pixi_python
        if not os.isfile(python) then
            raise("Pixi environment interpreter is missing; run `pixi install`")
        end
        os.execv(python, {path.join(project_root, "scripts", "check_native_includes.py"),
            "--engine-source", source_root})
    end)
target_end()

-- The declared contract is independent of the Engine and of tools.json.
target("tool_registry")
    set_kind("static")
    set_default(false)
    add_deps("native_include_check")
    add_packages("glaze", "ngcpp-proxy", "unordered_dense", "amc", {public = true})
    add_includedirs(path.join(source_root, "engrave", "include"))
    add_files(path.join(host_dir, "json.cpp"), path.join(host_dir, "registry.cpp"), path.join(host_dir, "schema_defs.cpp"))
target_end()

-- The document store uses the public engine API.
target("document_store")
    set_kind("static")
    set_default(false)
    add_deps("engrave", "tool_registry", {public = true})
    add_packages("glaze", "ngcpp-proxy", "unordered_dense", "amc", {public = true})
    add_includedirs(path.join(source_root, "engrave", "include"))
    add_files(path.join(host_dir, "api_json.cpp"), path.join(host_dir, "api_edit_json.cpp"),
        path.join(host_dir, "host_api.cpp"))
target_end()

target("registry_dump")
    set_kind("binary")
    set_default(false)
    add_deps("tool_registry")
    add_files(path.join(host_dir, "tools", "registry_dump.cpp"))
target_end()

target("registry_test")
    set_kind("binary")
    set_default(false)
    add_deps("tool_registry")
    add_files(path.join(host_dir, "tests", "registry_test.cpp"))
target_end()

target("registry_check")
    set_kind("phony")
    set_default(false)
    add_deps("registry_dump", "registry_test")
    on_build(function (target)
        local python = pixi_python
        if not os.isfile(python) then
            raise("Pixi environment interpreter is missing; run `pixi install`")
        end
        os.execv(python, {path.join(project_root, "tests", "integration", "test_tool_schema.py"),
            "--dump", target:dep("registry_dump"):targetfile(),
            "--registry-test", target:dep("registry_test"):targetfile()})
    end)
target_end()

target("host_publication")
    set_kind("static")
    set_default(false)
    add_deps("native_include_check")
    add_packages("ngcpp-proxy", "unordered_dense", "amc", {public = true})
    add_files(path.join(host_dir, "publication.cpp"))
    if is_plat("windows") then add_syslinks("advapi32", {public = true}) end
target_end()

target("engrave_mcp_core")
    set_kind("static")
    set_default(false)
    add_deps("document_store", "host_publication", {public = true})
    add_packages("asio", "glaze", "ngcpp-proxy", "unordered_dense", "amc", {public = true})
    add_files(
        path.join(host_dir, "api_output.cpp"),
        path.join(host_dir, "async_session.cpp"),
        path.join(host_dir, "mcp_transport.cpp"),
        path.join(host_dir, "server_main.cpp")
    )
    if is_plat("windows") then
        add_defines("_WIN32_WINNT=0x0A00", {public = true})
        add_syslinks("ws2_32", "mswsock", {public = true})
    end
target_end()

-- Production stdio JSON-RPC host and test-hook entrypoint sharing engrave_mcp_core.
local function add_host_binary(name, basename, entry_source)
    target(name)
        set_kind("binary")
        set_default(false)
        set_basename(basename)
        add_deps("muse_generated", "engrave_mcp_core")
        add_files(path.join(host_dir, entry_source))
        if is_plat("windows") then
            add_files(path.join(host_dir, "utf8.manifest"))
        end
    target_end()
end
add_host_binary("engrave_mcp", "engrave-mcp", "main.cpp")
add_host_binary("engrave_mcp_test", "engrave-mcp-test", "tests/main_test.cpp")

target("host_api_test")
    set_kind("binary")
    set_default(false)
    add_deps("muse_generated", "document_store")
    add_packages("glaze")
    add_files(path.join(host_dir, "tests", "host_api_test.cpp"))
    if is_plat("windows") then
        add_files(path.join(host_dir, "utf8.manifest"))
    end
target_end()


-- Direct public API oracle; no worker or snapshot in either comparison.
target("live_export_test")
    set_kind("binary")
    set_default(false)
    add_deps("muse_generated", "engrave_mcp_core")
    add_packages("glaze")
    add_files(path.join(host_dir, "tests", "live_export_test.cpp"))
    if is_plat("windows") then
        add_files(path.join(host_dir, "utf8.manifest"))
    end
target_end()

-- Linux fsync ordering/failure injection, independent of the engine.
if is_plat("linux") then
    target("publication_test")
        set_kind("binary")
        set_default(false)
        add_deps("host_publication")
        add_files(path.join(host_dir, "tests", "publication_test.cpp"))
        add_ldflags("-Wl,--wrap=fsync", {force = true})
    target_end()
end

-- Private staging permissions/ACL, independent of the engine.
target("staging_test")
    set_kind("binary")
    set_default(false)
    add_deps("host_publication")
    add_files(path.join(host_dir, "tests", "staging_test.cpp"))
    if is_plat("windows") then
        add_files(path.join(host_dir, "utf8.manifest"))
        add_syslinks("advapi32")
    end
target_end()






