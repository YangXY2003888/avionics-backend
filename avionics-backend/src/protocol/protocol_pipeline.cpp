#include "core/field_value.hpp"
#include "core/protocol_impl.hpp"
#include "plugin_api/bus_plugin.h"
#include <iterator>
#include <stdexcept>
#include <utility>

namespace avionics {
DictionaryMessageDecoder::DictionaryMessageDecoder(BackendConfiguration configuration)
    : config_(std::move(configuration)) {}

std::vector<ParameterSample> DictionaryMessageDecoder::decode(const ProtocolMessage& message) {
    std::vector<ParameterSample> samples;
    const auto& address = message.stream.address;
    const auto source = address.origin_source.empty() ? address.source : address.source + "/" + address.origin_source;
    std::uint64_t record = 0, sequence = 0, capture = 0;
    std::uint32_t clock_domain = 0;
    if (!message.evidence.empty()) {
        record = message.evidence.front().record_index;
        sequence = message.evidence.front().sequence;
        capture = message.evidence.front().capture_time_ns;
        clock_domain = message.evidence.front().clock_domain;
    }
    for (const auto& field : config_.fields) {
        if (field.protocol != message.protocol || field.channel != address.channel) continue;
        if (field.source != "*" && field.source != address.source) continue;
        ParameterSample sample;
        sample.source = source; sample.parameter_id = field.id; sample.unit = field.unit;
        sample.generation = message.stream.generation; sample.origin_generation = message.stream.origin_generation;
        sample.capture_time_ns = capture; sample.ingest_time_ns = capture;
        sample.clock_domain = clock_domain; sample.raw_record_index = record;
        sample.protocol = message.protocol; sample.channel = address.channel; sample.origin_source = address.origin_source;
        sample.config_version = config_.version; sample.nominal_period_ns = field.nominal_period_ns;
        sample.sequence = sequence; sample.sequence_step = field.sequence_step; sample.max_age_ns = field.max_age_ns;
        sample.decoder_version = config_.version; sample.valid = (message.flags & BUS_FRAME_INVALID) == 0;
        try { decodeFieldValue(field, message.payload, sample.value, sample.valid); }
        catch (const std::out_of_range&) { sample.valid = false; sample.value = 0.0; }
        samples.push_back(std::move(sample));
    }
    return samples;
}

ProtocolPipelineDecoder::ProtocolPipelineDecoder(std::unique_ptr<ProtocolRouter> router,
    std::unique_ptr<IProtocolMessageDecoder> message_decoder)
    : router_(std::move(router)), message_decoder_(std::move(message_decoder)) {
    if (!router_ || !message_decoder_) throw std::invalid_argument("pipeline decoder requires router and message decoder");
}

std::vector<ParameterSample> ProtocolPipelineDecoder::decode(const RawFrame& frame) {
    std::vector<ParameterSample> samples;
    const auto routed = router_->route(frame);
    if (routed.parsing.status != ProtocolParseStatus::Complete) return samples;
    for (const auto& message : routed.parsing.messages) {
        auto produced = message_decoder_->decode(message);
        samples.insert(samples.end(), std::make_move_iterator(produced.begin()), std::make_move_iterator(produced.end()));
    }
    return samples;
}
}
