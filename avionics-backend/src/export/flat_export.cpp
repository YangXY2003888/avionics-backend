#include "core/flat_export.hpp"
#include <algorithm>
#include <cmath>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <type_traits>
#include <variant>

namespace avionics {
namespace {
constexpr std::uint64_t max_schedule_slots = 10000000ull;
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
std::string opt(const std::optional<std::uint64_t>& value) { return value ? std::to_string(*value) : std::string(); }
std::string opt(const std::optional<std::int64_t>& value) { return value ? std::to_string(*value) : std::string(); }
std::string opt(const std::optional<std::uint32_t>& value) { return value ? std::to_string(*value) : std::string(); }
std::string opt(const std::optional<bool>& value) { return value ? (*value ? "true" : "false") : std::string(); }
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
                "origin_generation,decoder_version,config_version,flags\n";
    }
    if (options_.jsonl) {
        jsonl_.open(options_.directory / "parameters_flat.jsonl", std::ios::binary | std::ios::trunc);
        if (!jsonl_) throw std::runtime_error("cannot create flat JSONL export");
    }
}
FlatExporter::~FlatExporter() { try { finish(); } catch (...) {} }

const FieldDefinition* FlatExporter::fieldFor(const std::string& source, const std::string& parameter) const {
    for (const auto& field : config_.fields)
        if (field.id == parameter && (field.source == source || field.source == "*")) return &field;
    return nullptr;
}

FlatExporter::Stream& FlatExporter::streamFor(const ParameterSample& sample) {
    auto& stream = streams_[{sample.source, sample.parameter_id}];
    if (stream.source.empty()) {
        stream.source = sample.source; stream.parameter = sample.parameter_id; stream.unit = sample.unit;
        stream.channel = sample.channel; stream.protocol = sample.protocol;
        stream.decoder_version = sample.decoder_version; stream.config_version = sample.config_version;
        stream.origin_source = sample.origin_source;
        for (const auto& clock : config_.clocks)
            if (clock.source == sample.source && clock.domain == sample.clock_domain) {
                stream.clock_group = clock.group; stream.offset = clock.offset_ns; stream.uncertainty = clock.uncertainty_ns;
            }
        if (const auto* field = fieldFor(sample.source, sample.parameter_id)) {
            stream.period = field->nominal_period_ns; stream.phase = field->phase_offset_ns; stream.epoch = field->epoch_ns;
            stream.jitter = field->jitter_tolerance_ns; stream.arrival = field->arrival_delay_tolerance_ns;
            stream.unit = field->unit;
        }
    }
    return stream;
}

