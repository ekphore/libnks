// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "libnks/nks.h"
#include "libnks/wire.h"

#include <algorithm>
#include <array>
#include <bit>
#include <limits>
#include <set>

namespace libnks {

namespace {

bool is_discrete(std::uint8_t type) { return type == 3 || type == 4; }

std::size_t parameter_count(const std::vector<NavigationSection> &sections) {
    std::size_t result = 0;
    for (const auto &section : sections)
        result += section.parameters.size();
    return result;
}

bool has_valid_parameter(const NavigationSection &section) {
    return std::ranges::any_of(
            section.parameters, [](const NavigationParameter &parameter) {
                return parameter.id != -1;
            });
}

} // namespace

std::optional<NksMetadataValidationError> validate_nks_metadata_v1(
        const NksMetadataV1 &metadata) {
    using Error = NksMetadataValidationError;
    for (const auto &[id, attributes] : metadata.parameter_descriptive_info) {
        (void)id;
        const auto type = attributes.type;
        const auto style = attributes.display_style;
        const auto display_count = attributes.values.size();

        // styles 0 and 2 work for every type; the others depend on the
        // parameter type
        const bool style_valid = style == 0 || style == 2 ||
                                 ((type == 0 || type == 1) && style == 1) ||
                                 (type == 2 && (style == 3 || style == 4)) ||
                                 (type == 3 && style >= 5 && style <= 7);
        if (!style_valid)
            return Error::invalid_style;
        if (is_discrete(type) && attributes.value_count == 0 &&
                display_count == 0)
            return Error::missing_discrete_value_count;
        if (is_discrete(type) && attributes.value_count != 0 &&
                display_count != 0 && display_count != attributes.value_count)
            return Error::inconsistent_discrete_value_count;
        if (type == 2 && display_count != 0 && display_count != 2)
            return Error::toggle_display_value_count;
        const auto bipolar_count =
                attributes.value_count != 0
                        ? static_cast<std::size_t>(attributes.value_count)
                        : display_count;
        if (type == 4 && bipolar_count != 0 && bipolar_count % 2 == 0)
            return Error::invalid_bipolar_discrete_value_count;
        if (is_discrete(type) &&
                ((attributes.value_count != 0 && attributes.value_count < 3) ||
                        (display_count != 0 && display_count < 3)))
            return Error::discrete_value_count_below_minimum;
        if (is_discrete(type) &&
                (attributes.value_count > 128 || display_count > 128))
            return Error::discrete_value_count_above_maximum;
        if ((style == 6 || style == 7) && display_count == 0)
            return Error::missing_icon_display_values;
    }

    const auto &navigation = metadata.parameter_navigation;
    if (navigation.sections.empty())
        return Error::missing_performance_group;
    for (const auto &group : navigation.groups)
        if (group.name.empty())
            return Error::missing_group_name;
    std::set<std::string> group_names;
    for (const auto &group : navigation.groups)
        if (!group_names.insert(group.name).second)
            return Error::duplicate_group_name;

    // a lone section can be unnamed
    if (navigation.sections.size() > 1) {
        for (const auto &section : navigation.sections)
            if (section.name.empty())
                return Error::missing_performance_section_name;
    }
    for (const auto &section : navigation.sections)
        if (!has_valid_parameter(section))
            return Error::performance_section_without_parameter;
    if (parameter_count(navigation.sections) > 16)
        return Error::too_many_performance_parameters;

    for (const auto &group : navigation.groups) {
        if (group.sections.size() > 1) {
            for (const auto &section : group.sections)
                if (section.name.empty())
                    return Error::missing_group_section_name;
        }
        for (const auto &section : group.sections)
            if (!has_valid_parameter(section))
                return Error::group_section_without_parameter;
        if (parameter_count(group.sections) > 48)
            return Error::too_many_group_parameters;
    }
    return std::nullopt;
}

std::optional<std::vector<std::byte>> encode_nks_metadata_v1(
        const NksMetadataV1 &metadata) {
    MsgpackValue::Map descriptive;
    descriptive.reserve(metadata.parameter_descriptive_info.size());
    std::set<std::uint32_t> ids;
    for (const auto &[id, attributes] : metadata.parameter_descriptive_info) {
        if (attributes.type > 4 || attributes.display_style > 7 ||
                !ids.insert(id).second)
            return std::nullopt;
        MsgpackValue::Array display_values;
        display_values.reserve(attributes.values.size());
        for (const auto &value : attributes.values)
            display_values.emplace_back(value);
        descriptive.emplace_back(MsgpackValue(std::uint64_t(id)),
                MsgpackValue(MsgpackValue::Map{
                        { "type",
                                MsgpackValue(std::uint64_t(attributes.type)) },
                        { "style", MsgpackValue(std::uint64_t(
                                           attributes.display_style)) },
                        { "value_count", MsgpackValue(std::uint64_t(
                                                 attributes.value_count)) },
                        { "display_values",
                                MsgpackValue(std::move(display_values)) } }));
    }
    const auto parameter = [](const NavigationParameter &input) {
        return MsgpackValue(MsgpackValue::Map{
                { "id", MsgpackValue(std::uint64_t(
                                static_cast<std::uint32_t>(input.id))) },
                { "name", MsgpackValue(input.name) } });
    };
    const auto section = [&](const NavigationSection &input) {
        MsgpackValue::Array parameters;
        parameters.reserve(input.parameters.size());
        for (const auto &item : input.parameters)
            parameters.push_back(parameter(item));
        return MsgpackValue(MsgpackValue::Map{
                { "name", MsgpackValue(input.name) },
                { "parameters", MsgpackValue(std::move(parameters)) } });
    };
    MsgpackValue::Array performance_mode;
    performance_mode.reserve(metadata.parameter_navigation.sections.size());
    for (const auto &item : metadata.parameter_navigation.sections)
        performance_mode.push_back(section(item));
    MsgpackValue::Array groups;
    groups.reserve(metadata.parameter_navigation.groups.size());
    for (const auto &group : metadata.parameter_navigation.groups) {
        MsgpackValue::Array sections;
        sections.reserve(group.sections.size());
        for (const auto &item : group.sections)
            sections.push_back(section(item));
        groups.emplace_back(
                MsgpackValue::Map{ { "name", MsgpackValue(group.name) },
                        { "sections", MsgpackValue(std::move(sections)) } });
    }
    return encode_msgpack_value(MsgpackValue(MsgpackValue::Map{
            { "version", MsgpackValue(std::uint64_t{ 1 }) },
            { "parameter_descriptive_info",
                    MsgpackValue(std::move(descriptive)) },
            { "parameter_navigation",
                    MsgpackValue(MsgpackValue::Map{
                            { "performance_mode",
                                    MsgpackValue(std::move(performance_mode)) },
                            { "groups",
                                    MsgpackValue(std::move(groups)) } }) } }));
}

namespace {

template <class T>
void append_storage_integer(
        std::vector<std::byte> &output, T value, Nks1StorageByteOrder order) {
    using U = std::make_unsigned_t<T>;
    const auto bits = static_cast<U>(value);
    for (std::size_t i = 0; i < sizeof(T); ++i) {
        const auto shift = order == Nks1StorageByteOrder::little_endian
                                   ? i * 8
                                   : (sizeof(T) - i - 1) * 8;
        output.push_back(std::byte((bits >> shift) & 0xff));
    }
}

std::vector<std::uint16_t> utf8_to_utf16(std::string_view input) {
    std::vector<std::uint16_t> output;
    for (std::size_t i = 0; i < input.size();) {
        const auto first = static_cast<std::uint8_t>(input[i]);
        std::uint32_t codepoint = 0xfffd;
        std::size_t width = 1;
        if (first < 0x80)
            codepoint = first;
        else {
            std::size_t expected = 0;
            std::uint32_t minimum = 0;
            if ((first & 0xe0) == 0xc0) {
                expected = 2;
                codepoint = first & 0x1f;
                minimum = 0x80;
            } else if ((first & 0xf0) == 0xe0) {
                expected = 3;
                codepoint = first & 0x0f;
                minimum = 0x800;
            } else if ((first & 0xf8) == 0xf0) {
                expected = 4;
                codepoint = first & 0x07;
                minimum = 0x10000;
            }
            if (expected != 0 && i + expected <= input.size()) {
                bool valid = true;
                for (std::size_t j = 1; j < expected; ++j) {
                    const auto next = static_cast<std::uint8_t>(input[i + j]);
                    if ((next & 0xc0) != 0x80) {
                        valid = false;
                        break;
                    }
                    codepoint = (codepoint << 6) | (next & 0x3f);
                }
                if (valid && codepoint >= minimum && codepoint <= 0x10ffff &&
                        !(codepoint >= 0xd800 && codepoint <= 0xdfff))
                    width = expected;
                else
                    codepoint = 0xfffd;
            }
        }
        i += width;
        if (codepoint <= 0xffff)
            output.push_back(static_cast<std::uint16_t>(codepoint));
        else {
            codepoint -= 0x10000;
            output.push_back(
                    static_cast<std::uint16_t>(0xd800 + (codepoint >> 10)));
            output.push_back(
                    static_cast<std::uint16_t>(0xdc00 + (codepoint & 0x3ff)));
        }
    }
    return output;
}

void append_wstring(std::vector<std::byte> &output, std::string_view value,
        Nks1StorageByteOrder order) {
    const auto units = utf8_to_utf16(value);
    append_storage_integer(
            output, static_cast<std::uint32_t>(units.size()), order);
    for (const auto unit : units)
        append_storage_integer(output, unit, order);
}

class StorageReader {
  public:
    StorageReader(std::span<const std::byte> bytes, Nks1StorageByteOrder order)
        : bytes(bytes), order(order) {}

