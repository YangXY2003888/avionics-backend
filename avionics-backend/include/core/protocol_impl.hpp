#pragma once
#include "core/configuration.hpp"
#include "core/protocol_interfaces.hpp"
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <tuple>
#include <vector>

namespace avionics {
// Internal identifiers for the built-in software protocols. They do not claim
// any real bus numbering; the numbering is defined only inside this program.
inline constexpr ProtocolId protocol_word_stream = 0x00010001u;
inline constexpr ProtocolId protocol_framed_stream = 0x00010002u;
inline constexpr ProtocolId protocol_mil1553_stream = 0x00010003u;
inline constexpr ProtocolId protocol_can_stream = 0x00010004u;
inline constexpr ProtocolId protocol_can_log_stream = 0x00010005u;

// Represented capture formats. A stream must state one of these explicitly
// before the router may restrict detection to a matching representation.
inline constexpr const char* representation_captured_words = "captured_words";
inline constexpr const char* representation_captured_mil_words = "captured_mil_words";
inline constexpr const char* representation_wire_bytes = "wire_bytes";
inline constexpr const char* representation_captured_can = "captured_can_frames";

class BuiltinProtocolFactory final : public IProtocolFactory {
public:
    std::vector<ProtocolDescriptor> descriptors() const override;
    std::unique_ptr<IProtocolDetector> createDetector(ProtocolId) const override;
    std::unique_ptr<IProtocolHandler> createHandler(ProtocolId, const ProtocolStreamKey&) const override;
    bool supports(ProtocolId) const noexcept;
};

class ProtocolRouter final : public IProtocolRouter {
public:
    explicit ProtocolRouter(std::shared_ptr<const IProtocolFactory> factory);
    void setSelection(const StreamAddress&, ProtocolSelection) override;
    ProtocolSelection selection(const StreamAddress&) const override;
    void setInputDescriptor(const StreamAddress&, const ProtocolInputDescriptor&) override;
    ProtocolInputDescriptor inputDescriptor(const StreamAddress&) const override;
    void setLimits(const ProtocolRoutingLimits&) override;
    ProtocolRouteResult route(const RawFrame&) override;
    void resetStream(const StreamAddress&, ProtocolResetReason) override;
    void removeStream(const StreamAddress&) override;
private:
    struct AddressState {
        ProtocolSelection selection;
        ProtocolInputDescriptor descriptor;
    };
    struct StreamState {
        ProtocolStreamKey key{};
        std::optional<ProtocolId> selected;
        ProtocolSelectionBasis basis{ProtocolSelectionBasis::None};
        std::unique_ptr<IProtocolHandler> handler;
        std::vector<RawFrame> observations;
        std::size_t observation_bytes{};
        std::vector<ProtocolCandidate> candidates;
    };
    using AddressKey = std::tuple<std::string, std::uint32_t, std::string>;
    using StreamKeyTuple = std::tuple<std::string, std::uint32_t, std::string, std::uint64_t, std::uint64_t>;
    static AddressKey addressKey(const StreamAddress&);
    static StreamKeyTuple streamKey(const ProtocolStreamKey&);
    const AddressState& addressOr(const StreamAddress&) const;
    void clearAddress(const StreamAddress&);
    ProtocolDetectionReport probeStream(StreamState&, const ProtocolInputDescriptor&, const RawFrame&);
    ProtocolParseResult replayObservations(StreamState&);
    bool supported(ProtocolId) const;
    ProtocolDescriptor descriptorFor(ProtocolId) const;
    std::shared_ptr<const IProtocolFactory> factory_;
    ProtocolRoutingLimits limits_{};
    std::map<AddressKey, AddressState> addresses_;
    std::map<StreamKeyTuple, StreamState> streams_;
};

class DictionaryMessageDecoder final : public IProtocolMessageDecoder {
public:
    explicit DictionaryMessageDecoder(BackendConfiguration configuration);
    std::vector<ParameterSample> decode(const ProtocolMessage&) override;
private:
    BackendConfiguration config_;
};

class ProtocolPipelineDecoder final : public IProtocolPipelineDecoder {
public:
    ProtocolPipelineDecoder(std::unique_ptr<ProtocolRouter> router,
        std::unique_ptr<IProtocolMessageDecoder> message_decoder);
    IProtocolRouter& router() noexcept override { return *router_; }
    IProtocolMessageDecoder& messageDecoder() noexcept override { return *message_decoder_; }
    std::vector<ParameterSample> decode(const RawFrame&) override;
private:
    std::unique_ptr<ProtocolRouter> router_;
    std::unique_ptr<IProtocolMessageDecoder> message_decoder_;
};
}
