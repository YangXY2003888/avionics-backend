#include "core/protocol_impl.hpp"
#include <bit>
#include <cstdint>
#include <iostream>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

using namespace avionics;

namespace {
void check(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}
template<class F> void throws(F&& function, const std::string& message) {
    bool caught = false;
    try { function(); } catch (const std::exception&) { caught = true; }
    check(caught, message);
}

std::uint32_t makeWord(std::uint8_t label, std::uint32_t data, std::uint8_t sdi = 0, std::uint8_t ssm = 0) {
    std::uint32_t word = (std::uint32_t(label) << 24) | (std::uint32_t(sdi & 0x3) << 22) |
        ((data & 0x1FFFFu) << 3) | (std::uint32_t(ssm & 0x3) << 1);
    if (std::popcount(word) % 2 == 0) word |= 1u;
    return word;
}

RawFrame wordFrame(const std::string& source, std::uint32_t channel, const std::vector<std::uint32_t>& words,
    std::uint64_t sequence = 0, std::uint64_t generation = 1) {
    RawFrame frame;
    frame.source = source; frame.channel = channel; frame.generation = generation; frame.sequence = sequence;
    frame.record_index = sequence + 1; frame.capture_time_ns = 1000000000 + sequence * 1000000;
    for (const auto word : words) {
        frame.payload.push_back(static_cast<std::uint8_t>(word >> 24));
        frame.payload.push_back(static_cast<std::uint8_t>(word >> 16));
        frame.payload.push_back(static_cast<std::uint8_t>(word >> 8));
        frame.payload.push_back(static_cast<std::uint8_t>(word));
    }
    return frame;
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

std::vector<std::uint8_t> framedBytes(const std::vector<std::uint8_t>& data) {
    std::vector<std::uint8_t> out{0xB5, 0x00};
    const auto length = static_cast<std::uint16_t>(data.size());
    out.push_back(static_cast<std::uint8_t>(length & 0xff));
    out.push_back(static_cast<std::uint8_t>(length >> 8));
    out.insert(out.end(), data.begin(), data.end());
    const auto crc = crc16(out.data() + 2, out.size() - 2);
    out.push_back(static_cast<std::uint8_t>(crc & 0xff));
    out.push_back(static_cast<std::uint8_t>(crc >> 8));
    return out;
}

RawFrame bytesFrame(const std::string& source, std::uint32_t channel, const std::vector<std::uint8_t>& bytes,
    std::uint64_t sequence = 0, std::uint64_t generation = 1) {
    RawFrame frame;
    frame.source = source; frame.channel = channel; frame.generation = generation; frame.sequence = sequence;
    frame.record_index = sequence + 1; frame.capture_time_ns = 1000000000 + sequence * 1000000;
    frame.payload = bytes;
    return frame;
}

std::uint16_t crc15(const std::uint8_t* data, std::size_t size) {
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

std::uint32_t makeMilWord(std::uint8_t sync3, std::uint16_t info) {
    std::uint32_t word = (std::uint32_t(sync3 & 0x7u) << 17) | (std::uint32_t(info) << 1);
    if (std::popcount(word) % 2 == 0) word |= 1u;
    return word;
}

RawFrame milFrame(const std::string& source, std::uint32_t channel, const std::vector<std::uint32_t>& words,
    std::uint64_t sequence = 0, std::uint64_t generation = 1) {
    RawFrame frame;
    frame.source = source; frame.channel = channel; frame.generation = generation; frame.sequence = sequence;
    frame.record_index = sequence + 1; frame.capture_time_ns = 1000000000 + sequence * 1000000;
    for (const auto word : words) {
        frame.payload.push_back(static_cast<std::uint8_t>(word & 0xff));
        frame.payload.push_back(static_cast<std::uint8_t>((word >> 8) & 0xff));
        frame.payload.push_back(static_cast<std::uint8_t>((word >> 16) & 0xff));
        frame.payload.push_back(static_cast<std::uint8_t>((word >> 24) & 0xff));
    }
    return frame;
}

std::vector<std::uint8_t> canBytes(std::uint32_t id, bool extended, bool remote, const std::vector<std::uint8_t>& data) {
    std::vector<std::uint8_t> out;
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<std::uint8_t>((id >> (8 * i)) & 0xff));
    out.push_back(static_cast<std::uint8_t>((extended ? 1 : 0) | (remote ? 2 : 0)));
    out.push_back(static_cast<std::uint8_t>(data.size()));
    out.insert(out.end(), data.begin(), data.end());
    const auto crc = crc15(out.data(), out.size());
    out.push_back(static_cast<std::uint8_t>(crc & 0xff));
    out.push_back(static_cast<std::uint8_t>((crc >> 8) & 0xff));
    return out;
}

std::vector<std::uint8_t> canLogBytes(std::uint32_t id, bool extended, const std::vector<std::uint8_t>& data) {
    std::vector<std::uint8_t> out;
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<std::uint8_t>((id >> (8 * i)) & 0xff));
    out.push_back(static_cast<std::uint8_t>(extended ? 1 : 0));
    out.push_back(static_cast<std::uint8_t>(data.size()));
    out.insert(out.end(), data.begin(), data.end());
    return out;
}

ProtocolRoutingLimits limitsOf(std::size_t streams, std::size_t frames, std::size_t bytes, std::size_t candidates) {
    ProtocolRoutingLimits limits;
    limits.max_streams = streams;
    limits.max_observation_frames_per_stream = frames;
    limits.max_observation_bytes_per_stream = bytes;
    limits.max_candidates_per_stream = candidates;
    return limits;
}

constexpr ProtocolId blind_protocol_a = 0x00020001u;
constexpr ProtocolId blind_protocol_b = 0x00020002u;

class BlindDetector final : public IProtocolDetector {
public:
    explicit BlindDetector(ProtocolId protocol) : protocol_(protocol) {}
    ProtocolId protocolId() const noexcept override { return protocol_; }
    ProtocolDetectionReport probe(const ProtocolDetectionContext&, std::span<const RawFrame> observations) const override {
        ProtocolDetectionReport report;
        if (observations.empty()) return report;
        ProtocolCandidate candidate;
        candidate.protocol = protocol_;
        candidate.strength = CandidateStrength::Strong;
        candidate.evidence.push_back({ProtocolEvidenceKind::Structure, "blind test double"});
        report.status = ProtocolDetectionStatus::Identified;
        report.basis = ProtocolSelectionBasis::ContentEvidence;
        report.selected = protocol_;
        report.candidates.push_back(std::move(candidate));
        return report;
    }
private:
    ProtocolId protocol_;
};

class PassHandler final : public IProtocolHandler {
public:
    explicit PassHandler(ProtocolStreamKey key) : key_(std::move(key)) {}
    ProtocolId protocolId() const noexcept override { return key_.address.channel == 0 ? blind_protocol_a : blind_protocol_b; }
    const ProtocolStreamKey& stream() const noexcept override { return key_; }
    ProtocolParseResult parse(const RawFrame&) override { return {}; }
    void reset(ProtocolResetReason) noexcept override {}
private:
    ProtocolStreamKey key_;
};

class AmbiguousFactory final : public IProtocolFactory {
public:
    std::vector<ProtocolDescriptor> descriptors() const override {
        return {{blind_protocol_a, "blind-a", "1", {"wire_bytes"}}, {blind_protocol_b, "blind-b", "1", {"wire_bytes"}}};
    }
    std::unique_ptr<IProtocolDetector> createDetector(ProtocolId protocol) const override {
        if (protocol != blind_protocol_a && protocol != blind_protocol_b) return nullptr;
        return std::make_unique<BlindDetector>(protocol);
    }
    std::unique_ptr<IProtocolHandler> createHandler(ProtocolId protocol, const ProtocolStreamKey& key) const override {
        if (protocol != blind_protocol_a && protocol != blind_protocol_b) return nullptr;
        return std::make_unique<PassHandler>(key);
    }
};

void testFactory() {
    const BuiltinProtocolFactory factory;
    const auto descriptors = factory.descriptors();
    check(descriptors.size() == 5, "factory must describe five built-in protocols");
    check(descriptors[0].id != descriptors[1].id && descriptors[2].id != descriptors[3].id, "protocol descriptors must be unique");
    check(!descriptors[0].name.empty() && !descriptors[4].name.empty(), "descriptors need names");
    check(factory.supports(protocol_word_stream) && factory.supports(protocol_framed_stream), "built-in protocols must be supported");
    check(factory.supports(protocol_mil1553_stream) && factory.supports(protocol_can_stream), "bus protocols must be supported");
    check(factory.supports(protocol_can_log_stream), "captured CAN log protocol must be supported");
    check(!factory.supports(unknown_protocol), "unknown protocol must not be supported");
    check(factory.createDetector(unknown_protocol) == nullptr, "unknown detector must be null");
    const ProtocolStreamKey key{{"src", 0, ""}, 1, 0};
    check(factory.createHandler(unknown_protocol, key) == nullptr, "unknown handler must be null");
    check(factory.createDetector(protocol_word_stream)->protocolId() == protocol_word_stream, "word detector identity");
    check(factory.createHandler(protocol_framed_stream, key)->stream() == key, "handler must retain its stream");
    std::cout << "PASS built-in factory descriptors and creation\n";
}

void testSelectionPolicy() {
    ProtocolRouter router(std::make_shared<BuiltinProtocolFactory>());
    const StreamAddress address{"capture", 0, ""};
    check(router.selection(address).mode() == ProtocolSelectionMode::Automatic, "unconfigured address must be automatic");
    check(!router.selection(address).protocol(), "automatic selection must not name a protocol");
    check(router.inputDescriptor(address).capture_representation.empty(), "unconfigured descriptor must be empty");
    check(!router.inputDescriptor(address).source_hint, "unconfigured descriptor must not hint");
    throws([&] { router.setSelection(address, ProtocolSelection::manual(999999u)); },
        "manual unregistered protocol must be rejected");
    check(router.selection(address).mode() == ProtocolSelectionMode::Automatic, "rejected manual selection must not change policy");
    router.setSelection(address, ProtocolSelection::manual(protocol_word_stream));
    check(router.selection(address).protocol() == protocol_word_stream, "manual policy was not stored");
    throws([&] { (void)ProtocolSelection::manual(unknown_protocol); }, "manual unknown id must be rejected");
    router.setSelection(address, ProtocolSelection::automatic());
    check(router.selection(address).mode() == ProtocolSelectionMode::Automatic, "policy must return to automatic");
    std::cout << "PASS selection policy validation and defaults\n";
}

void testWordDetection() {
    ProtocolRouter router(std::make_shared<BuiltinProtocolFactory>());
    const StreamAddress address{"word-src", 0, ""};
    router.setInputDescriptor(address, ProtocolInputDescriptor{std::nullopt, false, representation_captured_words});
    const auto words = std::vector<std::uint32_t>{makeWord(0x10, 100), makeWord(0x11, 200), makeWord(0x12, 300), makeWord(0x13, 400)};
    const auto first = router.route(wordFrame("word-src", 0, words));
    check(first.detection.status == ProtocolDetectionStatus::Identified, "valid words must identify");
    check(first.detection.selected == protocol_word_stream, "word protocol must be selected");
    check(first.detection.basis == ProtocolSelectionBasis::ContentEvidence, "content basis expected");
    check(first.parsing.status == ProtocolParseStatus::Complete, "word replay must complete");
    check(first.parsing.messages.size() == 1 && first.parsing.messages[0].message_kind == "arinc429", "ARINC 429 message expected");
    const auto second = router.route(wordFrame("word-src", 0, words, 1));
    check(second.detection.selected == protocol_word_stream && second.parsing.status == ProtocolParseStatus::Complete,
        "confirmed stream must reuse the word handler");
    std::cout << "PASS automatic word-stream detection and reuse\n";
}

void testFramedDetection() {
    ProtocolRouter router(std::make_shared<BuiltinProtocolFactory>());
    const StreamAddress address{"frame-src", 0, ""};
    const std::vector<std::uint8_t> data{0x11, 0x22, 0x33};
    const auto whole = framedBytes(data);
    const auto complete = router.route(bytesFrame("frame-src", 0, whole));
    check(complete.detection.selected == protocol_framed_stream, "framed protocol must be selected");
    check(complete.parsing.status == ProtocolParseStatus::Complete, "framed record must parse");
    check(complete.parsing.messages[0].payload == data, "framed payload must round-trip");

    ProtocolRouter split(std::make_shared<BuiltinProtocolFactory>());
    std::vector<std::uint8_t> head(whole.begin(), whole.begin() + 5);
    std::vector<std::uint8_t> tail(whole.begin() + 5, whole.end());
    const auto partial = split.route(bytesFrame("frame-src", 0, head));
    check(partial.detection.status == ProtocolDetectionStatus::NeedMoreData, "partial frame must wait for data");
    check(!partial.detection.selected, "partial frame must not name a protocol");
    const auto finished = split.route(bytesFrame("frame-src", 0, tail, 1));
    check(finished.detection.selected == protocol_framed_stream, "split frame must identify once complete");
    check(finished.parsing.status == ProtocolParseStatus::Complete && finished.parsing.messages[0].payload == data,
        "replayed observation must recover the split frame");
    std::cout << "PASS automatic framed detection across frames\n";
}

void testMil1553Detection() {
    ProtocolRouter router(std::make_shared<BuiltinProtocolFactory>());
    const StreamAddress address{"mil-src", 0, ""};
    router.setInputDescriptor(address, ProtocolInputDescriptor{std::nullopt, false, representation_captured_mil_words});
    const std::uint16_t command = static_cast<std::uint16_t>((5u << 11) | (1u << 10) | (2u << 5) | 4u);
    const auto words = std::vector<std::uint32_t>{makeMilWord(0b100, command), makeMilWord(0b001, 0x1234),
        makeMilWord(0b001, 0x5678), makeMilWord(0b001, 0x9ABC)};
    const auto routed = router.route(milFrame("mil-src", 0, words));
    check(routed.detection.selected == protocol_mil1553_stream, "1553B stream must identify");
    check(routed.parsing.status == ProtocolParseStatus::Complete, "1553B frame must parse");
    check(routed.parsing.messages.size() == 1 &&
        routed.parsing.messages[0].message_kind == "mil1553_command_or_status", "1553B command word expected");
    std::cout << "PASS MIL-STD-1553B detection and parsing\n";
}

void testCanDetection() {
    ProtocolRouter router(std::make_shared<BuiltinProtocolFactory>());
    const StreamAddress address{"can-src", 0, ""};
    router.setInputDescriptor(address, ProtocolInputDescriptor{std::nullopt, false, representation_wire_bytes});
    const std::vector<std::uint8_t> data{0x11, 0x22, 0x33, 0x44};
    const auto bytes = canBytes(0x123u, false, false, data);
    const auto routed = router.route(bytesFrame("can-src", 0, bytes));
    check(routed.detection.selected == protocol_can_stream, "CAN stream must identify");
    check(routed.parsing.status == ProtocolParseStatus::Complete, "CAN frame must parse");
    check(routed.parsing.messages.size() == 1 && routed.parsing.messages[0].payload == data, "CAN payload must round-trip");
    check(routed.parsing.messages[0].message_kind == "can_frame", "CAN message kind expected");
    std::cout << "PASS CAN 2.0 detection and parsing\n";
}

void testCanLogDetection() {
    ProtocolRouter router(std::make_shared<BuiltinProtocolFactory>());
    const StreamAddress address{"canlog-src", 0, ""};
    router.setInputDescriptor(address, ProtocolInputDescriptor{std::nullopt, false, representation_captured_can});
    std::vector<std::uint8_t> bytes;
    for (int i = 0; i < 5; ++i) {
        const auto frame = canLogBytes(0x7E8u, false, {0x03, 0x41, 0x04, static_cast<std::uint8_t>(i), 0, 0, 0, 0});
        bytes.insert(bytes.end(), frame.begin(), frame.end());
    }
    const auto routed = router.route(bytesFrame("canlog-src", 0, bytes));
    check(routed.detection.selected == protocol_can_log_stream, "captured CAN log must identify");
    check(routed.parsing.status == ProtocolParseStatus::Complete, "captured CAN log must parse");
    check(routed.parsing.messages.size() == 5, "every captured CAN frame must produce a message");
    check(routed.parsing.messages[0].message_kind == "can_frame" && routed.parsing.messages[0].payload.size() == 8,
        "captured CAN payload must round-trip");
    std::cout << "PASS captured CAN log detection and parsing\n";
}

void testIsolation() {
    ProtocolRouter router(std::make_shared<BuiltinProtocolFactory>());
    const auto words = std::vector<std::uint32_t>{makeWord(0x10, 1), makeWord(0x11, 2), makeWord(0x12, 3), makeWord(0x13, 4)};
    const auto channel0 = router.route(wordFrame("shared", 0, words));
    check(channel0.detection.selected == protocol_word_stream, "channel 0 must select words");
    const auto channel1 = router.route(bytesFrame("shared", 1, framedBytes({0x42})));
    check(channel1.detection.selected == protocol_framed_stream, "channel 1 must select framed independently");
    check(router.route(wordFrame("shared", 0, words, 1)).detection.selected == protocol_word_stream,
        "channel 0 detection must not be disturbed by channel 1");

    const auto generation2 = router.route(bytesFrame("shared", 0, framedBytes({0x42}), 0, 2));
    check(generation2.detection.selected == protocol_framed_stream, "new generation must re-identify independently");

    ProtocolRouter replay(std::make_shared<BuiltinProtocolFactory>());
    const auto live = replay.route(wordFrame("live", 0, words, 1));
    check(live.detection.selected == protocol_word_stream, "live stream must select words");
    RawFrame origin_frame = bytesFrame("live", 0, framedBytes({0x5A}), 0, 1);
    origin_frame.origin_source = "original"; origin_frame.origin_generation = 9;
    const auto origin = replay.route(origin_frame);
    check(origin.detection.selected == protocol_framed_stream, "replayed origin must be detected independently");
    check(origin.stream.address.origin_source == "original" && origin.stream.origin_generation == 9,
        "replayed identity was lost");
    std::cout << "PASS channel, generation and origin isolation\n";
}

void testBadFrameNoSwitch() {
    ProtocolRouter manual(std::make_shared<BuiltinProtocolFactory>());
    const StreamAddress address{"manual-src", 0, ""};
    manual.setSelection(address, ProtocolSelection::manual(protocol_framed_stream));
    const auto bad = manual.route(bytesFrame("manual-src", 0, {0x01, 0x02, 0x03, 0x04}));
    check(bad.parsing.status == ProtocolParseStatus::Malformed, "malformed manual input must be reported");
    check(bad.detection.selected == protocol_framed_stream, "malformed input must not change manual selection");
    const auto good = manual.route(bytesFrame("manual-src", 0, framedBytes({0x07}), 1));
    check(good.parsing.status == ProtocolParseStatus::Complete, "manual selection must recover on good input");

    ProtocolRouter automatic(std::make_shared<BuiltinProtocolFactory>());
    const auto words = std::vector<std::uint32_t>{makeWord(0x10, 1), makeWord(0x11, 2), makeWord(0x12, 3), makeWord(0x13, 4)};
    automatic.route(wordFrame("auto-src", 0, words));
    const auto stray = automatic.route(bytesFrame("auto-src", 0, {0x01, 0x02, 0x03, 0x04, 0x05}, 1));
    check(stray.detection.selected == protocol_word_stream, "bad frame must not switch a confirmed protocol");
    check(stray.parsing.status == ProtocolParseStatus::Malformed, "bad frame for confirmed protocol must be malformed");
    std::cout << "PASS bad frames never switch the selected protocol\n";
}

void testAmbiguityAndLimits() {
    ProtocolRouter router(std::make_shared<AmbiguousFactory>());
    const auto ambiguous = router.route(bytesFrame("amb", 0, {0x01, 0x02, 0x03}));
    check(ambiguous.detection.status == ProtocolDetectionStatus::Ambiguous, "two strong claims must be ambiguous");
    check(!ambiguous.detection.selected, "ambiguous result must not select a protocol");

    ProtocolRouter bounded(std::make_shared<BuiltinProtocolFactory>());
    bounded.setLimits(limitsOf(1, 4, 4096, 8));
    bounded.route(bytesFrame("one", 0, {0x01, 0x02, 0x03}));
    const auto exhausted = bounded.route(bytesFrame("two", 0, {0x01, 0x02, 0x03}));
    check(exhausted.detection.status == ProtocolDetectionStatus::Rejected, "stream budget must reject new streams");
    throws([&] { bounded.setLimits(limitsOf(0, 4, 4096, 8)); }, "zero limits must be rejected");
    check(bounded.selection(StreamAddress{"one", 0, ""}).mode() == ProtocolSelectionMode::Automatic,
        "rejected limits must keep prior policy defaults");

    ProtocolRouter observed(std::make_shared<BuiltinProtocolFactory>());
    observed.setLimits(limitsOf(8, 3, 4096, 8));
    observed.route(bytesFrame("garbage", 0, {0x01, 0x02, 0x03}));
    observed.route(bytesFrame("garbage", 0, {0x01, 0x02, 0x03}, 1));
    const auto budget = observed.route(bytesFrame("garbage", 0, {0x01, 0x02, 0x03}, 2));
    check(budget.detection.status == ProtocolDetectionStatus::Rejected, "observation budget must reject unidentified streams");
    std::cout << "PASS ambiguity resolution and routing limits\n";
}

void testInputDescriptorAndReset() {
    const std::vector<std::uint32_t> words{makeWord(0x10, 1), makeWord(0x11, 2), makeWord(0x12, 3), makeWord(0x13, 4)};
    const std::vector<std::uint8_t> framed = framedBytes({0x11, 0x22, 0x33});

    ProtocolRouter hinted(std::make_shared<BuiltinProtocolFactory>());
    const StreamAddress hinted_address{"hint", 0, ""};
    hinted.setInputDescriptor(hinted_address, ProtocolInputDescriptor{protocol_word_stream, true, {}});
    const auto authoritative = hinted.route(bytesFrame("hint", 0, framed));
    check(authoritative.detection.selected == protocol_word_stream, "authoritative hint must select without content");
    check(authoritative.detection.basis == ProtocolSelectionBasis::SourceDescriptor, "authoritative hint basis expected");

    ProtocolRouter restricted(std::make_shared<BuiltinProtocolFactory>());
    restricted.setLimits(limitsOf(8, 1, 4096, 8));
    const StreamAddress restricted_address{"restricted", 0, ""};
    restricted.setInputDescriptor(restricted_address, ProtocolInputDescriptor{std::nullopt, false, representation_wire_bytes});
    const auto rejected = restricted.route(wordFrame("restricted", 0, words));
    check(rejected.detection.status == ProtocolDetectionStatus::Rejected, "representation must restrict detection");
    check(!rejected.detection.selected, "restricted detection must not select another protocol");

    ProtocolRouter unknown_representation(std::make_shared<BuiltinProtocolFactory>());
    const StreamAddress unknown_address{"unknown", 0, ""};
    unknown_representation.setInputDescriptor(unknown_address, ProtocolInputDescriptor{std::nullopt, false, "mystery"});
    const auto mismatch = unknown_representation.route(wordFrame("unknown", 0, words));
    check(mismatch.detection.status == ProtocolDetectionStatus::Rejected, "unknown representation must be rejected");

    ProtocolRouter reset(std::make_shared<BuiltinProtocolFactory>());
    const StreamAddress address{"reset", 0, ""};
    reset.setSelection(address, ProtocolSelection::manual(protocol_word_stream));
    reset.route(wordFrame("reset", 0, words));
    reset.resetStream(address, ProtocolResetReason::ExplicitReset);
    check(reset.selection(address).protocol() == protocol_word_stream, "reset must keep the manual policy");
    reset.removeStream(address);
    check(reset.selection(address).mode() == ProtocolSelectionMode::Automatic, "removal must drop the policy");
    std::cout << "PASS input descriptors, reset and removal semantics\n";
}

void testPipelineDecoder() {
    BackendConfiguration config;
    config.version = "protocol-test-v1";
    FieldDefinition field;
    field.id = "alt"; field.source = "src"; field.unit = "ft";
    field.protocol = protocol_word_stream; field.channel = 0;
    field.bit_offset = 10; field.bit_width = 19; field.big_endian = true;
    field.type = FieldType::Unsigned; field.scale = 1; field.offset = 0;
    field.max_age_ns = 1000000000; field.sequence_step = 0;
    config.fields.push_back(field);

    auto factory = std::make_shared<BuiltinProtocolFactory>();
    ProtocolPipelineDecoder pipeline(std::make_unique<ProtocolRouter>(factory),
        std::make_unique<DictionaryMessageDecoder>(config));
    (void)pipeline.router();
    (void)pipeline.messageDecoder();
    const std::vector<std::uint32_t> words{makeWord(0x10, 0x1234), makeWord(0x11, 1), makeWord(0x12, 2), makeWord(0x13, 3)};
    const auto samples = pipeline.decode(wordFrame("src", 0, words));
    check(samples.size() == 1, "pipeline decoder must emit one parameter");
    check(samples[0].parameter_id == "alt" && samples[0].unit == "ft" && samples[0].source == "src",
        "message decoder must keep dictionary identity");
    check(std::get<std::uint64_t>(samples[0].value) == 0x1234u, "message decoder must extract the mapped field");
    check(samples[0].valid && samples[0].raw_record_index == 1, "message decoder must carry evidence and validity");
    check(pipeline.decode(bytesFrame("src", 0, {0x01, 0x02})).empty(), "undecodable input must produce no parameters");
    std::cout << "PASS message decoder and pipeline decoder integration\n";
}
}

int main() {
    try {
        testFactory();
        testSelectionPolicy();
        testWordDetection();
        testFramedDetection();
        testMil1553Detection();
        testCanDetection();
        testCanLogDetection();
        testIsolation();
        testBadFrameNoSwitch();
        testAmbiguityAndLimits();
        testInputDescriptorAndReset();
        testPipelineDecoder();
        std::cout << "ALL PROTOCOL RUNTIME TESTS PASSED\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "PROTOCOL RUNTIME FAILURE: " << error.what() << '\n';
        return 1;
    }
}
