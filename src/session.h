// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "libnks/events.h"
#include "libnks/transport.h"

#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace libnks {

inline constexpr auto odr_agent_retry_interval = std::chrono::milliseconds(20);
inline constexpr auto odr_agent_retry_timeout = std::chrono::seconds(2);

enum class OdrSessionState : std::uint8_t {
    disconnected,
    transport_connected,
    ready,
};

enum class OdrConnectError : std::uint8_t {
    none,
    transport_connect,
    agent_start,
    send,
    receive,
    malformed_message,
    rpc_error,
    invalid_handshake,
};

struct OdrConnectResult {
    OdrConnectError error{ OdrConnectError::none };
    int system_error{};

    [[nodiscard]] explicit operator bool() const noexcept {
        return error == OdrConnectError::none;
    }
};

enum class OdrCallError : std::uint8_t {
    none,
    not_ready,
    send,
    receive,
    malformed_message,
    rpc_error,
    invalid_result,
};

struct OdrCallResult {
    OdrCallError error{ OdrCallError::none };
    int system_error{};
    MsgpackValue result;

    [[nodiscard]] explicit operator bool() const noexcept {
        return error == OdrCallError::none;
    }
};

struct OdrDeviceConnectResult {
    OdrCallError error{ OdrCallError::none };
    int system_error{};
    bool connected{};

    [[nodiscard]] explicit operator bool() const noexcept {
        return error == OdrCallError::none && connected;
    }
};

enum class KksHostSessionState : std::uint8_t {
    stopped,
    awaiting_handshake_reply,
    ready,
};

struct KksHostReceiveResult {
    KksDeviceMessage message;
    KksDecodedDevicePayload payload;
    std::vector<KksRoutedFrame> outgoing;
};

// only the KKS handshake, you do the endpoint I/O and heartbeats
class KksHostSession {
  public:
    explicit KksHostSession(KksHostHandshake handshake);

