#include "core/analysis.hpp"
#include "plugin_api/bus_plugin.h"
#include <bit>
#include <stdexcept>

namespace avionics {
std::vector<ParameterSample> SimulatedDecoder::decode(const RawFrame& frame) {
    if (frame.protocol != BUS_PROTOCOL_SIMULATED) return {};
    if (frame.payload.size() != 8) throw std::invalid_argument("simulated payload must contain 8 bytes");
    std::uint64_t bits{};
    for (std::size_t i = 0; i < 8; ++i) bits |= std::uint64_t(frame.payload[i]) << (8 * i);
    ParameterSample sample;
    sample.parameter_id = "demo.temperature.ch" + std::to_string(frame.channel);
    // Replays retain original source identity in the parameter stream; generation
    // remains the NEW replay session generation to prevent cross-session joining.
    sample.source = frame.origin_source.empty() ? frame.source : frame.source + "/" + frame.origin_source;
    sample.generation = frame.generation;
    sample.origin_generation = frame.origin_generation;
    sample.unit = "degC"; sample.decoder_version = "simulated-v1";
    sample.capture_time_ns = frame.capture_time_ns; sample.ingest_time_ns = frame.ingest_time_ns;
    sample.clock_domain = frame.clock_domain; sample.raw_record_index = frame.record_index;
    sample.protocol = frame.protocol; sample.channel = frame.channel; sample.origin_source = frame.origin_source;
    sample.value = static_cast<double>(std::bit_cast<std::int64_t>(bits)) / 1000.0;
    sample.valid = (frame.flags & BUS_FRAME_INVALID) == 0;
    return {std::move(sample)};
}
}