    template <class T>
    std::optional<T> integer() {
        if (bytes.size() - offset < sizeof(T))
            return std::nullopt;
        using U = std::make_unsigned_t<T>;
        U value = 0;
        for (std::size_t i = 0; i < sizeof(T); ++i) {
            const auto shift = order == Nks1StorageByteOrder::little_endian
                                       ? i * 8
                                       : (sizeof(T) - i - 1) * 8;
            value |= U(std::to_integer<std::uint8_t>(bytes[offset + i]))
                     << shift;
        }
        offset += sizeof(T);
        return static_cast<T>(value);
    }

    std::optional<std::string> wstring() {
        const auto count = integer<std::uint32_t>();
        if (!count || *count > (bytes.size() - offset) / 2)
            return std::nullopt;
        std::string result;
        for (std::uint32_t i = 0; i < *count; ++i) {
            const auto first = integer<std::uint16_t>();
            if (!first)
                return std::nullopt;
            std::uint32_t codepoint = *first;
            if (*first >= 0xd800 && *first <= 0xdbff) {
                if (i + 1 == *count)
                    return std::nullopt;
                const auto second = integer<std::uint16_t>();
                if (!second || *second < 0xdc00 || *second > 0xdfff)
                    return std::nullopt;
                ++i;
                codepoint = 0x10000 + ((std::uint32_t(*first) - 0xd800) << 10) +
                            (std::uint32_t(*second) - 0xdc00);
            } else if (*first >= 0xdc00 && *first <= 0xdfff)
                return std::nullopt;
            if (codepoint < 0x80)
                result.push_back(static_cast<char>(codepoint));
            else if (codepoint < 0x800) {
                result.push_back(static_cast<char>(0xc0 | (codepoint >> 6)));
                result.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
            } else if (codepoint < 0x10000) {
                result.push_back(static_cast<char>(0xe0 | (codepoint >> 12)));
                result.push_back(
                        static_cast<char>(0x80 | ((codepoint >> 6) & 0x3f)));
                result.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
            } else {
                result.push_back(static_cast<char>(0xf0 | (codepoint >> 18)));
                result.push_back(
                        static_cast<char>(0x80 | ((codepoint >> 12) & 0x3f)));
                result.push_back(
                        static_cast<char>(0x80 | ((codepoint >> 6) & 0x3f)));
                result.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
            }
        }
        return result;
    }

