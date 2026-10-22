// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace libnks {

enum class Generation : std::uint8_t { nks1 = 1, nks2 = 2 };

// raw bytes since we don't know all of them, ParameterKind and
// ParameterDisplayStyle name the ones we do
struct ParameterAttributes {
    std::uint8_t type{};
    std::uint8_t display_style{};
    std::uint32_t value_count{};
    std::vector<std::string> values;
};

[[nodiscard]] ParameterAttributes normalize(ParameterAttributes attributes);

enum class ParameterKind : std::uint8_t {
    continuous = 0,
    continuous_bipolar = 1,
    toggle = 2,
    discrete = 3,
    discrete_bipolar = 4
};

enum class ParameterDisplayStyle : std::uint8_t {
    style_0 = 0,
    style_1 = 1,
    style_2 = 2,
    style_3 = 3,
    style_4 = 4,
    menu = 5,
    waveform = 6,
    filter_type = 7
};

using ParameterValue = std::variant<double, bool, std::int32_t>;

struct ParameterModel {
    ParameterKind kind{ ParameterKind::continuous };
    ParameterDisplayStyle display_style{ ParameterDisplayStyle::style_0 };
    std::uint32_t value_count{};
    std::vector<std::string> values;
    ParameterValue value{ 0.0 };
    std::string display_value;
};

[[nodiscard]] ParameterModel make_parameter_model(
        ParameterAttributes attributes, double normalized_value,
        std::string display_value = {});

struct NavigationParameter {
    std::int32_t id{ -1 };
    std::string name;
};

struct NavigationSection {
    std::string name;
    std::vector<NavigationParameter> parameters;
};

struct NavigationGroup {
    std::string name;
    std::vector<NavigationSection> sections;
};

// source group zero supplies top-level sections; later groups keep their names
struct Nks2Navigation {
    std::vector<NavigationSection> sections;
    std::vector<NavigationGroup> groups;
};

struct NksMetadataV1 {
    std::vector<std::pair<std::uint32_t, ParameterAttributes>>
            parameter_descriptive_info;
    Nks2Navigation parameter_navigation;
};

// first failure only. a document can serialize fine and still fail these, so
// don't fold this into the encoder
enum class NksMetadataValidationError {
    invalid_style,
    missing_discrete_value_count,
    inconsistent_discrete_value_count,
    toggle_display_value_count,
    invalid_bipolar_discrete_value_count,
    discrete_value_count_below_minimum,
    discrete_value_count_above_maximum,
    missing_icon_display_values,
    missing_performance_group,
    missing_group_name,
    duplicate_group_name,
    missing_performance_section_name,
    performance_section_without_parameter,
    too_many_performance_parameters,
    missing_group_section_name,
    group_section_without_parameter,
    too_many_group_parameters,
};

[[nodiscard]] std::optional<NksMetadataValidationError>
validate_nks_metadata_v1(const NksMetadataV1 &metadata);

// invalid type/style bytes and duplicate parameter IDs can't be serialized
[[nodiscard]] std::optional<std::vector<std::byte>> encode_nks_metadata_v1(
        const NksMetadataV1 &metadata);

enum class Nks1StorageByteOrder { little_endian, big_endian };

// we only know what assignment_name and client_parameter_id are, the rest
// keep their raw values and are named by wire order
struct Nks1StoredControllerAssignment {
    std::string assignment_name;
    float wire_float_0{}; // serialized first
    float wire_float_1{}; // serialized second
    std::uint32_t client_parameter_id{
        std::numeric_limits<std::uint32_t>::max()
    };
    std::uint32_t wire_u32{};
};

struct Nks1StoredControllerGroup {
    std::string name;
    std::vector<Nks1StoredControllerAssignment> assignments;
};

struct Nks1StoredControllerCollection {
    std::vector<Nks1StoredControllerGroup> groups;
};