void FlatExporter::writeRow(const Row& row) {
    if (csv_) {
        const std::vector<std::string> fields{
            options_.run_id, options_.session_id, row.source, opt(row.channel), opt(row.protocol), row.parameter,
            row.unit, row.value_type, row.value_num, row.value_str, opt(row.validity), row.status,
            opt(row.observation), opt(row.available), opt(row.ingest), row.clock_group, opt(row.offset),
            opt(row.uncertainty), opt(row.nominal), opt(row.sequence), opt(row.sequence_step), opt(row.raw_record_index),
            row.origin_source, opt(row.origin_generation), row.decoder_version, row.config_version, row.flags};
        for (std::size_t i = 0; i < fields.size(); ++i) csv_ << (i ? "," : "") << csvField(fields[i]);
        csv_ << '\n';
    }
    if (jsonl_) {
        const auto text = [](const std::string& value, bool as_string) {
            return value.empty() ? std::string("null") : (as_string ? "\"" + jsonEscape(value) + "\"" : value);
        };
        jsonl_ << "{\"run_id\":\"" << jsonEscape(options_.run_id) << "\",\"session_id\":\"" << jsonEscape(options_.session_id)
            << "\",\"source_id\":\"" << jsonEscape(row.source) << "\",\"channel\":" << text(opt(row.channel), false)
            << ",\"protocol_id\":" << text(opt(row.protocol), false) << ",\"parameter_id\":\"" << jsonEscape(row.parameter)
            << "\",\"unit\":\"" << jsonEscape(row.unit) << "\",\"value_type\":" << text(row.value_type, true)
            << ",\"value_num\":" << text(row.value_num, false) << ",\"value_str\":" << text(row.value_str, true)
            << ",\"validity\":" << (row.validity ? (*row.validity ? "true" : "false") : "null") << ",\"status\":\""
            << jsonEscape(row.status) << "\",\"observation_time_ns\":" << text(opt(row.observation), false)
            << ",\"available_time_ns\":" << text(opt(row.available), false) << ",\"ingest_time_ns\":" << text(opt(row.ingest), false)
            << ",\"clock_group\":\"" << jsonEscape(row.clock_group) << "\",\"offset_ns\":" << text(opt(row.offset), false)
            << ",\"uncertainty_ns\":" << text(opt(row.uncertainty), false) << ",\"nominal_period_ns\":"
            << text(opt(row.nominal), false) << ",\"sequence\":" << text(opt(row.sequence), false)
            << ",\"sequence_step\":" << text(opt(row.sequence_step), false) << ",\"raw_record_index\":"
            << text(opt(row.raw_record_index), false) << ",\"origin_source\":\"" << jsonEscape(row.origin_source)
            << "\",\"origin_generation\":" << text(opt(row.origin_generation), false) << ",\"decoder_version\":\""
            << jsonEscape(row.decoder_version) << "\",\"config_version\":\"" << jsonEscape(row.config_version)
            << "\",\"flags\":\"" << jsonEscape(row.flags) << "\"}\n";
    }
}

std::string FlatExporter::writeObservationRow(const AlignedSample& sample, const Stream& stream) {
    const auto& s = sample.sample;
    Row row;
    row.source = s.source; row.parameter = s.parameter_id; row.unit = s.unit;
    row.channel = s.channel; row.protocol = s.protocol;
    std::string type;
    std::visit([&](const auto& entry) {
        using T = std::decay_t<decltype(entry)>;
        if constexpr (std::is_same_v<T, std::int64_t>) { type = "int64"; row.value_num = std::to_string(entry); }
        else if constexpr (std::is_same_v<T, std::uint64_t>) { type = "uint64"; row.value_num = std::to_string(entry); }
        else if constexpr (std::is_same_v<T, double>) { type = "double"; row.value_num = std::isfinite(entry) ? numberText(entry) : ""; }
        else if constexpr (std::is_same_v<T, bool>) { type = "bool"; row.value_num = entry ? "1" : "0"; row.value_str = entry ? "true" : "false"; }
        else { type = "enum"; row.value_num = std::to_string(entry.code); row.value_str = entry.label; }
    }, s.value);
    row.value_type = type;
    row.validity = s.valid;
    const auto observation = s.capture_time_ns;
    const auto now = options_.mode == "online" ? monotonicNowNs() : s.ingest_time_ns;
    row.observation = observation;
    row.ingest = s.ingest_time_ns;
    row.available = std::max({observation, s.ingest_time_ns, now});
    row.clock_group = sample.time_group;
    row.offset = stream.offset; row.uncertainty = stream.uncertainty; row.nominal = stream.period;
    row.sequence = s.sequence; row.sequence_step = s.sequence_step; row.raw_record_index = s.raw_record_index;
    row.origin_source = s.origin_source; row.origin_generation = s.origin_generation;
    row.decoder_version = s.decoder_version; row.config_version = s.config_version;
    if (stream.period == 0) {
        row.status = "unknown_schedule";
    } else if (!s.valid) {
        row.status = "invalid";
    } else {
        row.status = "ok";
        const auto start = (stream.epoch != 0 ? stream.epoch : stream.first_observation) + stream.phase;
        if (observation < start) {
            row.flags = "off_schedule";
        } else {
            const auto ratio = static_cast<long double>(observation - start) / static_cast<long double>(stream.period);
            const auto nearest = static_cast<std::uint64_t>(std::llround(ratio));
            const auto slot = start + nearest * stream.period;
            const auto difference = observation > slot ? observation - slot : slot - observation;
            if (difference > stream.period / 2) row.flags = "off_schedule";
        }
    }
    writeRow(row);
    return row.status;
}

