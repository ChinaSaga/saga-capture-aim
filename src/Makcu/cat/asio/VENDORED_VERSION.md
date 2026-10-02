Asio 1.38.2 (standalone, header-only)

Updated: 2026-10-02
Official stable version: https://think-async.com/Asio/LatestStableRelease.html
Source repository: https://github.com/chriskohlhoff/asio
Source tag: asio-1-38-2
Source commit: 8806a6803cde7054c3049d3666d3ec36786568c5
Release date: 2026-07-19
License: Boost Software License 1.0, included as LICENSE_1_0.txt.

The include directory is copied directly from this upstream tag. No upstream
headers were modified. SagaApp retains ASIO_USE_TS_EXECUTOR_AS_DEFAULT in its
project definitions for its existing executor configuration. Cat's deprecated
address::from_string call must use ip::make_address with this version.

The previous complete vendored tree (1.30.2), including any local changes, was
preserved in .workbuddy/dependencies-backup/asio-1.30.2-before-update before
replacement. That directory is a local backup, not a shipped dependency.
