-- Pinned standalone headers, managed by xmake. SHA-256 matches xmake-repo's
-- asio-1-36-0 archive pin; keep the recipe local to avoid mutable registry policy.
package("asio")
    set_kind("library", {headeronly = true})
    set_homepage("https://think-async.com/Asio/")
    set_description("Standalone asynchronous I/O and coroutine support")
    set_license("BSL-1.0")
    add_urls("https://github.com/chriskohlhoff/asio/archive/refs/tags/asio-$(version).tar.gz", {
        version = function (version) return version:gsub("%.", "-") end
    })
    add_versions("1.36.0", "0310a76b27e1854f09f696b30de57dc490b5e1b17faed1eb8c9a2891f956e52b")
    add_defines("ASIO_STANDALONE")
    set_cachedir(path.join(os.projectdir(), ".xmake", "cache", "asio", "1.36.0"))
    on_install(function (package)
        local root = os.isdir("asio") and "asio/include" or "include"
        os.cp(path.join(root, "asio.hpp"), package:installdir("include"))
        os.cp(path.join(root, "asio"), package:installdir("include"))
    end)
package_end()
