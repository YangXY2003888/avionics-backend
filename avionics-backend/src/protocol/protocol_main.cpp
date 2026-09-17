#include "core/archive.hpp"
#include "core/path.hpp"
#include "core/protocol_impl.hpp"
#include <bit>
#include <cstdint>
#include <iostream>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace avionics;

const char* detectionName(ProtocolDetectionStatus status) {
    switch (status) {
    case ProtocolDetectionStatus::Unknown: return "unknown";
    case ProtocolDetectionStatus::NeedMoreData: return "need_more_data";
    case ProtocolDetectionStatus::Identified: return "identified";
    case ProtocolDetectionStatus::Ambiguous: return "ambiguous";
    case ProtocolDetectionStatus::Rejected: return "rejected";
    }
    return "?";
}

const char* basisName(ProtocolSelectionBasis basis) {
    switch (basis) {
    case ProtocolSelectionBasis::None: return "none";
    case ProtocolSelectionBasis::Manual: return "manual";
    case ProtocolSelectionBasis::SourceDescriptor: return "source_descriptor";
    case ProtocolSelectionBasis::ContentEvidence: return "content_evidence";
    }
    return "?";
}

const char* parseName(ProtocolParseStatus status) {
    switch (status) {
    case ProtocolParseStatus::NotAttempted: return "not_attempted";
    case ProtocolParseStatus::Complete: return "complete";
    case ProtocolParseStatus::NeedMoreData: return "need_more_data";
    case ProtocolParseStatus::Malformed: return "malformed";
    case ProtocolParseStatus::Unsupported: return "unsupported";
    }
    return "?";
}

std::string streamName(const ProtocolStreamKey& key) {
    return key.address.source + ":" + std::to_string(key.address.channel) + "#" + std::to_string(key.generation) +
        (key.address.origin_source.empty() ? std::string() : "@" + key.address.origin_source + "/" +
            std::to_string(key.origin_generation));
}

std::uint32_t encodeWordBigEndian(std::uint8_t label, std::uint32_t data) {
    std::uint32_t word = (std::uint32_t(label) << 24) | ((data & 0x1FFFFu) << 3);
    if (std::popcount(word) % 2 == 0) word |= 1u;
    return word;
}

void appendWord(std::vector<std::uint8_t>& payload, std::uint32_t word) {
    payload.push_back(static_cast<std::uint8_t>(word >> 24));
    payload.push_back(static_cast<std::uint8_t>(word >> 16));
    payload.push_back(static_cast<std::uint8_t>(word >> 8));
    payload.push_back(static_cast<std::uint8_t>(word));
}

std::uint16_t crc16(const std::uint8_t* data, std::size_t size) {
    std::uint16_t crc = 0xFFFF;
    for (std::size_t i = 0; i < size; ++i) {
        crc ^= std::uint16_t(std::uint16_t(data[i]) << 8);
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc & 0x8000) ? std::uint16_t((crc << 1) ^ 0x1021) : std::uint16_t(crc << 1);
    }
    return crc;
}

std::vector<std::uint8_t> encodeFramed(const std::vector<std::uint8_t>& data) {
    std::vector<std::uint8_t> out{0xB5, 0x00};
    const auto length = static_cast<std::uint16_t>(data.size());
    out.push_back(static_cast<std::uint8_t>(length & 0xff));
    out.push_back(static_cast<std::uint8_t>(length >> 8));
    out.insert(out.end(), data.begin(), data.end());
    const auto value = crc16(out.data() + 2, out.size() - 2);
    out.push_back(static_cast<std::uint8_t>(value & 0xff));
    out.push_back(static_cast<std::uint8_t>(value >> 8));
    return out;
}

std::uint16_t crc15Can(const std::uint8_t* data, std::size_t size) {
    std::uint16_t crc = 0;
    for (std::size_t i = 0; i < size; ++i) {
        for (int bit = 7; bit >= 0; --bit) {
            const int current = (data[i] >> bit) & 1, top = (crc >> 14) & 1;
            crc = static_cast<std::uint16_t>((crc << 1) & 0x7FFF);
            if (top ^ current) crc ^= 0x4599;
        }
    }
    return crc;
}

