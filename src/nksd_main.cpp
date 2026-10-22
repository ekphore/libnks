// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <atomic>
#include <charconv>
#include <csignal>
#include <iostream>
#include <libnks/daemon.h>
#include <thread>

namespace {
volatile std::sig_atomic_t interrupted{};
void interrupt(int) { interrupted = 1; }
bool clients_alive(const std::filesystem::path &root) {
    namespace fs = std::filesystem;
    std::error_code error;
    for (const auto &entry : fs::directory_iterator(root, error)) {
        const auto id = entry.path().filename().string();
        if (id.size() != 32 || !std::all_of(id.begin(), id.end(), [](char c) {
                return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
            }))
            continue;
        const auto alive = entry.path() / "alive.msg";
        if (fs::symlink_status(alive, error).type() != fs::file_type::regular)
            continue;
        const auto time = fs::last_write_time(alive, error);
        if (!error && fs::file_time_type::clock::now() - time <
                              std::chrono::seconds(5))
            return true;
    }
    return false;
}
} // namespace
int main(int argc, char **argv) {
    try {
        auto root = libnks::daemon::runtime_directory();
        bool virtual_device = false;
        std::size_t device = 0;
        unsigned idle_timeout = 0;
        for (int i = 1; i < argc; ++i) {
            const std::string_view arg = argv[i];
            if (arg == "--virtual")
                virtual_device = true;
            else if (arg == "--runtime-dir" && i + 1 < argc)
                root = std::filesystem::u8path(argv[++i]);
            else if (arg == "--device" && i + 1 < argc)
                device = std::stoul(argv[++i]);
            else if (arg == "--idle-timeout" && i + 1 < argc) {
                const std::string_view value = argv[++i];
                const auto parsed = std::from_chars(value.data(),
                        value.data() + value.size(), idle_timeout);
                if (parsed.ec != std::errc{} ||
                        parsed.ptr != value.data() + value.size() ||
                        idle_timeout > 3600)
                    throw std::runtime_error(
                            "Idle timeout must be 0..3600 seconds");
            } else if (arg == "--help") {
                std::cout
                        << "nksd [--virtual] [--runtime-dir PATH] [--device "
                           "INDEX] [--idle-timeout SECONDS]\n"
                           "libnks keyboard daemon. Does not use NI's daemon.\n"
                           "Clients discover the same directory through "
                           "LIBNKS_RUNTIME_DIR.\n"
                           "An idle timeout of 0 keeps the daemon running "
                           "(default).\n";
                return 0;
            } else {
                std::cerr << "Unknown or incomplete option: " << arg << '\n';
                return 1;
            }
        }
        std::signal(SIGINT, interrupt);
        std::signal(SIGTERM, interrupt);
        int result = 1;
        std::atomic<bool> done{};
        std::jthread server([&](std::stop_token stop) {
            result = libnks::daemon::run_host(
                    stop, root, virtual_device, device);
            done.store(true);
        });
        auto last_client = std::chrono::steady_clock::now();
        auto next_check = last_client;
        while (!interrupted && !done.load()) {
            const auto time = std::chrono::steady_clock::now();
            if (idle_timeout && time >= next_check) {
                next_check = time + std::chrono::milliseconds(250);
                if (clients_alive(root))
                    last_client = time;
                else if (time - last_client >=
                         std::chrono::seconds(idle_timeout)) {
                    std::cout << "nksd idle; releasing keyboard\n";
                    break;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        server.request_stop();
        server.join();
        return result;
    } catch (const std::exception &e) {
        std::cerr << "nksd: " << e.what() << '\n';
        return 1;
    }
}
