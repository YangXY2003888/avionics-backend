#include "plugin_api/plugin_support.hpp"
#include <array>
#include <bit>
#include <cstdint>
#include <limits>
#include <random>
#include <string>

namespace {
using namespace avionics::plugin;

enum class Profile { None, Command, Feedback, SensorA, SensorB };

Profile parseProfile(const std::string& text) {
    if (text == "none") return Profile::None;
    if (text == "command") return Profile::Command;
    if (text == "feedback") return Profile::Feedback;
    if (text == "sensor_a") return Profile::SensorA;
    if (text == "sensor_b") return Profile::SensorB;
    throw std::invalid_argument("unknown profile: " + text);
}

class RandomSource final : public Worker {
public:
    using Worker::Worker;
    ~RandomSource() override { stop(); }
    void configure(const char* text) override {
        prepareConfigure();
        auto options = parse(text);
        const auto period = integer(options, "period_ms", 10, 1, 60000);
        const auto random_mode = integer(options, "random", 0, 0, 1);
        const auto seed = integer(options, "seed", 0, 0, INT64_MAX);
        const auto initial = integer(options, "initial_milli", 20000, INT32_MIN, INT32_MAX);
        const auto step = integer(options, "step_milli", 100, INT32_MIN, INT32_MAX);
        const auto spread = integer(options, "spread_milli", 100, 0, INT32_MAX);
        const auto channel = integer(options, "channel", 0, 0, 65535);
        const auto fail = integer(options, "fail_start", 0, 0, 1);
        const auto scenario_seed = integer(options, "scenario_seed", 1, 0, INT64_MAX);
        const auto response_delay = integer(options, "response_delay", 1, 0, 1000);
        profile_ = parseProfile(take(options, "profile", "none"));
        noUnknown(options);
        period_ms_ = period; random_mode_ = random_mode != 0; seed_ = seed;
        initial_ = initial; step_ = step; spread_ = spread;
        channel_ = static_cast<std::uint32_t>(channel); fail_start_ = fail != 0;
        scenario_seed_ = static_cast<std::uint64_t>(scenario_seed);
        response_delay_ = static_cast<std::uint64_t>(response_delay);
        configured_ = true;
    }
private:
    void beforeStart() override { if (fail_start_) throw std::runtime_error("injected start failure"); }
    std::int64_t nextValue(std::uint64_t sequence) {
        if (!random_mode_) return initial_ + static_cast<std::int64_t>(sequence % 1000) * step_;
        std::uniform_int_distribution<std::int64_t> distribution(-spread_, spread_);
        return initial_ + distribution(generator_);
    }
    std::uint64_t mix(std::uint64_t salt, std::uint64_t bucket) const {
        std::uint64_t value = scenario_seed_ ^ (salt * 0x9E3779B97F4A7C15ull) ^ (bucket * 0xD6E8FEB86659FD93ull);
        value += 0x9E3779B97F4A7C15ull;
        value = (value ^ (value >> 30)) * 0xBF58476D1CE4E5B9ull;
        value = (value ^ (value >> 27)) * 0x94D049BB133111EBull;
        return value ^ (value >> 31);
    }
    bool commandHigh(std::uint64_t bucket) const { return (mix(1, bucket / 8) % 2) == 0; }
    bool feedbackHigh(std::uint64_t bucket) const {
        if ((mix(2, bucket / 8) % 4) == 0) return false;
        const auto delayed = bucket >= response_delay_ ? bucket - response_delay_ : 0;
        return commandHigh(delayed);
    }
    std::int64_t sensorBase(std::uint64_t bucket) const {
        return 10000 + static_cast<std::int64_t>(mix(3, bucket / 2) % 400) - 200;
    }
    std::int64_t sensorB(std::uint64_t bucket) const {
        const auto base = sensorBase(bucket);
        if ((mix(4, bucket / 2) % 5) == 0) {
            const auto magnitude = static_cast<std::int64_t>(mix(5, bucket) % 800) + 500;
            return base + ((mix(6, bucket) % 2) ? magnitude : -magnitude);
        }
        return base + static_cast<std::int64_t>(mix(7, bucket) % 100) - 50;
    }
    std::int64_t scenarioValue(std::uint64_t bucket) const {
        switch (profile_) {
        case Profile::Command: return commandHigh(bucket) ? 1000 : 0;
        case Profile::Feedback: return feedbackHigh(bucket) ? 1000 : 0;
        case Profile::SensorA: return sensorBase(bucket);
        case Profile::SensorB: return sensorB(bucket);
        case Profile::None: break;
        }
        return 0;
    }
    void send(std::uint64_t& sequence, std::int64_t value, std::uint64_t capture_ns) {
        const auto bits = std::bit_cast<std::uint64_t>(value);
        std::array<std::uint8_t, 8> bytes{};
        for (std::size_t i = 0; i < bytes.size(); ++i) bytes[i] = static_cast<std::uint8_t>(bits >> (8 * i));
        BusFrameView frame{};
        frame.struct_size = sizeof(frame); frame.protocol = BUS_PROTOCOL_SIMULATED;
        frame.channel = channel_; frame.clock_domain = BUS_CLOCK_HOST_MONOTONIC;
        frame.capture_time_ns = capture_ns; frame.sequence = sequence++;
        frame.payload_size = static_cast<std::uint32_t>(bytes.size()); frame.payload = bytes.data();
        emit(frame);
    }
    void runRamp() {
        if (random_mode_) {
            if (seed_ != 0) generator_.seed(static_cast<std::uint64_t>(seed_));
            else generator_.seed(std::random_device{}());
        }
        std::uint64_t sequence{};
        while (!stop_requested_) {
            send(sequence, nextValue(sequence), host_.monotonic_now_ns(host_.context));
            if (waitFor(std::chrono::milliseconds(period_ms_))) break;
        }
    }
    void runScenario() {
        const auto period_ns = static_cast<std::uint64_t>(period_ms_) * 1000000ull;
        std::uint64_t sequence{};
        while (!stop_requested_) {
            const auto now_ns = host_.monotonic_now_ns(host_.context);
            const auto bucket = now_ns / period_ns;
            send(sequence, scenarioValue(bucket), bucket * period_ns);
            const auto next = std::chrono::steady_clock::time_point(std::chrono::nanoseconds((bucket + 1) * period_ns));
            if (waitUntil(next)) break;
        }
    }
    void run() override { if (profile_ != Profile::None) runScenario(); else runRamp(); }
    std::int64_t period_ms_{10}, initial_{20000}, step_{100}, spread_{100}, seed_{};
    std::uint32_t channel_{};
    bool random_mode_{}, fail_start_{};
    Profile profile_{Profile::None};
    std::uint64_t scenario_seed_{1}, response_delay_{1};
    std::mt19937_64 generator_;
};
}
extern "C" BUS_EXPORT std::int32_t BUS_CALL bus_plugin_query(std::uint32_t abi, BusPluginApi* output) {
    return Glue<RandomSource>::query(abi, output, "random-v1");
}