    [[nodiscard]] KksRoutedFrame start();
    [[nodiscard]] std::optional<KksRoutedFrame> heartbeat() const;
    [[nodiscard]] std::optional<KksRoutedFrame> stop();
    void disconnect() noexcept;
    [[nodiscard]] std::optional<KksHostReceiveResult> receive(
            std::span<const std::byte> frame);
    [[nodiscard]] std::optional<KksRoutedFrame> add_instance(
            const RpcMethodId &client, const OdrInstanceInfo &info) const;
    [[nodiscard]] std::optional<KksRoutedFrame> remove_instance(
            const RpcMethodId &client) const;
    [[nodiscard]] std::optional<KksRoutedFrame> acquire_focus(
            const RpcMethodId &client) const;
    [[nodiscard]] std::optional<KksRoutedFrame> release_focus(
            const RpcMethodId &client) const;
    [[nodiscard]] std::optional<KksRoutedFrame> set_page(
            const RpcMethodId &client, OdrInstancePage page) const;
    [[nodiscard]] std::optional<KksRoutedFrame> add_asset(
            std::uint64_t device_asset_id,
            const std::array<std::byte, 32> &checksum,
            std::span<const std::byte> payload) const;
    [[nodiscard]] std::optional<KksRoutedFrame> remove_asset(
            std::uint64_t device_asset_id) const;
    [[nodiscard]] std::optional<KksRoutedFrame> reset_assets() const;
    [[nodiscard]] std::optional<KksRoutedFrame> set_host_owned_instance_state(
            const RpcMethodId &client,
            const OdrHostOwnedInstanceState &state) const;
    [[nodiscard]] std::optional<KksRoutedFrame> set_device_owned_instance_state(
            const RpcMethodId &client,
            const OdrDeviceOwnedInstanceState &state) const;
    [[nodiscard]] std::optional<KksRoutedFrame> set_parameter_page(
            const RpcMethodId &client, const KksParameterPageData &data) const;
    [[nodiscard]] std::optional<KksRoutedFrame> set_parameter_page_plugin_data(
            const RpcMethodId &client,
            const KksParameterPagePluginData &data) const;
    [[nodiscard]] std::optional<KksRoutedFrame>
    set_parameter_page_host_owned_viewstate(const RpcMethodId &client,
            const OdrParameterPageHostViewState &state) const;
    [[nodiscard]] std::optional<KksRoutedFrame>
    set_parameter_page_device_owned_viewstate(const RpcMethodId &client,
            const OdrParameterPageDeviceViewState &state) const;
    [[nodiscard]] std::optional<KksRoutedFrame> update_parameter_page_property(
            const RpcMethodId &client,
            KksParameterPagePluginDataProperty property,
            const KksParameterPagePluginDataPropertyValue &value) const;
    [[nodiscard]] std::optional<KksRoutedFrame> update_parameter_page_value(
            const RpcMethodId &client, std::uint32_t parameter_id,
            const OdrParameterValue &value) const;
    [[nodiscard]] std::optional<KksRoutedFrame> set_lightguide(
            const RpcMethodId &client, const LightguideState &state) const;
    [[nodiscard]] std::optional<KksRoutedFrame> set_plugin_chain(
            const RpcMethodId &client, const OdrPluginChainData &data) const;
    [[nodiscard]] std::optional<KksRoutedFrame> set_plugin_chain_current_index(
            const RpcMethodId &client, std::int32_t current_plugin_index) const;
    [[nodiscard]] std::optional<KksRoutedFrame> set_mixer_track_data(
            const RpcMethodId &client,
            const KksMixerModelTrackData &data) const;
    [[nodiscard]] std::optional<KksRoutedFrame> update_mixer_track_property(
            const RpcMethodId &client, std::int32_t track_index,
            KksMixerTrackProperty property,
            const KksMixerTrackPropertyValue &value) const;
    [[nodiscard]] std::optional<KksRoutedFrame> update_mixer_property(
            const RpcMethodId &client, KksMixerProperty property,
            const KksMixerPropertyValue &value) const;
    [[nodiscard]] std::optional<KksRoutedFrame> set_mixer_meters(
            const RpcMethodId &client,
            std::span<const std::array<float, 2>> meters) const;
    [[nodiscard]] std::optional<KksRoutedFrame> set_browser_data(
            const RpcMethodId &client, const KksBrowserData &data) const;
    [[nodiscard]] std::optional<KksRoutedFrame> set_browser_settings(
            const RpcMethodId &client,
            const OdrBrowserSettings &settings) const;
    [[nodiscard]] std::optional<KksRoutedFrame> set_browser_sound_selection(
            const RpcMethodId &client,
            std::optional<std::int32_t> selected_index) const;
    [[nodiscard]] std::optional<KksRoutedFrame> set_browser_filters(
            const RpcMethodId &client, const KksBrowserFilters &filters) const;
    [[nodiscard]] std::optional<KksRoutedFrame> set_browser_filter(
            const RpcMethodId &client, OdrBrowserFilterProperty property,
            const KksBrowserFilter &filter) const;
    [[nodiscard]] std::optional<KksRoutedFrame> set_browser_filter_selection(
            const RpcMethodId &client, OdrBrowserFilterProperty property,
            const KksBrowserFilterSelection &selection) const;
    [[nodiscard]] std::optional<KksRoutedFrame> set_browser_sounds(
            const RpcMethodId &client,
            const KksBrowserSoundResults &sounds) const;
    [[nodiscard]] std::optional<KksRoutedFrame> set_browser_sound_item(
            const RpcMethodId &client, std::int32_t index,
            const KksBrowserSoundItem &item) const;
    [[nodiscard]] std::optional<KksRoutedFrame> set_smartplay_data(
            const RpcMethodId &client,
            const KksSmartplayEngineState &state) const;
    [[nodiscard]] std::optional<KksRoutedFrame>
    set_smartplay_device_owned_viewstate(const RpcMethodId &client,
            const KksSmartplayDeviceViewState &state) const;
    [[nodiscard]] std::optional<KksRoutedFrame> set_project_tree(
            const RpcMethodId &client, const KksProjectTree &tree) const;
    [[nodiscard]] std::optional<KksRoutedFrame> set_selected_track(
            const RpcMethodId &client, std::uint32_t selected_track) const;
    [[nodiscard]] std::optional<KksRoutedFrame> set_track_orientation(
            const RpcMethodId &client,
            std::optional<KksTrackOrientation> orientation) const;
    [[nodiscard]] std::optional<KksRoutedFrame> set_track_plugin_chain(
            const RpcMethodId &client,
            const KksTrackPluginChainData &data) const;
    [[nodiscard]] std::optional<KksRoutedFrame>
    set_track_plugin_chain_selected_plugin(const RpcMethodId &client,
            const std::optional<std::array<std::byte, 16>> &selected_plugin_id)
            const;

