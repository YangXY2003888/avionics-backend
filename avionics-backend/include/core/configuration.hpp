#pragma once
#include "core/frame_pipeline.hpp"
#include <optional>
#include <utility>

namespace avionics {
enum class FieldType { Signed, Unsigned, Float64, Boolean, Enumeration };
struct FieldDefinition {
    std::string id, source, unit;
    std::uint32_t protocol{}, channel{}, bit_offset{}, bit_width{};
    bool big_endian{};
    FieldType type{};
    double scale{1}, offset{};
    std::optional<std::uint32_t> valid_bit;
    std::map<std::int64_t, std::string> enum_values;
    std::uint64_t max_age_ns{}, sequence_step{};
    // When set, the parameter is decoded from a captured CAN frame's data bytes.
    std::optional<std::uint32_t> can_id;
    std::optional<std::pair<std::uint32_t, std::uint32_t>> can_match;
    // Declared update schedule. Zero means unknown; it is never guessed.
    std::uint64_t nominal_period_ns{}, phase_offset_ns{}, epoch_ns{};
    std::uint64_t jitter_tolerance_ns{}, arrival_delay_tolerance_ns{};
};
struct ClockDefinition {
    std::string source, group;
    std::uint32_t domain{};
    std::int64_t offset_ns{};
    std::uint64_t uncertainty_ns{};
};
struct Selector {
    std::string source, parameter;
    bool matches(const ParameterSample& sample) const { return source == sample.source && parameter == sample.parameter_id; }
};
struct ConsistencyDefinition {
    std::string id;
    Selector left, right;
    double tolerance{};
    std::uint64_t window_ns{};
};
struct ResponseDefinition {
    std::string id;
    Selector command, response;
    double command_threshold{}, response_threshold{};
    std::uint64_t max_delay_ns{};
};
struct ReorderDefinition {
    std::string group;
    std::uint64_t window_ns{};
};
enum class FusionMethod { Mean, Median, Vote };
struct FusionChannel {
    std::string source, parameter;
};
struct FusionDefinition {
    std::string id;
    std::vector<FusionChannel> channels;
    FusionMethod method{FusionMethod::Mean};
    double tolerance{};
    std::uint64_t window_ns{};
};
struct DeduplicationDefinition {
    std::string id, source, parameter;
    std::uint64_t window_ns{};
};
struct BackendConfiguration {
    std::string version;
    std::vector<FieldDefinition> fields;
    std::vector<ClockDefinition> clocks;
    std::vector<ConsistencyDefinition> consistency;
    std::vector<ResponseDefinition> responses;
    std::vector<ReorderDefinition> reorder;
    std::vector<DeduplicationDefinition> dedup;
    std::vector<FusionDefinition> fusion;
    static BackendConfiguration load(const std::filesystem::path&);
};
class DictionaryDecoder final : public IParameterDecoder {
public:
    explicit DictionaryDecoder(BackendConfiguration configuration) : config_(std::move(configuration)) {}
    std::vector<ParameterSample> decode(const RawFrame&) override;
private:
    BackendConfiguration config_;
};
}
