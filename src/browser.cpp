#include "browser.h"
#include "association_target.h"

#include <wil/registry.h>
#include "platform.h"

namespace dfa::browser {

registry::Result<std::vector<association::Specification>> Resolve(
    std::wstring_view name, const association::Specification& defaults) {
    HKEY root = HKEY_CURRENT_USER;
    auto capabilities = registry::ReadString(root, L"Software\\RegisteredApplications", name);
    // 当前用户注册优先；只有不存在时才查整机，权限或类型错误不能被误当作“未安装”。
    if (!capabilities && (capabilities.error().status == ERROR_FILE_NOT_FOUND ||
                          capabilities.error().status == ERROR_PATH_NOT_FOUND)) {
        root = HKEY_LOCAL_MACHINE;
        capabilities = registry::ReadString(root, L"Software\\RegisteredApplications", name);
    }
    if (!capabilities) {
        return std::unexpected(capabilities.error());
    }
    if (capabilities->size() < 9 || !platform::EqualsInsensitive(capabilities->substr(0, 9), L"Software\\")) {
        return std::unexpected(registry::Error{ERROR_INVALID_DATA, L"Invalid browser Capabilities path"});
    }
    // RegisteredApplications 是共享键，但第三方 HKLM Capabilities 路径未必共享。
    // 优先按原生系统视图读取；若只存在 32 位注册，则回退到默认视图，不拼 Wow6432Node。
    wil::unique_hkey capabilities_key;
    HRESULT opened = wil::reg::open_unique_key_nothrow(root, capabilities->c_str(), capabilities_key,
                                                       wil::reg::key_access::read64);
    if (opened == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND) || opened == HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND)) {
        opened = wil::reg::open_unique_key_nothrow(root, capabilities->c_str(), capabilities_key);
    }
    if (FAILED(opened)) {
        return std::unexpected(registry::Error{HRESULT_FACILITY(opened) == FACILITY_WIN32
            ? static_cast<LSTATUS>(HRESULT_CODE(opened)) : ERROR_GEN_FAILURE, L"Open browser Capabilities"});
    }
    std::vector<association::Specification> plan;
    for (const auto* identifier : {L"http", L"https", L".htm", L".html"}) {
        // 如 HKLM\Software\Clients\StartMenuInternet\Google Chrome\Capabilities\URLAssociations。
        // http/https 与 htm/html 可指向不同 ProgID，尤其不能把 FirefoxURL 当作 FirefoxHTML。
        const auto key = association::IsProtocol(identifier) ? L"URLAssociations" : L"FileAssociations";
        auto prog_id = registry::ReadString(capabilities_key.get(), key, identifier);
        if (!prog_id) {
            auto error = prog_id.error();
            error.operation = std::wstring(identifier) + L": " + error.operation;
            return std::unexpected(error);
        }
        if (!association::IsValidProgId(*prog_id)) {
            return std::unexpected(registry::Error{ERROR_INVALID_DATA, L"Invalid browser ProgID"});
        }
        auto command = association::ReadRegisteredCommand(*prog_id);
        if (!command) {
            auto error = command.error();
            error.operation = std::wstring(identifier) + L": " + error.operation;
            return std::unexpected(error);
        }
        auto specification = defaults;
        specification.extension = identifier;
        specification.prog_id = *prog_id;
        specification.reuse_registered = true;
        plan.push_back(std::move(specification));
    }
    return plan;
}

} // namespace dfa::browser
