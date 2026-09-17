#include "core/flat_export.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace avionics;
namespace {
void check(bool condition, const std::string& message) { if (!condition) throw std::runtime_error(message); }
std::vector<std::string> splitCsv(const std::string& line) {
    std::vector<std::string> out;
    std::string item;
    bool quoted = false;
    for (std::size_t i = 0; i < line.size(); ++i) {
        const char character = line[i];
        if (quoted) {
            if (character == '"') { if (i + 1 < line.size() && line[i + 1] == '"') { item += '"'; ++i; } else quoted = false; }
            else item += character;
        } else if (character == '"') quoted = true;
        else if (character == ',') { out.push_back(item); item.clear(); }
        else item += character;
    }
    out.push_back(item);
    return out;
}
AlignedSample makeSample(const std::string& parameter, const std::string& unit, std::uint64_t observation, double value, bool valid) {
    AlignedSample aligned;
    aligned.sample.source = "sensor"; aligned.sample.parameter_id = parameter; aligned.sample.unit = unit;
    aligned.sample.generation = 1; aligned.sample.capture_time_ns = observation; aligned.sample.ingest_time_ns = observation + 1000;
    aligned.sample.clock_domain = 1; aligned.sample.raw_record_index = observation / 1000;
    aligned.sample.sequence = observation / 1000; aligned.sample.sequence_step = 1; aligned.sample.max_age_ns = 1000000;
    aligned.sample.protocol = 65535; aligned.sample.channel = 0;
    aligned.sample.decoder_version = "dictionary-v1"; aligned.sample.config_version = "flat-test-v1";
    aligned.sample.value = value; aligned.sample.valid = valid;
    aligned.time_ns = observation; aligned.time_group = "host"; aligned.uncertainty_ns = 0;
    return aligned;
}
const std::vector<std::string> expected{"run_id","session_id","source_id","channel","protocol_id","parameter_id",
    "unit","value_type","value_num","value_str","validity","status","observation_time_ns","available_time_ns",
    "ingest_time_ns","clock_group","offset_ns","uncertainty_ns","nominal_period_ns","sequence","sequence_step",
    "raw_record_index","origin_source","origin_generation","decoder_version","config_version","flags"};
}
int main(int argc, char** argv) {
    try {
        if (argc != 2) throw std::invalid_argument("expected output directory");
        BackendConfiguration config; config.version = "flat-test-v1";
        ClockDefinition clock; clock.source = "sensor"; clock.domain = 1; clock.group = "host"; clock.offset_ns = 5;
        config.clocks.push_back(clock);
        FieldDefinition temperature; temperature.id = "temperature"; temperature.source = "sensor"; temperature.protocol = 65535;
        temperature.unit = "degC"; temperature.bit_width = 8; temperature.max_age_ns = 1000000;
        temperature.epoch_ns = 1000000000ull; temperature.nominal_period_ns = 10000000ull;
        FieldDefinition pressure; pressure.id = "pressure"; pressure.source = "sensor"; pressure.protocol = 65535;
        pressure.unit = "kPa"; pressure.bit_width = 8; pressure.max_age_ns = 1000000;
        config.fields = {temperature, pressure};
        const auto directory = std::filesystem::path(argv[1]) / std::to_string(monotonicNowNs());
        FlatExportOptions options;
        options.directory = directory; options.run_id = "run-test"; options.session_id = "sess-test"; options.mode = "offline";
        {
            FlatExporter exporter(options, config);
            exporter.observe(makeSample("temperature", "degC", 990000000ull, 20.0, true));   // before epoch -> off_schedule
            exporter.observe(makeSample("temperature", "degC", 1000000000ull, 21.5, true));  // ok
            exporter.observe(makeSample("temperature", "degC", 1010000000ull, 22.0, false)); // invalid
            exporter.observe(makeSample("temperature", "degC", 1030000000ull, 23.0, true));  // ok (skips one slot)
            exporter.observe(makeSample("pressure", "kPa", 1000000000ull, 101.0, true));     // unknown_schedule
            exporter.finish();
            check(exporter.samples() == 5, "flat export sample count");
            const auto counts = exporter.statusCounts();
            const auto at = [&](const std::string& key) { const auto it = counts.find(key); return it == counts.end() ? 0ull : it->second; };
            check(at("ok") == 4, "flat export ok count");                       // 3 observations + 1 satisfied slot
            check(at("invalid") == 2, "flat export invalid count");             // 1 observation + 1 slot
            check(at("expected_absent") == 1, "flat export expected_absent count");
            check(at("not_due") == 1, "flat export not_due count");             // last slot tolerance window still open
            check(at("unknown_schedule") == 1, "flat export unknown_schedule count");
        }
        std::ifstream csv(directory / "parameters_flat.csv");
        std::string header; std::getline(csv, header);
        check(splitCsv(header) == expected, "flat CSV header mismatch");
        std::string row;
        std::getline(csv, row);
        const auto first = splitCsv(row);
        check(first.size() == expected.size(), "flat CSV row width");
        check(first[0] == "run-test" && first[1] == "sess-test" && first[2] == "sensor", "flat row identity");
        check(first[7] == "double" && first[8] == "20" && first[9].empty(), "flat row value split");
        check(first[10] == "true" && first[11] == "ok" && first[26] == "off_schedule", "flat row off_schedule flag");
        check(first[15] == "host" && first[16] == "5" && first[18] == "10000000", "flat row clock fields");
        const auto observation = std::stoull(first[12]), ingest = std::stoull(first[14]), available = std::stoull(first[13]);
        check(observation <= ingest && ingest <= available, "time order observation <= ingest <= available violated");
        bool saw_absent = false, saw_unknown = false, saw_invalid = false;
        std::string line;
        while (std::getline(csv, line)) {
            const auto fields = splitCsv(line);
            if (fields[11] == "expected_absent" && fields[8].empty() && fields[10].empty()) saw_absent = true;
            if (fields[11] == "unknown_schedule") saw_unknown = true;
            if (fields[11] == "invalid") saw_invalid = true;
        }
        check(saw_absent, "expected_absent row missing or not empty-valued");
        check(saw_unknown, "unknown_schedule row missing");
        check(saw_invalid, "invalid row missing");
        check(std::filesystem::exists(directory / "parameters_flat.jsonl"), "flat JSONL missing");
        check(std::filesystem::exists(directory / "export_metadata.json"), "flat metadata missing");
        std::cout << "PASS flat export fields, five statuses, off_schedule flag and time order\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << "FLAT EXPORT FAILURE: " << error.what() << '\n'; return 1; }
}