    [[nodiscard]] KksHostSessionState state() const noexcept {
        return session_state;
    }
    [[nodiscard]] const KksHostHandshake &handshake() const noexcept {
        return host_handshake;
    }
    [[nodiscard]] const std::optional<KksDeviceHandshakeReply> &
    handshake_reply() const noexcept {
        return reply;
    }

  private:
    KksHostHandshake host_handshake;
    OdrSymbolRegistry registry;
    KksHostSessionState session_state{ KksHostSessionState::stopped };
    std::optional<KksDeviceHandshakeReply> reply;
};

enum class KksHostConnectionError : std::uint8_t {
    none,
    not_connected,
    open,
    send,
    timeout,
    receive,
    protocol,
};
class KksInstanceClient;

// you still drive connect/send/receive and heartbeats
class KksHostConnection {
  public:
    explicit KksHostConnection(KksHostHandshake handshake,
            std::unique_ptr<KksUsbTransport> transport =
                    std::make_unique<KksLibusbDevice>());
    ~KksHostConnection();
    KksHostConnection(const KksHostConnection &) = delete;
    KksHostConnection &operator=(const KksHostConnection &) = delete;

    [[nodiscard]] bool connect(const KksUsbDeviceLocation &location);
    void close() noexcept;
    void shutdown() noexcept;
    [[nodiscard]] bool connected() const noexcept;
    [[nodiscard]] bool send(const KksRoutedFrame &frame);
    [[nodiscard]] bool send(const std::optional<KksRoutedFrame> &frame);
    [[nodiscard]] bool flush();
    [[nodiscard]] bool heartbeat();
    [[nodiscard]] std::optional<KksHostReceiveResult> receive(
            unsigned timeout_ms = 3'600'000);
    [[nodiscard]] KksInstanceClient instance(RpcMethodId client);
    [[nodiscard]] KksHostConnectionError last_error() const noexcept {
        return connection_error;
    }
    [[nodiscard]] int system_error() const noexcept { return system_errno; }
    [[nodiscard]] std::string_view transport_error_stage() const noexcept {
        return transport ? transport->error_stage()
                         : "USB transport unavailable";
    }
    [[nodiscard]] std::string_view transport_error_detail() const noexcept {
        return transport ? transport->error_detail() : std::string_view{};
    }
    [[nodiscard]] KksHostSession &session() noexcept { return active_session; }
    [[nodiscard]] const KksHostSession &session() const noexcept {
        return active_session;
    }

  private:
    void fail(KksHostConnectionError error) noexcept;

    KksHostSession active_session;
    std::unique_ptr<KksUsbTransport> transport;
    KksHostConnectionError connection_error{ KksHostConnectionError::none };
    int system_errno{};
};

