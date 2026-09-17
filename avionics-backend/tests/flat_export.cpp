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
AlignedSample makeSample(std::uint64_t observation, double value, bool valid) {
    AlignedSample aligned;
    aligned.sample.source = "sensor"; aligned.sample.parameter_id = "temperature"; aligned.sample.unit = "degC";
    aligned.sample.generation = 1; aligned.sample.capture_time_ns = observation; aligned.sample.ingest_time_ns = observation + 1000;
    aligned.sample.clock_domain = 1; aligned.sample.raw_record_index = observation / 1000;
    aligned.sample.sequence = observation / 1000; aligned.sample.sequence_step = 1; aligned.sample.max_age_ns = 1000000;
    aligned.sample.protocol = 65535; aligned.sample.channel = 0;
    aligned.sample.decoder_version = "dictionary-v1"; aligned.sample.config_version = "flat-test-v1";
    aligned.sample.nominal_period_ns = 10000000; aligned.sample.value = value; aligned.sample.valid = valid;
    aligned.time_ns = observation; aligned.time_group = "host"; aligned.uncertainty_ns = 0;
    return aligned;
}
}
int main(int argc, char** argv) {
    try {
        if (argc != 2) throw std::invalid_argument("expected output directory");
        BackendConfiguration config; config.version = "flat-test-v1";
        ClockDefinition clock; clock.source = "sensor"; clock.domain = 1; clock.group = "host"; clock.offset_ns = 5;
        config.clocks.push_back(clock);
        FieldDefinition field; field.id = "temperature"; field.source = "sensor"; field.protocol = 65535;
        field.unit = "degC"; field.bit_width = 8; field.max_age_ns = 1000000; field.nominal_period_ns = 10000000;
        config.fields.push_back(field);
        const auto directory = std::filesystem::path(argv[1]) / std::to_string(monotonicNowNs());
        FlatExportOptions options;
        options.directory = directory; options.run_id = "run-test"; options.session_id = "sess-test"; options.mode = "offline";
        {
            FlatExporter exporter(options, config);
            exporter.observe(makeSample(1000000000ull, 21.5, true));
            exporter.observe(makeSample(1010000000ull, 22.0, false));
            exporter.finish();
            check(exporter.samples() == 2, "flat export sample count");
            check(exporter.statusCounts().at("ok") == 1, "flat export ok count");
            check(exporter.statusCounts().at("invalid") == 1, "flat export invalid count");
        }
        std::ifstream csv(directory / "parameters_flat.csv");
        std::string header; std::getline(csv, header);
        const std::vector<std::string> expected{"run_id","session_id","source_id","channel","protocol_id","parameter_id",
            "unit","value_type","value_num","value_str","validity","status","observation_time_ns","available_time_ns",
            "ingest_time_ns","clock_group","offset_ns","uncertainty_ns","nominal_period_ns","sequence","sequence_step",
            "raw_record_index","origin_source","origin_generation","decoder_version","config_version"};
        check(splitCsv(header) == expected, "flat CSV header mismatch");
        std::string row; std::getline(csv, row);
        const auto fields = splitCsv(row);
        check(fields.size() == expected.size(), "flat CSV row width");
        check(fields[0] == "run-test" && fields[1] == "sess-test" && fields[2] == "sensor", "flat row identity");
        check(fields[4] == "65535" && fields[5] == "temperature" && fields[6] == "degC", "flat row protocol/parameter");
        check(fields[7] == "double" && fields[8] == "21.5" && fields[9].empty(), "flat row value split");
        check(fields[10] == "true" && fields[11] == "ok", "flat row validity/status");
        check(fields[15] == "host" && fields[16] == "5" && fields[18] == "10000000", "flat row clock fields");
        check(fields[24] == "dictionary-v1" && fields[25] == "flat-test-v1", "flat row versions");
        const auto observation = std::stoull(fields[12]), ingest = std::stoull(fields[14]), available = std::stoull(fields[13]);
        check(observation <= ingest && ingest <= available, "time order observation <= ingest <= available violated");
        std::string second; std::getline(csv, second);
        const auto invalid = splitCsv(second);
        check(invalid[10] == "false" && invalid[11] == "invalid", "flat row invalid classification");
        check(std::filesystem::exists(directory / "parameters_flat.jsonl"), "flat JSONL missing");
        check(std::filesystem::exists(directory / "export_metadata.json"), "flat metadata missing");
        std::cout << "PASS flat export fields, value split, status and time order\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << "FLAT EXPORT FAILURE: " << error.what() << '\n'; return 1; }
}
