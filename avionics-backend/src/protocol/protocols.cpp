#include "core/bitfield.hpp"
#include "core/protocol_impl.hpp"
#include "plugin_api/bus_plugin.h"
#include <bit>
#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <utility>

namespace avionics {
namespace {
constexpr std::size_t arinc_bytes = 4;
constexpr std::size_t arinc_min_words = 4;
constexpr std::size_t frame_sync0 = 0xB5;
constexpr std::size_t frame_sync1 = 0x00;
constexpr std::size_t frame_header_bytes = 4;
constexpr std::size_t frame_crc_bytes = 2;
constexpr std::size_t max_frame_payload = 4096;
constexpr std::size_t mil_word_bytes = 4;
constexpr std::size_t mil_min_words = 4;
constexpr std::size_t can_header_bytes = 6;
constexpr std::size_t can_crc_bytes = 2;

ProtocolFrameReference referenceOf(const ProtocolStreamKey& key, const RawFrame& frame) {
    return {key, frame.record_index, frame.sequence, frame.capture_time_ns, frame.clock_domain};
}

std::uint8_t reverse8(std::uint8_t value) {
    value = static_cast<std::uint8_t>((value >> 4) | (value << 4));
    value = static_cast<std::uint8_t>(((value & 0xCC) >> 2) | ((value & 0x33) << 2));
    value = static_cast<std::uint8_t>(((value & 0xAA) >> 1) | ((value & 0x55) << 1));
    return value;
}

std::uint32_t readBig32(const std::uint8_t* bytes) {
    return (std::uint32_t(bytes[0]) << 24) | (std::uint32_t(bytes[1]) << 16) |
        (std::uint32_t(bytes[2]) << 8) | std::uint32_t(bytes[3]);
}

std::uint32_t readLittle32(const std::uint8_t* bytes) {
    return std::uint32_t(bytes[0]) | (std::uint32_t(bytes[1]) << 8) |
        (std::uint32_t(bytes[2]) << 16) | (std::uint32_t(bytes[3]) << 24);
}

std::uint16_t readLittle16(const std::uint8_t* bytes) {
    return std::uint16_t(std::uint16_t(bytes[0]) | (std::uint16_t(bytes[1]) << 8));
}

bool arincWordValid(std::uint32_t word) {
    if (std::popcount(word) % 2 == 0) return false;
    return ((word >> 24) & 0xffu) != 0;
}

std::uint16_t crc16Ccitt(const std::uint8_t* data, std::size_t size) {
    std::uint16_t crc = 0xFFFF;
    for (std::size_t i = 0; i < size; ++i) {
        crc ^= std::uint16_t(std::uint16_t(data[i]) << 8);
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc & 0x8000) ? std::uint16_t((crc << 1) ^ 0x1021) : std::uint16_t(crc << 1);
    }
    return crc;
}

std::uint16_t crc15Can(const std::uint8_t* data, std::size_t size) {
    std::uint16_t crc = 0;
    for (std::size_t i = 0; i < size; ++i) {
        for (int bit = 7; bit >= 0; --bit) {
            const int current = (data[i] >> bit) & 1;
            const int top = (crc >> 14) & 1;
            crc = static_cast<std::uint16_t>((crc << 1) & 0x7FFF);
            if (top ^ current) crc ^= 0x4599;
        }
    }
    return crc;
}

bool milWordValid(std::uint32_t word) {
    if ((word >> 20) != 0) return false;
    if (std::popcount(word) % 2 == 0) return false;
    const auto sync = (word >> 17) & 0x7u;
    return sync == 0b100u || sync == 0b001u;
}

struct CanFrame {
    std::uint32_t id{};
    bool extended{}, remote{};
    std::uint8_t dlc{};
    std::size_t total{};
};

bool parseCanFrame(std::span<const std::uint8_t> bytes, std::size_t offset, CanFrame& out) {
    if (bytes.size() - offset < can_header_bytes) return false;
    const auto id = readLittle32(bytes.data() + offset);
    const auto flags = bytes[offset + 4];
    const auto dlc = bytes[offset + 5];
    if (dlc > 8) return false;
    const auto total = can_header_bytes + dlc + can_crc_bytes;
    if (bytes.size() - offset < total) return false;
    const bool extended = (flags & 0x1u) != 0;
    if (extended ? id > 0x1FFFFFFFu : id > 0x7FFu) return false;
    const auto declared = readLittle16(bytes.data() + offset + can_header_bytes + dlc);
    if (declared != crc15Can(bytes.data() + offset, can_header_bytes + dlc)) return false;
    out.id = id; out.extended = extended; out.remote = (flags & 0x2u) != 0;
    out.dlc = dlc; out.total = total;
    return true;
}

class Arinc429Detector final : public IProtocolDetector {
public:
    ProtocolId protocolId() const noexcept override { return protocol_word_stream; }
    ProtocolDetectionReport probe(const ProtocolDetectionContext&, std::span<const RawFrame> observations) const override {
        ProtocolDetectionReport report;
        if (observations.empty()) return report;
        std::uint64_t words = 0;
        for (const auto& frame : observations) {
            if (frame.payload.empty() || frame.payload.size() % arinc_bytes != 0) return report;
            for (std::size_t offset = 0; offset < frame.payload.size(); offset += arinc_bytes) {
                if (!arincWordValid(readBig32(frame.payload.data() + offset))) return report;
                ++words;
            }
        }
        ProtocolCandidate candidate;
        candidate.protocol = protocol_word_stream;
        candidate.strength = words >= arinc_min_words ? CandidateStrength::Strong : CandidateStrength::Tentative;
        candidate.evidence = {
            {ProtocolEvidenceKind::Structure, "ARINC 429 32-bit word (label/SDI/data/SSM)"},
            {ProtocolEvidenceKind::Checksum, "odd parity per word"},
            {ProtocolEvidenceKind::Sequence, "contiguous same-stream window"}};
        report.candidates.push_back(std::move(candidate));
        if (words >= arinc_min_words) {
            report.status = ProtocolDetectionStatus::Identified;
            report.basis = ProtocolSelectionBasis::ContentEvidence;
            report.selected = protocol_word_stream;
            report.reason = "ARINC 429 parity and label valid across " + std::to_string(words) + " words";
        } else {
            report.status = ProtocolDetectionStatus::NeedMoreData;
            report.reason = "only " + std::to_string(words) + " words observed";
        }
        return report;
    }
};

class Mil1553Detector final : public IProtocolDetector {
public:
    ProtocolId protocolId() const noexcept override { return protocol_mil1553_stream; }
    ProtocolDetectionReport probe(const ProtocolDetectionContext&, std::span<const RawFrame> observations) const override {
        ProtocolDetectionReport report;
        if (observations.empty()) return report;
        std::uint64_t words = 0;
        for (const auto& frame : observations) {
            if (frame.payload.empty() || frame.payload.size() % mil_word_bytes != 0) return report;
            for (std::size_t offset = 0; offset < frame.payload.size(); offset += mil_word_bytes) {
                if (!milWordValid(readLittle32(frame.payload.data() + offset))) return report;
                ++words;
            }
        }
        ProtocolCandidate candidate;
        candidate.protocol = protocol_mil1553_stream;
        candidate.strength = words >= mil_min_words ? CandidateStrength::Strong : CandidateStrength::Tentative;
        candidate.evidence = {
            {ProtocolEvidenceKind::Structure, "MIL-STD-1553B 20-bit word (sync/information/parity)"},
            {ProtocolEvidenceKind::Checksum, "odd parity per word"},
            {ProtocolEvidenceKind::Sequence, "contiguous same-stream window"}};
        report.candidates.push_back(std::move(candidate));
        if (words >= mil_min_words) {
            report.status = ProtocolDetectionStatus::Identified;
            report.basis = ProtocolSelectionBasis::ContentEvidence;
            report.selected = protocol_mil1553_stream;
            report.reason = "1553B sync and parity valid across " + std::to_string(words) + " words";
        } else {
            report.status = ProtocolDetectionStatus::NeedMoreData;
            report.reason = "only " + std::to_string(words) + " words observed";
        }
        return report;
    }
};

class CanDetector final : public IProtocolDetector {
public:
    ProtocolId protocolId() const noexcept override { return protocol_can_stream; }
    ProtocolDetectionReport probe(const ProtocolDetectionContext&, std::span<const RawFrame> observations) const override {
        ProtocolDetectionReport report;
        std::vector<std::uint8_t> buffer;
        for (const auto& frame : observations) buffer.insert(buffer.end(), frame.payload.begin(), frame.payload.end());
        if (buffer.empty()) return report;
        std::size_t offset = 0;
        std::uint64_t frames = 0;
        bool incomplete = false;
        while (offset < buffer.size()) {
            if (buffer.size() - offset < can_header_bytes) { incomplete = true; break; }
            const auto dlc = buffer[offset + 5];
            if (dlc <= 8 && buffer.size() - offset < can_header_bytes + dlc + can_crc_bytes) { incomplete = true; break; }
            CanFrame parsed;
            if (!parseCanFrame(buffer, offset, parsed)) break;
            ++frames;
            offset += parsed.total;
        }
        if (frames == 0) {
            if (incomplete) { report.status = ProtocolDetectionStatus::NeedMoreData; report.reason = "could not parse a CAN frame yet"; }
            return report;
        }
        ProtocolCandidate candidate;
        candidate.protocol = protocol_can_stream;
        candidate.strength = CandidateStrength::Strong;
        candidate.evidence = {
            {ProtocolEvidenceKind::Structure, "CAN 2.0 identifier and DLC container"},
            {ProtocolEvidenceKind::Checksum, "CRC-15 over identifier/flags/DLC/data"},
            {ProtocolEvidenceKind::Sequence, "length-delimited frame order"}};
        report.candidates.push_back(std::move(candidate));
        report.status = ProtocolDetectionStatus::Identified;
        report.basis = ProtocolSelectionBasis::ContentEvidence;
        report.selected = protocol_can_stream;
        report.reason = "validated " + std::to_string(frames) + " CAN frame(s)";
        return report;
    }
};

class FramedDetector final : public IProtocolDetector {
public:
    ProtocolId protocolId() const noexcept override { return protocol_framed_stream; }
    ProtocolDetectionReport probe(const ProtocolDetectionContext&, std::span<const RawFrame> observations) const override {
        ProtocolDetectionReport report;
        std::vector<std::uint8_t> buffer;
        for (const auto& frame : observations) buffer.insert(buffer.end(), frame.payload.begin(), frame.payload.end());
        if (buffer.empty()) return report;
        std::size_t offset = 0;
        std::uint64_t frames = 0;
        bool need_more = false;
        while (offset + frame_header_bytes <= buffer.size()) {
            if (buffer[offset] != frame_sync0 || buffer[offset + 1] != frame_sync1) return report;
            const auto length = readLittle16(buffer.data() + offset + 2);
            if (length > max_frame_payload) return report;
            const auto total = frame_header_bytes + length + frame_crc_bytes;
            if (offset + total > buffer.size()) { need_more = true; break; }
            const auto declared = readLittle16(buffer.data() + offset + frame_header_bytes + length);
            if (declared != crc16Ccitt(buffer.data() + offset + 2, 2 + length)) return report;
            ++frames;
            offset += total;
        }
        if (frames == 0) {
            if (need_more) { report.status = ProtocolDetectionStatus::NeedMoreData; report.reason = "sync matched, frame incomplete"; }
            return report;
        }
        ProtocolCandidate candidate;
        candidate.protocol = protocol_framed_stream;
        candidate.strength = CandidateStrength::Strong;
        candidate.evidence = {
            {ProtocolEvidenceKind::Structure, "sync B5 00 plus little-endian length"},
            {ProtocolEvidenceKind::Checksum, "CRC-16/CCITT-FALSE"},
            {ProtocolEvidenceKind::Sequence, "length-delimited frame order"}};
        report.candidates.push_back(std::move(candidate));
        report.status = ProtocolDetectionStatus::Identified;
        report.basis = ProtocolSelectionBasis::ContentEvidence;
        report.selected = protocol_framed_stream;
        report.reason = "validated " + std::to_string(frames) + " framed record(s)";
        return report;
    }
};

class Arinc429Handler final : public IProtocolHandler {
public:
    explicit Arinc429Handler(ProtocolStreamKey key) : key_(std::move(key)) {}
    ProtocolId protocolId() const noexcept override { return protocol_word_stream; }
    const ProtocolStreamKey& stream() const noexcept override { return key_; }
    ProtocolParseResult parse(const RawFrame& frame) override {
        ProtocolParseResult result;
        if (frame.payload.empty() || frame.payload.size() % arinc_bytes != 0) {
            result.status = ProtocolParseStatus::Malformed;
            result.diagnostics.push_back("ARINC 429 payload is not a whole number of 32-bit words");
            return result;
        }
        std::uint64_t words = 0;
        std::uint64_t label = 0, sdi = 0, data = 0, ssm = 0;
        for (std::size_t offset = 0; offset < frame.payload.size(); offset += arinc_bytes) {
            const auto word = readBig32(frame.payload.data() + offset);
            if (!arincWordValid(word)) {
                result.status = ProtocolParseStatus::Malformed;
                result.diagnostics.push_back("ARINC 429 parity or label invalid");
                return result;
            }
            if (words == 0) {
                label = reverse8(static_cast<std::uint8_t>((word >> 24) & 0xffu));
                sdi = (word >> 22) & 0x3u;
                data = (word >> 3) & 0x1FFFFu;
                ssm = (word >> 1) & 0x3u;
            }
            ++words;
        }
        ProtocolMessage message;
        message.stream = key_;
        message.protocol = protocol_word_stream;
        message.message_kind = "arinc429";
        message.payload = frame.payload;
        message.metadata.push_back({"word_count", words});
        message.metadata.push_back({"label", label});
        message.metadata.push_back({"sdi", sdi});
        message.metadata.push_back({"data", data});
        message.metadata.push_back({"ssm", ssm});
        message.evidence.push_back(referenceOf(key_, frame));
        message.flags = frame.flags;
        result.status = ProtocolParseStatus::Complete;
        result.messages.push_back(std::move(message));
        return result;
    }
    void reset(ProtocolResetReason) noexcept override {}
private:
    ProtocolStreamKey key_;
};

class Mil1553Handler final : public IProtocolHandler {
public:
    explicit Mil1553Handler(ProtocolStreamKey key) : key_(std::move(key)) {}
    ProtocolId protocolId() const noexcept override { return protocol_mil1553_stream; }
    const ProtocolStreamKey& stream() const noexcept override { return key_; }
    ProtocolParseResult parse(const RawFrame& frame) override {
        ProtocolParseResult result;
        if (frame.payload.empty() || frame.payload.size() % mil_word_bytes != 0) {
            result.status = ProtocolParseStatus::Malformed;
            result.diagnostics.push_back("1553B payload is not a whole number of 20-bit words");
            return result;
        }
        std::uint64_t words = 0, sync = 0, info = 0;
        for (std::size_t offset = 0; offset < frame.payload.size(); offset += mil_word_bytes) {
            const auto word = readLittle32(frame.payload.data() + offset);
            if (!milWordValid(word)) {
                result.status = ProtocolParseStatus::Malformed;
                result.diagnostics.push_back("1553B sync or parity invalid");
                return result;
            }
            if (words == 0) { sync = (word >> 17) & 0x7u; info = (word >> 1) & 0xFFFFu; }
            ++words;
        }
        ProtocolMessage message;
        message.stream = key_;
        message.protocol = protocol_mil1553_stream;
        message.payload = frame.payload;
        if (sync == 0b100u) {
            message.message_kind = "mil1553_command_or_status";
            message.metadata.push_back({"rt_address", (info >> 11) & 0x1Fu});
            message.metadata.push_back({"tr", (info >> 10) & 0x1u});
            message.metadata.push_back({"subaddress", (info >> 5) & 0x1Fu});
            message.metadata.push_back({"word_count", info & 0x1Fu});
        } else {
            message.message_kind = "mil1553_data";
            message.metadata.push_back({"data", info});
        }
        message.metadata.push_back({"words", words});
        message.evidence.push_back(referenceOf(key_, frame));
        message.flags = frame.flags;
        result.status = ProtocolParseStatus::Complete;
        result.messages.push_back(std::move(message));
        return result;
    }
    void reset(ProtocolResetReason) noexcept override {}
private:
    ProtocolStreamKey key_;
};

class CanHandler final : public IProtocolHandler {
public:
    explicit CanHandler(ProtocolStreamKey key) : key_(std::move(key)) {}
    ProtocolId protocolId() const noexcept override { return protocol_can_stream; }
    const ProtocolStreamKey& stream() const noexcept override { return key_; }
    ProtocolParseResult parse(const RawFrame& frame) override {
        ProtocolParseResult result;
        std::size_t offset = 0;
        while (offset < frame.payload.size()) {
            CanFrame parsed;
            if (!parseCanFrame(frame.payload, offset, parsed)) {
                result.status = ProtocolParseStatus::Malformed;
                result.diagnostics.push_back("CAN frame container is invalid");
                return result;
            }
            ProtocolMessage message;
            message.stream = key_;
            message.protocol = protocol_can_stream;
            message.message_kind = "can_frame";
            message.payload.assign(frame.payload.begin() + static_cast<std::ptrdiff_t>(offset + can_header_bytes),
                frame.payload.begin() + static_cast<std::ptrdiff_t>(offset + can_header_bytes + parsed.dlc));
            message.metadata.push_back({"identifier", std::uint64_t(parsed.id)});
            message.metadata.push_back({"extended", parsed.extended});
            message.metadata.push_back({"remote", parsed.remote});
            message.metadata.push_back({"dlc", std::uint64_t(parsed.dlc)});
            message.evidence.push_back(referenceOf(key_, frame));
            message.flags = frame.flags;
            result.messages.push_back(std::move(message));
            offset += parsed.total;
        }
        result.status = result.messages.empty() ? ProtocolParseStatus::NeedMoreData : ProtocolParseStatus::Complete;
        return result;
    }
    void reset(ProtocolResetReason) noexcept override {}
private:
    ProtocolStreamKey key_;
};

class FramedHandler final : public IProtocolHandler {
public:
    explicit FramedHandler(ProtocolStreamKey key) : key_(std::move(key)) {}
    ProtocolId protocolId() const noexcept override { return protocol_framed_stream; }
    const ProtocolStreamKey& stream() const noexcept override { return key_; }
    ProtocolParseResult parse(const RawFrame& frame) override {
        buffer_.insert(buffer_.end(), frame.payload.begin(), frame.payload.end());
        references_.push_back(referenceOf(key_, frame));
        ProtocolParseResult result;
        std::size_t offset = 0;
        while (offset + frame_header_bytes <= buffer_.size()) {
            if (buffer_[offset] != frame_sync0 || buffer_[offset + 1] != frame_sync1) {
                discard();
                result.status = ProtocolParseStatus::Malformed;
                result.diagnostics.push_back("framed sync mismatch");
                return result;
            }
            const auto length = readLittle16(buffer_.data() + offset + 2);
            if (length > max_frame_payload) {
                discard();
                result.status = ProtocolParseStatus::Malformed;
                result.diagnostics.push_back("framed length exceeds limit");
                return result;
            }
            const auto total = frame_header_bytes + length + frame_crc_bytes;
            if (offset + total > buffer_.size()) break;
            const auto declared = readLittle16(buffer_.data() + offset + frame_header_bytes + length);
            const auto actual = crc16Ccitt(buffer_.data() + offset + 2, 2 + length);
            if (declared != actual) {
                discard();
                result.status = ProtocolParseStatus::Malformed;
                result.diagnostics.push_back("framed CRC mismatch");
                return result;
            }
            ProtocolMessage message;
            message.stream = key_;
            message.protocol = protocol_framed_stream;
            message.message_kind = "frame";
            message.payload.assign(buffer_.begin() + static_cast<std::ptrdiff_t>(offset + frame_header_bytes),
                buffer_.begin() + static_cast<std::ptrdiff_t>(offset + frame_header_bytes + length));
            message.metadata.push_back({"length", std::uint64_t(length)});
            message.metadata.push_back({"crc16", std::uint64_t(actual)});
            message.evidence = references_;
            message.flags = frame.flags;
            result.messages.push_back(std::move(message));
            offset += total;
        }
        if (offset > 0) {
            buffer_.erase(buffer_.begin(), buffer_.begin() + static_cast<std::ptrdiff_t>(offset));
            references_.clear();
        }
        if (!result.messages.empty()) result.status = ProtocolParseStatus::Complete;
        else result.status = ProtocolParseStatus::NeedMoreData;
        return result;
    }
    void reset(ProtocolResetReason) noexcept override { discard(); }
private:
    void discard() { buffer_.clear(); references_.clear(); }
    ProtocolStreamKey key_;
    std::vector<std::uint8_t> buffer_;
    std::vector<ProtocolFrameReference> references_;
};
}