// nothing goes out until you flush
class KksInstanceClient {
  public:
    [[nodiscard]] bool add(const OdrInstanceInfo &info);
    [[nodiscard]] bool remove();
    [[nodiscard]] bool acquire_focus();
    [[nodiscard]] bool release_focus();
    [[nodiscard]] bool set_page(OdrInstancePage page);
    [[nodiscard]] bool set_host_owned_state(
            const OdrHostOwnedInstanceState &state);
    [[nodiscard]] bool set_device_owned_state(
            const OdrDeviceOwnedInstanceState &state);
    [[nodiscard]] bool set_parameter_page(const KksParameterPageData &data);
    [[nodiscard]] bool set_parameter_page_plugin_data(
            const KksParameterPagePluginData &data);
    [[nodiscard]] bool set_parameter_page_host_viewstate(
            const OdrParameterPageHostViewState &state);
    [[nodiscard]] bool set_parameter_page_device_viewstate(
            const OdrParameterPageDeviceViewState &state);
    [[nodiscard]] bool update_parameter_page_property(
            KksParameterPagePluginDataProperty property,
            const KksParameterPagePluginDataPropertyValue &value);
    [[nodiscard]] bool update_parameter_value(
            std::uint32_t parameter_id, const OdrParameterValue &value);
    [[nodiscard]] bool set_lightguide(const LightguideState &state);
    [[nodiscard]] bool set_plugin_chain(const OdrPluginChainData &data);
    [[nodiscard]] bool set_plugin_chain_current_index(std::int32_t index);
    [[nodiscard]] bool set_mixer(const KksMixerModelTrackData &data);
    [[nodiscard]] bool update_mixer_property(
            KksMixerProperty property, const KksMixerPropertyValue &value);
    [[nodiscard]] bool update_mixer_track_property(std::int32_t track_index,
            KksMixerTrackProperty property,
            const KksMixerTrackPropertyValue &value);
    [[nodiscard]] bool set_mixer_meters(
            std::span<const std::array<float, 2>> meters);
    [[nodiscard]] bool set_browser(const KksBrowserData &data);
    [[nodiscard]] bool set_browser_settings(const OdrBrowserSettings &settings);
    [[nodiscard]] bool set_browser_sound_selection(
            std::optional<std::int32_t> index);
    [[nodiscard]] bool set_browser_filters(const KksBrowserFilters &filters);
    [[nodiscard]] bool set_browser_filter(
            OdrBrowserFilterProperty property, const KksBrowserFilter &filter);
    [[nodiscard]] bool set_browser_filter_selection(
            OdrBrowserFilterProperty property,
            const KksBrowserFilterSelection &selection);
    [[nodiscard]] bool set_browser_sounds(const KksBrowserSoundResults &sounds);
    [[nodiscard]] bool set_browser_sound_item(
            std::int32_t index, const KksBrowserSoundItem &item);
    [[nodiscard]] bool set_smartplay(const KksSmartplayEngineState &state);
    [[nodiscard]] bool set_smartplay_device_viewstate(
            const KksSmartplayDeviceViewState &state);
    [[nodiscard]] bool set_project_tree(const KksProjectTree &tree);
    [[nodiscard]] bool set_selected_track(std::uint32_t selected_track);
    [[nodiscard]] bool set_track_orientation(
            std::optional<KksTrackOrientation> orientation);
    [[nodiscard]] bool set_track_plugin_chain(
            const KksTrackPluginChainData &data);
    [[nodiscard]] bool set_track_plugin_chain_selected_plugin(
            const std::optional<std::array<std::byte, 16>> &selected_plugin_id);
    [[nodiscard]] bool flush();
    [[nodiscard]] const RpcMethodId &client() const noexcept {
        return client_value;
    }

  private:
    friend class KksHostConnection;
    KksInstanceClient(KksHostConnection &connection, RpcMethodId client)
        : connection(&connection), client_value(std::move(client)) {}

    KksHostConnection *connection{};
    RpcMethodId client_value;
};

enum class OdrMidiDeviceProperty : std::uint8_t { pad_base_note };
class OdrInstanceClient;

// ODR calls block, so don't call this from somewhere that can't wait
class OdrSession {
  public:
    explicit OdrSession(std::string client_identity);
    OdrSession(std::string client_identity,
            std::unique_ptr<OdrFrameTransport> transport);

    [[nodiscard]] OdrConnectResult connect(
            std::string_view endpoint, const OdrInstanceInfo &client_info);
    [[nodiscard]] OdrConnectResult connect_with_agent_start(
            std::string_view endpoint, const OdrInstanceInfo &client_info,
            const std::function<bool()> &start_agent);
    void disconnect() noexcept;
    [[nodiscard]] bool send(std::span<const std::byte> frame);
    [[nodiscard]] std::optional<std::vector<std::byte>> receive();
    [[nodiscard]] std::optional<OdrIncomingEvent> receive_event();
    [[nodiscard]] OdrCallResult call(
            std::string_view method, std::span<const MsgpackValue> parameters);
    [[nodiscard]] OdrDeviceConnectResult connect_device(
            std::string_view device_identifier);
    [[nodiscard]] OdrDeviceConnectResult connect_device(
            const OdrUsbDeviceInfo &device) {
        return connect_device(device.serial_number);
    }
    [[nodiscard]] bool subscribe_device_property(
            std::string_view device_identifier, std::string_view property);
    [[nodiscard]] bool subscribe_device_property(
            std::string_view device_identifier, OdrMidiDeviceProperty property);
    [[nodiscard]] bool upload_asset(const std::array<std::byte, 32> &handle,
            std::span<const std::byte> payload);
    [[nodiscard]] OdrInstanceClient instance(std::string instance_id);

