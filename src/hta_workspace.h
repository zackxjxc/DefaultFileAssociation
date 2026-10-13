#pragma once

#include <filesystem>
#include <utility>

namespace dfa::user_choice {

// 只有原子创建成功才取得目录所有权；同名文件或目录绝不复用。
class HtaWorkspace {
public:
    explicit HtaWorkspace(std::filesystem::path directory)
        : directory_(std::move(directory)), file_(directory_ / L"choice.hta") {
        // 输入如 <Temp>\dfa1234.tmp.d，file_ 就是 <Temp>\dfa1234.tmp.d\choice.hta。
        // create_directory 同时判断并创建；禁止“先 exists 再 rename”的竞争窗口。
        std::error_code error;
        owned_ = std::filesystem::create_directory(directory_, error) && !error;
    }

    HtaWorkspace(const HtaWorkspace&) = delete;
    HtaWorkspace& operator=(const HtaWorkspace&) = delete;

    ~HtaWorkspace() {
        // 创建失败的对象没有所有权，其析构不能触碰同名的已有文件或目录。
        if (owned_) {
            // 只删除本工具的文件和空目录，不递归删除意外出现的其他内容。
            std::error_code ignored;
            std::filesystem::remove(file_, ignored);
            std::filesystem::remove(directory_, ignored);
        }
    }

    [[nodiscard]] explicit operator bool() const noexcept { return owned_; }
    [[nodiscard]] const std::filesystem::path& file() const noexcept { return file_; }

private:
    std::filesystem::path directory_;
    std::filesystem::path file_;
    bool owned_ = false;
};

} // namespace dfa::user_choice