std::uint32_t encodeMilWord(std::uint8_t sync3, std::uint16_t info) {
    std::uint32_t word = (std::uint32_t(sync3 & 0x7u) << 17) | (std::uint32_t(info) << 1);
    if (std::popcount(word) % 2 == 0) word |= 1u;
    return word;
}

void appendMilWord(std::vector<std::uint8_t>& payload, std::uint32_t word) {
    for (int i = 0; i < 4; ++i) payload.push_back(static_cast<std::uint8_t>((word >> (8 * i)) & 0xff));
}

std::vector<std::uint8_t> encodeCan(std::uint32_t id, bool extended, const std::vector<std::uint8_t>& data) {
    std::vector<std::uint8_t> out;
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<std::uint8_t>((id >> (8 * i)) & 0xff));
    out.push_back(static_cast<std::uint8_t>(extended ? 1 : 0));
    out.push_back(static_cast<std::uint8_t>(data.size()));
    out.insert(out.end(), data.begin(), data.end());
    const auto crc = crc15Can(out.data(), out.size());
    out.push_back(static_cast<std::uint8_t>(crc & 0xff));
    out.push_back(static_cast<std::uint8_t>((crc >> 8) & 0xff));
    return out;
}

RawFrame makeFrame(const std::string& source, std::uint32_t channel, std::vector<std::uint8_t> payload,
    std::uint64_t sequence, std::uint64_t generation = 1) {
    RawFrame frame;
    frame.source = source; frame.channel = channel; frame.generation = generation; frame.sequence = sequence;
    frame.record_index = sequence + 1; frame.capture_time_ns = 1000000000 + sequence * 1000000;
    frame.payload = std::move(payload);
    return frame;
}

void printRoute(const ProtocolRouteResult& result) {
    std::cout << "route stream=" << streamName(result.stream) << " detection=" << detectionName(result.detection.status)
        << " basis=" << basisName(result.detection.basis) << " selected="
        << (result.detection.selected ? std::to_string(*result.detection.selected) : std::string("none"))
        << " parsing=" << parseName(result.parsing.status) << " messages=" << result.parsing.messages.size();
    if (!result.detection.reason.empty()) std::cout << " reason=\"" << result.detection.reason << '"';
    std::cout << '\n';
}

void runDemo() {
    ProtocolRouter router(std::make_shared<BuiltinProtocolFactory>());
    router.setLimits(ProtocolRoutingLimits{64, 4, 65536, 16});
    router.setInputDescriptor({"word-src", 0, ""}, ProtocolInputDescriptor{std::nullopt, false, representation_captured_words});
    router.setInputDescriptor({"mil-src", 0, ""}, ProtocolInputDescriptor{std::nullopt, false, representation_captured_mil_words});
    router.setInputDescriptor({"frame-src", 0, ""}, ProtocolInputDescriptor{std::nullopt, false, representation_wire_bytes});
    router.setInputDescriptor({"can-src", 0, ""}, ProtocolInputDescriptor{std::nullopt, false, representation_wire_bytes});

    std::vector<std::uint8_t> words;
    for (int i = 0; i < 4; ++i) appendWord(words, encodeWordBigEndian(static_cast<std::uint8_t>(0x10 + i), 100u + i));
    printRoute(router.route(makeFrame("word-src", 0, words, 0)));

    std::vector<std::uint8_t> mil;
    appendMilWord(mil, encodeMilWord(0b100, static_cast<std::uint16_t>((5u << 11) | (1u << 10) | (2u << 5) | 4u)));
    for (int i = 0; i < 3; ++i) appendMilWord(mil, encodeMilWord(0b001, static_cast<std::uint16_t>(0x1000 + i)));
    printRoute(router.route(makeFrame("mil-src", 0, mil, 0)));

    printRoute(router.route(makeFrame("can-src", 0, encodeCan(0x123u, false, {0x11, 0x22, 0x33, 0x44}), 0)));
    printRoute(router.route(makeFrame("frame-src", 0, encodeFramed({0x11, 0x22, 0x33}), 0)));
    for (std::uint64_t i = 0; i < 4; ++i) printRoute(router.route(makeFrame("noisy-src", 0, {0x01, 0x02, 0x03}, i)));
}

