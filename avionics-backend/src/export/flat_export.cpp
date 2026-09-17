#include "core/flat_export.hpp"
#include <algorithm>
#include <cmath>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <type_traits>
#include <variant>

namespace avionics {
namespace {
const char* statusText(const AlignedSample& sample) {
    if (!sample.usable()) return "invalid";
    return sample.sample.valid ? "ok" : "invalid";
}
std::string csvField(const std::string& value) {
    if (value.find_first_of(",\"\n\r") == std::string::npos) return value;
    std::string out = "\"";
    for (const char character : value) out += (character == '"') ? "\"\"" : std::string(1, character);
    out += "\"";
    return out;
}
std::string jsonEscape(const std::string& value) {
    std::string out;
    for (const char character : value) {
        switch (character) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default: out += character;
        }
    }
    return out;
}
std::string numberText(double value) {
    std::ostringstream stream; stream << std::setprecision(17) << value; return stream.str();
}
}

FlatExporter::FlatExporter(FlatExportOptions options, const BackendConfiguration& config)
    : options_(std::move(options)), config_(config) {
    std::filesystem::create_directories(options_.directory);
    if (options_.csv) {
        csv_.open(options_.directory / "parameters_flat.csv", std::ios::binary | std::ios::trunc);
        if (!csv_) throw std::runtime_error("cannot create flat CSV export");
        csv_ << "run_id,session_id,source_id,channel,protocol_id,parameter_id,unit,value_type,value_num,value_str,"
                "validity,status,observation_time_ns,available_time_ns,ingest_time_ns,clock_group,offset_ns,"
                "uncertainty_ns,nominal_period_ns,sequence,sequence_step,raw_record_index,origin_source,"
                "origin_generation,decoder_version,config_version\n";
    }
    if (options_.jsonl) {
        jsonl_.open(options_.directory / "parameters_flat.jsonl", std::ios::binary | std::ios::trunc);
        if (!jsonl_) throw std::runtime_error("cannot create flat JSONL export");
    }
}
FlatExporter::~FlatExporter() { try { finish(); } catch (...) {} }

std::int64_t FlatExporter::offsetFor(const ParameterSample& sample) const {
    for (const auto& clock : config_.clocks)
        if (clock.source == sample.source && clock.domain == sample.clock_domain) return clock.offset_ns;
    return 0;
}

