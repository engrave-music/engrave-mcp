"""Compatibility entry point for the fork-owned check."""

from engrave_testing.integration.test_upstream_gtest import main

if __name__ == "__main__":
    raise SystemExit(main())