    [[nodiscard]] std::size_t remaining() const {
        return bytes.size() - offset;
    }

  private:
    std::span<const std::byte> bytes;
    Nks1StorageByteOrder order;
    std::size_t offset{};
};

} // namespace

std::vector<std::byte> encode_nks1_controller_collection(
        const Nks1StoredControllerCollection &collection,
        Nks1StorageByteOrder order) {
    std::vector<std::byte> output;
    append_storage_integer(output, std::uint32_t{ 1 }, order);
    append_storage_integer(output,
            static_cast<std::uint32_t>(collection.groups.size()), order);
    for (const auto &group : collection.groups) {
        append_storage_integer(output, std::uint32_t{ 1 }, order);
        append_storage_integer(output,
                static_cast<std::uint32_t>(group.assignments.size()), order);
        append_wstring(output, group.name, order);
        for (const auto &assignment : group.assignments) {
            append_storage_integer(output, std::uint32_t{ 1 }, order);
            append_wstring(output, assignment.assignment_name, order);
            append_storage_integer(output,
                    std::bit_cast<std::uint32_t>(assignment.wire_float_0),
                    order);
            append_storage_integer(output,
                    std::bit_cast<std::uint32_t>(assignment.wire_float_1),
                    order);
            append_storage_integer(
                    output, assignment.client_parameter_id, order);
            append_storage_integer(output, assignment.wire_u32, order);
        }
    }
    return output;
}

std::optional<Nks1StoredControllerCollection> decode_nks1_controller_collection(
        std::span<const std::byte> bytes, Nks1StorageByteOrder order) {
    StorageReader reader(bytes, order);
    const auto version = reader.integer<std::uint32_t>();
    const auto group_count = reader.integer<std::uint32_t>();
    if (!version || *version != 1 || !group_count)
        return std::nullopt;
    Nks1StoredControllerCollection result;
    if (*group_count > reader.remaining() / 12)
        return std::nullopt;
    result.groups.reserve(*group_count);
    for (std::uint32_t group_index = 0; group_index < *group_count;
            ++group_index) {
        const auto group_version = reader.integer<std::uint32_t>();
        const auto assignment_count = reader.integer<std::uint32_t>();
        const auto name = reader.wstring();
        if (!group_version || *group_version != 1 || !assignment_count || !name)
            return std::nullopt;
        Nks1StoredControllerGroup group;
        group.name = *name;
        if (*assignment_count > reader.remaining() / 24)
            return std::nullopt;
        group.assignments.reserve(*assignment_count);
        for (std::uint32_t index = 0; index < *assignment_count; ++index) {
            const auto assignment_version = reader.integer<std::uint32_t>();
            const auto assignment_name = reader.wstring();
            const auto float_0 = reader.integer<std::uint32_t>();
            const auto float_1 = reader.integer<std::uint32_t>();
            const auto parameter_id = reader.integer<std::uint32_t>();
            const auto wire_u32 = reader.integer<std::uint32_t>();
            if (!assignment_version || *assignment_version != 1 ||
                    !assignment_name || !float_0 || !float_1 || !parameter_id ||
                    !wire_u32)
                return std::nullopt;
            group.assignments.push_back({ *assignment_name,
                    std::bit_cast<float>(*float_0),
                    std::bit_cast<float>(*float_1), *parameter_id, *wire_u32 });
        }
        result.groups.push_back(std::move(group));
    }
    if (reader.remaining() != 0)
        return std::nullopt;
    return result;
}

MicroKpiMessage micro_kpi_navigation(
        const Nks1StoredControllerCollection &collection) {
    return { 0x11, collection.groups.empty()
                           ? std::vector<std::byte>{}
                           : encode_nks1_controller_collection(collection) };
}

std::optional<MicroKpiMessage> micro_kpi_pressed_key(
        std::uint8_t note, bool pressed, bool pressed_support_enabled) {
    if (!pressed_support_enabled)
        return std::nullopt;
    return MicroKpiMessage{ 0x13,
        { std::byte{ note }, pressed ? std::byte{ 1 } : std::byte{ 0 } } };
}

MicroKpiMessage micro_kpi_key_color(
        std::uint8_t note, std::uint32_t kontakt_color) {
    constexpr std::array<std::uint8_t, 21> colors{ 17, 17, 3, 5, 0, 10, 8, 11,
        1, 2, 4, 6, 7, 9, 11, 12, 13, 14, 15, 16, 17 };
    const auto color = kontakt_color < colors.size() ? colors[kontakt_color]
                                                     : std::uint8_t{ 0x10 };
    return { 0x14, { std::byte{ note }, std::byte{ color } } };
}

std::uint8_t mk3_led_value(std::uint8_t palette, bool pressed) noexcept {
    return palette ? std::uint8_t(
                             (unsigned(palette) << 2) | (pressed ? 2U : 0U))
                   : 0;
}

std::uint8_t mk3_key_color(std::uint32_t kontakt_color) noexcept {

    constexpr std::array<std::uint8_t, 21> colors{ 10, 31, 4, 6, 1, 11, 9, 0, 2,
        3, 5, 7, 8, 10, 12, 13, 14, 15, 16, 10, 0 };
    return kontakt_color < colors.size() ? colors[kontakt_color] : 10;
}

MicroKpiMessage micro_kpi_note_name(std::uint8_t note, std::string_view name) {
    MicroKpiMessage result{ 0x15, std::vector<std::byte>(130) };
    result.payload[0] = std::byte{ note };
    const auto length = std::min<std::size_t>(name.size(), 128);
    for (std::size_t i = 0; i < length; ++i)
        result.payload[i + 1] = std::byte(static_cast<unsigned char>(name[i]));
    // all 130 bytes go over the wire, so keep the unused tail zeroed
    result.payload[length + 1] = std::byte{ 0 };
    return result;
}

MicroKpiMessage micro_kpi_pressed_support(bool enabled) {
    return { 0x16, { enabled ? std::byte{ 1 } : std::byte{ 0 } } };
}

std::optional<MicroKpiMessage> micro_kpi_touchstrip_mode(
        std::uint32_t strip, std::uint8_t mode) {
    if (strip > 1)
        return std::nullopt;
    return MicroKpiMessage{ 0x17,
        { strip == 0 ? std::byte{ 1 } : std::byte{ 2 }, std::byte{ mode } } };
}

namespace {

MicroKpiMessage micro_kpi_float_message(std::uint32_t command,
        std::uint8_t strip, std::span<const float> values) {
    MicroKpiMessage result{ command, { std::byte{ strip } } };
    result.payload.reserve(1 + values.size() * sizeof(float));
    for (const auto value : values)
        append_storage_integer(result.payload,
                std::bit_cast<std::uint32_t>(value),
                Nks1StorageByteOrder::little_endian);
    return result;
}

} // namespace

MicroKpiMessage micro_kpi_touchstrip_current_value(
        std::uint8_t strip, float value) {
    return micro_kpi_float_message(
            0x18, strip, std::span<const float>(&value, 1));
}

MicroKpiMessage micro_kpi_touchstrip_default_value(
        std::uint8_t strip, float value) {
    return micro_kpi_float_message(
            0x19, strip, std::span<const float>(&value, 1));
}

MicroKpiMessage micro_kpi_touchstrip_parameters(
        std::uint8_t strip, float first, float second, float third) {
    const std::array values{ first, second, third };
    return micro_kpi_float_message(0x1a, strip, values);
}

MicroKpiMessage micro_kpi_note_type(
        std::uint8_t note, std::uint32_t kontakt_type) {
    return { 0x1b, { std::byte{ note }, kontakt_type == 2 ? std::byte{ 1 }
                                                          : std::byte{ 0 } } };
}
namespace {

constexpr std::array<std::byte, 5> kNks2Prefix{ std::byte{ 'N' },
    std::byte{ 'K' }, std::byte{ 'S' }, std::byte{ '2' }, std::byte{ ' ' } };

} // namespace

ParameterAttributes normalize(ParameterAttributes attributes) {
    if (attributes.type > 4)
        attributes.type = 0;
    if (attributes.display_style > 7)
        attributes.display_style = 0;
    return attributes;
}

ParameterModel make_parameter_model(ParameterAttributes attributes,
        double normalized_value, std::string display_value) {
    attributes = normalize(std::move(attributes));
    ParameterModel result;
    result.kind = static_cast<ParameterKind>(attributes.type);
    result.display_style =
            static_cast<ParameterDisplayStyle>(attributes.display_style);
    result.value_count =
            attributes.values.empty()
                    ? attributes.value_count
                    : static_cast<std::uint32_t>(attributes.values.size());
    result.values = std::move(attributes.values);
    result.display_value = std::move(display_value);

    switch (result.kind) {
        case ParameterKind::toggle:
            result.value = normalized_value >= 0.5;
            break;
        case ParameterKind::discrete:
        case ParameterKind::discrete_bipolar: {
            const auto count = static_cast<std::int32_t>(result.value_count);
            const auto scaled = static_cast<std::int32_t>(
                    static_cast<double>(count) * normalized_value);
            result.value = std::min(scaled, count - 1);
            break;
        }
        default:
            result.value = normalized_value;
            break;
    }
    return result;
}

DeviceParameterLayout make_device_layout(const Nks2Navigation &navigation,
        const ParameterNameResolver &resolve_parameter_name) {
    DeviceParameterLayout result;
    auto convert_sections =
            [&resolve_parameter_name](
                    const std::vector<NavigationSection> &sections) {
                std::vector<DeviceLayoutSection> converted;
                converted.reserve(sections.size());
                for (const auto &section : sections) {
                    DeviceLayoutSection target{ section.name, {} };
                    target.parameters.reserve(section.parameters.size());
                    for (const auto &parameter : section.parameters) {
                        if (parameter.id == -1) {
                            target.parameters.push_back(std::nullopt);
                            continue;
                        }
                        std::string name = parameter.name;
                        if (name.empty() && resolve_parameter_name) {
                            name = resolve_parameter_name(
                                    static_cast<std::uint32_t>(parameter.id));
                        } else if (name.empty()) {
                            name = "No Name";
                        }
                        target.parameters.push_back(DeviceParameterPlacement{
                                static_cast<std::uint32_t>(parameter.id),
                                std::move(name) });
                    }
                    converted.push_back(std::move(target));
                }
                return converted;
            };

    result.sections = convert_sections(navigation.sections);
    result.groups.reserve(navigation.groups.size());
    for (const auto &group : navigation.groups) {
        result.groups.push_back(
                { group.name, convert_sections(group.sections) });
    }
    return result;
}

std::vector<Nks1ParameterPage> make_nks1_parameter_layout(
        const std::vector<Nks1ControllerGroup> &groups) {
    std::vector<Nks1ParameterPage> pages;
    std::size_t controls_on_page = 0;
    for (const auto &group : groups) {
        if (pages.empty() || controls_on_page >= 8 ||
                group.assignments.size() > 8 - controls_on_page) {
            pages.emplace_back();
            controls_on_page = 0;
        }
        DeviceLayoutSection section{ group.name, {} };
        section.parameters.reserve(group.assignments.size());
        for (const auto &assignment : group.assignments) {
            if (!assignment || assignment->parameter_id == -1) {
                section.parameters.push_back(std::nullopt);
            } else {
                section.parameters.push_back(DeviceParameterPlacement{
                        static_cast<std::uint32_t>(assignment->parameter_id),
                        assignment->name });
            }
        }
        pages.back().push_back(std::move(section));
        controls_on_page += group.assignments.size();
    }
    return pages;
}

std::string_view rpc_method_name(RpcMethod method) noexcept {
    using enum RpcMethod;
    switch (method) {
        case instance_hello:
            return "instance_hello";
        case instance_goodbye:
            return "instance_goodbye";
        case connect_device:
            return "connect_device";
        case subscribe_device_properties:
            return "subscribe_device_properties";
        case client_request_focus:
            return "client_request_focus";
        case client_release_focus:
            return "client_release_focus";
        case client_set_page:
            return "client_set_page";
        case client_lightguide_set_leds:
            return "client_lightguide_set_leds";
        case client_plugin_chain_set_data:
            return "client_plugin_chain_set_data";
        case client_browser_set_data_model:
            return "client_browser_set_data_model";
        case client_browser_set_sound_item:
            return "client_browser_set_sound_item";
        case client_browser_set_sound_selection:
            return "client_browser_set_sound_selection";
        case client_browser_set_filters:
            return "client_browser_set_filters";
        case client_browser_set_filter:
            return "client_browser_set_filter";
        case client_browser_set_filter_selection:
            return "client_browser_set_filter_selection";
        case client_browser_set_sounds:
            return "client_browser_set_sounds";
        case client_browser_set_settings:
            return "client_browser_set_settings";
        case client_parameter_page_set_data:
            return "client_parameter_page_set_data";
        case client_parameter_page_set_parameter_value:
            return "client_parameter_page_set_parameter_value";
        case client_set_host_owned_instance_client_state:
            return "client_set_host_owned_instance_client_state";
        case add_asset:
            return "add_asset";
        case device_connected:
            return "device_connected";
        case device_disconnected:
            return "device_disconnected";
        case midi_plugin_action:
            return "midi_plugin_action";
        case plugin_action:
            return "plugin_action";
        case active_instance_client_changed:
            return "active_instance_client_changed";
        case focus_changed:
            return "focus_changed";
        case device_properties:
            return "device_properties";
        case device_property_changed:
            return "device_property_changed";
        case firmware_update_available:
            return "firmware_update_available";
        case device_owned_instance_client_state_changed:
            return "device_owned_instance_client_state_changed";
    }
    return {};
}

void NavigationBuilder::clear() { groups.clear(); }

void NavigationBuilder::touch() noexcept { ++change_count; }

bool NavigationBuilder::resize(std::int32_t group, std::int32_t slot) {
    if (group < 0 || static_cast<std::size_t>(group) >= max_groups ||
            slot < -1 || static_cast<std::size_t>(slot + 1) > max_slots) {
        return false;
    }
    const auto old_size = groups.size();
    groups.resize(static_cast<std::size_t>(group) + 1);
    for (std::size_t i = old_size; i < groups.size(); ++i) {
        groups[i].name = "Group " + std::to_string(i);
    }
    if (slot >= 0) {
        auto &source = groups[static_cast<std::size_t>(group)];
        const auto size = static_cast<std::size_t>(slot) + 1;
        source.parameters.resize(size, -1);
        source.names.resize(size);
    }
    return true;
}

void NavigationBuilder::shrink() {
    std::size_t last_group{};
    for (std::size_t group_index = groups.size(); group_index != 0;
            --group_index) {
        auto &group = groups[group_index - 1];
        std::size_t last_slot{};
        for (std::size_t slot =
                        std::max(group.parameters.size(), group.names.size());
                slot != 0; --slot) {
            const auto index = slot - 1;
            const bool has_parameter = index < group.parameters.size() &&
                                       group.parameters[index] >= 0;
            const bool has_name =
                    index < group.names.size() && !group.names[index].empty();
            if (has_parameter || has_name) {
                last_slot = slot;
                break;
            }
        }
        group.parameters.resize(last_slot, -1);
        group.names.resize(last_slot);
        if (last_group == 0 && (!group.name.empty() || last_slot != 0)) {
            last_group = group_index;
        }
    }
    groups.resize(last_group);
}

bool NavigationBuilder::set_name(
        std::int32_t group, std::int32_t slot, std::string value) {
    if (!resize(group, slot))
        return false;
    auto &source = groups[static_cast<std::size_t>(group)];
    auto &destination = slot == -1
                                ? source.name
                                : source.names[static_cast<std::size_t>(slot)];
    if (destination != value) {
        destination = std::move(value);
        touch();
    }
    shrink();
    return true;
}

bool NavigationBuilder::set_parameter(
        std::int32_t group, std::int32_t slot, std::int32_t parameter_id) {
    if (group < 0 || slot < 0 || !resize(group, slot))
        return false;
    auto &destination = groups[static_cast<std::size_t>(group)]
                                .parameters[static_cast<std::size_t>(slot)];
    if (destination != parameter_id) {
        destination = parameter_id;
        touch();
    }
    shrink();
    return true;
}

std::int32_t NavigationBuilder::parameter(
        std::int32_t group, std::int32_t slot) const noexcept {
    if (group < 0 || slot < 0 ||
            static_cast<std::size_t>(group) >= groups.size())
        return -1;
    const auto &values = groups[static_cast<std::size_t>(group)].parameters;
    if (static_cast<std::size_t>(slot) >= values.size())
        return -1;
    return values[static_cast<std::size_t>(slot)];
}

std::string_view NavigationBuilder::name(
        std::int32_t group, std::int32_t slot) const noexcept {
    if (group < 0 || slot < -1 ||
            static_cast<std::size_t>(group) >= groups.size())
        return {};
    const auto &source = groups[static_cast<std::size_t>(group)];
    if (slot == -1)
        return source.name;
    if (static_cast<std::size_t>(slot) >= source.names.size())
        return {};
    return source.names[static_cast<std::size_t>(slot)];
}

Nks2Navigation NavigationBuilder::export_nks2(
        std::int32_t parameter_id_offset) const {
    Nks2Navigation result;
    auto add_sections = [parameter_id_offset](
                                std::vector<NavigationSection> &destination,
                                const SourceGroup &source) {
        for (std::size_t slot = 0; slot < source.names.size(); ++slot) {
            if (slot == 0 || !source.names[slot].empty()) {
                destination.push_back({ source.names[slot], {} });
            }
            auto id = source.parameters[slot];
            if (id >= 0) {
                id = static_cast<std::int32_t>(
                        static_cast<std::uint32_t>(id) +
                        static_cast<std::uint32_t>(parameter_id_offset));
            }
            destination.back().parameters.push_back({ id, {} });
        }
    };

    for (std::size_t index = 0; index < groups.size(); ++index) {
        if (index == 0) {
            add_sections(result.sections, groups[index]);
        } else {
            result.groups.push_back({ groups[index].name, {} });
            add_sections(result.groups.back().sections, groups[index]);
        }
    }
    return result;
}

std::vector<std::byte> make_nks2_navigation_message(
        std::span<const std::byte> validated_payload) {
    std::vector<std::byte> result;
    result.reserve(kNks2Prefix.size() + validated_payload.size());
    result.insert(result.end(), kNks2Prefix.begin(), kNks2Prefix.end());
    result.insert(
            result.end(), validated_payload.begin(), validated_payload.end());
    return result;
}

std::optional<std::span<const std::byte>> parse_nks2_navigation_message(
        std::span<const std::byte> message) {
    if (message.size() < kNks2Prefix.size() ||
            !std::equal(
                    kNks2Prefix.begin(), kNks2Prefix.end(), message.begin())) {
        return std::nullopt;
    }
    return message.subspan(kNks2Prefix.size());
}

} // namespace libnks
