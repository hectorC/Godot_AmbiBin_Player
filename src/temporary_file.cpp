#include "temporary_file.h"

#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <cerrno>
#include <cstdlib>
#include <unistd.h>
#endif

#include <system_error>

namespace ambi {

TemporaryFile::TemporaryFile(const std::filesystem::path& directory)
{
    const auto absolute = std::filesystem::absolute(directory);
#ifdef _WIN32
    wchar_t name[MAX_PATH]{};
    if (!GetTempFileNameW(absolute.c_str(), L"amb", 0, name)) {
        throw std::system_error(static_cast<int>(GetLastError()), std::system_category(),
                                "Could not create a temporary audio file");
    }
    path_ = name;
#else
    auto name = (absolute / "amb-XXXXXX").string();
    const int descriptor = mkstemp(name.data());
    if (descriptor == -1) {
        throw std::system_error(errno, std::generic_category(), "Could not create a temporary audio file");
    }
    close(descriptor);
    path_ = name;
#endif
}

TemporaryFile::~TemporaryFile()
{
    if (!path_.empty()) {
        std::error_code ignored;
        std::filesystem::remove(path_, ignored);
    }
}

void TemporaryFile::replace(const std::filesystem::path& destination)
{
    std::filesystem::rename(path_, destination);
    path_.clear();
}

} // namespace ambi