void FlatExporter::writeRow(const AlignedSample& sample) {
    const auto& s = sample.sample;
    std::string type, number, text;
    std::visit([&](const auto& entry) {
        using T = std::decay_t<decltype(entry)>;
        if constexpr (std::is_same_v<T, std::int64_t>) { type = "int64"; number = std::to_string(entry); }
        else if constexpr (std::is_same_v<T, std::uint64_t>) { type = "uint64"; number = std::to_string(entry); }
        else if constexpr (std::is_same_v<T, double>) { type = "double"; number = std::isfinite(entry) ? numberText(entry) : ""; }
        else if constexpr (std::is_same_v<T, bool>) { type = "bool"; number = entry ? "1" : "0"; text = entry ? "true" : "false"; }
        else { type = "enum"; number = std::to_string(entry.code); text = entry.label; }
    }, s.value);
    const auto observation = s.capture_time_ns;
    const auto now = options_.mode == "online" ? monotonicNowNs() : s.ingest_time_ns;
    const auto available = std::max({observation, s.ingest_time_ns, now});
    const auto offset = offsetFor(s);
    const char* status = statusText(sample);
    const auto channel = std::to_string(s.channel);
    const auto protocol = std::to_string(s.protocol);
    const auto observation_text = std::to_string(observation);
    const auto available_text = std::to_string(available);
    const auto ingest_text = std::to_string(s.ingest_time_ns);
    const auto offset_text = std::to_string(offset);
    const auto uncertainty_text = std::to_string(sample.uncertainty_ns);
    const auto period_text = std::to_string(s.nominal_period_ns);
    const auto sequence = std::to_string(s.sequence);
    const auto step = std::to_string(s.sequence_step);
    const auto record = std::to_string(s.raw_record_index);
    const auto origin_generation = std::to_string(s.origin_generation);
    if (csv_) {
        std::vector<std::string> fields{
            options_.run_id, options_.session_id, s.source, channel, protocol, s.parameter_id, s.unit, type, number,
            text, s.valid ? "true" : "false", status, observation_text, available_text, ingest_text, sample.time_group,
            offset_text, uncertainty_text, period_text, sequence, step, record, s.origin_source, origin_generation,
            s.decoder_version, s.config_version};
        for (std::size_t i = 0; i < fields.size(); ++i) csv_ << (i ? "," : "") << csvField(fields[i]);
        csv_ << '\n';
    }
    if (jsonl_) {
        jsonl_ << "{\"run_id\":\"" << jsonEscape(options_.run_id) << "\",\"session_id\":\"" << jsonEscape(options_.session_id)
            << "\",\"source_id\":\"" << jsonEscape(s.source) << "\",\"channel\":" << channel
            << ",\"protocol_id\":" << protocol << ",\"parameter_id\":\"" << jsonEscape(s.parameter_id)
            << "\",\"unit\":\"" << jsonEscape(s.unit) << "\",\"value_type\":\"" << type << "\",\"value_num\":"
            << (number.empty() ? "null" : number) << ",\"value_str\":" << (text.empty() ? "null" : "\"" + jsonEscape(text) + "\"")
            << ",\"validity\":" << (s.valid ? "true" : "false") << ",\"status\":\"" << status
            << "\",\"observation_time_ns\":" << observation_text << ",\"available_time_ns\":" << available_text
            << ",\"ingest_time_ns\":" << ingest_text << ",\"clock_group\":\"" << jsonEscape(sample.time_group)
            << "\",\"offset_ns\":" << offset_text << ",\"uncertainty_ns\":" << uncertainty_text
            << ",\"nominal_period_ns\":" << period_text << ",\"sequence\":" << sequence
            << ",\"sequence_step\":" << step << ",\"raw_record_index\":" << record
            << ",\"origin_source\":\"" << jsonEscape(s.origin_source) << "\",\"origin_generation\":" << origin_generation
            << ",\"decoder_version\":\"" << jsonEscape(s.decoder_version) << "\",\"config_version\":\""
            << jsonEscape(s.config_version) << "\"}\n";
    }
}

void FlatExporter::observe(const AlignedSample& sample) {
    writeRow(sample);
    ++samples_;
    ++counts_[statusText(sample)];
    const auto time = sample.sample.capture_time_ns;
    if (!has_time_) { min_time_ = max_time_ = time; has_time_ = true; }
    else { min_time_ = std::min(min_time_, time); max_time_ = std::max(max_time_, time); }
}

void FlatExporter::writeMetadata() {
    std::ofstream out(options_.directory / "export_metadata.json", std::ios::binary | std::ios::trunc);
    if (!out) throw std::runtime_error("cannot write export metadata");
    const auto count = [this](const std::string& key) { const auto it = counts_.find(key); return it == counts_.end() ? 0ull : it->second; };
    out << "{\"schema\":1,\"format\":\"flat-1\",\"mode\":\"" << options_.mode << "\",\"run_id\":\""
        << jsonEscape(options_.run_id) << "\",\"session_id\":\"" << jsonEscape(options_.session_id)
        << "\",\"config_version\":\"" << jsonEscape(config_.version) << "\",\"samples\":" << samples_
        << ",\"counts\":{\"ok\":" << count("ok") << ",\"invalid\":" << count("invalid")
        << ",\"expected_absent\":" << count("expected_absent") << ",\"not_due\":" << count("not_due")
        << "},\"observation_time_range_ns\":{\"min\":" << min_time_ << ",\"max\":" << max_time_ << "}}\n";
    out.flush();
    if (!out) throw std::runtime_error("export metadata write failed");
}

void FlatExporter::finish() {
    if (finished_) return;
    finished_ = true;
    if (csv_) csv_.flush();
    if (jsonl_) jsonl_.flush();
    writeMetadata();
}
}
