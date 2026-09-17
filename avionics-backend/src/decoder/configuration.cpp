#include "core/configuration.hpp"
#include "plugin_api/bus_plugin.h"
#include <charconv>
#include <cmath>
#include <set>
#include <sstream>
#include <stdexcept>

namespace avionics {
namespace {
using Values = std::map<std::string, std::string>;
std::string trim(std::string s) {
    const auto first = s.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    return s.substr(first, s.find_last_not_of(" \t\r\n") - first + 1);
}
std::string take(Values& values, const std::string& key, std::optional<std::string> fallback = {}) {
    const auto found = values.find(key);
    if (found == values.end()) {
        if (fallback) return *fallback;
        throw std::invalid_argument("missing configuration key: " + key);
    }
    auto text = found->second; values.erase(found); return text;
}
template<class T> T number(const std::string& text) {
    T value{};
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size())
        throw std::invalid_argument("invalid numeric value: " + text);
    if constexpr (std::is_floating_point_v<T>) if (!std::isfinite(value)) throw std::invalid_argument("nonfinite config value");
    return value;
}
std::uint64_t milliseconds(const std::string& text) {
    const auto value = number<std::uint64_t>(text);
    if (value > 86400000) throw std::invalid_argument("duration exceeds 24 hours");
    return value * 1000000;
}
void empty(const Values& values) {
    if (!values.empty()) throw std::invalid_argument("unknown configuration key: " + values.begin()->first);
}
Selector selector(Values& values, const std::string& prefix) {
    return {take(values, prefix + "_source"), take(values, prefix + "_parameter")};
}
}
BackendConfiguration BackendConfiguration::load(const std::filesystem::path& path) {
    std::ifstream input(path);
    if (!input) throw std::invalid_argument("cannot open backend configuration");
    std::map<std::string, Values> sections;
    Values* current = nullptr;
    std::string line;
    std::size_t line_number{};
    while (std::getline(input, line)) {
        ++line_number;
        if (line_number == 1 && line.starts_with("\xef\xbb\xbf")) line.erase(0, 3);
        line = trim(line);
        if (line.empty() || line[0] == '#' || line[0] == ';') continue;
        if (line[0] == '[' && line.back() == ']') {
            const auto name = trim(line.substr(1, line.size() - 2));
            if (name.empty() || !sections.emplace(name, Values{}).second) throw std::invalid_argument("empty/duplicate section");
            current = &sections.at(name);
        } else {
            const auto equal = line.find('=');
            if (!current || equal == std::string::npos) throw std::invalid_argument("invalid INI line " + std::to_string(line_number));
            const auto key = trim(line.substr(0, equal)), value = trim(line.substr(equal + 1));
            if (key.empty() || value.empty() || !current->emplace(key, value).second) throw std::invalid_argument("empty/duplicate key");
        }
    }
    if (input.bad()) throw std::invalid_argument("failed to read configuration");
    BackendConfiguration config;
    auto metadata = sections.find("dictionary");
    if (metadata == sections.end()) throw std::invalid_argument("missing [dictionary]");
    if (take(metadata->second, "schema") != "1") throw std::invalid_argument("unsupported configuration schema");
    config.version = take(metadata->second, "version"); empty(metadata->second); sections.erase(metadata);
    std::set<std::string> field_ids;
    for (auto& [section, values] : sections) {
        const auto space = section.find(' ');
        if (space == std::string::npos) throw std::invalid_argument("section requires type and id: " + section);
        const auto type = section.substr(0, space), id = trim(section.substr(space + 1));
        if (id.empty()) throw std::invalid_argument("empty section id");
        if (type == "parameter") {
            FieldDefinition field; field.id = id;
            field.source = take(values, "source"); field.unit = take(values, "unit");
            field.protocol = number<std::uint32_t>(take(values, "protocol"));
            field.channel = number<std::uint32_t>(take(values, "channel"));
            field.bit_offset = number<std::uint32_t>(take(values, "bit_offset"));
            field.bit_width = number<std::uint32_t>(take(values, "bit_width"));
            const auto order = take(values, "byte_order");
            if (order != "little" && order != "big") throw std::invalid_argument("byte_order must be little or big");
            field.big_endian = order == "big";
            const auto encoding = take(values, "type");
            if (encoding == "signed") field.type = FieldType::Signed;
            else if (encoding == "unsigned") field.type = FieldType::Unsigned;
            else if (encoding == "float64") field.type = FieldType::Float64;
            else if (encoding == "bool") field.type = FieldType::Boolean;
            else if (encoding == "enum") field.type = FieldType::Enumeration;
            else throw std::invalid_argument("unsupported field type");
            field.scale = number<double>(take(values, "scale", "1"));
            field.offset = number<double>(take(values, "offset", "0"));
            field.max_age_ns = milliseconds(take(values, "max_age_ms"));
            field.sequence_step = number<std::uint64_t>(take(values, "sequence_step", "0"));
            if (values.contains("valid_bit")) field.valid_bit = number<std::uint32_t>(take(values, "valid_bit"));
            if (field.type == FieldType::Enumeration) {
                std::istringstream items(take(values, "enum_values")); std::string item;
                while (std::getline(items, item, ',')) {
                    const auto colon = item.find(':');
                    if (colon == std::string::npos) throw std::invalid_argument("enum_values requires code:label pairs");
                    const auto code = number<std::int64_t>(trim(item.substr(0, colon)));
                    const auto label = trim(item.substr(colon + 1));
                    if (code < 0 || label.empty() || !field.enum_values.emplace(code, label).second)
                        throw std::invalid_argument("invalid/duplicate enum code");
                }
            }
            if (!field.bit_width || field.bit_width > 64 || std::uint64_t(field.bit_offset) + field.bit_width > BUS_MAX_PAYLOAD * 8ull ||
                (field.valid_bit && *field.valid_bit >= BUS_MAX_PAYLOAD * 8u) || !field.max_age_ns)
                throw std::invalid_argument("field exceeds payload bounds or max_age is zero");
            if (field.type == FieldType::Float64 && (field.bit_width != 64 || field.bit_offset % 8 != 0))
                throw std::invalid_argument("float64 requires aligned 64 bits");
            if (field.type == FieldType::Boolean && field.bit_width != 1) throw std::invalid_argument("bool requires one bit");
            if ((field.type == FieldType::Boolean || field.type == FieldType::Enumeration) && (field.scale != 1 || field.offset != 0))
                throw std::invalid_argument("bool and enum cannot be scaled");
            if (field.type == FieldType::Enumeration) {
                if (field.bit_width > 63 || field.enum_values.empty()) throw std::invalid_argument("enum requires 1..63 bits and labels");
                for (const auto& [code, label] : field.enum_values) {
                    (void)label;
                    if (static_cast<std::uint64_t>(code) >= (1ull << field.bit_width)) throw std::invalid_argument("enum code exceeds width");
                }
            }
            field_ids.insert(id); config.fields.push_back(std::move(field));
        } else if (type == "clock") {
            ClockDefinition clock; clock.source = id;
            clock.group = take(values, "group"); clock.domain = number<std::uint32_t>(take(values, "domain"));
            clock.offset_ns = number<std::int64_t>(take(values, "offset_ns", "0"));
            clock.uncertainty_ns = number<std::uint64_t>(take(values, "uncertainty_ns", "0"));
            if (clock.uncertainty_ns > 86400000000000ull) throw std::invalid_argument("clock uncertainty exceeds 24h");
            config.clocks.push_back(std::move(clock));
        } else if (type == "consistency") {
            ConsistencyDefinition rule; rule.id = id;
            rule.left = selector(values, "left"); rule.right = selector(values, "right");
            rule.tolerance = number<double>(take(values, "tolerance"));
            rule.window_ns = milliseconds(take(values, "window_ms"));
            if (rule.tolerance < 0 || !rule.window_ns) throw std::invalid_argument("invalid consistency rule");
            config.consistency.push_back(std::move(rule));
        } else if (type == "response") {
            ResponseDefinition rule; rule.id = id;
            rule.command = selector(values, "command"); rule.response = selector(values, "response");
            rule.command_threshold = number<double>(take(values, "command_threshold"));
            rule.response_threshold = number<double>(take(values, "response_threshold"));
            rule.max_delay_ns = milliseconds(take(values, "max_delay_ms"));
            if (!rule.max_delay_ns) throw std::invalid_argument("response window must be positive");
            config.responses.push_back(std::move(rule));
        } else if (type == "reorder") {
            ReorderDefinition definition; definition.group = id;
            definition.window_ns = milliseconds(take(values, "window_ms"));
            if (!definition.window_ns) throw std::invalid_argument("reorder window must be positive");
            config.reorder.push_back(std::move(definition));
        } else if (type == "dedup") {
            DeduplicationDefinition definition; definition.id = id;
            definition.source = take(values, "source");
            definition.parameter = take(values, "parameter");
            definition.window_ns = milliseconds(take(values, "window_ms"));
            if (!definition.window_ns) throw std::invalid_argument("dedup window must be positive");
            config.dedup.push_back(std::move(definition));
        } else if (type == "fusion") {
            FusionDefinition definition; definition.id = id;
            const auto count = number<std::size_t>(take(values, "count"));
            if (count < 2 || count > 16) throw std::invalid_argument("fusion requires 2..16 channels");
            for (std::size_t index = 1; index <= count; ++index) {
                FusionChannel channel;
                channel.source = take(values, "source_" + std::to_string(index));
                channel.parameter = take(values, "parameter_" + std::to_string(index));
                definition.channels.push_back(std::move(channel));
            }
            const auto method = take(values, "method");
            if (method == "mean") definition.method = FusionMethod::Mean;
            else if (method == "median") definition.method = FusionMethod::Median;
            else if (method == "vote") definition.method = FusionMethod::Vote;
            else throw std::invalid_argument("unsupported fusion method: " + method);
            definition.tolerance = number<double>(take(values, "tolerance"));
            definition.window_ns = milliseconds(take(values, "window_ms"));
            if (definition.tolerance < 0 || !definition.window_ns) throw std::invalid_argument("invalid fusion rule");
            config.fusion.push_back(std::move(definition));
        } else throw std::invalid_argument("unknown section type: " + type);
        empty(values);
    }
    if (config.fields.empty()) throw std::invalid_argument("dictionary has no parameters");
    auto validate_selector = [&](const Selector& selected) {
        for (const auto& field : config.fields)
            if (field.id == selected.parameter && (field.source == selected.source || field.source == "*")) return;
        throw std::invalid_argument("rule refers to unknown source/parameter: " + selected.source + "/" + selected.parameter);
    };
    auto distinct = [](const Selector& left, const Selector& right) {
        if (left.source == right.source && left.parameter == right.parameter) throw std::invalid_argument("rule requires distinct parameters");
    };
    for (const auto& rule : config.consistency) { validate_selector(rule.left); validate_selector(rule.right); distinct(rule.left, rule.right); }
    for (const auto& rule : config.responses) { validate_selector(rule.command); validate_selector(rule.response); distinct(rule.command, rule.response); }
    for (const auto& definition : config.reorder) {
        bool known = false;
        for (const auto& clock : config.clocks) if (clock.group == definition.group) { known = true; break; }
        if (!known) throw std::invalid_argument("reorder refers to unknown clock group: " + definition.group);
    }
    for (const auto& definition : config.dedup) validate_selector(Selector{definition.source, definition.parameter});
    for (const auto& rule : config.fusion) {
        for (const auto& channel : rule.channels) validate_selector(Selector{channel.source, channel.parameter});
        for (std::size_t a = 0; a < rule.channels.size(); ++a)
            for (std::size_t b = a + 1; b < rule.channels.size(); ++b)
                if (rule.channels[a].source == rule.channels[b].source && rule.channels[a].parameter == rule.channels[b].parameter)
                    throw std::invalid_argument("fusion channels must be distinct");
    }
    return config;
}
}
