// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "libnks/wire.h"

#include <map>

namespace libnks {

struct OdrDeviceConnectionEvent {
    bool connected{};
    OdrUsbDeviceInfo device_info;
};
struct OdrMidiPluginActionEvent {
    std::string device_id;
    MsgpackValue action;
    bool auxiliary_flag{};
};
struct OdrUniversalMidiPacket {
    std::array<std::byte, 16> bytes{};
};
struct OdrPadMidiEvent {
    OdrUniversalMidiPacket packet;
};
// keep all 16 bytes of pad_midi_event; the MIDI layer interprets the UMP type
[[nodiscard]] std::optional<OdrPadMidiEvent> decode_pad_midi_action(
        const OdrMidiPluginActionEvent &event);
[[nodiscard]] std::optional<OdrPadMidiEvent> decode_kks_pad_midi_action(
        const KksMidiPluginAction &event);
struct OdrPluginActionEvent {
    std::string instance_id;
    MsgpackValue action;
    bool auxiliary_flag{};
};

struct OdrParameterValueSetAction {
    std::uint32_t parameter_id{};
    float value{};
};
struct OdrParameterValueChangeAction {
    std::uint32_t parameter_id{};
    float increment{};
};
struct OdrParameterTouchedAction {
    std::uint32_t parameter_id{};
    bool touched{};
};
struct OdrParameterPageAddressAction {
    std::variant<OdrNks1ViewAddress, OdrNks2ViewAddress> address;
};
using OdrParameterAction =
        std::variant<OdrParameterValueSetAction, OdrParameterValueChangeAction,
                OdrParameterTouchedAction, OdrParameterPageAddressAction>;

[[nodiscard]] std::optional<OdrParameterAction> decode_parameter_action(
        const OdrPluginActionEvent &event);

enum class OdrBrowserSoundActionKind : std::uint8_t {
    toggle_favorite,
    load,
    previous,
    next
};
struct OdrBrowserSoundAction {
    OdrBrowserSoundActionKind kind{};
    std::int64_t value{};
};
[[nodiscard]] std::optional<OdrBrowserSoundAction> decode_browser_sound_action(
        const OdrPluginActionEvent &event);

enum class OdrBrowserTriggerKind : std::uint8_t {
    toggle_filter_favorite_presets,
    toggle_filter_user_presets,
    quick_browse,
    toggle_speak_preset,
    toggle_prehear,
    reset_filters
};
struct OdrBrowserTriggerAction {
    OdrBrowserTriggerKind kind{};
};
[[nodiscard]] std::optional<OdrBrowserTriggerAction>
decode_browser_trigger_action(const OdrPluginActionEvent &event);

enum class OdrBrowserNavigationKind : std::uint8_t {
    brand,
    filetype,
    product,
    bank,
    subbank,
    type,
    subtype,
    character,
    sound
};
struct OdrBrowserNavigationAction {
    OdrBrowserNavigationKind kind{};
    std::int32_t increment{};
};
[[nodiscard]] std::optional<OdrBrowserNavigationAction>
decode_browser_navigation_action(const OdrPluginActionEvent &event);

struct OdrSetTempoAction {
    float tempo{};
};
struct OdrTapTempoAction {
    std::int64_t value{};
};
enum class OdrDawTriggerKind : std::uint8_t {
    play,
    restart,
    stop,
    record,
    metro,
    loop,
    count_in,
    undo,
    redo,
    quantize,
    quantize_half
};
struct OdrDawTriggerAction {
    OdrDawTriggerKind kind{};
};
using OdrDawAction =
        std::variant<OdrSetTempoAction, OdrTapTempoAction, OdrDawTriggerAction>;

// tap_tempo's integer meaning is still unknown, so keep the wire value intact
[[nodiscard]] std::optional<OdrDawAction> decode_daw_action(
        const OdrPluginActionEvent &event);

enum class OdrHostPadMode : std::uint8_t { pad, keyboard };
struct OdrSetHostPadModeAction {
    OdrHostPadMode mode{};
};
struct OdrOctaveChangeAction {
    std::int32_t value{};
};
using OdrKeyboardAction =
        std::variant<OdrSetHostPadModeAction, OdrOctaveChangeAction>;
[[nodiscard]] std::optional<OdrKeyboardAction> decode_keyboard_action(
        const OdrPluginActionEvent &event);

using OdrUuid = std::array<std::byte, 16>;
enum class OdrTrackNodeActionKind : std::uint8_t {
    toggle_collapsed,
    select,
    mute,
    solo
};
struct OdrTrackNodeAction {
    OdrTrackNodeActionKind kind{};
    std::uint32_t track_id{};
};
enum class OdrNavigationDirection : std::uint8_t { forward, backward };
enum class OdrTrackNavigationKind : std::uint8_t { track, depth };
struct OdrTrackNavigationAction {
    OdrTrackNavigationKind kind{};
    OdrNavigationDirection direction{};
};
enum class OdrTrackContinuousKind : std::uint8_t { volume, pan };
struct OdrTrackContinuousAction {
    OdrTrackContinuousKind kind{};
    std::uint32_t track_id{};
    float increment{};
};
enum class OdrHostPluginActionKind : std::uint8_t { select, toggle_container };
struct OdrHostPluginAction {
    OdrHostPluginActionKind kind{};
    OdrUuid plugin_id{};
};
using OdrTrackAction =
        std::variant<OdrTrackNodeAction, OdrTrackNavigationAction,
                OdrTrackContinuousAction, OdrHostPluginAction>;
[[nodiscard]] std::optional<OdrTrackAction> decode_track_action(
        const OdrPluginActionEvent &event);

struct OdrPluginChainNavigateAction {
    std::int32_t increment{};
};
struct OdrPluginChainMoveAction {
    std::int32_t amount{};
    OdrUuid identifier{};
};
enum class OdrPluginChainEntryActionKind : std::uint8_t { bypass, remove };
struct OdrPluginChainEntryAction {
    OdrPluginChainEntryActionKind kind{};
    OdrUuid identifier{};
};
struct OdrMasterVolumeChangeAction {
    float increment{};
};
using OdrPluginChainAction =
        std::variant<OdrPluginChainNavigateAction, OdrPluginChainMoveAction,
                OdrPluginChainEntryAction, OdrMasterVolumeChangeAction>;
[[nodiscard]] std::optional<OdrPluginChainAction> decode_plugin_chain_action(
        const OdrPluginActionEvent &event);

enum class OdrArpEngineChangeType : std::uint8_t {
    enabled = 0,
    engine_mode = 1,
    arp_type_selected = 2,
    arp_rate_selected = 3,
    arp_sequence_selected = 4,
    arp_swing_change_by = 5,
    arp_octaves_selected = 6,
    arp_dynamic_change_by = 7,
    arp_gate_change_by = 8,
    arp_retrigger_selected = 9,
    arp_repeat_selected = 10,
    arp_offset_selected = 11,
    arp_inversion_selected = 12,
    arp_min_key_change = 13,
    arp_max_key_change = 14,
    arp_hold_changed = 15,
    note_repeat_rate_selected = 16,
    note_repeat_swing_change_by = 17,
    note_repeat_gate_change_by = 18,
    note_repeat_hold_changed = 19
};
enum class OdrArpEngineTriggerType : std::uint8_t {
    arp_shortcut_0_triggered = 0,
    arp_shortcut_1_triggered = 1,
    arp_shortcut_2_triggered = 2,
    arp_shortcut_3_triggered = 3,
    arp_gate_reset_triggered = 4
};
enum class OdrScaleEngineChangeType : std::uint8_t {
    enabled = 0,
    position_selected = 1,
    rootnote_selected = 2,
    scale_mode_selected = 3,
    key_mapping_selected = 4,
    scale_bank_selected = 5,
    scale_type_selected = 6,
    type_selected = 7
};
using OdrSmartplayValue =
        std::variant<std::monostate, bool, float, std::int32_t, std::string>;
struct OdrArpEngineChangeAction {
    OdrArpEngineChangeType type{};
    OdrSmartplayValue value;
};
struct OdrArpEngineTriggerAction {
    OdrArpEngineTriggerType type{};
};
struct OdrScaleEngineChangeAction {
    OdrScaleEngineChangeType type{};
    OdrSmartplayValue value;
};
struct OdrSmartplayViewstateChangedAction {
    OdrSmartplayViewFocus focus{};
    std::uint8_t page{};
};
using OdrSmartplayAction =
        std::variant<OdrArpEngineChangeAction, OdrArpEngineTriggerAction,
                OdrScaleEngineChangeAction, OdrSmartplayViewstateChangedAction>;
[[nodiscard]] std::optional<OdrSmartplayAction> decode_smartplay_action(
        const OdrPluginActionEvent &event);

using OdrDecodedPluginAction = std::variant<OdrParameterAction,
        OdrBrowserSoundAction, OdrBrowserTriggerAction,
        OdrBrowserNavigationAction, OdrDawAction, OdrKeyboardAction,
        OdrTrackAction, OdrPluginChainAction, OdrSmartplayAction>;
[[nodiscard]] std::optional<OdrDecodedPluginAction> decode_plugin_action(
        const OdrPluginActionEvent &event);
[[nodiscard]] std::optional<OdrDecodedPluginAction> decode_kks_plugin_action(
        const KksPluginAction &event);

// we don't know what most of these do yet, keep them opaque
enum class OdrPluginActionKind : std::uint8_t {
    toggle_filter_favorite_presets,
    toggle_filter_user_presets,
    toggle_favorite,
    quick_browse,
    toggle_speak_preset,
    toggle_prehear,
    load,
    load_previous_sound,
    load_next_sound,
    reset_filters,
    navigate_brand,
    navigate_filetype,
    navigate_product,
    navigate_bank,
    navigate_subbank,
    navigate_type,
    navigate_subtype,
    navigate_character,
    navigate_sound,
    parameter_value_set,
    parameter_value_change,
    parameter_erp_touched,
    parameter_page_view_address_change,
    set_tempo,
    tap_tempo,
    play,
    restart,
    stop,
    record,
    metro,
    loop,
    count_in,
    undo,
    redo,
    quantize,
    quantize_half,
    set_host_pad_mode,
    toggle_node_collapsed,
    select_track,
    navigate_track,
    navigate_track_depth,
    mute_track,
    solo_track,
    track_volume_change,
    track_pan_change,
    select_host_plugin,
    toggle_plugin_container,
    octave_change,
    plugin_chain_navigate,
    plugin_chain_entry_move,
    plugin_chain_entry_bypass,
    plugin_chain_entry_remove,
    master_volume_change,
    arp_engine_change,
    arp_engine_trigger,
    scale_engine_change,
    smartplay_device_owned_viewstate_changed
};
struct OdrActiveInstanceChangedEvent {
    std::string device_id;
    std::optional<std::string> instance_id;
};
struct OdrFocusChangedEvent {
    std::string device_id;
    std::string instance_id;
};
using OdrDevicePropertyValue =
        std::variant<std::monostate, bool, float, std::int32_t, std::string>;
using OdrDevicePropertyMap = std::map<std::string, OdrDevicePropertyValue>;
struct OdrDevicePropertiesEvent {
    std::string device_id;
    OdrDevicePropertyMap properties;
};
struct OdrDevicePropertyChangedEvent {
    std::string device_id;
    RpcMethodId property;
    OdrDevicePropertyValue value;
};
[[nodiscard]] std::optional<OdrDevicePropertyValue>
decode_device_property_value(const MsgpackValue &value);
struct OdrFirmwareUpdateAvailableEvent {
    std::string first;
    std::string second;
};
struct OdrDeviceOwnedStateChangedEvent {
    std::string instance_id;
    OdrDeviceOwnedInstanceState state;
};

using OdrIncomingEvent = std::variant<OdrDeviceConnectionEvent,
        OdrMidiPluginActionEvent, OdrPluginActionEvent,
        OdrActiveInstanceChangedEvent, OdrFocusChangedEvent,
        OdrDevicePropertiesEvent, OdrDevicePropertyChangedEvent,
        OdrFirmwareUpdateAvailableEvent, OdrDeviceOwnedStateChangedEvent>;

// unknown nested models stay MsgpackValue
[[nodiscard]] std::optional<OdrIncomingEvent> decode_odr_event(
        const RpcNotification &notification);
[[nodiscard]] std::optional<OdrIncomingEvent> decode_odr_event(
        const RpcRequest &request);
[[nodiscard]] std::optional<OdrIncomingEvent> decode_odr_event(
        const RpcNotification &notification, const OdrSymbolRegistry &registry);
[[nodiscard]] std::optional<OdrIncomingEvent> decode_odr_event(
        const RpcRequest &request, const OdrSymbolRegistry &registry);

} // namespace libnks
