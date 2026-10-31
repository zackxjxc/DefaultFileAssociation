# DefaultFileAssociation

[中文说明](README.zh-CN.md) · English

`DefaultFileAssociation.exe` registers a per-user Windows file association and
sets it as the current user's default when the Windows version permits the
legacy `UserChoice` compatibility path. It is deliberately a user-level tool:
it never requests elevation. If the process is already elevated, it exits with
the message `请以普通用户权限启动此工具。` so that the association is not
written into an unintended administrator profile.

## Build

The project uses C++23, CMake, and Visual Studio 2026 with the **v143 (VS 2022)**
toolset to target Windows 7 SP1 and later. Install the v143 x64/x86 build tools
from Visual Studio Installer's individual components. The default VS 2026
v145 runtime requires Windows 10 and imports APIs unavailable on Windows 7;
defining `_WIN32_WINNT` alone cannot change that precompiled runtime.
The primary release is x86: one executable runs on both 32-bit Windows and
64-bit Windows with WOW64. Configure and build it with:

```powershell
cmake --preset windows-x86
cmake --build --preset windows-x86-release
```

Configure, build, and run the tests with:

```powershell
cmake --preset windows-x86-tests
cmake --build --preset windows-x86-tests-debug
ctest --test-dir out/build/windows-x86-tests-v143 -C Debug --output-on-failure
```

MSVC uses the static runtime for this small standalone utility, so the
resulting executable does not require a separate Visual C++ Redistributable
installation.
The release is written to `out/build/windows-x86-v143/Release`. The optional
`windows-x64` presets remain available for native x64 comparison. Builds declare
Windows 7 as the minimum API version and check the executable's imports for
known newer APIs. This regression check does not replace runtime testing on
each supported Windows version.

The configure step downloads the pinned CLI11 and Microsoft WIL revisions via
CMake `FetchContent`. Set `-DDFA_FETCH_DEPENDENCIES=OFF` when supplying those
packages through a package manager or an existing CMake toolchain.

## Architecture and WOW64