    [[nodiscard]] OdrSessionState state() const noexcept {
        return session_state;
    }
    [[nodiscard]] bool ready() const noexcept {
        return session_state == OdrSessionState::ready;
    }
    [[nodiscard]] int last_system_error() const noexcept {
        return transport->last_error();
    }
    [[nodiscard]] const std::optional<OdrHandshakeReply> &
    handshake_reply() const noexcept {
        return reply;
    }
    [[nodiscard]] bool supports_protocol_2_1() const noexcept {
        return reply && reply->supports_protocol_2_1;
    }
    [[nodiscard]] const std::vector<OdrUsbDeviceInfo> &
    available_devices() const noexcept {
        return devices;
    }
    [[nodiscard]] OdrClientMessages &messages() noexcept {
        return client_messages;
    }
    [[nodiscard]] const OdrClientMessages &messages() const noexcept {
        return client_messages;
    }

  private:
    [[nodiscard]] OdrConnectResult fail(OdrConnectError error) noexcept;

    std::unique_ptr<OdrFrameTransport> transport;
    OdrClientMessages client_messages;
    OdrSessionState session_state{ OdrSessionState::disconnected };
    std::uint32_t next_request_id{};
    std::optional<OdrHandshakeReply> reply;
    std::vector<OdrUsbDeviceInfo> devices;
    std::deque<std::vector<std::byte>> pending_frames;
};

// the session has to outlive this
class OdrInstanceClient {
  public:
    OdrInstanceClient(OdrSession &session, std::string instance_id)
        : active_session(&session), id(std::move(instance_id)) {}

    [[nodiscard]] const std::string &instance_id() const noexcept { return id; }
    [[nodiscard]] bool request_focus();
    [[nodiscard]] bool release_focus();
    [[nodiscard]] bool set_page(std::uint8_t page);
    [[nodiscard]] bool set_parameter_value(
            std::uint32_t parameter_id, const OdrParameterValue &value);
    [[nodiscard]] bool set_parameter_page_data(
            const OdrParameterPageData &data);
    [[nodiscard]] bool set_plugin_chain_data(const OdrPluginChainData &data);
    [[nodiscard]] bool set_lightguide(const LightguideState &state);
    [[nodiscard]] bool set_host_owned_state(
            const OdrHostOwnedInstanceState &state);
    [[nodiscard]] bool set_browser_data(const OdrBrowserData &data);
    [[nodiscard]] bool set_browser_sound_item(
            std::int32_t index, const OdrBrowserSoundItem &item);
    [[nodiscard]] bool set_browser_sound_selection(
            std::optional<std::int32_t> index);
    [[nodiscard]] bool set_browser_sounds(const OdrBrowserSoundResults &sounds);
    [[nodiscard]] bool set_browser_filters(const OdrBrowserFilters &filters);
    [[nodiscard]] bool set_browser_filter(
            OdrBrowserFilterProperty property, const OdrBrowserFilter &filter);
    [[nodiscard]] bool set_browser_filter_selection(
            OdrBrowserFilterProperty property,
            const OdrBrowserFilterSelection &selection);
    [[nodiscard]] bool set_browser_settings(const OdrBrowserSettings &settings);

  private:
    OdrSession *active_session;
    std::string id;
};

enum class SamplerInstanceBackend : std::uint8_t { direct_kks, hosted_odr };

// register the instance first, you still have to connect, dispatch events and
// send heartbeats yourself. use Client if you want to go through nksd
class SamplerInstanceClient {
  public:
    explicit SamplerInstanceClient(KksInstanceClient client)
        : client_value(std::move(client)) {}
    explicit SamplerInstanceClient(OdrInstanceClient client)
        : client_value(std::move(client)) {}

    [[nodiscard]] SamplerInstanceBackend backend() const noexcept;
    [[nodiscard]] bool request_focus();
    [[nodiscard]] bool release_focus();
    [[nodiscard]] bool set_page(OdrInstancePage page);
    [[nodiscard]] bool publish(const PluginInfo &plugin,
            std::span<const Page> pages,
            std::string dawcontrol_plugin_identifier = "NIKB00",
            float tempo = 120.0F);
    [[nodiscard]] bool update_parameter_value(
            std::uint32_t parameter_id, const OdrParameterValue &value);
    [[nodiscard]] bool set_lightguide(const LightguideState &state);
    [[nodiscard]] bool flush();

  private:
    std::variant<KksInstanceClient, OdrInstanceClient> client_value;
};

} // namespace libnks
