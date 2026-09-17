#include "core/archive.hpp"
#include "core/configuration.hpp"
#include "core/flat_export.hpp"
#include "core/path.hpp"
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>

int main(int argc, char** argv) {
    using namespace avionics;
    try {
        if (argc < 4) {
            std::cout << "Usage: bus_export CONFIG_INI INPUT_ARCHIVE OUTPUT_DIRECTORY [--session-id ID] [--mode offline|online]\n";
            return argc == 1 ? 0 : 2;
        }
        const auto config_path = pathFromUtf8(argv[1]);
        const auto input_path = pathFromUtf8(argv[2]);
        const auto output = pathFromUtf8(argv[3]);
        std::string session = input_path.filename().string();
        std::string mode = "offline";
        for (int index = 4; index < argc; ++index) {
            const std::string option = argv[index];
            if (option == "--session-id" && index + 1 < argc) session = argv[++index];
            else if (option == "--mode" && index + 1 < argc) mode = argv[++index];
            else throw std::invalid_argument("unknown option: " + option);
        }
        const auto config = BackendConfiguration::load(config_path);
        FlatExportOptions options;
        options.directory = output;
        options.run_id = "run-" + std::to_string(monotonicNowNs());
        options.session_id = session;
        options.mode = mode;
        FlatExporter exporter(options, config);
        DictionaryDecoder decoder(config);
        TimeQualityProcessor quality(config.clocks);
        ArchiveReader reader(input_path);
        RawFrame frame;
        std::uint64_t records = 0;
        while (reader.next(frame)) {
            ++records;
            for (const auto& sample : decoder.decode(frame)) exporter.observe(quality.process(sample));
        }
        exporter.finish();
        std::filesystem::copy_file(config_path, output / "configuration.ini",
            std::filesystem::copy_options::overwrite_existing);
        const auto counts = exporter.statusCounts();
        const auto count = [&](const std::string& key) {
            const auto found = counts.find(key); return found == counts.end() ? 0ull : found->second;
        };
        std::cout << "records=" << records << " samples=" << exporter.samples()
            << " ok=" << count("ok") << " invalid=" << count("invalid")
            << " expected_absent=" << count("expected_absent") << " not_due=" << count("not_due") << '\n';
        std::cout << "export=" << std::filesystem::absolute(output).string() << '\n';
        return 0;
    } catch (const std::exception& error) { std::cerr << "export failed: " << error.what() << '\n'; return 1; }
}
