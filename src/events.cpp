// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "libnks/events.h"

#include <bit>
#include <cmath>
#include <limits>

namespace libnks {
namespace {

const std::string *string_at(
        const MsgpackValue::Array &values, std::size_t index) {
    if (index >= values.size())
        return nullptr;
    return std::get_if<std::string>(&values[index].value);
}

std::optional<RpcMethodId> symbol_at(
        const MsgpackValue::Array &values, std::size_t index) {
    if (const auto *text = string_at(values, index))
        return *text;
    if (index < values.size()) {
        if (const auto *number =
                        std::get_if<std::uint64_t>(&values[index].value);
                number && *number <= 0xffff) {
            return static_cast<std::uint16_t>(*number);
        }
    }
    return std::nullopt;
}

std::optional<std::uint32_t> u32(const MsgpackValue &value) {
    if (const auto *number = std::get_if<std::uint64_t>(&value.value);
            number && *number <= 0xffffffffu)
        return static_cast<std::uint32_t>(*number);
    return std::nullopt;
}

std::optional<float> number_as_float(const MsgpackValue &value) {
    if (const auto *number = std::get_if<float>(&value.value))
        return *number;
    if (const auto *number = std::get_if<double>(&value.value))
        return static_cast<float>(*number);
    if (const auto *number = std::get_if<std::uint64_t>(&value.value))
        return static_cast<float>(*number);
    if (const auto *number = std::get_if<std::int64_t>(&value.value))
        return static_cast<float>(*number);
    return std::nullopt;
}

const MsgpackValue *map_value(
        const MsgpackValue::Map &map, std::string_view key) {
    for (const auto &[candidate, value] : map) {
        const auto *text = std::get_if<std::string>(&candidate.value);
        if (text && *text == key)
            return &value;
    }
    return nullptr;
}

std::optional<std::uint8_t> u8(const MsgpackValue &value) {
    const auto number = u32(value);
    if (number && *number <= 0xff)
        return static_cast<std::uint8_t>(*number);
    return std::nullopt;
}

std::optional<std::int64_t> i64(const MsgpackValue &value) {
    if (const auto *number = std::get_if<std::int64_t>(&value.value))
        return *number;
    if (const auto *number = std::get_if<std::uint64_t>(&value.value);
            number && *number <= std::uint64_t{ 0x7fffffffffffffff })
        return static_cast<std::int64_t>(*number);
    return std::nullopt;
}

std::optional<std::int32_t> i32(const MsgpackValue &value) {
    if (const auto *number = std::get_if<std::int64_t>(&value.value)) {
        if (*number < std::numeric_limits<std::int32_t>::min())
            return std::nullopt;
        const auto truncated = static_cast<std::uint32_t>(*number);
        return std::bit_cast<std::int32_t>(truncated);
    }
    if (const auto *number = std::get_if<std::uint64_t>(&value.value);
            number &&
            *number <= static_cast<std::uint64_t>(
                               std::numeric_limits<std::int32_t>::max()))
        return static_cast<std::int32_t>(*number);
    return std::nullopt;
}

std::optional<OdrUuid> uuid(const MsgpackValue &value) {
    if (const auto *bytes = std::get_if<MsgpackValue::Binary>(&value.value)) {
        if (bytes->size() != 16)
            return std::nullopt;
        OdrUuid result{};
        std::copy(bytes->begin(), bytes->end(), result.begin());
        return result;
    }
    const auto *text = std::get_if<std::string>(&value.value);
    if (!text || text->size() != 36 || (*text)[8] != '-' ||
            (*text)[13] != '-' || (*text)[18] != '-' || (*text)[23] != '-')
        return std::nullopt;
    OdrUuid result{};
    std::size_t output = 0;
    int high = -1;
    for (const char character : *text) {
        if (character == '-')
            continue;
        int digit = -1;
        if (character >= '0' && character <= '9')
            digit = character - '0';
        else if (character >= 'a' && character <= 'f')
            digit = character - 'a' + 10;
        else if (character >= 'A' && character <= 'F')
            digit = character - 'A' + 10;
        if (digit < 0)
            return std::nullopt;
        if (high < 0)
            high = digit;
        else {
            if (output >= result.size())
                return std::nullopt;
            result[output++] = static_cast<std::byte>((high << 4) | digit);
            high = -1;
        }
    }
    if (output != result.size() || high >= 0)
        return std::nullopt;
    return result;
}

} // namespace

std::optional<OdrPadMidiEvent> decode_pad_midi_action(
        const OdrMidiPluginActionEvent &event) {
    const auto *variant = std::get_if<MsgpackValue::Map>(&event.action.value);
    if (!variant || variant->size() != 1)
        return std::nullopt;
    const auto *kind = std::get_if<std::string>(&(*variant)[0].first.value);
    const auto *fields =
            std::get_if<MsgpackValue::Map>(&(*variant)[0].second.value);
    if (!kind || *kind != "pad_midi_event" || !fields || fields->size() != 1)
        return std::nullopt;
    const auto *field = std::get_if<std::string>(&(*fields)[0].first.value);
    const auto *packet =
            std::get_if<MsgpackValue::Binary>(&(*fields)[0].second.value);
    if (!field || *field != "packet" || !packet || packet->size() != 16)
        return std::nullopt;
    OdrPadMidiEvent result;
    std::copy(packet->begin(), packet->end(), result.packet.bytes.begin());
    return result;
}

std::optional<OdrParameterAction> decode_parameter_action(
        const OdrPluginActionEvent &event) {
    const auto *variant = std::get_if<MsgpackValue::Map>(&event.action.value);
    if (!variant || variant->size() != 1)
        return std::nullopt;
    const auto *kind = std::get_if<std::string>(&(*variant)[0].first.value);
    if (!kind)
        return std::nullopt;
    if (*kind == "plugin_parameter_set_page_index") {
        const auto *address_variant =
                std::get_if<MsgpackValue::Map>(&(*variant)[0].second.value);
        if (!address_variant || address_variant->size() != 1)
            return std::nullopt;
        const auto *address_kind =
                std::get_if<std::string>(&(*address_variant)[0].first.value);
        if (!address_kind)
            return std::nullopt;
        if (*address_kind == "nks1_view_address") {
            const auto page = u8((*address_variant)[0].second);
            if (page)
                return OdrParameterPageAddressAction{ OdrNks1ViewAddress{
                        *page } };
        } else if (*address_kind == "nks2_view_address") {
            const auto *fields = std::get_if<MsgpackValue::Map>(
                    &(*address_variant)[0].second.value);
            if (!fields)
                return std::nullopt;
            const auto *section_value = map_value(*fields, "section_index");
            const auto *parameter_value = map_value(*fields, "parameter_index");
            if (!section_value || !parameter_value)
                return std::nullopt;
            const auto section = u8(*section_value);
            const auto parameter = u8(*parameter_value);
            if (!section || !parameter)
                return std::nullopt;
            std::optional<std::uint8_t> group;
            if (const auto *group_value = map_value(*fields, "group_index");
                    group_value && !std::holds_alternative<std::monostate>(
                                           group_value->value)) {
                group = u8(*group_value);
                if (!group)
                    return std::nullopt;
            }
            return OdrParameterPageAddressAction{ OdrNks2ViewAddress{
                    group, *section, *parameter } };
        }
        return std::nullopt;
    }
    const auto *fields =
            std::get_if<MsgpackValue::Array>(&(*variant)[0].second.value);
    if (!fields || fields->size() != 2)
        return std::nullopt;
    const auto id = u32((*fields)[0]);
    if (!id)
        return std::nullopt;
    if (*kind == "plugin_parameter_value_set") {
        const auto value = number_as_float((*fields)[1]);
        if (value)
            return OdrParameterValueSetAction{ *id, *value };
    } else if (*kind == "plugin_parameter_value_change") {
        const auto increment = number_as_float((*fields)[1]);
        if (increment)
            return OdrParameterValueChangeAction{ *id, *increment };
    } else if (*kind == "plugin_parameter_erp_touched") {
        if (const auto *touched = std::get_if<bool>(&(*fields)[1].value))
            return OdrParameterTouchedAction{ *id, *touched };
    }
    return std::nullopt;
}

std::optional<OdrBrowserSoundAction> decode_browser_sound_action(
        const OdrPluginActionEvent &event) {
    const auto *variant = std::get_if<MsgpackValue::Map>(&event.action.value);
    if (!variant || variant->size() != 1)
        return std::nullopt;
    const auto *kind = std::get_if<std::string>(&(*variant)[0].first.value);
    const auto value = i64((*variant)[0].second);
    if (!kind || !value)
        return std::nullopt;
    if (*kind == "browser_toggle_favorite")
        return OdrBrowserSoundAction{
            OdrBrowserSoundActionKind::toggle_favorite, *value
        };
    if (*kind == "browser_load")
        return OdrBrowserSoundAction{ OdrBrowserSoundActionKind::load, *value };
    if (*kind == "browser_load_previous_sound")
        return OdrBrowserSoundAction{ OdrBrowserSoundActionKind::previous,
            *value };
    if (*kind == "browser_load_next_sound")
        return OdrBrowserSoundAction{ OdrBrowserSoundActionKind::next, *value };
    return std::nullopt;
}

std::optional<OdrBrowserTriggerAction> decode_browser_trigger_action(
        const OdrPluginActionEvent &event) {
    const auto *variant = std::get_if<MsgpackValue::Map>(&event.action.value);
    if (!variant || variant->size() != 1 ||
            !std::holds_alternative<std::monostate>((*variant)[0].second.value))
        return std::nullopt;
    const auto *kind = std::get_if<std::string>(&(*variant)[0].first.value);
    if (!kind)
        return std::nullopt;
    if (*kind == "browser_toggle_filter_favorite_presets")
        return OdrBrowserTriggerAction{
            OdrBrowserTriggerKind::toggle_filter_favorite_presets
        };
    if (*kind == "browser_toggle_filter_user_presets")
        return OdrBrowserTriggerAction{
            OdrBrowserTriggerKind::toggle_filter_user_presets
        };
    if (*kind == "browser_quick_browse")
        return OdrBrowserTriggerAction{ OdrBrowserTriggerKind::quick_browse };
    if (*kind == "browser_toggle_speak_preset")
        return OdrBrowserTriggerAction{
            OdrBrowserTriggerKind::toggle_speak_preset
        };
    if (*kind == "browser_toggle_prehear")
        return OdrBrowserTriggerAction{ OdrBrowserTriggerKind::toggle_prehear };
    if (*kind == "browser_reset_filters")
        return OdrBrowserTriggerAction{ OdrBrowserTriggerKind::reset_filters };
    return std::nullopt;
}

std::optional<OdrBrowserNavigationAction> decode_browser_navigation_action(
        const OdrPluginActionEvent &event) {
    const auto *variant = std::get_if<MsgpackValue::Map>(&event.action.value);
    if (!variant || variant->size() != 1)
        return std::nullopt;
    const auto *kind = std::get_if<std::string>(&(*variant)[0].first.value);
    const auto increment = i32((*variant)[0].second);
    if (!kind || !increment)
        return std::nullopt;
    if (*kind == "browser_navigate_brand")
        return OdrBrowserNavigationAction{ OdrBrowserNavigationKind::brand,
            *increment };
    if (*kind == "browser_navigate_filetype")
        return OdrBrowserNavigationAction{ OdrBrowserNavigationKind::filetype,
            *increment };
    if (*kind == "browser_navigate_product")
        return OdrBrowserNavigationAction{ OdrBrowserNavigationKind::product,
            *increment };
    if (*kind == "browser_navigate_bank")
        return OdrBrowserNavigationAction{ OdrBrowserNavigationKind::bank,
            *increment };
    if (*kind == "browser_navigate_subbank")
        return OdrBrowserNavigationAction{ OdrBrowserNavigationKind::subbank,
            *increment };
    if (*kind == "browser_navigate_type")
        return OdrBrowserNavigationAction{ OdrBrowserNavigationKind::type,
            *increment };
    if (*kind == "browser_navigate_subtype")
        return OdrBrowserNavigationAction{ OdrBrowserNavigationKind::subtype,
            *increment };
    if (*kind == "browser_navigate_character")
        return OdrBrowserNavigationAction{ OdrBrowserNavigationKind::character,
            *increment };
    if (*kind == "browser_navigate_sound")
        return OdrBrowserNavigationAction{ OdrBrowserNavigationKind::sound,
            *increment };
    return std::nullopt;
}

std::optional<OdrDawAction> decode_daw_action(
        const OdrPluginActionEvent &event) {
    const auto *variant = std::get_if<MsgpackValue::Map>(&event.action.value);
    if (!variant || variant->size() != 1)
        return std::nullopt;
    const auto *kind = std::get_if<std::string>(&(*variant)[0].first.value);
    if (!kind)
        return std::nullopt;
    const auto &payload = (*variant)[0].second;
    if (*kind == "daw_set_tempo") {
        const auto tempo = number_as_float(payload);
        if (tempo)
            return OdrSetTempoAction{ *tempo };
        return std::nullopt;
    }
    if (*kind == "daw_tap_tempo") {
        const auto value = i64(payload);
        if (value)
            return OdrTapTempoAction{ *value };
        return std::nullopt;
    }
    if (!std::holds_alternative<std::monostate>(payload.value))
        return std::nullopt;
    if (*kind == "daw_play")
        return OdrDawTriggerAction{ OdrDawTriggerKind::play };
    if (*kind == "daw_restart")
        return OdrDawTriggerAction{ OdrDawTriggerKind::restart };
    if (*kind == "daw_stop")
        return OdrDawTriggerAction{ OdrDawTriggerKind::stop };
    if (*kind == "daw_record")
        return OdrDawTriggerAction{ OdrDawTriggerKind::record };
    if (*kind == "daw_metro")
        return OdrDawTriggerAction{ OdrDawTriggerKind::metro };
    if (*kind == "daw_loop")
        return OdrDawTriggerAction{ OdrDawTriggerKind::loop };
    if (*kind == "daw_count_in")
        return OdrDawTriggerAction{ OdrDawTriggerKind::count_in };
    if (*kind == "daw_undo")
        return OdrDawTriggerAction{ OdrDawTriggerKind::undo };
    if (*kind == "daw_redo")
        return OdrDawTriggerAction{ OdrDawTriggerKind::redo };
    if (*kind == "daw_quantize")
        return OdrDawTriggerAction{ OdrDawTriggerKind::quantize };
    if (*kind == "daw_quantize_half")
        return OdrDawTriggerAction{ OdrDawTriggerKind::quantize_half };
    return std::nullopt;
}

std::optional<OdrKeyboardAction> decode_keyboard_action(
        const OdrPluginActionEvent &event) {
    const auto *variant = std::get_if<MsgpackValue::Map>(&event.action.value);
    if (!variant || variant->size() != 1)
        return std::nullopt;
    const auto *kind = std::get_if<std::string>(&(*variant)[0].first.value);
    if (!kind)
        return std::nullopt;
    const auto &payload = (*variant)[0].second;
    if (*kind == "daw_octave_change") {
        const auto value = i32(payload);
        if (value)
            return OdrOctaveChangeAction{ *value };
        return std::nullopt;
    }
    if (*kind != "daw_set_host_pad_mode")
        return std::nullopt;
    const auto *tuple = std::get_if<MsgpackValue::Array>(&payload.value);
    if (!tuple || tuple->size() != 1)
        return std::nullopt;
    const auto *mode = std::get_if<std::string>(&(*tuple)[0].value);
    if (!mode)
        return std::nullopt;
    if (*mode == "pad_mode")
        return OdrSetHostPadModeAction{ OdrHostPadMode::pad };
    if (*mode == "keyboard_mode")
        return OdrSetHostPadModeAction{ OdrHostPadMode::keyboard };
    return std::nullopt;
}

std::optional<OdrTrackAction> decode_track_action(
        const OdrPluginActionEvent &event) {
    const auto *variant = std::get_if<MsgpackValue::Map>(&event.action.value);
    if (!variant || variant->size() != 1)
        return std::nullopt;
    const auto *kind = std::get_if<std::string>(&(*variant)[0].first.value);
    if (!kind)
        return std::nullopt;
    const auto &payload = (*variant)[0].second;

    const auto node = u32(payload);
    if (*kind == "daw_toggle_node_collapsed" && node)
        return OdrTrackNodeAction{ OdrTrackNodeActionKind::toggle_collapsed,
            *node };
    if (*kind == "daw_select_track" && node)
        return OdrTrackNodeAction{ OdrTrackNodeActionKind::select, *node };
    if (*kind == "daw_mute_track" && node)
        return OdrTrackNodeAction{ OdrTrackNodeActionKind::mute, *node };
    if (*kind == "daw_solo_track" && node)
        return OdrTrackNodeAction{ OdrTrackNodeActionKind::solo, *node };

    if (*kind == "daw_navigate_track" || *kind == "daw_navigate_track_depth") {
        const auto *tuple = std::get_if<MsgpackValue::Array>(&payload.value);
        if (!tuple || tuple->size() != 1)
            return std::nullopt;
        const auto *direction = std::get_if<std::string>(&(*tuple)[0].value);
        if (!direction)
            return std::nullopt;
        OdrNavigationDirection decoded;
        if (*direction == "forward")
            decoded = OdrNavigationDirection::forward;
        else if (*direction == "backward")
            decoded = OdrNavigationDirection::backward;
        else
            return std::nullopt;
        return OdrTrackNavigationAction{
            *kind == "daw_navigate_track" ? OdrTrackNavigationKind::track
                                          : OdrTrackNavigationKind::depth,
            decoded
        };
    }

    if (*kind == "daw_track_volume_change" || *kind == "daw_track_pan_change") {
        const auto *tuple = std::get_if<MsgpackValue::Array>(&payload.value);
        if (!tuple || tuple->size() != 2)
            return std::nullopt;
        const auto track = u32((*tuple)[0]);
        const auto increment = number_as_float((*tuple)[1]);
        if (!track || !increment)
            return std::nullopt;
        return OdrTrackContinuousAction{
            *kind == "daw_track_volume_change" ? OdrTrackContinuousKind::volume
                                               : OdrTrackContinuousKind::pan,
            *track, *increment
        };
    }

    if (*kind == "daw_select_host_plugin" ||
            *kind == "daw_toggle_plugin_container") {
        const auto id = uuid(payload);
        if (!id)
            return std::nullopt;
        return OdrHostPluginAction{
            *kind == "daw_select_host_plugin"
                    ? OdrHostPluginActionKind::select
                    : OdrHostPluginActionKind::toggle_container,
            *id
        };
    }
    return std::nullopt;
}

std::optional<OdrPluginChainAction> decode_plugin_chain_action(
        const OdrPluginActionEvent &event) {
    const auto *variant = std::get_if<MsgpackValue::Map>(&event.action.value);
    if (!variant || variant->size() != 1)
        return std::nullopt;
    const auto *kind = std::get_if<std::string>(&(*variant)[0].first.value);
    if (!kind)
        return std::nullopt;
    const auto &payload = (*variant)[0].second;
    if (*kind == "plugin_chain_navigate") {
        const auto increment = i32(payload);
        if (increment)
            return OdrPluginChainNavigateAction{ *increment };
        return std::nullopt;
    }
    if (*kind == "plugin_chain_entry_move") {
        const auto *tuple = std::get_if<MsgpackValue::Array>(&payload.value);
        if (!tuple || tuple->size() != 2)
            return std::nullopt;
        const auto amount = i32((*tuple)[0]);
        const auto identifier = uuid((*tuple)[1]);
        if (amount && identifier)
            return OdrPluginChainMoveAction{ *amount, *identifier };
        return std::nullopt;
    }
    if (*kind == "plugin_chain_entry_bypass" ||
            *kind == "plugin_chain_entry_remove") {
        const auto identifier = uuid(payload);
        if (!identifier)
            return std::nullopt;
        return OdrPluginChainEntryAction{
            *kind == "plugin_chain_entry_bypass"
                    ? OdrPluginChainEntryActionKind::bypass
                    : OdrPluginChainEntryActionKind::remove,
            *identifier
        };
    }
    if (*kind == "master_volume_change") {
        const auto increment = number_as_float(payload);
        if (increment)
            return OdrMasterVolumeChangeAction{ *increment };
    }
    return std::nullopt;
}

std::optional<OdrSmartplayAction> decode_smartplay_action(
        const OdrPluginActionEvent &event) {
    const auto *variant = std::get_if<MsgpackValue::Map>(&event.action.value);
    if (!variant || variant->size() != 1)
        return std::nullopt;
    const auto *kind = std::get_if<std::string>(&(*variant)[0].first.value);
    const auto *tuple =
            std::get_if<MsgpackValue::Array>(&(*variant)[0].second.value);
    if (!kind || !tuple)
        return std::nullopt;

    const auto arp_change_type =
            [](std::string_view name) -> std::optional<OdrArpEngineChangeType> {
        static constexpr std::array names{ "enabled", "engine_mode",
            "arp_type_selected", "arp_rate_selected", "arp_sequence_selected",
            "arp_swing_change_by", "arp_octaves_selected",
            "arp_dynamic_change_by", "arp_gate_change_by",
            "arp_retrigger_selected", "arp_repeat_selected",
            "arp_offset_selected", "arp_inversion_selected",
            "arp_min_key_change", "arp_max_key_change", "arp_hold_changed",
            "note_repeat_rate_selected", "note_repeat_swing_change_by",
            "note_repeat_gate_change_by", "note_repeat_hold_changed" };
        for (std::size_t index = 0; index < names.size(); ++index)
            if (name == names[index])
                return static_cast<OdrArpEngineChangeType>(index);
        return std::nullopt;
    };
    const auto scale_change_type = [](std::string_view name)
            -> std::optional<OdrScaleEngineChangeType> {
        static constexpr std::array names{ "enabled", "position_selected",
            "rootnote_selected", "scale_mode_selected", "key_mapping_selected",
            "scale_bank_selected", "scale_type_selected", "type_selected" };
        for (std::size_t index = 0; index < names.size(); ++index)
            if (name == names[index])
                return static_cast<OdrScaleEngineChangeType>(index);
        return std::nullopt;
    };
    const auto arp_trigger_type = [](std::string_view name)
            -> std::optional<OdrArpEngineTriggerType> {
        static constexpr std::array names{ "arp_shortcut_0_triggered",
            "arp_shortcut_1_triggered", "arp_shortcut_2_triggered",
            "arp_shortcut_3_triggered", "arp_gate_reset_triggered" };
        for (std::size_t index = 0; index < names.size(); ++index)
            if (name == names[index])
                return static_cast<OdrArpEngineTriggerType>(index);
        return std::nullopt;
    };

    if (*kind == "smartplay_arp_engine_change" ||
            *kind == "smartplay_scale_engine_change") {
        if (tuple->size() != 2)
            return std::nullopt;
        const auto *type_name = std::get_if<std::string>(&(*tuple)[0].value);
        const auto value = decode_device_property_value((*tuple)[1]);
        if (!type_name || !value)
            return std::nullopt;
        if (*kind == "smartplay_arp_engine_change") {
            const auto type = arp_change_type(*type_name);
            if (type)
                return OdrArpEngineChangeAction{ *type, *value };
        } else {
            const auto type = scale_change_type(*type_name);
            if (type)
                return OdrScaleEngineChangeAction{ *type, *value };
        }
        return std::nullopt;
    }
    if (*kind == "smartplay_arp_engine_trigger") {
        if (tuple->size() != 1)
            return std::nullopt;
        const auto *type_name = std::get_if<std::string>(&(*tuple)[0].value);
        if (!type_name)
            return std::nullopt;
        const auto type = arp_trigger_type(*type_name);
        if (type)
            return OdrArpEngineTriggerAction{ *type };
        return std::nullopt;
    }
    if (*kind != "smartplay_device_owned_viewstate_changed" ||
            tuple->size() != 1)
        return std::nullopt;
    const auto *fields = std::get_if<MsgpackValue::Map>(&(*tuple)[0].value);
    if (!fields)
        return std::nullopt;
    OdrSmartplayViewFocus focus = OdrSmartplayViewFocus::arpeggiator;
    std::uint8_t page = 0;
    bool have_focus = false;
    bool have_page = false;
    for (const auto &[field, value] : *fields) {
        const auto *name = std::get_if<std::string>(&field.value);
        if (!name)
            continue;
        if (*name == "focus" && !have_focus) {
            const auto *focus_name = std::get_if<std::string>(&value.value);
            if (!focus_name)
                return std::nullopt;
            if (*focus_name == "arpeggiator")
                focus = OdrSmartplayViewFocus::arpeggiator;
            else if (*focus_name == "scale")
                focus = OdrSmartplayViewFocus::scale;
            else
                return std::nullopt;
            have_focus = true;
        } else if (*name == "page" && !have_page) {
            const auto decoded = u8(value);
            if (!decoded)
                return std::nullopt;
            page = *decoded;
            have_page = true;
        }
    }
    // we accept missing focus/page fields and ignore names we don't recognize
    return OdrSmartplayViewstateChangedAction{ focus, page };
}

std::optional<OdrDecodedPluginAction> decode_plugin_action(
        const OdrPluginActionEvent &event) {
    if (auto action = decode_parameter_action(event))
        return std::move(*action);
    if (auto action = decode_browser_sound_action(event))
        return std::move(*action);
    if (auto action = decode_browser_trigger_action(event))
        return std::move(*action);
    if (auto action = decode_browser_navigation_action(event))
        return std::move(*action);
    if (auto action = decode_daw_action(event))
        return std::move(*action);
    if (auto action = decode_keyboard_action(event))
        return std::move(*action);
    if (auto action = decode_track_action(event))
        return std::move(*action);
    if (auto action = decode_plugin_chain_action(event))
        return std::move(*action);
    if (auto action = decode_smartplay_action(event))
        return std::move(*action);
    return std::nullopt;
}

std::optional<OdrDecodedPluginAction> decode_kks_plugin_action(
        const KksPluginAction &event) {
    return decode_plugin_action(
            OdrPluginActionEvent{ "", event.action, false });
}

std::optional<OdrPadMidiEvent> decode_kks_pad_midi_action(
        const KksMidiPluginAction &event) {
    return decode_pad_midi_action(
            OdrMidiPluginActionEvent{ "", event.action, false });
}

std::optional<OdrIncomingEvent> decode_odr_event(
        const RpcNotification &notification) {
    const auto *method = std::get_if<std::string>(&notification.method);
    if (!method)
        return std::nullopt;
    const auto &p = notification.parameters;

    if (*method == "device_connected" || *method == "device_disconnected") {
        if (p.size() != 1)
            return std::nullopt;
        auto device = decode_odr_usb_device_info(p[0]);
        if (!device)
            return std::nullopt;
        return OdrDeviceConnectionEvent{ *method == "device_connected",
            std::move(*device) };
    }
    if (*method == "midi_plugin_action") {
        const auto *device = string_at(p, 0);
        const auto *flag =
                p.size() == 3 ? std::get_if<bool>(&p[2].value) : nullptr;
        if (!device || !flag)
            return std::nullopt;
        return OdrMidiPluginActionEvent{ *device, p[1], *flag };
    }
    if (*method == "plugin_action") {
        const auto *instance = string_at(p, 0);
        const auto *flag =
                p.size() == 3 ? std::get_if<bool>(&p[2].value) : nullptr;
        if (!instance || !flag)
            return std::nullopt;
        return OdrPluginActionEvent{ *instance, p[1], *flag };
    }
    if (*method == "active_instance_client_changed") {
        const auto *device = string_at(p, 0);
        if (!device || p.size() != 2)
            return std::nullopt;
        if (std::holds_alternative<std::monostate>(p[1].value))
            return OdrActiveInstanceChangedEvent{ *device, std::nullopt };
        const auto *instance = string_at(p, 1);
        if (!instance)
            return std::nullopt;
        return OdrActiveInstanceChangedEvent{ *device, *instance };
    }
    if (*method == "focus_changed") {
        const auto *device = string_at(p, 0);
        const auto *instance = string_at(p, 1);
        if (!device || !instance || p.size() != 2)
            return std::nullopt;
        return OdrFocusChangedEvent{ *device, *instance };
    }
    if (*method == "device_properties") {
        const auto *device = string_at(p, 0);
        const auto *map = p.size() == 2
                                  ? std::get_if<MsgpackValue::Map>(&p[1].value)
                                  : nullptr;
        if (!device || !map)
            return std::nullopt;
        OdrDevicePropertyMap properties;
        for (const auto &[key, raw_value] : *map) {
            const auto *name = std::get_if<std::string>(&key.value);
            auto value = decode_device_property_value(raw_value);
            if (!name || !value ||
                    !properties.emplace(*name, std::move(*value)).second)
                return std::nullopt;
        }
        return OdrDevicePropertiesEvent{ *device, std::move(properties) };
    }
    if (*method == "device_property_changed") {
        const auto *device = string_at(p, 0);
        const auto property = symbol_at(p, 1);
        if (!device || !property || p.size() != 3)
            return std::nullopt;
        auto value = decode_device_property_value(p[2]);
        if (!value)
            return std::nullopt;
        return OdrDevicePropertyChangedEvent{ *device, *property,
            std::move(*value) };
    }
    if (*method == "firmware_update_available") {
        const auto *first = string_at(p, 0);
        const auto *second = string_at(p, 1);
        if (!first || !second || p.size() != 2)
            return std::nullopt;
        return OdrFirmwareUpdateAvailableEvent{ *first, *second };
    }
    if (*method == "device_owned_instance_client_state_changed") {
        const auto *instance = string_at(p, 0);
        const auto *fields =
                p.size() == 2 ? std::get_if<MsgpackValue::Map>(&p[1].value)
                              : nullptr;
        if (!instance || !fields)
            return std::nullopt;
        OdrDeviceOwnedInstanceState state;
        bool have_page = false;
        bool have_focus = false;
        for (const auto &[field, value] : *fields) {
            const auto *name = std::get_if<std::string>(&field.value);
            if (!name)
                continue;
            if (*name == "page" && !have_page) {
                const auto *page = std::get_if<std::string>(&value.value);
                if (!page)
                    return std::nullopt;
                if (*page == "browser")
                    state.page = OdrInstancePage::browser;
                else if (*page == "mixer")
                    state.page = OdrInstancePage::mixer;
                else if (*page == "parameter")
                    state.page = OdrInstancePage::parameter;
                else if (*page == "smartplay")
                    state.page = OdrInstancePage::smartplay;
                else
                    return std::nullopt;
                have_page = true;
            } else if (*name == "has_lightguide_focus" && !have_focus) {
                const auto *focused = std::get_if<bool>(&value.value);
                if (!focused)
                    return std::nullopt;
                state.has_lightguide_focus = *focused;
                have_focus = true;
            }
        }
        return OdrDeviceOwnedStateChangedEvent{ *instance, state };
    }
    return std::nullopt;
}

std::optional<OdrDevicePropertyValue> decode_device_property_value(
        const MsgpackValue &value) {
    if (std::holds_alternative<std::monostate>(value.value))
        return OdrDevicePropertyValue(std::monostate{});
    if (const auto *boolean = std::get_if<bool>(&value.value))
        return OdrDevicePropertyValue(*boolean);
    if (const auto *number = std::get_if<float>(&value.value))
        return OdrDevicePropertyValue(*number);
    if (const auto *number = std::get_if<double>(&value.value))
        return OdrDevicePropertyValue(static_cast<float>(*number));
    if (std::holds_alternative<std::int64_t>(value.value) ||
            std::holds_alternative<std::uint64_t>(value.value)) {
        const auto integer = i32(value);
        if (!integer)
            return std::nullopt;
        return OdrDevicePropertyValue(*integer);
    }
    if (const auto *text = std::get_if<std::string>(&value.value))
        return OdrDevicePropertyValue(*text);
    return std::nullopt;
}

std::optional<OdrIncomingEvent> decode_odr_event(const RpcRequest &request) {
    return decode_odr_event(
            RpcNotification{ request.method, request.parameters });
}

namespace {

void expand_map_symbols(
        MsgpackValue &value, const OdrSymbolRegistry &registry) {
    if (auto *array = std::get_if<MsgpackValue::Array>(&value.value)) {
        for (auto &child : *array)
            expand_map_symbols(child, registry);
        return;
    }
    auto *map = std::get_if<MsgpackValue::Map>(&value.value);
    if (!map)
        return;
    for (auto &[key, child] : *map) {
        if (const auto *index = std::get_if<std::uint64_t>(&key.value);
                index && *index <= 0xffff) {
            if (const auto symbol =
                            registry.symbol(static_cast<std::uint16_t>(*index)))
                key = MsgpackValue(*symbol);
        }
        expand_map_symbols(child, registry);
    }
}

void expand_plugin_action_symbol_values(
        MsgpackValue &action, const OdrSymbolRegistry &registry) {
    auto *variant = std::get_if<MsgpackValue::Map>(&action.value);
    if (!variant || variant->size() != 1)
        return;
    const auto *kind = std::get_if<std::string>(&(*variant)[0].first.value);
    if (!kind)
        return;
    auto *tuple = std::get_if<MsgpackValue::Array>(&(*variant)[0].second.value);
    if (!tuple || tuple->empty())
        return;
    const auto resolve = [&registry](MsgpackValue &value) {
        const auto *index = std::get_if<std::uint64_t>(&value.value);
        if (index && *index <= 0xffff) {
            if (const auto symbol =
                            registry.symbol(static_cast<std::uint16_t>(*index)))
                value = MsgpackValue(*symbol);
        }
    };
    if (*kind == "daw_set_host_pad_mode" || *kind == "daw_navigate_track" ||
            *kind == "daw_navigate_track_depth" ||
            *kind == "smartplay_arp_engine_change" ||
            *kind == "smartplay_arp_engine_trigger" ||
            *kind == "smartplay_scale_engine_change") {
        resolve((*tuple)[0]);
        return;
    }
    if (*kind != "smartplay_device_owned_viewstate_changed" ||
            tuple->size() != 1)
        return;
    auto *fields = std::get_if<MsgpackValue::Map>(&(*tuple)[0].value);
    if (!fields)
        return;
    for (auto &[field, value] : *fields) {
        const auto *name = std::get_if<std::string>(&field.value);
        if (name && *name == "focus")
            resolve(value);
    }
}

void expand_device_owned_state_symbol_values(
        MsgpackValue &state, const OdrSymbolRegistry &registry) {
    auto *fields = std::get_if<MsgpackValue::Map>(&state.value);
    if (!fields)
        return;
    for (auto &[field, value] : *fields) {
        const auto *name = std::get_if<std::string>(&field.value);
        if (!name || *name != "page")
            continue;
        if (const auto *index = std::get_if<std::uint64_t>(&value.value);
                index && *index <= 0xffff) {
            if (const auto symbol =
                            registry.symbol(static_cast<std::uint16_t>(*index)))
                value = MsgpackValue(*symbol);
        }
        return;
    }
}

std::optional<std::string> resolved_method(
        const RpcMethodId &method, const OdrSymbolRegistry &registry) {
    if (const auto *text = std::get_if<std::string>(&method))
        return *text;
    const auto index = std::get<std::uint16_t>(method);
    if (const auto symbol = registry.symbol(index))
        return std::string(*symbol);
    return std::nullopt;
}

} // namespace

std::optional<OdrIncomingEvent> decode_odr_event(
        const RpcNotification &notification,
        const OdrSymbolRegistry &registry) {
    auto method = resolved_method(notification.method, registry);
    if (!method)
        return std::nullopt;
    RpcNotification expanded{ std::move(*method), notification.parameters };
    for (auto &parameter : expanded.parameters)
        expand_map_symbols(parameter, registry);
    if (std::get<std::string>(expanded.method) == "plugin_action" &&
            expanded.parameters.size() == 3)
        expand_plugin_action_symbol_values(expanded.parameters[1], registry);
    if (std::get<std::string>(expanded.method) ==
                    "device_owned_instance_client_state_changed" &&
            expanded.parameters.size() == 2)
        expand_device_owned_state_symbol_values(
                expanded.parameters[1], registry);
    auto event = decode_odr_event(expanded);
    if (event) {
        if (auto *changed =
                        std::get_if<OdrDevicePropertyChangedEvent>(&*event)) {
            if (const auto *index =
                            std::get_if<std::uint16_t>(&changed->property)) {
                if (const auto symbol = registry.symbol(*index))
                    changed->property = std::string(*symbol);
            }
        }
    }
    return event;
}

std::optional<OdrIncomingEvent> decode_odr_event(
        const RpcRequest &request, const OdrSymbolRegistry &registry) {
    return decode_odr_event(
            RpcNotification{ request.method, request.parameters }, registry);
}

} // namespace libnks
