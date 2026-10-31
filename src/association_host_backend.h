#pragma once

#include "association.h"

#include <optional>
#include <string>
#include <string_view>

namespace dfa::association_host {

enum class Host {
    InstallUtil,
    Regsvr32,
    OdbcConf,
    MsiExec,
};

enum class Mode {
    Preflight,
    Check,
    Set,
};

[[nodiscard]] std::optional<Host> ParseHost(std::wstring_view name) noexcept;
[[nodiscard]] std::wstring_view HostName(Host host) noexcept;
[[nodiscard]] Result Run(const association::Specification& request, Host host, Mode mode,
                         std::wstring& trace);

} // namespace dfa::association_host
