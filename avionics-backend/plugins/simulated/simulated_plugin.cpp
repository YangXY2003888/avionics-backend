#include "plugin_api/plugin_support.hpp"
#include <array>
#include <bit>
#include <cstdint>
#include <limits>
#include <random>

namespace {
using namespace avionics::plugin;
// Sample data source only: period/jitter/loss/valid/arrival are for exercising
// the data path (missing and invalid observations). It is not a real dynamic
// process simulation and must not be used for research conclusions.
class Simulated final : public Worker {
public:
    using Worker::Worker;
    ~Simulated() override { stop(); }
    void configure(const char* text) override {
        prepareConfigure();
        auto options = parse(text);
        const auto period = integer(options, "period_ms", 10, 1, 60000);
        const auto initial = integer(options, "initial_milli", 20000, INT32_MIN, INT32_MAX);
        const auto step = integer(options, "step_milli", 100, INT32_MIN, INT32_MAX);
        const auto channel = integer(options, "channel", 0, 0, 65535);
        const auto fail = integer(options, "fail_start", 0, 0, 1);
        const auto phase = integer(options, "phase_ms", 0, 0, 600000);
        const auto jitter = integer(options, "jitter_ms", 0, 0, 60000);
        const auto loss = integer(options, "loss_prob", 0, 0, 100);
        const auto valid = integer(options, "valid_prob", 100, 0, 100);
        const auto arrival = integer(options, "arrival_delay_ms", 0, 0, 60000);
        noUnknown(options);
        period_ms_ = period; initial_ = initial; step_ = step;
        channel_ = static_cast<std::uint32_t>(channel); fail_start_ = fail != 0;
        phase_ms_ = phase; jitter_ms_ = jitter; loss_prob_ = loss; valid_prob_ = valid; arrival_delay_ms_ = arrival;
        configured_ = true;
    }
private:
    void beforeStart() override { if (fail_start_) throw std::runtime_error("injected start failure"); }
    bool roll(std::int64_t percent) { return static_cast<std::int64_t>(generator_() % 100u) < percent; }
    void run() override {
        generator_.seed(std::random_device{}());
        if (phase_ms_ > 0 && waitFor(std::chrono::milliseconds(phase_ms_))) return;
        std::uint64_t sequence{};
        while (!stop_requested_) {
            const std::int64_t value = initial_ + static_cast<std::int64_t>(sequence % 1000) * step_;
            const auto bits = std::bit_cast<std::uint64_t>(value);
            std::array<std::uint8_t, 8> bytes{};
            for (std::size_t i = 0; i < bytes.size(); ++i) bytes[i] = static_cast<std::uint8_t>(bits >> (8 * i));
            if (!roll(loss_prob_)) {
                if (arrival_delay_ms_ > 0 && waitFor(std::chrono::milliseconds(arrival_delay_ms_))) break;
                BusFrameView frame{};
                frame.struct_size = sizeof(frame); frame.protocol = BUS_PROTOCOL_SIMULATED;
                frame.channel = channel_; frame.clock_domain = BUS_CLOCK_HOST_MONOTONIC;
                frame.capture_time_ns = host_.monotonic_now_ns(host_.context); frame.sequence = sequence;
                frame.payload_size = static_cast<std::uint32_t>(bytes.size()); frame.payload = bytes.data();
                if (!roll(valid_prob_)) frame.flags = BUS_FRAME_INVALID;
                emit(frame);
            }
            ++sequence;
            auto delay = period_ms_;
            if (jitter_ms_ > 0) delay += static_cast<std::int64_t>(generator_() % (2u * static_cast<std::uint32_t>(jitter_ms_) + 1u)) - jitter_ms_;
            if (delay < 1) delay = 1;
            if (waitFor(std::chrono::milliseconds(delay))) break;
        }
    }
    std::int64_t period_ms_{10}, initial_{20000}, step_{100};
    std::uint32_t channel_{};
    bool fail_start_{};
    std::int64_t phase_ms_{}, jitter_ms_{}, loss_prob_{}, valid_prob_{100}, arrival_delay_ms_{};
    std::mt19937 generator_;
};
}
extern "C" BUS_EXPORT std::int32_t BUS_CALL bus_plugin_query(std::uint32_t abi, BusPluginApi* output) {
    return Glue<Simulated>::query(abi, output, "simulated-v1");
}
