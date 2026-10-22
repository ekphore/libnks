// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "libnks/daemon.h"
#include <system_error>

namespace libnks {

// not thread safe, lock around it yourself. plug-ins don't need raw USB or
// frames, that's what the session/daemon APIs are for
using ClientState = daemon::State;
using ClientSound = daemon::Sound;
using ClientAction = OdrDecodedPluginAction;
using ParameterAction = OdrParameterAction;
using BrowserSoundAction = OdrBrowserSoundAction;
using BrowserTriggerAction = OdrBrowserTriggerAction;
using BrowserNavigationAction = OdrBrowserNavigationAction;

enum class ClientMode : std::uint8_t { plugin, standalone };
struct ClientOptions {
    ClientMode mode{ ClientMode::plugin };
    std::filesystem::path runtime_directory{ daemon::runtime_directory() };
    OdrInstanceInfo instance_info{ "NKS", "0.1.0", OdrInstanceType::vst3,
        false };
    bool allow_hardware_fallback{ true }; // ignored in plugin mode
    // standalone tries nksd before these optional backends; plugin mode never
    // calls them
    std::function<std::unique_ptr<KksUsbTransport>()> transport;
    std::function<std::vector<KksUsbDeviceLocation>()> enumerate;
};

enum class ClientErrorCode : std::uint8_t {
    unavailable,
    invalid_state,
    io,
    action_handler,
    hardware_owner
};
enum class ClientOperation : std::uint8_t {
    validate,
    publish,
    poll,
    restore,
    own_hardware
};
struct ClientError {
    ClientErrorCode code{ ClientErrorCode::unavailable };
    ClientOperation operation{ ClientOperation::publish };
    std::error_code system;
    std::string message;
};
struct ClientResult {
    std::optional<ClientError> error;
    explicit operator bool() const noexcept { return !error; }
};

[[nodiscard]] ClientResult validate_client_state(const ClientState &state);
// only returns paths from the catalog we sent, parameter changes need a setter
[[nodiscard]] std::optional<std::filesystem::path> apply_action(
        ClientState &state, const ClientAction &action,
        const std::function<bool(std::uint32_t, double)> &set_parameter = {});

class Client {
  public:
    explicit Client(ClientOptions options = {});
    ~Client();
    Client(const Client &) = delete;
    Client &operator=(const Client &) = delete;

    // the host endpoint can be available before the keyboard finishes its
    // handshake
    [[nodiscard]] bool available() const;
    [[nodiscard]] ClientMode mode() const noexcept;
    [[nodiscard]] const std::string &id() const;
    void focus();
    [[nodiscard]] ClientResult publish(ClientState &state);
    // unknown actions get dropped
    [[nodiscard]] ClientResult poll(ClientState &state,
            const std::function<void(const ClientAction &)> &handler);
    [[nodiscard]] ClientResult restore_browser(ClientState &state);
    [[nodiscard]] const ClientResult &last_result() const noexcept;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace libnks
