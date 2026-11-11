# Contributing

## Scope

This repository currently focuses on a C++20 publisher pipeline for OpenMOQ:

- file and live media ingest and packaging
- MSF/CMSF catalogs and publish plans
- MOQT drafts 16, 18, 21, 22 (default), and 23 in the main CLI
- draft-14 and draft-17 compatibility for API callers and the MSFTS example

Transport integration is separated from packaging. Contributions should preserve
that boundary and account for the native and optional libmoq publishing backends.

## Development

```bash
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

## Contribution Guidelines

- Keep changes C++20-compatible on Linux, macOS, and Windows; document platform-specific limitations.
- Prefer incremental changes with tests for parser, segmenter, or packaging behavior.
- Avoid unnecessary media copies in the MP4 to CMSF path.
- Document draft-specific MOQT behavior in [docs/protocol-mapping.md](docs/protocol-mapping.md) when protocol assumptions change. Review the corresponding draft text under `docs/` first.
- If a feature depends on unstable draft behavior, isolate it behind a clearly named abstraction.

## Pull Requests

- Explain the user-visible or interoperability impact.
- Identify the affected draft profiles, publishing backends, and platforms.
- Include a minimal reproduction or test case when fixing parser or packaging bugs.

