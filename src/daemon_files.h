// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <vector>
#ifndef _WIN32
#include <charconv>
#include <fcntl.h>
#include <limits>
#include <pwd.h>
#include <sys/stat.h>
#include <unistd.h>

namespace libnks::daemon::files {
struct User {
    uid_t uid;
    gid_t gid;
    std::filesystem::path home;
};
inline std::optional<User> sudo_user() {
    if (geteuid() != 0)
        return {};
    const auto *text = std::getenv("SUDO_UID");
    if (!text || !*text)
        return {};
    const std::string_view value(text);
    unsigned long long id{};
    const auto parsed =
            std::from_chars(value.data(), value.data() + value.size(), id);
    if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() ||
            id == 0 || id > std::numeric_limits<uid_t>::max())
        throw std::runtime_error("Invalid sudo user identity for nksd");
    passwd record{}, *user{};
    std::vector<char> buffer(16384);
    int result{};
    while ((result = getpwuid_r(static_cast<uid_t>(id), &record, buffer.data(),
                    buffer.size(), &user)) == ERANGE &&
            buffer.size() < 1024 * 1024)
        buffer.resize(buffer.size() * 2);
    if (result != 0 || !user)
        throw std::runtime_error(
                "Cannot find the sudo user's account for nksd");
    return User{ user->pw_uid, user->pw_gid, user->pw_dir };
}
inline void runtime_permissions(const std::filesystem::path &directory) {
    const auto user = sudo_user();
    const int fd =
            ::open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
    if (fd < 0)
        throw std::runtime_error(
                "Cannot open the nksd runtime directory safely");
    bool ok = true;
    if (user)
        ok = fchown(fd, user->uid, user->gid) == 0;
    if (ok)
        ok = fchmod(fd, 0700) == 0;
    ::close(fd);
    if (!ok)
        throw std::runtime_error("Cannot give the invoking user access to the "
                                 "nksd runtime directory");
}
// USB might need root but the IPC files should still belong to the directory
// owner
inline bool owner_like_directory(int fd, int parent) {
    if (geteuid() != 0)
        return true;
    struct stat info{};
    return fstat(parent, &info) == 0 &&
           fchown(fd, info.st_uid, info.st_gid) == 0;
}
inline bool owner_like_directory(
        int fd, const std::filesystem::path &directory) {
    if (geteuid() != 0)
        return true;
    const int parent =
            ::open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
    if (parent < 0)
        return false;
    const bool ok = owner_like_directory(fd, parent);
    ::close(parent);
    return ok;
}
} // namespace libnks::daemon::files
#endif
