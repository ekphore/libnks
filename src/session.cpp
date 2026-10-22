// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "libnks/session.h"

#include <algorithm>
#include <thread>
#include <utility>

namespace libnks {

KksHostSession::KksHostSession(KksHostHandshake handshake)
    : host_handshake(std::move(handshake)),
      registry(host_handshake.symbol_registry) {}

KksRoutedFrame KksHostSession::start() {
    reply.reset();
    session_state = KksHostSessionState::awaiting_handshake_reply;
    return encode_kks_host_handshake(host_handshake);
}

std::optional<KksRoutedFrame> KksHostSession::heartbeat() const {
    if (session_state != KksHostSessionState::ready)
        return std::nullopt;
    return encode_kks_heartbeat();
}

std::optional<KksRoutedFrame> KksHostSession::stop() {
    if (session_state == KksHostSessionState::stopped)
        return std::nullopt;
    session_state = KksHostSessionState::stopped;
    reply.reset();
    return encode_kks_goodbye();
}

void KksHostSession::disconnect() noexcept {
    session_state = KksHostSessionState::stopped;
    reply.reset();
}

std::optional<KksHostReceiveResult> KksHostSession::receive(
        std::span<const std::byte> frame) {
    if (session_state == KksHostSessionState::stopped)
        return std::nullopt;
    auto message = decode_kks_device_message(frame, &registry);
    if (!message)
        return std::nullopt;
    auto payload = decode_kks_device_payload(*message, &registry);
    if (!payload)
        return std::nullopt;

    KksHostReceiveResult result{ std::move(*message), std::move(*payload), {} };
    if (result.message.method ==
            KksDeviceMethod::agent_heartbeat_request_handshake) {
        // the new handshake goes first, the request's empty reply comes after
        session_state = KksHostSessionState::awaiting_handshake_reply;
        reply.reset();
        result.outgoing.push_back(encode_kks_host_handshake(host_handshake));
    } else if (const auto *reply =
                       std::get_if<KksDeviceHandshakeReply>(&result.payload)) {
        this->reply = *reply;
        session_state = KksHostSessionState::ready;
    }
    if (result.message.request_id) {
        result.outgoing.push_back(KksRoutedFrame{ KksUsbChannel::primary,
                encode_rpc_success_response(*result.message.request_id) });
    }
    return result;
}

std::optional<KksRoutedFrame> KksHostSession::add_instance(
        const RpcMethodId &client, const OdrInstanceInfo &info) const {
    if (session_state != KksHostSessionState::ready)
        return std::nullopt;
    return encode_kks_client_instance_add(client, info, &registry);
}

std::optional<KksRoutedFrame> KksHostSession::remove_instance(
        const RpcMethodId &client) const {
    if (session_state != KksHostSessionState::ready)
        return std::nullopt;
    return encode_kks_client_instance_remove(client, &registry);
}

std::optional<KksRoutedFrame> KksHostSession::acquire_focus(
        const RpcMethodId &client) const {
    if (session_state != KksHostSessionState::ready)
        return std::nullopt;
    return encode_kks_client_focus_acquire(client, &registry);
}

std::optional<KksRoutedFrame> KksHostSession::release_focus(
        const RpcMethodId &client) const {
    if (session_state != KksHostSessionState::ready)
        return std::nullopt;
    return encode_kks_client_focus_release(client, &registry);
}

std::optional<KksRoutedFrame> KksHostSession::set_page(
        const RpcMethodId &client, OdrInstancePage page) const {
    if (session_state != KksHostSessionState::ready)
        return std::nullopt;
    return encode_kks_client_page_set(client, page, &registry);
}

std::optional<KksRoutedFrame> KksHostSession::add_asset(
        std::uint64_t device_asset_id,
        const std::array<std::byte, 32> &checksum,
        std::span<const std::byte> payload) const {
    if (session_state != KksHostSessionState::ready)
        return std::nullopt;
    return encode_kks_asset_add(device_asset_id, checksum, payload);
}

std::optional<KksRoutedFrame> KksHostSession::remove_asset(
        std::uint64_t device_asset_id) const {
    if (session_state != KksHostSessionState::ready)
        return std::nullopt;
    return encode_kks_asset_remove(device_asset_id);
}

std::optional<KksRoutedFrame> KksHostSession::reset_assets() const {
    if (session_state != KksHostSessionState::ready)
        return std::nullopt;
    return encode_kks_assets_reset();
}

std::optional<KksRoutedFrame> KksHostSession::set_host_owned_instance_state(
        const RpcMethodId &client,
        const OdrHostOwnedInstanceState &state) const {
    if (session_state != KksHostSessionState::ready)
        return std::nullopt;
    return encode_kks_client_instance_set_host_owned_state(
            client, state, &registry);
}

std::optional<KksRoutedFrame> KksHostSession::set_device_owned_instance_state(
        const RpcMethodId &client,
        const OdrDeviceOwnedInstanceState &state) const {
    if (session_state != KksHostSessionState::ready)
        return std::nullopt;
    return encode_kks_client_instance_set_device_owned_state(
            client, state, &registry);
}

std::optional<KksRoutedFrame> KksHostSession::set_parameter_page(
        const RpcMethodId &client, const KksParameterPageData &data) const {
    if (session_state != KksHostSessionState::ready)
        return std::nullopt;
    return encode_kks_parameter_page_set(client, data, &registry);
}

std::optional<KksRoutedFrame> KksHostSession::set_parameter_page_plugin_data(
        const RpcMethodId &client,
        const KksParameterPagePluginData &data) const {
    if (session_state != KksHostSessionState::ready)
        return std::nullopt;
    return encode_kks_parameter_page_set_plugin_data(client, data, &registry);
}

std::optional<KksRoutedFrame>
KksHostSession::set_parameter_page_host_owned_viewstate(
        const RpcMethodId &client,
        const OdrParameterPageHostViewState &state) const {
    if (session_state != KksHostSessionState::ready)
        return std::nullopt;
    return encode_kks_parameter_page_set_host_owned_viewstate(
            client, state, &registry);
}

std::optional<KksRoutedFrame>
KksHostSession::set_parameter_page_device_owned_viewstate(
        const RpcMethodId &client,
        const OdrParameterPageDeviceViewState &state) const {
    if (session_state != KksHostSessionState::ready)
        return std::nullopt;
    return encode_kks_parameter_page_set_device_owned_viewstate(
            client, state, &registry);
}

std::optional<KksRoutedFrame> KksHostSession::update_parameter_page_property(
        const RpcMethodId &client, KksParameterPagePluginDataProperty property,
        const KksParameterPagePluginDataPropertyValue &value) const {
    if (session_state != KksHostSessionState::ready)
        return std::nullopt;
    return encode_kks_parameter_page_update_property(
            client, property, value, &registry);
}

std::optional<KksRoutedFrame> KksHostSession::update_parameter_page_value(
        const RpcMethodId &client, std::uint32_t parameter_id,
        const OdrParameterValue &value) const {
    if (session_state != KksHostSessionState::ready)
        return std::nullopt;
    return encode_kks_parameter_page_update_parameter_value(
            client, parameter_id, value, &registry);
}

std::optional<KksRoutedFrame> KksHostSession::set_lightguide(
        const RpcMethodId &client, const LightguideState &state) const {
    if (session_state != KksHostSessionState::ready)
        return std::nullopt;
    return encode_kks_client_instance_set_lightguide(client, state, &registry);
}

std::optional<KksRoutedFrame> KksHostSession::set_plugin_chain(
        const RpcMethodId &client, const OdrPluginChainData &data) const {
    if (session_state != KksHostSessionState::ready)
        return std::nullopt;
    return encode_kks_client_instance_set_plugin_chain(client, data, &registry);
}

std::optional<KksRoutedFrame> KksHostSession::set_plugin_chain_current_index(
        const RpcMethodId &client, std::int32_t current_plugin_index) const {
    if (session_state != KksHostSessionState::ready)
        return std::nullopt;
    return encode_kks_client_instance_set_plugin_chain_index(
            client, current_plugin_index, &registry);
}

std::optional<KksRoutedFrame> KksHostSession::set_mixer_track_data(
        const RpcMethodId &client, const KksMixerModelTrackData &data) const {
    if (session_state != KksHostSessionState::ready)
        return std::nullopt;
    return encode_kks_mixer_set_track_data(client, data, &registry);
}

std::optional<KksRoutedFrame> KksHostSession::update_mixer_track_property(
        const RpcMethodId &client, std::int32_t track_index,
        KksMixerTrackProperty property,
        const KksMixerTrackPropertyValue &value) const {
    if (session_state != KksHostSessionState::ready)
        return std::nullopt;
    return encode_kks_mixer_update_track_property(
            client, track_index, property, value, &registry);
}

std::optional<KksRoutedFrame> KksHostSession::update_mixer_property(
        const RpcMethodId &client, KksMixerProperty property,
        const KksMixerPropertyValue &value) const {
    if (session_state != KksHostSessionState::ready)
        return std::nullopt;
    return encode_kks_mixer_update_property(client, property, value, &registry);
}

std::optional<KksRoutedFrame> KksHostSession::set_mixer_meters(
        const RpcMethodId &client,
        std::span<const std::array<float, 2>> meters) const {
    if (session_state != KksHostSessionState::ready)
        return std::nullopt;
    return encode_kks_mixer_set_meters(client, meters, &registry);
}

std::optional<KksRoutedFrame> KksHostSession::set_browser_data(
        const RpcMethodId &client, const KksBrowserData &data) const {
    if (session_state != KksHostSessionState::ready)
        return std::nullopt;
    return encode_kks_browser_set_data(client, data, &registry);
}

std::optional<KksRoutedFrame> KksHostSession::set_browser_settings(
        const RpcMethodId &client, const OdrBrowserSettings &settings) const {
    if (session_state != KksHostSessionState::ready)
        return std::nullopt;
    return encode_kks_browser_set_settings(client, settings, &registry);
}

std::optional<KksRoutedFrame> KksHostSession::set_browser_sound_selection(
        const RpcMethodId &client,
        std::optional<std::int32_t> selected_index) const {
    if (session_state != KksHostSessionState::ready)
        return std::nullopt;
    return encode_kks_browser_set_sound_selection(
            client, selected_index, &registry);
}

std::optional<KksRoutedFrame> KksHostSession::set_browser_filters(
        const RpcMethodId &client, const KksBrowserFilters &filters) const {
    if (session_state != KksHostSessionState::ready)
        return std::nullopt;
    return encode_kks_browser_set_filters(client, filters, &registry);
}

std::optional<KksRoutedFrame> KksHostSession::set_browser_filter(
        const RpcMethodId &client, OdrBrowserFilterProperty property,
        const KksBrowserFilter &filter) const {
    if (session_state != KksHostSessionState::ready)
        return std::nullopt;
    return encode_kks_browser_set_filter(client, property, filter, &registry);
}

std::optional<KksRoutedFrame> KksHostSession::set_browser_filter_selection(
        const RpcMethodId &client, OdrBrowserFilterProperty property,
        const KksBrowserFilterSelection &selection) const {
    if (session_state != KksHostSessionState::ready)
        return std::nullopt;
    return encode_kks_browser_set_filter_selection(
            client, property, selection, &registry);
}

std::optional<KksRoutedFrame> KksHostSession::set_browser_sounds(
        const RpcMethodId &client, const KksBrowserSoundResults &sounds) const {
    if (session_state != KksHostSessionState::ready)
        return std::nullopt;
    return encode_kks_browser_set_sounds(client, sounds, &registry);
}

std::optional<KksRoutedFrame> KksHostSession::set_browser_sound_item(
        const RpcMethodId &client, std::int32_t index,
        const KksBrowserSoundItem &item) const {
    if (session_state != KksHostSessionState::ready)
        return std::nullopt;
    return encode_kks_browser_set_sound_item(client, index, item, &registry);
}

std::optional<KksRoutedFrame> KksHostSession::set_smartplay_data(
        const RpcMethodId &client, const KksSmartplayEngineState &state) const {
    if (session_state != KksHostSessionState::ready)
        return std::nullopt;
    return encode_kks_smartplay_set_data(client, state, &registry);
}

std::optional<KksRoutedFrame>
KksHostSession::set_smartplay_device_owned_viewstate(const RpcMethodId &client,
        const KksSmartplayDeviceViewState &state) const {
    if (session_state != KksHostSessionState::ready)
        return std::nullopt;
    return encode_kks_smartplay_set_device_owned_viewstate(
            client, state, &registry);
}

std::optional<KksRoutedFrame> KksHostSession::set_project_tree(
        const RpcMethodId &client, const KksProjectTree &tree) const {
    if (session_state != KksHostSessionState::ready)
        return std::nullopt;
    return encode_kks_host_set_project_tree(client, tree, &registry);
}

std::optional<KksRoutedFrame> KksHostSession::set_selected_track(
        const RpcMethodId &client, std::uint32_t selected_track) const {
    if (session_state != KksHostSessionState::ready)
        return std::nullopt;
    return encode_kks_host_set_selected_track(
            client, selected_track, &registry);
}

std::optional<KksRoutedFrame> KksHostSession::set_track_orientation(
        const RpcMethodId &client,
        std::optional<KksTrackOrientation> orientation) const {
    if (session_state != KksHostSessionState::ready)
        return std::nullopt;
    return encode_kks_host_set_track_orientation(
            client, orientation, &registry);
}

std::optional<KksRoutedFrame> KksHostSession::set_track_plugin_chain(
        const RpcMethodId &client, const KksTrackPluginChainData &data) const {
    if (session_state != KksHostSessionState::ready)
        return std::nullopt;
    return encode_kks_host_set_track_plugin_chain(client, data, &registry);
}

std::optional<KksRoutedFrame>
KksHostSession::set_track_plugin_chain_selected_plugin(
        const RpcMethodId &client,
        const std::optional<std::array<std::byte, 16>> &selected_plugin_id)
        const {
    if (session_state != KksHostSessionState::ready)
        return std::nullopt;
    return encode_kks_host_set_track_plugin_chain_selected_plugin(
            client, selected_plugin_id, &registry);
}

KksHostConnection::KksHostConnection(
        KksHostHandshake handshake, std::unique_ptr<KksUsbTransport> transport)
    : active_session(std::move(handshake)), transport(std::move(transport)) {}

KksHostConnection::~KksHostConnection() { close(); }

bool KksHostConnection::connected() const noexcept {
    return transport && transport->connected();
}

void KksHostConnection::fail(KksHostConnectionError error) noexcept {
    connection_error = error;
    system_errno = transport ? transport->last_error() : 0;
    active_session.disconnect();
    if (transport)
        transport->close();
}

bool KksHostConnection::connect(const KksUsbDeviceLocation &location) {
    close();
    if (!transport || !transport->open(location)) {
        connection_error = KksHostConnectionError::open;
        system_errno = transport ? transport->last_error() : 0;
        return false;
    }
    auto handshake = active_session.start();
    if (!transport->send(handshake) || !transport->flush()) {
        fail(KksHostConnectionError::send);
        return false;
    }
    connection_error = KksHostConnectionError::none;
    system_errno = 0;
    return true;
}

void KksHostConnection::close() noexcept {
    active_session.disconnect();
    if (transport)
        transport->close();
}

void KksHostConnection::shutdown() noexcept {
    if (!transport)
        return;
    try {
        if (auto goodbye = active_session.stop();
                goodbye && transport->connected()) {
            if (!transport->send(*goodbye) || !transport->flush()) {
                connection_error = KksHostConnectionError::send;
                system_errno = transport->last_error();
            }
        }
    } catch (...) {}
    transport->close();
}

bool KksHostConnection::send(const KksRoutedFrame &frame) {
    if (!connected()) {
        connection_error = KksHostConnectionError::not_connected;
        system_errno = transport ? transport->last_error() : 0;
        return false;
    }
    if (!transport->send(frame)) {
        fail(KksHostConnectionError::send);
        return false;
    }
    connection_error = KksHostConnectionError::none;
    system_errno = 0;
    return true;
}

bool KksHostConnection::send(const std::optional<KksRoutedFrame> &frame) {
    if (!frame) {
        connection_error = connected() ? KksHostConnectionError::protocol
                                       : KksHostConnectionError::not_connected;
        system_errno = 0;
        return false;
    }
    return send(*frame);
}

bool KksHostConnection::heartbeat() { return send(active_session.heartbeat()); }

bool KksHostConnection::flush() {
    if (!connected()) {
        connection_error = KksHostConnectionError::not_connected;
        system_errno = transport ? transport->last_error() : 0;
        return false;
    }
    if (!transport->flush()) {
        fail(KksHostConnectionError::send);
        return false;
    }
    connection_error = KksHostConnectionError::none;
    system_errno = 0;
    return true;
}

std::optional<KksHostReceiveResult> KksHostConnection::receive(
        unsigned timeout_ms) {
    if (!connected()) {
        connection_error = KksHostConnectionError::not_connected;
        system_errno = transport ? transport->last_error() : 0;
        return std::nullopt;
    }
    auto frame = transport->receive_frame(timeout_ms);
    if (!frame) {
        if (transport->receive_timed_out()) {
            connection_error = KksHostConnectionError::timeout;
            system_errno = transport->last_error();
            return std::nullopt;
        }
        fail(KksHostConnectionError::receive);
        return std::nullopt;
    }
    auto result = active_session.receive(*frame);
    if (!result) {
        // a bad frame is just a protocol error, don't drop USB or the
        // handshake over it
        connection_error = KksHostConnectionError::protocol;
        system_errno = 0;
        return std::nullopt;
    }
    for (const auto &outgoing : result->outgoing) {
        if (!transport->send(outgoing)) {
            fail(KksHostConnectionError::send);
            return std::nullopt;
        }
    }
    connection_error = KksHostConnectionError::none;
    system_errno = 0;
    return result;
}

KksInstanceClient KksHostConnection::instance(RpcMethodId client) {
    return KksInstanceClient(*this, std::move(client));
}

bool KksInstanceClient::add(const OdrInstanceInfo &info) {
    return connection->send(
            connection->session().add_instance(client_value, info));
}

bool KksInstanceClient::remove() {
    return connection->send(
            connection->session().remove_instance(client_value));
}

bool KksInstanceClient::acquire_focus() {
    return connection->send(connection->session().acquire_focus(client_value));
}

bool KksInstanceClient::release_focus() {
    return connection->send(connection->session().release_focus(client_value));
}

bool KksInstanceClient::set_page(OdrInstancePage page) {
    return connection->send(connection->session().set_page(client_value, page));
}

bool KksInstanceClient::set_host_owned_state(
        const OdrHostOwnedInstanceState &state) {
    return connection->send(connection->session().set_host_owned_instance_state(
            client_value, state));
}

bool KksInstanceClient::set_device_owned_state(
        const OdrDeviceOwnedInstanceState &state) {
    return connection->send(
            connection->session().set_device_owned_instance_state(
                    client_value, state));
}

bool KksInstanceClient::set_parameter_page(const KksParameterPageData &data) {
    return connection->send(
            connection->session().set_parameter_page(client_value, data));
}

bool KksInstanceClient::set_parameter_page_plugin_data(
        const KksParameterPagePluginData &data) {
    return connection->send(
            connection->session().set_parameter_page_plugin_data(
                    client_value, data));
}

bool KksInstanceClient::set_parameter_page_host_viewstate(
        const OdrParameterPageHostViewState &state) {
    return connection->send(
            connection->session().set_parameter_page_host_owned_viewstate(
                    client_value, state));
}

bool KksInstanceClient::set_parameter_page_device_viewstate(
        const OdrParameterPageDeviceViewState &state) {
    return connection->send(
            connection->session().set_parameter_page_device_owned_viewstate(
                    client_value, state));
}

bool KksInstanceClient::update_parameter_page_property(
        KksParameterPagePluginDataProperty property,
        const KksParameterPagePluginDataPropertyValue &value) {
    return connection->send(
            connection->session().update_parameter_page_property(
                    client_value, property, value));
}

bool KksInstanceClient::update_parameter_value(
        std::uint32_t parameter_id, const OdrParameterValue &value) {
    return connection->send(connection->session().update_parameter_page_value(
            client_value, parameter_id, value));
}

bool KksInstanceClient::set_lightguide(const LightguideState &state) {
    return connection->send(
            connection->session().set_lightguide(client_value, state));
}

bool KksInstanceClient::set_plugin_chain(const OdrPluginChainData &data) {
    return connection->send(
            connection->session().set_plugin_chain(client_value, data));
}

bool KksInstanceClient::set_plugin_chain_current_index(std::int32_t index) {
    return connection->send(
            connection->session().set_plugin_chain_current_index(
                    client_value, index));
}

bool KksInstanceClient::set_mixer(const KksMixerModelTrackData &data) {
    return connection->send(
            connection->session().set_mixer_track_data(client_value, data));
}

bool KksInstanceClient::update_mixer_property(
        KksMixerProperty property, const KksMixerPropertyValue &value) {
    return connection->send(connection->session().update_mixer_property(
            client_value, property, value));
}

bool KksInstanceClient::update_mixer_track_property(std::int32_t track_index,
        KksMixerTrackProperty property,
        const KksMixerTrackPropertyValue &value) {
    return connection->send(connection->session().update_mixer_track_property(
            client_value, track_index, property, value));
}

bool KksInstanceClient::set_mixer_meters(
        std::span<const std::array<float, 2>> meters) {
    return connection->send(
            connection->session().set_mixer_meters(client_value, meters));
}

bool KksInstanceClient::set_browser(const KksBrowserData &data) {
    return connection->send(
            connection->session().set_browser_data(client_value, data));
}

bool KksInstanceClient::set_browser_settings(
        const OdrBrowserSettings &settings) {
    return connection->send(
            connection->session().set_browser_settings(client_value, settings));
}

bool KksInstanceClient::set_browser_sound_selection(
        std::optional<std::int32_t> index) {
    return connection->send(connection->session().set_browser_sound_selection(
            client_value, index));
}

bool KksInstanceClient::set_browser_filters(const KksBrowserFilters &filters) {
    return connection->send(
            connection->session().set_browser_filters(client_value, filters));
}

bool KksInstanceClient::set_browser_filter(
        OdrBrowserFilterProperty property, const KksBrowserFilter &filter) {
    return connection->send(connection->session().set_browser_filter(
            client_value, property, filter));
}

bool KksInstanceClient::set_browser_filter_selection(
        OdrBrowserFilterProperty property,
        const KksBrowserFilterSelection &selection) {
    return connection->send(connection->session().set_browser_filter_selection(
            client_value, property, selection));
}

bool KksInstanceClient::set_browser_sounds(
        const KksBrowserSoundResults &sounds) {
    return connection->send(
            connection->session().set_browser_sounds(client_value, sounds));
}

bool KksInstanceClient::set_browser_sound_item(
        std::int32_t index, const KksBrowserSoundItem &item) {
    return connection->send(connection->session().set_browser_sound_item(
            client_value, index, item));
}

bool KksInstanceClient::set_smartplay(const KksSmartplayEngineState &state) {
    return connection->send(
            connection->session().set_smartplay_data(client_value, state));
}

bool KksInstanceClient::set_smartplay_device_viewstate(
        const KksSmartplayDeviceViewState &state) {
    return connection->send(
            connection->session().set_smartplay_device_owned_viewstate(
                    client_value, state));
}

bool KksInstanceClient::set_project_tree(const KksProjectTree &tree) {
    return connection->send(
            connection->session().set_project_tree(client_value, tree));
}

bool KksInstanceClient::set_selected_track(std::uint32_t selected_track) {
    return connection->send(connection->session().set_selected_track(
            client_value, selected_track));
}

bool KksInstanceClient::set_track_orientation(
        std::optional<KksTrackOrientation> orientation) {
    return connection->send(connection->session().set_track_orientation(
            client_value, orientation));
}

bool KksInstanceClient::set_track_plugin_chain(
        const KksTrackPluginChainData &data) {
    return connection->send(
            connection->session().set_track_plugin_chain(client_value, data));
}

bool KksInstanceClient::set_track_plugin_chain_selected_plugin(
        const std::optional<std::array<std::byte, 16>> &selected_plugin_id) {
    return connection->send(
            connection->session().set_track_plugin_chain_selected_plugin(
                    client_value, selected_plugin_id));
}

bool KksInstanceClient::flush() { return connection->flush(); }

OdrSession::OdrSession(std::string client_identity)
    : OdrSession(
              std::move(client_identity), std::make_unique<OdrUnixSocket>()) {}

OdrSession::OdrSession(std::string client_identity,
        std::unique_ptr<OdrFrameTransport> transport)
    : transport(std::move(transport)),
      client_messages(std::move(client_identity)) {
    if (!this->transport)
        this->transport = std::make_unique<OdrUnixSocket>();
}

OdrConnectResult OdrSession::fail(OdrConnectError error) noexcept {
    const int system_error = transport->last_error();
    disconnect();
    return { error, system_error };
}

OdrConnectResult OdrSession::connect(
        std::string_view endpoint, const OdrInstanceInfo &client_info) {
    disconnect();
    if (!transport->connect(endpoint))
        return fail(OdrConnectError::transport_connect);
    session_state = OdrSessionState::transport_connected;

    const std::uint32_t request_id = next_request_id++;
    const auto hello = encode_odr_instance_hello(
            request_id, client_messages.client_identity(), client_info);
    if (hello.empty() || !transport->send_frame(hello))
        return fail(OdrConnectError::send);

    for (;;) {
        auto frame = transport->receive_frame();
        if (!frame)
            return fail(OdrConnectError::receive);
        auto response = decode_rpc_response(*frame);
        if (!response) {
            if (!decode_rpc_notification(*frame) && !decode_rpc_request(*frame))
                return fail(OdrConnectError::malformed_message);
            pending_frames.push_back(std::move(*frame));
            continue;
        }
        if (response->id != request_id) {
            pending_frames.push_back(std::move(*frame));
            continue;
        }
        if (!std::holds_alternative<std::monostate>(response->error.value))
            return fail(OdrConnectError::rpc_error);
        auto reply = decode_odr_handshake_reply(*response);
        if (!reply)
            return fail(OdrConnectError::invalid_handshake);

        client_messages.install_symbol_registry(
                OdrSymbolRegistry(reply->symbol_registry));
        devices = reply->available_devices;
        this->reply = std::move(reply);
        session_state = OdrSessionState::ready;
        return {};
    }
}

OdrConnectResult OdrSession::connect_with_agent_start(std::string_view endpoint,
        const OdrInstanceInfo &client_info,
        const std::function<bool()> &start_agent) {
    auto result = connect(endpoint, client_info);
    if (result || result.error != OdrConnectError::transport_connect)
        return result;
    if (!start_agent || !start_agent())
        return { OdrConnectError::agent_start, result.system_error };

    constexpr auto attempts =
            odr_agent_retry_timeout / odr_agent_retry_interval;
    for (std::int64_t attempt = 0; attempt < attempts; ++attempt) {
        std::this_thread::sleep_for(odr_agent_retry_interval);
        result = connect(endpoint, client_info);
        if (result || result.error != OdrConnectError::transport_connect)
            return result;
    }
    return result;
}

void OdrSession::disconnect() noexcept {
    transport->close();
    client_messages.clear_symbol_registry();
    reply.reset();
    devices.clear();
    pending_frames.clear();
    session_state = OdrSessionState::disconnected;
}

bool OdrSession::send(std::span<const std::byte> frame) {
    if (!ready())
        return false;
    if (transport->send_frame(frame))
        return true;
    disconnect();
    return false;
}

std::optional<std::vector<std::byte>> OdrSession::receive() {
    if (!pending_frames.empty()) {
        auto frame = std::move(pending_frames.front());
        pending_frames.pop_front();
        return frame;
    }
    if (!ready())
        return std::nullopt;
    auto frame = transport->receive_frame();
    if (!frame)
        disconnect();
    return frame;
}

std::optional<OdrIncomingEvent> OdrSession::receive_event() {
    auto frame = receive();
    if (!frame || !client_messages.symbol_registry())
        return std::nullopt;
    std::optional<OdrIncomingEvent> event;
    if (const auto notification = decode_rpc_notification(*frame)) {
        event = decode_odr_event(
                *notification, *client_messages.symbol_registry());
    }
    if (const auto request = decode_rpc_request(*frame)) {
        event = decode_odr_event(*request, *client_messages.symbol_registry());
        if (!event || !send(encode_rpc_success_response(request->id)))
            return std::nullopt;
    }
    if (!event)
        return std::nullopt;
    if (const auto *connection =
                    std::get_if<OdrDeviceConnectionEvent>(&*event)) {
        const auto serial = connection->device_info.serial_number;
        const auto existing = std::ranges::find(
                devices, serial, &OdrUsbDeviceInfo::serial_number);
        if (connection->connected) {
            if (existing == devices.end())
                devices.push_back(connection->device_info);
            else
                *existing = connection->device_info;
        } else if (existing != devices.end()) {
            devices.erase(existing);
        }
    }
    return event;
}

OdrCallResult OdrSession::call(
        std::string_view method, std::span<const MsgpackValue> parameters) {
    if (!ready())
        return { OdrCallError::not_ready, 0, {} };
    const std::uint32_t request_id = next_request_id++;
    const auto frame = client_messages.request(request_id, method, parameters);
    if (!transport->send_frame(frame)) {
        const int error = transport->last_error();
        disconnect();
        return { OdrCallError::send, error, {} };
    }

    for (;;) {
        auto incoming = transport->receive_frame();
        if (!incoming) {
            const int error = transport->last_error();
            disconnect();
            return { OdrCallError::receive, error, {} };
        }
        auto response = decode_rpc_response(*incoming);
        if (!response) {
            if (!decode_rpc_notification(*incoming) &&
                    !decode_rpc_request(*incoming)) {
                disconnect();
                return { OdrCallError::malformed_message, 0, {} };
            }
            pending_frames.push_back(std::move(*incoming));
            continue;
        }
        if (response->id != request_id) {
            pending_frames.push_back(std::move(*incoming));
            continue;
        }
        if (!std::holds_alternative<std::monostate>(response->error.value))
            return { OdrCallError::rpc_error, 0, std::move(response->result) };
        return { OdrCallError::none, 0, std::move(response->result) };
    }
}

OdrDeviceConnectResult OdrSession::connect_device(
        std::string_view device_identifier) {
    const std::array parameters{ MsgpackValue(
                                         client_messages.client_identity()),
        MsgpackValue(device_identifier) };
    auto reply = call("connect_device", parameters);
    if (!reply)
        return { reply.error, reply.system_error, false };
    const auto *connected = std::get_if<bool>(&reply.result.value);
    if (!connected)
        return { OdrCallError::invalid_result, 0, false };
    return { OdrCallError::none, 0, *connected };
}

bool OdrSession::subscribe_device_property(
        std::string_view device_identifier, std::string_view property) {
    return send(client_messages.subscribe_device_property(
            device_identifier, property));
}

bool OdrSession::subscribe_device_property(
        std::string_view device_identifier, OdrMidiDeviceProperty property) {
    if (property != OdrMidiDeviceProperty::pad_base_note)
        return false;
    return subscribe_device_property(device_identifier, "pad_base_note");
}

bool OdrSession::upload_asset(const std::array<std::byte, 32> &handle,
        std::span<const std::byte> payload) {
    return send(client_messages.upload_asset(handle, payload));
}

OdrInstanceClient OdrSession::instance(std::string instance_id) {
    return OdrInstanceClient(*this, std::move(instance_id));
}

bool OdrInstanceClient::request_focus() {
    return active_session->send(active_session->messages().request_focus(id));
}

bool OdrInstanceClient::release_focus() {
    return active_session->send(active_session->messages().release_focus(id));
}

bool OdrInstanceClient::set_page(std::uint8_t page) {
    return active_session->send(active_session->messages().set_page(id, page));
}

bool OdrInstanceClient::set_parameter_value(
        std::uint32_t parameter_id, const OdrParameterValue &value) {
    return active_session->send(active_session->messages().set_parameter_value(
            id, parameter_id, value));
}

bool OdrInstanceClient::set_parameter_page_data(
        const OdrParameterPageData &data) {
    return active_session->send(
            active_session->messages().set_parameter_page_data(id, data));
}

bool OdrInstanceClient::set_plugin_chain_data(const OdrPluginChainData &data) {
    return active_session->send(
            active_session->messages().set_plugin_chain_data(id, data));
}

bool OdrInstanceClient::set_lightguide(const LightguideState &state) {
    return active_session->send(
            active_session->messages().set_lightguide(id, state));
}

bool OdrInstanceClient::set_host_owned_state(
        const OdrHostOwnedInstanceState &state) {
    return active_session->send(
            active_session->messages().set_host_owned_instance_state(
                    id, state));
}

bool OdrInstanceClient::set_browser_data(const OdrBrowserData &data) {
    return active_session->send(
            active_session->messages().set_browser_data_model(id, data));
}

bool OdrInstanceClient::set_browser_sound_item(
        std::int32_t index, const OdrBrowserSoundItem &item) {
    return active_session->send(
            active_session->messages().set_browser_sound_item(id, index, item));
}

bool OdrInstanceClient::set_browser_sound_selection(
        std::optional<std::int32_t> index) {
    return active_session->send(
            active_session->messages().set_browser_sound_selection(id, index));
}

bool OdrInstanceClient::set_browser_sounds(
        const OdrBrowserSoundResults &sounds) {
    return active_session->send(
            active_session->messages().set_browser_sounds(id, sounds));
}

bool OdrInstanceClient::set_browser_filters(const OdrBrowserFilters &filters) {
    return active_session->send(
            active_session->messages().set_browser_filters(id, filters));
}

bool OdrInstanceClient::set_browser_filter(
        OdrBrowserFilterProperty property, const OdrBrowserFilter &filter) {
    return active_session->send(active_session->messages().set_browser_filter(
            id, property, filter));
}

bool OdrInstanceClient::set_browser_filter_selection(
        OdrBrowserFilterProperty property,
        const OdrBrowserFilterSelection &selection) {
    return active_session->send(
            active_session->messages().set_browser_filter_selection(
                    id, property, selection));
}

bool OdrInstanceClient::set_browser_settings(
        const OdrBrowserSettings &settings) {
    return active_session->send(
            active_session->messages().set_browser_settings(id, settings));
}

SamplerInstanceBackend SamplerInstanceClient::backend() const noexcept {
    return std::holds_alternative<KksInstanceClient>(client_value)
                   ? SamplerInstanceBackend::direct_kks
                   : SamplerInstanceBackend::hosted_odr;
}

bool SamplerInstanceClient::request_focus() {
    return std::visit(
            [](auto &client) {
                using Client = std::remove_cvref_t<decltype(client)>;
                if constexpr (std::is_same_v<Client, KksInstanceClient>)
                    return client.acquire_focus();
                else
                    return client.request_focus();
            },
            client_value);
}

bool SamplerInstanceClient::release_focus() {
    return std::visit(
            [](auto &client) { return client.release_focus(); }, client_value);
}

bool SamplerInstanceClient::set_page(OdrInstancePage page) {
    return std::visit(
            [page](auto &client) {
                using Client = std::remove_cvref_t<decltype(client)>;
                if constexpr (std::is_same_v<Client, KksInstanceClient>)
                    return client.set_page(page);
                else
                    return client.set_page(static_cast<std::uint8_t>(page));
            },
            client_value);
}

bool SamplerInstanceClient::publish(const PluginInfo &plugin,
        std::span<const Page> pages, std::string dawcontrol_plugin_identifier,
        float tempo) {
    auto host = make_sampler_host_owned_instance_state(
            plugin.preset, std::move(dawcontrol_plugin_identifier), tempo);
    auto chain = make_sampler_plugin_chain(plugin.name);
    return std::visit(
            [&](auto &client) {
                using Client = std::remove_cvref_t<decltype(client)>;
                if constexpr (std::is_same_v<Client, KksInstanceClient>) {
                    const auto page = make_kks_parameter_page(plugin, pages);
                    return page && client.set_parameter_page(*page) &&
                           client.set_plugin_chain(chain) &&
                           client.set_host_owned_state(host) && client.flush();
                } else {
                    const auto page = make_odr_parameter_page(plugin, pages);
                    return page && client.set_parameter_page_data(*page) &&
                           client.set_plugin_chain_data(chain) &&
                           client.set_host_owned_state(host);
                }
            },
            client_value);
}

bool SamplerInstanceClient::update_parameter_value(
        std::uint32_t parameter_id, const OdrParameterValue &value) {
    return std::visit(
            [&](auto &client) {
                using Client = std::remove_cvref_t<decltype(client)>;
                if constexpr (std::is_same_v<Client, KksInstanceClient>)
                    return client.update_parameter_value(parameter_id, value);
                else
                    return client.set_parameter_value(parameter_id, value);
            },
            client_value);
}

bool SamplerInstanceClient::set_lightguide(const LightguideState &state) {
    return std::visit(
            [&](auto &client) { return client.set_lightguide(state); },
            client_value);
}

bool SamplerInstanceClient::flush() {
    return std::visit(
            [](auto &client) {
                using Client = std::remove_cvref_t<decltype(client)>;
                if constexpr (std::is_same_v<Client, KksInstanceClient>)
                    return client.flush();
                else
                    return true;
            },
            client_value);
}

} // namespace libnks