bool BuiltinProtocolFactory::supports(ProtocolId protocol) const noexcept {
    return protocol == protocol_word_stream || protocol == protocol_framed_stream ||
        protocol == protocol_mil1553_stream || protocol == protocol_can_stream;
}

std::vector<ProtocolDescriptor> BuiltinProtocolFactory::descriptors() const {
    return {
        {protocol_word_stream, "arinc429", "1", {representation_captured_words}},
        {protocol_framed_stream, "framed-crc16", "1", {representation_wire_bytes}},
        {protocol_mil1553_stream, "mil1553b", "1", {representation_captured_mil_words}},
        {protocol_can_stream, "can20", "1", {representation_wire_bytes}}};
}

std::unique_ptr<IProtocolDetector> BuiltinProtocolFactory::createDetector(ProtocolId protocol) const {
    if (protocol == protocol_word_stream) return std::make_unique<Arinc429Detector>();
    if (protocol == protocol_framed_stream) return std::make_unique<FramedDetector>();
    if (protocol == protocol_mil1553_stream) return std::make_unique<Mil1553Detector>();
    if (protocol == protocol_can_stream) return std::make_unique<CanDetector>();
    return nullptr;
}

std::unique_ptr<IProtocolHandler> BuiltinProtocolFactory::createHandler(ProtocolId protocol,
    const ProtocolStreamKey& key) const {
    if (protocol == protocol_word_stream) return std::make_unique<Arinc429Handler>(key);
    if (protocol == protocol_framed_stream) return std::make_unique<FramedHandler>(key);
    if (protocol == protocol_mil1553_stream) return std::make_unique<Mil1553Handler>(key);
    if (protocol == protocol_can_stream) return std::make_unique<CanHandler>(key);
    return nullptr;
}
}
