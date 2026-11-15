#pragma once

#include <filesystem>

namespace ambi {

class TemporaryFile {
public:
    explicit TemporaryFile(const std::filesystem::path& directory);
    ~TemporaryFile();
    TemporaryFile(const TemporaryFile&) = delete;
    TemporaryFile& operator=(const TemporaryFile&) = delete;

    const std::filesystem::path& path() const noexcept { return path_; }
    void replace(const std::filesystem::path& destination);

private:
    std::filesystem::path path_;
};

} // namespace ambi
