# Changelog

All notable changes to this project are recorded here. Release archives include the matching release notes.

## [Unreleased]

- Add future changes here before creating the next version tag.

## [v0.1.3] - 2026-11-01

- Add selectable PDF backends for MSBuild, InstallUtil, Regsvr32, ODBCConf, and msiexec with shared preflight, check, and Shell verification.
- Build and package the managed and native host DLLs required by the four additional backends.
- Add an independent Windows 11 26H2 UserChoiceLatest hash implementation with Machine ID normalization, compressor tables, parity tests, and unknown-DLL fallback.
- Document the experimental ordinary-user 26H2 path and its Machine ID/UserChoiceLatest algorithm.

## [v0.1.0] - 2026-10-31

- Initial Windows per-user file association tool release.
- Add PDF, protocol, and installed-browser association modes.
- Add version-gated Framework64 MSBuild PDF research mode and read-only diagnostics.
- Add bilingual README files and tag-triggered Windows build packaging.
- Document five tested host research paths and their Windows 11 26H2 limitations.
