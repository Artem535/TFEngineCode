# vcpkg overlay ports

`vcpkg.json` points `vcpkg-configuration.overlay-ports` here, so these ports
override the ones pinned by `builtin-baseline`.

## libunwind

Provenance: copied verbatim from `microsoft/vcpkg` `ports/libunwind`
(`port-version` 3, SHA512 `f761d5af...`).

Why: GitHub re-archived `libunwind/libunwind` `v1.8.3.tar.gz`, so the SHA512 in
the port at `builtin-baseline` `06da45aa` (`0619716b...`) no longer matches the
downloaded archive. Every CI and release run failed in the "Install vcpkg
Dependencies" step with `error: download ... had an unexpected hash`. Upstream
fixed the hash in `port-version` 2 and added the `ptrace`/`coredump` features in
3, but the baseline still resolves the old portfile; overriding the single port
avoids re-resolving every other dependency to a newer baseline.

Remove this directory and the `vcpkg-configuration` block once
`builtin-baseline` is bumped to a commit whose `libunwind` port already carries
SHA512 `f761d5af...`.