[[nodiscard]] std::vector<std::byte> encode_nks1_controller_collection(
        const Nks1StoredControllerCollection &collection,
        Nks1StorageByteOrder byte_order = Nks1StorageByteOrder::little_endian);
[[nodiscard]] std::optional<Nks1StoredControllerCollection>
decode_nks1_controller_collection(std::span<const std::byte> bytes,
        Nks1StorageByteOrder byte_order = Nks1StorageByteOrder::little_endian);

// just the command ID and payload, you have to deliver it yourself
struct MicroKpiMessage {
    std::uint32_t command{};
    std::vector<std::byte> payload;
};

[[nodiscard]] MicroKpiMessage micro_kpi_navigation(
        const Nks1StoredControllerCollection &collection);
[[nodiscard]] std::optional<MicroKpiMessage> micro_kpi_pressed_key(
        std::uint8_t note, bool pressed, bool pressed_support_enabled);
[[nodiscard]] MicroKpiMessage micro_kpi_key_color(
        std::uint8_t note, std::uint32_t kontakt_color);
[[nodiscard]] MicroKpiMessage micro_kpi_note_name(
        std::uint8_t note, std::string_view name);
[[nodiscard]] MicroKpiMessage micro_kpi_pressed_support(bool enabled);
[[nodiscard]] std::optional<MicroKpiMessage> micro_kpi_touchstrip_mode(
        std::uint32_t strip, std::uint8_t mode);
[[nodiscard]] MicroKpiMessage micro_kpi_touchstrip_current_value(
        std::uint8_t strip, float value);
[[nodiscard]] MicroKpiMessage micro_kpi_touchstrip_default_value(
        std::uint8_t strip, float value);
[[nodiscard]] MicroKpiMessage micro_kpi_touchstrip_parameters(
        std::uint8_t strip, float first, float second, float third);
[[nodiscard]] MicroKpiMessage micro_kpi_note_type(
        std::uint8_t note, std::uint32_t kontakt_type);

struct DeviceParameterPlacement {
    std::uint32_t id{};
    std::string name;
};

struct DeviceLayoutSection {
    std::string name;
    std::vector<std::optional<DeviceParameterPlacement>> parameters;
};

struct DeviceLayoutGroup {
    std::string name;
    std::vector<DeviceLayoutSection> sections;
};

struct DeviceParameterLayout {
    std::vector<DeviceLayoutSection> sections;
    std::vector<DeviceLayoutGroup> groups;
};

struct Nks1ControllerAssignment {
    // -1 is an unassigned slot, so it becomes an empty placement
    std::int32_t parameter_id{};
    std::string name;
};
struct Nks1ControllerGroup {
    std::string name;
    std::vector<std::optional<Nks1ControllerAssignment>> assignments;
};
using Nks1ParameterPage = std::vector<DeviceLayoutSection>;

// don't split a group across eight-control pages; start a page if it won't fit
[[nodiscard]] std::vector<Nks1ParameterPage> make_nks1_parameter_layout(
        const std::vector<Nks1ControllerGroup> &groups);

using ParameterNameResolver = std::function<std::string(std::uint32_t)>;

[[nodiscard]] DeviceParameterLayout make_device_layout(
        const Nks2Navigation &navigation,
        const ParameterNameResolver &resolve_parameter_name = {});

class NavigationBuilder {
  public:
    static constexpr std::size_t max_groups = 128;
    static constexpr std::size_t max_slots = 48;

    void clear();
    void touch() noexcept;
    [[nodiscard]] std::uint32_t revision() const noexcept {
        return change_count;
    }
    [[nodiscard]] bool empty() const noexcept { return groups.empty(); }

