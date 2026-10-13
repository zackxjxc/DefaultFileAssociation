#pragma once

#include <string>

namespace dfa {

enum class Status {
    Success,
    AlreadyDefault,
    RegisteredOnly,
    UserChoiceRejected,
    Failed,
};

struct Result {
    Status status = Status::Failed;
    std::wstring message;
    unsigned long win32_error = 0;
    bool hta_attempted = false;
};

} // namespace dfa
