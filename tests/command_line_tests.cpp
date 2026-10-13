#include "cli/command_line.h"
#include "platform.h"

#include <windows.h>

#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void Require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

dfa::cli::ParseResult Parse(std::vector<std::wstring> arguments) {
    std::vector<wchar_t*> pointers;
    for (auto& argument : arguments) {
        pointers.push_back(argument.data());
    }
    return dfa::cli::Parse(static_cast<int>(pointers.size()), pointers.data());
}

void Run() {
    const auto mshta = dfa::platform::GetMshtaPath();
    DWORD host_type = 0;
    Require(!mshta.empty() && ::GetBinaryTypeW(mshta.c_str(), &host_type), "Native HTA host is accessible");
    Require(host_type == static_cast<DWORD>(dfa::platform::Is64BitWindows() ? SCS_64BIT_BINARY : SCS_32BIT_BINARY),
            "HTA host matches OS architecture, including x86 under WOW64");
    Require(Parse({L"dfa.exe", L"--ext=.pdf", L"--exe-path=" + mshta.wstring(),
                   L"--dry-run"}).ok, "32-bit tool can register native 64-bit executables on 64-bit Windows");
    wchar_t system_directory[MAX_PATH]{};
    Require(::GetSystemDirectoryW(system_directory, MAX_PATH) != 0, "System directory");
    const std::wstring executable = L"--exe_path=" + std::wstring(system_directory) + L"\\cmd.exe";
    const auto aliases = Parse({L"dfa.exe", L"--ext=.PDF", executable,
                                L"--icon_idx=0", L"--prog_name=DocBox.pdf", L"--dry-run"});
    Require(aliases.ok && aliases.options.extension == L".pdf", "Aliases and normalized extension");
    Require(aliases.options.icon_index == 0, "Explicit icon zero is preserved");
    Require(aliases.options.prog_id == L"DocBox.pdf" && aliases.options.dry_run, "Alias values");
    Require(aliases.options.experiment == "default", "Ordinary commands use the fixed default behavior");

    const auto unicode = Parse({L"dfa.exe", L"set-default", L"--ext=.文档", executable,
                                L"--description=中文描述", L"--no-hta", L"--dry-run"});
    Require(unicode.ok && unicode.options.extension == L".文档" &&
                unicode.options.description == L"中文描述", "UTF-16 argument round trip");
    Require(!unicode.options.use_hta && !unicode.options.icon_index, "Optional defaults");
    const auto accented = Parse({L"dfa.exe", L"--ext=.ÉXT", executable, L"--dry-run"});
    Require(accented.ok && accented.options.extension == L".éxt", "Windows Unicode extension lowercase");

    const auto check = Parse({L"dfa.exe", L"check", L"--ext=.pdf", L"--prog-id=DocBox.pdf"});
    Require(check.ok && check.options.check, "Check command without executable");
    const auto latest = Parse({L"dfa.exe", L"--latest-native-probe", L"--ext=.PDF", L"--prog-id=docboxtest.pdf"});
    Require(latest.ok && latest.options.latest_native_probe && !latest.options.use_hta,
            "Native latest experiment bypasses the legacy HTA path");
    Require(Parse({L"dfa.exe", L"--latest-native-probe", L"--ext=.pdf", L"--prog-id=docboxtest.pdf", L"--check"}).ok,
            "Native latest read-only validation");
    Require(!Parse({L"dfa.exe", L"--latest-native-probe", L"--ext=.txt", L"--prog-id=docboxtest.pdf"}).ok,
            "Native latest experiment is limited to PDF");
    Require(!Parse({L"dfa.exe", L"--latest-native-probe", L"--ext=.pdf"}).ok,
            "Native latest experiment requires explicit ProgID");
    Require(!Parse({L"dfa.exe", L"--latest-native-probe", L"--ext=.pdf", L"--prog-id=docboxtest.pdf", executable}).ok,
            "Native latest experiment cannot change handler registration");
    Require(!Parse({L"dfa.exe", L"--latest-native-probe", L"--ext=.pdf", L"--prog-id=docboxtest.pdf", L"--diagnose"}).ok,
            "Native latest experiment rejects competing diagnostic mode");
    Require(!Parse({L"dfa.exe", L"--latest-native-probe", L"--ext=.pdf", L"--prog-id=docboxtest.pdf", L"--dry-run"}).ok,
            "Native latest experiment uses --check for read-only operation");
    Require(!Parse({L"dfa.exe", L"--check", L"--ext=.pdf"}).ok, "Check requires expected ProgID");
    Require(!Parse({L"dfa.exe", L"--check", L"--ext=.pdf", L"--prog-id=DocBox.pdf", executable}).ok,
            "Check rejects executable options");
    Require(!Parse({L"dfa.exe", L"--ext=pdf", executable}).ok, "Extension requires leading dot");
    Require(!Parse({L"dfa.exe", L"--ext=.pdf", executable, L"--icon_idx=-1"}).ok, "Negative icon index");
    Require(!Parse({L"dfa.exe", L"--ext=.pdf", executable, L"--prog-id=bad\\key"}).ok, "Invalid ProgID");
    const auto generated = Parse({L"dfa.exe", L"--ext=.pdf", executable, L"--dry-run"});
    Require(generated.ok && generated.options.prog_id == L"cmd.pdf", "Derived ProgID is finalized by parser");
    const auto boundary = Parse({L"dfa.exe", L"--ext=." + std::wstring(251, L'x'), executable});
    Require(boundary.ok && boundary.options.prog_id.size() == 255, "Generated ProgID length boundary");
    Require(!Parse({L"dfa.exe", L"--ext=." + std::wstring(252, L'x'), executable, L"--dry-run"}).ok,
            "Oversized generated ProgID rejected before dry-run");
    Require(Parse({L"dfa.exe", L"--ext=." + std::wstring(254, L'x'), executable,
                   L"--prog-id=Dfa.Short"}).ok, "Explicit short ProgID permits extension boundary");
    Require(!Parse({L"dfa.exe", L"--ext=." + std::wstring(255, L'x'), executable,
                    L"--prog-id=Dfa.Short"}).ok, "Oversized extension rejected");
    Require(!Parse({L"dfa.exe", L"--ext=.pdf", executable, L"--icon_idx=2",
                     L"--icon_path=" + std::wstring(system_directory) + L"\\cmd.exe"}).ok,
            "Icon index and icon path remain mutually exclusive");
    Require(!Parse({L"dfa.exe", L"--ext=.pdf", executable, L"--unknown"}).ok, "Unknown option");
    Require(Parse({L"dfa.exe", L"--help"}).show_help, "Help works without required values");
    for (const auto* mode : {L"default", L"none", L"toasts", L"notify", L"toasts-notify"}) {
        const auto experiment = Parse({L"dfa.exe", L"--ext=.pdf", executable,
                                        L"--experiment=" + std::wstring(mode), L"--log-file=中文日志.txt"});
        Require(experiment.ok && experiment.options.log_file == L"中文日志.txt", "Experiment and Unicode log path");
    }
    const auto diagnose = Parse({L"dfa.exe", L"--diagnose", L"--ext=.pdf", L"--prog-id=DocBox.pdf"});
    Require(diagnose.ok && diagnose.options.diagnose, "Read-only diagnosis without executable");
    Require(!Parse({L"dfa.exe", L"--diagnose", L"--ext=.pdf"}).ok, "Diagnosis requires ProgID");
    Require(!Parse({L"dfa.exe", L"--diagnose", L"--check", L"--ext=.pdf", L"--prog-id=DocBox.pdf"}).ok,
            "Read-only modes are mutually exclusive");
    Require(!Parse({L"dfa.exe", L"--check", L"--ext=.pdf", L"--prog-id=DocBox.pdf", L"--experiment=toasts"}).ok,
            "Read-only checks reject modifying experiments");
    Require(!Parse({L"dfa.exe", L"--ext=.pdf", executable, L"--experiment=unknown"}).ok,
            "Unknown experiment rejected");
    const auto protocol = Parse({L"dfa.exe", L"--protocol=HTTPS", L"--prog-id=Browser.Url", L"--dry-run"});
    Require(protocol.ok && protocol.options.protocol == L"https" && protocol.options.extension.empty(),
            "Protocol is normalized without inventing a leading dot");
    Require(Parse({L"dfa.exe", L"--protocol=http", L"--prog-id=Browser.Url", L"--check"}).ok,
            "Read-only protocol check");
    Require(Parse({L"dfa.exe", L"--browser=My Browser", L"--dry-run"}).ok,
            "Browser mode accepts a RegisteredApplications name with spaces");
    Require(Parse({L"dfa.exe", L"--browser=My Browser", L"--check"}).ok, "Browser batch check");
    Require(!Parse({L"dfa.exe", L"--protocol=http"}).ok, "Protocol requires installed ProgID");
    Require(!Parse({L"dfa.exe", L"--protocol=https:", L"--prog-id=Browser.Url"}).ok, "Protocol rejects colon");
    Require(!Parse({L"dfa.exe", L"--protocol=.html", L"--prog-id=Browser.Url"}).ok, "Protocol rejects file extension");
    Require(!Parse({L"dfa.exe", L"--protocol=1http", L"--prog-id=Browser.Url"}).ok, "Protocol starts with a letter");
    Require(!Parse({L"dfa.exe", L"--protocol=http", L"--ext=.pdf", L"--prog-id=Browser.Url"}).ok,
            "File and protocol modes are mutually exclusive");
    Require(!Parse({L"dfa.exe", L"--browser=My Browser", L"--protocol=http"}).ok, "Browser excludes protocol");
    Require(!Parse({L"dfa.exe", L"--browser=My Browser", L"--prog-id=Browser.Url"}).ok, "Browser resolves distinct ProgIDs");
    Require(!Parse({L"dfa.exe", L"--protocol=http", L"--prog-id=Browser.Url", executable}).ok,
            "Reuse mode never overwrites installed executable registration");
}

} // namespace

int main() {
    try {
        Run();
    } catch (const std::exception& error) {
        std::fprintf(stderr, "CLI test failure: %s\n", error.what());
        return 1;
    }
}
