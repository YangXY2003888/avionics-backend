#pragma once
#include <chrono>
#include <cstdint>
#include <string>
#include <variant>
#include <vector>

namespace avionics {
inline std::uint64_t monotonicNowNs() noexcept {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}
struct RawFrame {
    std::string source;
    std::uint64_t generation{};
    std::uint32_t protocol{}, channel{}, flags{}, clock_domain{};
    std::uint64_t capture_time_ns{}, ingest_time_ns{}, sequence{}, record_index{};
    std::string origin_source;
    std::uint64_t origin_generation{}, origin_record_index{};
    std::vector<std::uint8_t> payload;
};
struct EnumValue { std::int64_t code{}; std::string label; };
using ParameterValue = std::variant<std::int64_t, std::uint64_t, double, bool, EnumValue>;
struct ParameterSample {
    std::string parameter_id, source, unit, decoder_version;
    std::uint64_t generation{}, capture_time_ns{}, ingest_time_ns{}, raw_record_index{};
    std::uint64_t origin_generation{};
    std::uint64_t sequence{}, max_age_ns{}, sequence_step{};
    std::uint32_t clock_domain{};
    std::uint32_t protocol{}, channel{};
    std::string origin_source;
    std::string config_version;
    std::uint64_t nominal_period_ns{};
    ParameterValue value{0.0};
    bool valid{};
};
struct AnalysisEvent {
    std::string rule_id, source, parameter_id, description;
    std::uint64_t generation{}, time_ns{}, raw_record_index{};
    double value{};
};
}