void FlatExporter::observe(const AlignedSample& sample) {
    auto& stream = streamFor(sample.sample);
    const auto time = sample.sample.capture_time_ns;
    if (!stream.has_first) { stream.first_observation = time; stream.has_first = true; }
    stream.observations.emplace_back(time, sample.sample.valid);
    auto& watermark = group_watermarks_[sample.time_group];
    watermark = std::max(watermark, time);
    if (!has_time_) { min_time_ = max_time_ = time; has_time_ = true; }
    else { min_time_ = std::min(min_time_, time); max_time_ = std::max(max_time_, time); }
    ++samples_;
    const auto status = writeObservationRow(sample, stream);
    ++counts_[status];
}

void FlatExporter::finish() {
    if (finished_) return;
    finished_ = true;
    for (auto& [key, stream] : streams_) {
        (void)key;
        std::sort(stream.observations.begin(), stream.observations.end());
        if (stream.period == 0 || !stream.has_first) continue;
        const auto start = (stream.epoch != 0 ? stream.epoch : stream.first_observation) + stream.phase;
        const auto watermark_it = group_watermarks_.find(stream.clock_group);
        const auto watermark = watermark_it == group_watermarks_.end() ? max_time_ : watermark_it->second;
        const auto tolerance = stream.jitter + stream.uncertainty + stream.arrival;
        for (std::uint64_t index = 0; index < max_schedule_slots; ++index) {
            if (index > (std::numeric_limits<std::uint64_t>::max() - start) / stream.period) break;
            const auto slot = start + index * stream.period;
            if (slot > watermark) break;
            Row row;
            row.source = stream.source; row.parameter = stream.parameter; row.unit = stream.unit;
            row.channel = stream.channel; row.protocol = stream.protocol;
            row.observation = slot; row.ingest = slot; row.available = slot;
            row.clock_group = stream.clock_group; row.offset = stream.offset;
            row.uncertainty = stream.uncertainty; row.nominal = stream.period;
            row.decoder_version = stream.decoder_version; row.config_version = stream.config_version;
            if ((watermark - slot) <= tolerance) {
                row.status = "not_due";
            } else {
                const auto half = stream.period / 2;
                std::uint64_t nearest = std::numeric_limits<std::uint64_t>::max();
                bool found = false, valid = false;
                for (const auto& [time, observed_valid] : stream.observations) {
                    const auto difference = time > slot ? time - slot : slot - time;
                    if (difference <= half && difference < nearest) { nearest = difference; found = true; valid = observed_valid; }
                }
                row.status = found ? (valid ? "ok" : "invalid") : "expected_absent";
            }
            writeRow(row);
            ++counts_[row.status];
        }
    }
    if (csv_) csv_.flush();
    if (jsonl_) jsonl_.flush();
    std::ofstream out(options_.directory / "export_metadata.json", std::ios::binary | std::ios::trunc);
    if (!out) throw std::runtime_error("cannot write export metadata");
    const auto count = [this](const std::string& key) { const auto it = counts_.find(key); return it == counts_.end() ? 0ull : it->second; };
    out << "{\"schema\":1,\"format\":\"flat-1\",\"mode\":\"" << options_.mode << "\",\"run_id\":\""
        << jsonEscape(options_.run_id) << "\",\"session_id\":\"" << jsonEscape(options_.session_id)
        << "\",\"config_version\":\"" << jsonEscape(config_.version) << "\",\"samples\":" << samples_
        << ",\"counts\":{\"ok\":" << count("ok") << ",\"invalid\":" << count("invalid")
        << ",\"expected_absent\":" << count("expected_absent") << ",\"not_due\":" << count("not_due")
        << ",\"unknown_schedule\":" << count("unknown_schedule")
        << "},\"observation_time_range_ns\":{\"min\":" << min_time_ << ",\"max\":" << max_time_ << "}}\n";
    out.flush();
    if (!out) throw std::runtime_error("export metadata write failed");
}
}
