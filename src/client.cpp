// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <libnks/client.h>
#include <mutex>
#include <set>
#include <thread>

namespace libnks {
#ifdef __APPLE__
namespace daemon {
bool request_service(const std::filesystem::path &root);
}
#endif
namespace {
ClientResult failure(ClientErrorCode code, ClientOperation operation,
        std::string message, std::error_code system = {}) {
    return { ClientError{ code, operation, system, std::move(message) } };
}
ClientResult io_failure(
        ClientOperation operation, const std::exception &error) {
    const auto *fs_error =
            dynamic_cast<const std::filesystem::filesystem_error *>(&error);
    return failure(ClientErrorCode::io, operation, error.what(),
            fs_error ? fs_error->code() : std::error_code{});
}
} // namespace

ClientResult validate_client_state(const ClientState &state) {
    const auto invalid = [](std::string message) {
        return failure(ClientErrorCode::invalid_state,
                ClientOperation::validate, std::move(message));
    };
    if (state.pages.size() > 256 || state.sounds.size() > 50000 ||
            state.product_short_names.size() > 50000)
        return invalid("NKS state exceeds the supported catalog/page limits");
    std::set<std::uint64_t> ids;
    for (const auto &page : state.pages) {
        if (page.parameters.size() > 48)
            return invalid("NKS page exceeds 48 parameters");
        for (const auto &parameter : page.parameters) {
            if (parameter.id > UINT32_MAX || !ids.insert(parameter.id).second)
                return invalid(
                        "NKS parameter IDs must be unique and fit in 32 bits");
            if (!std::isfinite(parameter.normalized_value) ||
                    parameter.normalized_value < 0 ||
                    parameter.normalized_value > 1)
                return invalid("NKS parameter values must be finite and "
                               "normalized to [0, 1]");
        }
    }
    if (state.selected < -1 ||
            (state.selected >= 0 &&
                    std::size_t(state.selected) >= state.sounds.size()))
        return invalid("NKS sound selection is outside the published catalog");
    for (const auto &sound : state.sounds)
        if (!sound.path.is_absolute())
            return invalid("NKS catalog paths must be absolute");
    if (state.assets)
        for (const auto &[key, bytes] : *state.assets) {
            (void)bytes;
            if (key.identity.empty() ||
                    (key.role != ArtworkRole::product_thumbnail &&
                            key.role != ArtworkRole::instrument_banner))
                return invalid(
                        "NKS artwork requires a valid role and nonempty identity");
        }
    return {};
}

std::optional<std::filesystem::path> apply_action(ClientState &state,
        const ClientAction &action,
        const std::function<bool(std::uint32_t, double)> &setter) {
    return daemon::apply(state, action, setter);
}

struct Client::Impl {
    ClientOptions options;
    daemon::Client bridge;
    ClientResult result;
    std::atomic<int> owner_error{};
    // keep this last so it stops and joins before we destroy bridge and options
    std::jthread owner;
    explicit Impl(ClientOptions input)
        : options(std::move(input)), bridge(options.runtime_directory) {
#ifdef __APPLE__
        if (!options.transport && !options.enumerate &&
                options.runtime_directory == daemon::runtime_directory()) {
            owner = std::jthread([this](std::stop_token stop) {
                std::mutex mutex;
                std::condition_variable_any wake;
                bool warned = false;
                while (!stop.stop_requested()) {
                    bool requested = false;
                    try {
                        requested = bridge.available() ||
                                    daemon::request_service(
                                            options.runtime_directory);
                    } catch (const std::exception &error) {
                        if (!warned)
                            std::fprintf(stderr, "libnks service: %s\n",
                                    error.what());
                        warned = true;
                    }
                    if (!requested && options.mode == ClientMode::standalone &&
                            options.allow_hardware_fallback) {
                        try {
                            owner_error = daemon::run_standalone(
                                    stop, options.runtime_directory);
                        } catch (...) { owner_error = 1; }
                        return;
                    }
                    std::unique_lock lock(mutex);
                    wake.wait_for(lock, stop, std::chrono::seconds(5),
                            [] { return false; });
                }
            });
            return;
        }
#endif
        if (options.mode == ClientMode::standalone &&
                options.allow_hardware_fallback) {
            owner = std::jthread([this](std::stop_token stop) {
                try {
                    owner_error = daemon::run_standalone(stop,
                            options.runtime_directory, options.transport,
                            options.enumerate);
                } catch (...) { owner_error = 1; }
            });
        }
    }
    ClientResult ready() const {
        if (bridge.available())
            return {};
        if (owner_error.load())
            return failure(ClientErrorCode::hardware_owner,
                    ClientOperation::own_hardware,
                    "NKS standalone hardware owner could not start");
        return failure(ClientErrorCode::unavailable, ClientOperation::publish,
                "nksd is not available at " +
                        options.runtime_directory.string());
    }
};

Client::Client(ClientOptions options)
    : impl(std::make_unique<Impl>(std::move(options))) {}
Client::~Client() = default;
bool Client::available() const { return impl->bridge.available(); }
ClientMode Client::mode() const noexcept { return impl->options.mode; }
const std::string &Client::id() const { return impl->bridge.id(); }
void Client::focus() { impl->bridge.focus(); }
const ClientResult &Client::last_result() const noexcept {
    return impl->result;
}

ClientResult Client::publish(ClientState &state) {
    if (auto checked = validate_client_state(state); !checked)
        return impl->result = std::move(checked);
    if (auto ready = impl->ready(); !ready)
        return impl->result = std::move(ready);
    try {
        state.instance_info = impl->options.instance_info;
        impl->bridge.publish(state);
        return impl->result = {};
    } catch (const std::exception &error) {
        return impl->result = io_failure(ClientOperation::publish, error);
    }
}

ClientResult Client::poll(ClientState &state,
        const std::function<void(const ClientAction &)> &handler) {
    if (auto checked = validate_client_state(state); !checked)
        return impl->result = std::move(checked);
    if (auto ready = impl->ready(); !ready) {
        if (ready.error->code == ClientErrorCode::unavailable)
            ready.error->operation = ClientOperation::poll;
        return impl->result = std::move(ready);
    }
    ClientResult callback_result;
    try {
        state.instance_info = impl->options.instance_info;
        impl->bridge.poll(state, [&](const MsgpackValue &raw) {
            if (!callback_result)
                return;
            if (const auto action = decode_kks_plugin_action(
                        { std::string("libnks"), raw });
                    action && handler) {
                try {
                    handler(*action);
                } catch (const std::exception &error) {
                    callback_result = failure(ClientErrorCode::action_handler,
                            ClientOperation::poll, error.what());
                    throw;
                }
            }
            if (auto checked = validate_client_state(state); !checked) {
                callback_result = std::move(checked);
                throw std::runtime_error(callback_result.error->message);
            }
        });
        return impl->result = {};
    } catch (const std::exception &error) {
        return impl->result = callback_result
                                      ? io_failure(ClientOperation::poll, error)
                                      : callback_result;
    }
}

ClientResult Client::restore_browser(ClientState &state) {
    try {
        impl->bridge.restore_browser(state);
        return impl->result = {};
    } catch (const std::exception &error) {
        return impl->result = io_failure(ClientOperation::restore, error);
    }
}
} // namespace libnks
