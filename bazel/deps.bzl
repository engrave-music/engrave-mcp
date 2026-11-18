"""Host-only pinned third-party dependencies for engrave-mcp."""

load("@bazel_tools//tools/build_defs/repo:http.bzl", "http_archive")

def _host_deps_impl(_mctx):
    http_archive(
        name = "asio",
        build_file = Label("//bazel/third_party:asio.BUILD.bazel"),
        sha256 = "0310a76b27e1854f09f696b30de57dc490b5e1b17faed1eb8c9a2891f956e52b",
        strip_prefix = "asio-asio-1-36-0",
        urls = ["https://github.com/chriskohlhoff/asio/archive/refs/tags/asio-1-36-0.tar.gz"],
    )

host_deps = module_extension(implementation = _host_deps_impl)
