// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "daemon_files.h"
#include <CoreFoundation/CoreFoundation.h>
#include <dlfcn.h>
#include <fstream>
#include <libnks/daemon.h>
#include <mach-o/dyld.h>
#include <spawn.h>
#include <sys/file.h>
#include <sys/wait.h>

extern char **environ;

namespace libnks::daemon {
namespace {
namespace fs = std::filesystem;

int launchctl(std::initializer_list<std::string> args) {
    std::vector<std::string> strings{ "/bin/launchctl" };
    strings.insert(strings.end(), args);
    std::vector<char *> argv;
    for (auto &arg : strings)
        argv.push_back(arg.data());
    argv.push_back(nullptr);
    posix_spawn_file_actions_t actions;
    if (posix_spawn_file_actions_init(&actions) != 0)
        return -1;
    const auto out = posix_spawn_file_actions_addopen(
            &actions, STDOUT_FILENO, "/dev/null", O_WRONLY, 0);
    const auto err = posix_spawn_file_actions_addopen(
            &actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
    pid_t pid{};
    const auto spawned = out || err ? -1
                                    : posix_spawn(&pid, argv[0], &actions,
                                              nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    if (spawned != 0)
        return -1;
    int status{};
    pid_t waited;
    do {
        waited = waitpid(pid, &status, 0);
    } while (waited < 0 && errno == EINTR);
    return waited == pid && WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

fs::path helper_for(const fs::path &image) {
    const auto contents = image.parent_path().parent_path();
    if (image.parent_path().filename() != "MacOS" ||
            contents.filename() != "Contents")
        return {};
    const auto helper = contents / "Helpers/nksd";
    return fs::is_regular_file(helper) && access(helper.c_str(), X_OK) == 0
                   ? helper
                   : fs::path{};
}

fs::path helper() {
    // dladdr finds the plug-in, not the DAW executable that loaded it
    Dl_info info{};
    if (dladdr(reinterpret_cast<const void *>(&helper), &info) &&
            info.dli_fname)
        if (auto path = helper_for(fs::canonical(info.dli_fname));
                !path.empty())
            return path;
    std::uint32_t size{};
    _NSGetExecutablePath(nullptr, &size);
    std::vector<char> path(size);
    if (_NSGetExecutablePath(path.data(), &size) == 0)
        return helper_for(fs::canonical(path.data()));
    return {};
}

std::string xml(std::string_view text) {
    std::string result;
    for (const char c : text) {
        switch (c) {
            case '&':
                result += "&amp;";
                break;
            case '<':
                result += "&lt;";
                break;
            case '>':
                result += "&gt;";
                break;
            default:
                result += c;
        }
    }
    return result;
}

struct Descriptor {
    int fd{ -1 };
    ~Descriptor() {
        if (fd >= 0)
            close(fd);
    }
};

bool helper_removed(const fs::path &plist) {
    std::error_code error;
    if (!fs::is_regular_file(fs::symlink_status(plist, error)) ||
            fs::file_size(plist, error) > 65536 || error)
        return false;
    std::ifstream stream(plist, std::ios::binary);
    const std::string bytes{ std::istreambuf_iterator<char>(stream), {} };
    const auto data = CFDataCreate(nullptr,
            reinterpret_cast<const UInt8 *>(bytes.data()), bytes.size());
    if (!data)
        return false;
    const auto value = CFPropertyListCreateWithData(
            nullptr, data, kCFPropertyListImmutable, nullptr, nullptr);
    CFRelease(data);
    if (!value)
        return false;
    bool removed = false;
    if (CFGetTypeID(value) == CFDictionaryGetTypeID()) {
        const auto args = CFDictionaryGetValue(
                static_cast<CFDictionaryRef>(value), CFSTR("ProgramArguments"));
        if (args && CFGetTypeID(args) == CFArrayGetTypeID() &&
                CFArrayGetCount(static_cast<CFArrayRef>(args)) > 0) {
            const auto path =
                    CFArrayGetValueAtIndex(static_cast<CFArrayRef>(args), 0);
            if (CFGetTypeID(path) == CFStringGetTypeID()) {
                const auto string = static_cast<CFStringRef>(path);
                std::vector<char> text(CFStringGetMaximumSizeForEncoding(
                                               CFStringGetLength(string),
                                               kCFStringEncodingUTF8) +
                                       1);
                if (CFStringGetCString(string, text.data(), text.size(),
                            kCFStringEncodingUTF8))
                    removed = !fs::exists(text.data(), error) && !error;
            }
        }
    }
    CFRelease(value);
    return removed;
}
} // namespace

bool request_service(const fs::path &root) {
    // custom runtimes and sudo must NOT register a service in somebody else's
    // login session
    if (geteuid() == 0 || root != runtime_directory())
        return false;
    const auto executable = helper();
    if (executable.empty())
        return false;
    fs::create_directories(root);
    files::runtime_permissions(root);
    Descriptor directory{ open(
            root.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW) };
    if (directory.fd < 0)
        throw std::runtime_error("Cannot open nksd service directory");
    Descriptor lock{ openat(directory.fd, "service.lock",
            O_CREAT | O_RDWR | O_NOFOLLOW, 0600) };
    if (lock.fd < 0)
        throw std::runtime_error("Cannot lock nksd service registration");
    struct stat metadata{};
    if (fstat(lock.fd, &metadata) != 0 || !S_ISREG(metadata.st_mode) ||
            metadata.st_uid != geteuid())
        throw std::runtime_error("Invalid nksd service registration lock");
    if (flock(lock.fd, LOCK_EX | LOCK_NB) != 0) {
        if (errno == EWOULDBLOCK)
            return true;
        throw std::runtime_error("Cannot lock nksd service registration");
    }
    std::uint64_t hash = 14695981039346656037ULL;
    for (const unsigned char c : root.string()) {
        hash ^= c;
        hash *= 1099511628211ULL;
    }
    const auto label = "org.libnks.nksd." + std::to_string(hash);
    const auto domain = "gui/" + std::to_string(geteuid());
    const auto target = domain + "/" + label;
    const auto plist = root / "service.plist";
    // the user may move the app or uninstall the plug-in that registered us
    if (helper_removed(plist))
        (void)launchctl({ "bootout", target });
    // no -k: another plug-in may already be using this process
    if (launchctl({ "kickstart", target }) == 0)
        return true;
    const auto text =
            "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
            "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
            "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
            "<plist version=\"1.0\"><dict><key>Label</key><string>" +
            label + "</string><key>ProgramArguments</key><array><string>" +
            xml(executable.string()) +
            "</string><string>--runtime-dir</string><string>" +
            xml(root.string()) +
            "</string><string>--idle-timeout</string><string>30</string>"
            "</array><key>RunAtLoad</key><false/><key>KeepAlive</key><false/>"
            "<key>ProcessType</key><string>Interactive</string>"
            "<key>StandardOutPath</key><string>" +
            xml((root / "service.log").string()) +
            "</string><key>StandardErrorPath</key><string>" +
            xml((root / "service.log").string()) + "</string></dict></plist>\n";
    Descriptor file{ openat(directory.fd, "service.plist",
            O_CREAT | O_WRONLY | O_TRUNC | O_NOFOLLOW, 0600) };
    if (file.fd < 0)
        throw std::runtime_error("Cannot write nksd service definition");
    std::size_t offset{};
    while (offset < text.size()) {
        const auto count =
                write(file.fd, text.data() + offset, text.size() - offset);
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0)
            throw std::runtime_error("Cannot write nksd service definition");
        offset += std::size_t(count);
    }
    // not in Library/LaunchAgents on purpose, we only want it for this login
    // session and not on every login after
    if (const auto error = launchctl({ "bootstrap", domain, plist.string() });
            error != 0)
        throw std::runtime_error("Cannot register nksd with launchd (" +
                                 std::to_string(error) + ")");
    if (const auto error = launchctl({ "kickstart", target }); error != 0)
        throw std::runtime_error("Cannot start nksd with launchd (" +
                                 std::to_string(error) + ")");
    return true;
}
} // namespace libnks::daemon
