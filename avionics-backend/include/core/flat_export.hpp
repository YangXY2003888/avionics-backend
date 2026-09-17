#pragma once
#include "core/processing.hpp"
#include "core/configuration.hpp"
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

namespace avionics {
// Flat, one-row-per-record export intended as a stable interface for
// downstream analysis. Field set and time semantics are documented in
// docs/interface-for-analysis.md.
struct FlatExportOptions {
    std::filesystem::path directory;
    std::string run_id, session_id;
    std::string mode{"offline"}; // "offline" or "online"
    bool csv{true}, jsonl{true};
};
class FlatExporter {
public:
    FlatExporter(FlatExportOptions options, const BackendConfiguration& config);
    ~FlatExporter();
    FlatExporter(const FlatExporter&) = delete;
    FlatExporter& operator=(const FlatExporter&) = delete;
    void observe(const AlignedSample& sample);
    void finish();
    std::map<std::string, std::uint64_t> statusCounts() const { return counts_; }
    std::uint64_t samples() const { return samples_; }
private:
    struct Stream {
        std::string source, parameter, unit, clock_group, decoder_version, config_version, origin_source;
        std::uint32_t channel{}, protocol{};
        std::uint64_t period{}, phase{}, epoch{}, jitter{}, arrival{};
        std::int64_t offset{};
        std::uint64_t uncertainty{};
        std::uint64_t first_observation{};
        bool has_first{};
        std::vector<std::pair<std::uint64_t, bool>> observations; // time, valid
    };
    struct Row {
        std::string status, flags;
        std::string source, parameter, unit;
        std::optional<std::uint32_t> channel, protocol;
        std::string value_type, value_num, value_str;
        std::optional<bool> validity;
        std::optional<std::uint64_t> observation, ingest, available, uncertainty, nominal;
        std::optional<std::int64_t> offset;
        std::optional<std::uint64_t> sequence, sequence_step, raw_record_index, origin_generation;
        std::string clock_group, origin_source, decoder_version, config_version;
    };
    const FieldDefinition* fieldFor(const std::string& source, const std::string& parameter) const;
    Stream& streamFor(const ParameterSample& sample);
    std::string writeObservationRow(const AlignedSample& sample, const Stream& stream);
    void writeRow(const Row& row);
    void writeCompletionMetadata();
    FlatExportOptions options_;
    const BackendConfiguration& config_;
    std::ofstream csv_, jsonl_;
    std::map<std::pair<std::string, std::string>, Stream> streams_;
    std::map<std::string, std::uint64_t> group_watermarks_;
    std::uint64_t samples_{};
    std::uint64_t min_time_{}, max_time_{};
    bool has_time_{};
    std::map<std::string, std::uint64_t> counts_;
    bool finished_{};
};
}
