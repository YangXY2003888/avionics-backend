#include "core/configuration.hpp"
#include "core/field_value.hpp"
#include "plugin_api/bus_plugin.h"
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <utility>

namespace avionics {
namespace {
struct CanCapture {
    std::uint32_t id{};
    const std::uint8_t* data{};
    std::size_t size{};
};
std::optional<CanCapture> parseCanCapture(const std::vector<std::uint8_t>& payload) {
    if (payload.size() < 6) return std::nullopt;
    const auto id = std::uint32_t(payload[0]) | (std::uint32_t(payload[1]) << 8) |
        (std::uint32_t(payload[2]) << 16) | (std::uint32_t(payload[3]) << 24);
    const auto dlc = payload[5];
    if (dlc > 8 || payload.size() < 6u + dlc) return std::nullopt;
    return CanCapture{id, payload.data() + 6, dlc};
}
}
std::vector<ParameterSample> DictionaryDecoder::decode(const RawFrame& frame) {
    std::vector<ParameterSample> samples;
    for (const auto& field : config_.fields) {
        if (field.protocol != frame.protocol || field.channel != frame.channel ||
            (field.source != "*" && field.source != frame.source)) continue;
        std::span<const std::uint8_t> source = frame.payload;
        if (field.can_id) {
            const auto captured = parseCanCapture(frame.payload);
            if (!captured || captured->id != *field.can_id) continue;
            if (field.can_match && !(captured->size > field.can_match->first &&
                captured->data[field.can_match->first] == field.can_match->second)) continue;
            source = std::span<const std::uint8_t>(captured->data, captured->size);
        }
        ParameterSample sample;
        sample.source = frame.source; sample.parameter_id = field.id; sample.unit = field.unit;
        sample.generation = frame.generation; sample.origin_generation = frame.origin_generation;
        sample.capture_time_ns = frame.capture_time_ns; sample.ingest_time_ns = frame.ingest_time_ns;
        sample.clock_domain = frame.clock_domain; sample.raw_record_index = frame.record_index;
        sample.protocol = frame.protocol; sample.channel = frame.channel; sample.origin_source = frame.origin_source;
        sample.config_version = config_.version; sample.nominal_period_ns = field.nominal_period_ns;
        sample.sequence = frame.sequence; sample.sequence_step = field.sequence_step; sample.max_age_ns = field.max_age_ns;
        sample.decoder_version = config_.version; sample.valid = (frame.flags & BUS_FRAME_INVALID) == 0;
        try { decodeFieldValue(field, source, sample.value, sample.valid); }
        catch (const std::out_of_range&) { sample.valid = false; sample.value = 0.0; }
        samples.push_back(std::move(sample));
    }
    return samples;
}
}