    // slot -1 is the group name, otherwise 0..47
    [[nodiscard]] bool set_name(
            std::int32_t group, std::int32_t slot, std::string name);
    [[nodiscard]] bool set_parameter(
            std::int32_t group, std::int32_t slot, std::int32_t parameter_id);
    [[nodiscard]] std::int32_t parameter(
            std::int32_t group, std::int32_t slot) const noexcept;
    [[nodiscard]] std::string_view name(
            std::int32_t group, std::int32_t slot) const noexcept;
    [[nodiscard]] Nks2Navigation export_nks2(
            std::int32_t parameter_id_offset = 0) const;

  private:
    struct SourceGroup {
        std::string name;
        std::vector<std::int32_t> parameters;
        std::vector<std::string> names;
    };

    [[nodiscard]] bool resize(std::int32_t group, std::int32_t slot);
    void shrink();

    std::uint32_t change_count{};
    std::vector<SourceGroup> groups;
};

struct PluginInfo {
    std::string name;
    std::string vendor;
    std::string preset;
};

struct Parameter {
    std::uint64_t id{};
    std::string name;
    std::string value;
    double normalized_value{};
    ParameterAttributes attributes;
};

struct Page {
    std::string name;
    std::vector<Parameter> parameters;
};

enum class RpcMethod : std::uint8_t {
    instance_hello,
    instance_goodbye,
    connect_device,
    subscribe_device_properties,
    client_request_focus,
    client_release_focus,
    client_set_page,
    client_lightguide_set_leds,
    client_plugin_chain_set_data,
    client_browser_set_data_model,
    client_browser_set_sound_item,
    client_browser_set_sound_selection,
    client_browser_set_filters,
    client_browser_set_filter,
    client_browser_set_filter_selection,
    client_browser_set_sounds,
    client_browser_set_settings,
    client_parameter_page_set_data,
    client_parameter_page_set_parameter_value,
    client_set_host_owned_instance_client_state,
    add_asset,
    device_connected,
    device_disconnected,
    midi_plugin_action,
    plugin_action,
    active_instance_client_changed,
    focus_changed,
    device_properties,
    device_property_changed,
    firmware_update_available,
    device_owned_instance_client_state_changed
};

[[nodiscard]] std::string_view rpc_method_name(RpcMethod method) noexcept;

struct DeviceOwnedLightguide {};

struct LightguideLed {
    std::int8_t midi_note{};
    std::uint8_t value{};
};

// MK3 palette only, MicroKPI and desktop keyboards need their own mapping
[[nodiscard]] std::uint8_t mk3_key_color(std::uint32_t kontakt_color) noexcept;
// palette occupies bits 2..7 and pressed occupies bit 1; palette zero stays off
[[nodiscard]] std::uint8_t mk3_led_value(
        std::uint8_t palette, bool pressed) noexcept;

using LightguideState =
        std::variant<DeviceOwnedLightguide, std::vector<LightguideLed>>;

enum class EventKind : std::uint8_t {
    parameter_change,
    page_change,
    preset_previous,
    preset_next,
    button,
    key,
    encoder,
    device_connected,
    device_disconnected,
    focus_changed,
    plugin_action,
    device_property_changed
};

struct Event {
    EventKind kind{};
    std::uint64_t id{};
    double value{};
    bool pressed{};
};

// legacy, use Client instead
class Surface {
  public:
    virtual ~Surface() = default;
    virtual void request_focus() = 0;
    virtual void release_focus() = 0;
    virtual void set_page(std::uint8_t page) = 0;
    virtual void set_plugin(const PluginInfo &info) = 0;
    virtual void set_pages(std::span<const Page> pages) = 0;
    virtual void set_lightguide(const LightguideState &state) = 0;
    virtual std::optional<Event> poll() = 0;
};

// custom navigation needs the 5 byte "NKS2 " prefix in front
[[nodiscard]] std::vector<std::byte> make_nks2_navigation_message(
        std::span<const std::byte> validated_payload);
[[nodiscard]] std::optional<std::span<const std::byte>>
parse_nks2_navigation_message(std::span<const std::byte> message);

} // namespace libnks
