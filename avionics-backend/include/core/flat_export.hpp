#pragma once
#include "core/processing.hpp"
#include "core/configuration.hpp"
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>

namespace avionics {
// Flat, one-row-per-observation export intended as a stable interface for
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
    void writeRow(const AlignedSample& sample);
    std::int64_t offsetFor(const ParameterSample& sample) const;
    void writeMetadata();
    FlatExportOptions options_;
    const BackendConfiguration& config_;
    std::ofstream csv_, jsonl_;
    std::uint64_t samples_{};
    std::uint64_t min_time_{}, max_time_{};
    bool has_time_{};
    std::map<std::string, std::uint64_t> counts_;
    bool finished_{};
};
}