int runArchive(const std::filesystem::path& input, bool configure, const std::optional<StreamAddress>& target,
    ProtocolSelection selection, ProtocolInputDescriptor descriptor) {
    ProtocolRouter router(std::make_shared<BuiltinProtocolFactory>());
    auto apply = [&](const StreamAddress& address) {
        router.setSelection(address, selection);
        router.setInputDescriptor(address, descriptor);
    };
    if (configure && target) apply(*target);
    std::set<std::string> configured;
    ArchiveReader reader(input);
    RawFrame frame;
    std::map<std::string, ProtocolRouteResult> last;
    std::map<std::string, std::uint64_t> messages;
    std::uint64_t records = 0;
    while (reader.next(frame)) {
        ++records;
        if (configure && !target) {
            const StreamAddress address{frame.source, frame.channel, frame.origin_source};
            if (configured.insert(streamName(protocolStreamOf(frame))).second) apply(address);
        }
        auto result = router.route(frame);
        const auto name = streamName(result.stream);
        messages[name] += result.parsing.messages.size();
        last.insert_or_assign(name, std::move(result));
    }
    for (const auto& [name, result] : last) {
        std::cout << "stream=" << name << " records_detected_last=" << detectionName(result.detection.status)
            << " selected=" << (result.detection.selected ? std::to_string(*result.detection.selected) : std::string("none"))
            << " messages=" << messages[name] << '\n';
    }
    std::cout << "records=" << records << " streams=" << last.size() << '\n';
    return 0;
}

void usage() {
    std::cout << "Usage: bus_protocol --demo\n"
        << "       bus_protocol ARCHIVE [--source S] [--channel N] [--manual ID] [--representation REP]\n"
        << "                           [--hint ID] [--authoritative]\n";
}
}

int main(int argc, char** argv) {
    using namespace avionics;
    try {
        if (argc < 2) { usage(); return 2; }
        if (std::string(argv[1]) == "--demo") {
            if (argc != 2) { usage(); return 2; }
            runDemo();
            return 0;
        }
        const auto input = pathFromUtf8(argv[1]);
        bool configure = false, authoritative = false;
        std::string source;
        std::uint32_t channel = 0;
        ProtocolSelection selection = ProtocolSelection::automatic();
        ProtocolInputDescriptor descriptor;
        for (int index = 2; index < argc; ++index) {
            const std::string option = argv[index];
            auto value = [&]() -> std::string {
                if (index + 1 >= argc) throw std::invalid_argument("missing value for " + option);
                return argv[++index];
            };
            if (option == "--source") { source = value(); configure = true; }
            else if (option == "--channel") { channel = static_cast<std::uint32_t>(std::stoul(value())); configure = true; }
            else if (option == "--manual") { selection = ProtocolSelection::manual(static_cast<ProtocolId>(std::stoul(value()))); configure = true; }
            else if (option == "--representation") { descriptor.capture_representation = value(); configure = true; }
            else if (option == "--hint") { descriptor.source_hint = static_cast<ProtocolId>(std::stoul(value())); configure = true; }
            else if (option == "--authoritative") { descriptor.hint_is_authoritative = true; configure = true; }
            else throw std::invalid_argument("unknown option: " + option);
        }
        std::optional<StreamAddress> target;
        if (!source.empty()) target = StreamAddress{source, channel, ""};
        return runArchive(input, configure, target, selection, descriptor);
    } catch (const std::exception& error) {
        std::cerr << "protocol routing failed: " << error.what() << '\n';
        return 1;
    }
}