On Windows 7 and later, the HKCU Software keys and ordinary Classes file
associations used here are shared between x86 and x64 processes. ProgID, icon,
UserChoice, capabilities and toast paths remain identical; the utility does not
append `Wow6432Node` or write duplicate associations into both views. COM
CLSID/Interface registration has different rules and is outside this utility's
scope. See [Microsoft's shared-key table](https://learn.microsoft.com/en-us/windows/win32/winprog64/shared-registry-keys).

The HTA fallback uses native `mshta.exe`: `System32` on 32-bit Windows or in an
x64 process, and the `Sysnative` alias from an x86 process on 64-bit Windows.
This preserves the previous x64 host behavior without disabling filesystem
redirection. This internal host path is separate from the reader's registered
open command. Supply the reader's real installation path; system-directory
paths have WOW64 redirection rules and `Sysnative` is not usable by 64-bit
Explorer. See [Microsoft's redirector documentation](https://learn.microsoft.com/en-us/windows/win32/winprog64/file-system-redirector).

An x86 tool can register x86 or x64 readers on 64-bit Windows. A 64-bit reader
is rejected on 32-bit Windows. The UserChoice hash uses fixed 32-bit arithmetic
and UTF-16 input; process bitness does not change its format. Diagnostics read
the native HKLM OS-version view and report process bitness separately from OS
architecture. Tests on a 64-bit host do not replace a real 32-bit Windows VM run.

## Usage

The executable accepts UTF-16 Windows command-line arguments. The names with
underscores are retained as compatibility aliases for the original tool.

```text
DefaultFileAssociation.exe --ext=.pdf --exe-path="C:\Users\hudening\AppData\Local\Programs\SamplePdfReader\SamplePdfReader.exe" --icon-index=3 --prog-id=samplepdfreader
```

Important options:

* `--ext` is required and must be a dot-prefixed extension such as `.pdf`.
* `--exe-path` is required for a set operation and must be an existing absolute
  executable path.
* `--icon-index` selects an icon resource inside the executable. If omitted,
  Windows uses the executable's default icon. `--icon-path` can point to a
  separate `.ico`, `.exe`, or `.dll` resource file.
* `--prog-id` is the registry ProgID. If omitted, the tool derives one from the
  executable stem and extension (for example `SamplePdfReader.pdf`).
* `--description` controls the friendly name shown by Windows. It defaults to
  the executable stem.
* `--no-hta` disables the Windows 10/11 HTA compatibility fallback.
* `--dry-run` validates and prints the request without changing the registry.
* `--check --ext=.pdf --prog-id=samplepdfreader` verifies the current association.
* `--diagnose --ext=.pdf --prog-id=samplepdfreader` collects read-only registry and Shell
  diagnostics; a zero exit code means collection completed, not that the default works.
* `--log-file=run.log` appends UTF-8 output and before/after diagnostic snapshots.
* `--experiment=default|none|toasts|notify|toasts-notify` selects first-use VM
  comparisons. Ordinary commands use `default`, equivalent to `toasts`;
  `none` retains the previous behavior for comparison. See [VM test instructions](docs/first-use-vm-tests.md).

### Installed protocols and default browser

`--protocol` and `--browser` are mutually exclusive with `--ext`. They reuse
installed registration and reject EXE, icon and description overrides.

```text
DefaultFileAssociation.exe --protocol=https --prog-id=ChromeHTML --dry-run
DefaultFileAssociation.exe --protocol=https --prog-id=ChromeHTML
DefaultFileAssociation.exe --protocol=https --prog-id=ChromeHTML --check
DefaultFileAssociation.exe --browser="Google Chrome" --dry-run
DefaultFileAssociation.exe --browser="Google Chrome" --log-file=browser.log
DefaultFileAssociation.exe --browser="Google Chrome" --check
```

Replace `ChromeHTML` with the actual installed ProgID. Browser names are exact
value names in HKCU or HKLM `Software\RegisteredApplications`; Firefox commonly
includes an installation suffix, so do not assume that its registered name is
simply `Firefox`. These names can be inspected with `reg query` on those keys.

Browser mode reads all four mappings (`http`, `https`, `.htm`, `.html`) from
the selected application's Capabilities. URL and HTML mappings may use different
ProgIDs. User registration takes precedence, with machine registration used only
when the user entry is absent. Machine Capabilities are opened in the native
registry view, falling back to the default view when the native key is absent.
No browser registration, icon, capabilities or installed command is overwritten.
This mode requires a registered desktop handler with an open command and a
`%1`/`%L` placeholder; COM-only or packaged handlers lacking such a command are
rejected during preflight.

Protocol choices use `HKCU\Software\Microsoft\Windows\Shell\Associations\UrlAssociations\https\UserChoice`,
not `FileExts\https`. The hash identifier is `https`, without a dot or colon.
Shell validation uses `AT_URLPROTOCOL`; Win8+ command resolution uses the protocol
query flag, while Win7 first resolves the selected ProgID. Win7 reused handlers
receive a plain UserChoice ProgId, without a hash.

All four handlers are preflighted before changes. Setting is sequential and is
not a registry transaction: if Windows rejects one association, earlier choices
may already have changed. Every result is reported, and any failure makes the
batch exit non-zero. Each item is verified through UserChoice and the effective
Shell handler, rather than trusting `mshta` exit or successful registry writes.
`--check`, `--diagnose`, `--dry-run` and `--no-hta` also work in these modes.
Actual browser switching under UCPD and Win7/32-bit OS behavior still require VM
validation; automated tests never change the real HTTP/HTTPS defaults.

The tool writes only under `HKEY_CURRENT_USER`, including the per-user
`Software\\Classes` registration, application capabilities, and Explorer's
`FileExts` bookkeeping. Windows 8 and later also receive the UserChoice hash
used by the original reader. The hash implementation uses the documented
Windows CNG/crypt32 APIs and keeps the original algorithm's input and version
rules. The HTA fallback is plain source text; the original private Base64
obfuscation was a constant-string concealment and is not needed here.
The HTA file uses UTF-16LE so Unicode registry paths are preserved. The tool
registers only the generic `open` verb; it does not assume that the target
executable accepts the original reader's printing switches.

Existing UserChoice keys may deny value writes even to their owner. Before
replacing the key, the tool removes only the explicit deny-SET_VALUE entry
for the current user on a UserChoice key owned by that user. Other permissions
and ownership are retained; it does not grant access to everyone or elevate.
If UCPD blocks the direct update, the HTA compatibility path is still tried.

A successful command requires both a matching UserChoice hash (Windows 8+)
and a Windows Shell query confirming the effective ProgID and open command.
`--check` performs the same verification without writing registry values.
Setting an association again also refreshes its icon, description, and open
command, even when the application is already the effective default.
On Windows 10 and later, registration also updates per-extension
`ApplicationAssociationToasts` records for the target and existing OpenWith
candidates. This resolved the first-open chooser in the user's VM comparison;
the exact OS build and cross-version results still need to be collected.
Synchronous notification and the extra one-second wait remain optional experiments.

If Windows rejects the UserChoice update, the command returns a non-zero exit
code and points the user to the Default Apps settings page. It does not attempt
to bypass administrator policy or impersonate another user.

## Current Windows 11 research status

The EXE now embeds an explicit Framework64 MSBuild PDF backend and supports four
additional selectable Windows host backends. Each backend performs compatibility
preflight before candidate registration, sets the new-format record, and verifies
its Hash plus the effective Shell handler. The default icon index is **1**.

```powershell
DefaultFileAssociation.exe --msbuild --preflight --ext=.pdf --prog-id=samplepdfreader.pdf
DefaultFileAssociation.exe --msbuild --ext=.pdf --prog-id=samplepdfreader.pdf --exe-path="C:\Apps\SamplePdfReader.exe" --icon-index=1 --log-file=set.log
DefaultFileAssociation.exe --msbuild --check --ext=.pdf --prog-id=samplepdfreader.pdf
```

This requires 64-bit Windows, Framework64 MSBuild, HashVersion 1, and one of two
known system DLL fingerprints. Unknown builds fail before registration. The ISO
profile is statically matched and still needs VM execution validation. The task
is embedded, so no external project, Python, or Visual Studio is required at runtime.
See the [portable VM test instructions](docs/msbuild-vm-test-package.md) and
[three research directions](docs/default-association-research-roadmap.md).

The four host modes use the same arguments and `--preflight`/`--check` stages:

```powershell
DefaultFileAssociation-x64.exe --backend=installutil --preflight --ext=.pdf --prog-id=samplepdfreader.pdf
DefaultFileAssociation-x64.exe --backend=regsvr32 --ext=.pdf --prog-id=samplepdfreader.pdf --exe-path="C:\Apps\SamplePdfReader.exe"
DefaultFileAssociation-x64.exe --backend=odbcconf --check --ext=.pdf --prog-id=samplepdfreader.pdf
DefaultFileAssociation-x64.exe --backend=msiexec --ext=.pdf --prog-id=samplepdfreader.pdf --exe-path="C:\Apps\SamplePdfReader.exe"
```

The accepted backend names are `msbuild`, `installutil`, `regsvr32`, `odbcconf`,
and `msiexec`; `--msbuild` remains an alias for `--backend=msbuild`. The four
new modes load the managed or native host DLLs built beside the EXE. They are
limited to PDF, the `DefaultFileAssociation-x64.exe` build, and non-elevated users;
the x86 executable rejects these modes. Unknown system DLL
fingerprints fail before registration, and a failed selected backend does not
fall back to the legacy path.

The legacy setting/check path does not implement `UserChoiceLatest`. A separate
x64 `--latest-native-probe` can validate the new record on the exact researched
system DLL, but its direct write was denied on the tested host. The Framework
MSBuild experiment completed writes and independent validation on that host;
it is not a portable or supported Windows API implementation.

The HTA route is excluded from the current research plan. Workflow compiler
tests triggered Defender and are also excluded. The host backends remain
version-gated experiments and need VM validation on each supported system DLL
build. No further host execution tests should be run on the physical
workstation; VM validation must retain Defender and record both association
results and security detections.

### Modes and UCPD boundary

The executable provides one user-level association API for file extensions,
protocols, and installed browsers. PDF new-format writes can explicitly select
one of five host-based routes:

1. Framework64 MSBuild;
2. InstallUtil;
3. Regsvr32;
4. ODBCConf;
5. msiexec.

These are explicit host backends, not a UCPD bypass selector. The repository
does not patch UCPD, inject into a trusted process, spoof process identity,
disable Defender, or change system protection settings. HTA and the workflow
compiler are excluded after security detections. The native
`--latest-native-probe` is a separate, version-gated probe and its read-only
validation must not be confused with a supported setter.

The `--diagnose` output reports both hash layers without writing them:
`legacy_hash_profile` identifies the legacy, Windows 10 Threshold 1, or
Threshold 2-and-later algorithm selected from the OS version; `latest_hash_version`
and `hash_profile` identify a validated `UserChoiceLatest` record. A latest
record is recognized only when the protected `HashVersion` value and the record
shape are both present. An OS build number alone is not enough.

“Windows 11 26H2 UCPD 破解” is not a supported product claim. The tested
profile is version-specific compatibility research. The current host build
`26300.9711` has a read-only Hash validation profile in
`research/scripts/check_latest_9711.py`, but the product intentionally refuses
that new fingerprint for writes until a separate, normally authorized VM test
establishes the complete behavior. The Framework64 MSBuild experiment therefore
must not be described as a portable solution for the latest Windows 11 build.
Unknown builds fail before internal addresses are called.

## Release packaging

The GitHub Actions workflow runs on tags matching `vMAJOR.MINOR.PATCH`. It
builds x86 and x64 Release binaries with the v143 toolset, runs the x86 CTest
suite, creates a reproducible Windows ZIP archive, and publishes a GitHub
Release. The archive includes both README languages, the license files, VM
instructions, checksums, and the release notes for that tag.

Add the user-visible changes to `CHANGELOG.md` and, for a dedicated bilingual
release description, add `docs/releases/vX.Y.Z.md` before pushing the tag. The
packaging script reads that version-specific file first and stores it as
`RELEASE_NOTES.md` in the archive. Local reader samples are optional and are
never required by the CI build.

See the [research checkpoint](docs/research-checkpoint-20261013.md) for confirmed
results, hypotheses, and limits, and the [VM memory capture plan](docs/ucpd-vm-memory-capture-plan.md)
for ways to inspect loaded policy. The supplied host live dump has now yielded
the runtime PDF lists; see [live dump and alternate hosts](docs/livedump-and-alternatives-20261014.md)
for the msiexec switch test and public MakeDefault result. Raw evidence, system binaries, tools, memory dumps, and reader samples
are kept locally and excluded from Git.

## Source layout

* `src/cli/` contains command-line parsing and validation.
* `src/platform.*` contains Windows version, token, SID, and string
  helpers.
* `src/registry.*` is the small `HKEY_CURRENT_USER` registry wrapper.
* `src/browser.*` resolves registered browser capabilities into a four-item plan.
* `src/association_target.h` shares file/protocol UserChoice paths and ProgID validation.
* `src/association.*` registers the ProgID, icon, open command, capabilities, and
  Explorer bookkeeping.
* `src/user_choice.*` verifies and writes the versioned UserChoice hash, with
  the HTA fallback kept behind the same small interface.
* `src/result.h` is the shared association operation result.
* `src/crypto/` wraps Windows CNG/crypt32 and contains the UserChoice hash
  implementation.
* `tests/` contains crypto/UserChoice vectors and command-line/registry/association regression tests.

The executable and tests link the same `dfa_core` static library. Registry
integration tests use private keys; UserChoice and registration tests redirect
HKCU inside the test process and never launch HTA. Real Windows 7/8/10/11
validation remains necessary, particularly when UserChoice protection changes.

See [NOTICE](NOTICE) for the dependency licenses and pinned revisions.
See [open-source comparison](docs/open-source-review.md) for the Firefox,
Chromium, and association-tool review, resulting changes, and verification limits.
