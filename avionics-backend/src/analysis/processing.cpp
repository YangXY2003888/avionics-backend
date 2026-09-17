#include "core/processing.hpp"
#include <cmath>
#include <limits>
#include <stdexcept>

namespace avionics {
namespace {
bool sameEpoch(const AlignedSample& a, const AlignedSample& b) {
    return a.sample.generation == b.sample.generation && a.sample.origin_generation == b.sample.origin_generation &&
        a.sample.decoder_version == b.sample.decoder_version;
}
std::optional<double> numeric(const ParameterValue& value) {
    return std::visit([](const auto& item) -> std::optional<double> {
        using T = std::decay_t<decltype(item)>;
        if constexpr (std::is_same_v<T, EnumValue>) return {};
        else {
            // Keep exact integers in storage; only safely representable values enter double-valued rules.
            if constexpr (std::is_integral_v<T>) {
                const auto extended = static_cast<long double>(item);
                if (extended > 9007199254740992.0L || extended < -9007199254740992.0L) return {};
            }
            const auto converted = static_cast<double>(item);
            return std::isfinite(converted) ? std::optional<double>(converted) : std::nullopt;
        }
    }, value);
}
EvidenceRef evidence(const AlignedSample& value) {
    return {value.sample.source, value.sample.parameter_id, value.sample.decoder_version, value.sample.generation,
        value.sample.origin_generation, value.sample.raw_record_index, value.time_ns.value_or(0)};
}
bool fresh(const AlignedSample& sample, std::uint64_t now) {
    if (!sample.usable() || !sample.time_ns || now < *sample.time_ns) return false;
    const auto age = now - *sample.time_ns;
    return sample.uncertainty_ns <= sample.sample.max_age_ns && age <= sample.sample.max_age_ns - sample.uncertainty_ns;
}
std::uint64_t distance(std::uint64_t a, std::uint64_t b) { return a > b ? a - b : b - a; }
CorrelationResult event(const std::string& id, const std::string& outcome, const AlignedSample& first,
    const AlignedSample* second = nullptr) {
    CorrelationResult result;
    result.rule = id; result.outcome = outcome; result.time_group = first.time_group;
    result.time_ns = first.time_ns.value_or(0); result.evidence.push_back(evidence(first));
    if (second) { result.evidence.push_back(evidence(*second)); result.time_ns = second->time_ns.value_or(result.time_ns); }
    return result;
}
}
AlignedSample TimeQualityProcessor::align(const ParameterSample& sample) const {
    AlignedSample out; out.sample = sample;
    if (!sample.valid) out.quality.push_back("invalid_payload");
    const ClockDefinition* clock = nullptr;
    for (const auto& candidate : clocks_) if (candidate.source == sample.source && candidate.domain == sample.clock_domain) { clock = &candidate; break; }
    if (!clock) { out.quality.push_back("unmapped_clock"); return out; }
    out.time_group = clock->group; out.uncertainty_ns = clock->uncertainty_ns;
    if (clock->offset_ns >= 0) {
        const auto offset = static_cast<std::uint64_t>(clock->offset_ns);
        if (sample.capture_time_ns > UINT64_MAX - offset) { out.quality.push_back("time_overflow"); return out; }
        out.time_ns = sample.capture_time_ns + offset;
    } else {
        const auto offset = static_cast<std::uint64_t>(-(clock->offset_ns + 1)) + 1;
        if (sample.capture_time_ns < offset) { out.quality.push_back("time_underflow"); return out; }
        out.time_ns = sample.capture_time_ns - offset;
    }
    return out;
}
AlignedSample TimeQualityProcessor::process(const ParameterSample& sample) {
    auto out = align(sample);
    // An invalid payload must not poison the ordering baseline with an arbitrary timestamp.
    if (!out.time_ns || !sample.valid) return out;
    const auto key = std::make_pair(sample.source, sample.parameter_id);
    auto found = previous_.find(key);
    if (found != previous_.end() && found->second.sample.generation == sample.generation &&
        found->second.sample.origin_generation == sample.origin_generation && found->second.sample.decoder_version == sample.decoder_version) {
        const auto& last = found->second;
        if (*out.time_ns < last.time_ns) out.quality.push_back("out_of_order");
        if (sample.sequence == last.sample.sequence) out.quality.push_back("duplicate_sequence");
        else if (sample.sequence_step && (sample.sequence < last.sample.sequence ||
            sample.sequence - last.sample.sequence != sample.sequence_step)) out.quality.push_back("sequence_gap");
        if (*out.time_ns < last.time_ns || sample.sequence == last.sample.sequence) return out;
    }
    if (!previous_.contains(key) && previous_.size() >= 4096) throw std::runtime_error("time quality state capacity exceeded");
    previous_.insert_or_assign(key, Last{sample, *out.time_ns});
    return out;
}
ProcessingService::ProcessingService(BackendConfiguration config, std::shared_ptr<IAnalysisSink> sink)
    : config_(std::move(config)), quality_(config_.clocks), sink_(std::move(sink)),
      pairs_(config_.consistency.size()), responses_(config_.responses.size()),
      dedup_last_(config_.dedup.size()) {
    if (!sink_) throw std::invalid_argument("analysis sink is required");
    for (const auto& definition : config_.reorder) reorder_windows_[definition.group] = definition.window_ns;
}
void ProcessingService::emit(CorrelationResult result) {
    sink_->result(result);
    ++stats_.results; ++stats_.outcomes[result.outcome];
    history_.push_back(std::move(result));
    if (history_.size() > 1024) history_.pop_front();
}
void ProcessingService::consume(const ParameterSample& sample) {
    std::lock_guard lock(mutex_);
    if (finished_) throw std::logic_error("analysis is already finished");
    const auto probe = quality_.align(sample);
    const auto window_it = probe.time_group.empty() ? reorder_windows_.end() : reorder_windows_.find(probe.time_group);
    if (probe.time_ns && window_it != reorder_windows_.end() && window_it->second > 0) {
        const auto group = probe.time_group;
        const auto time = *probe.time_ns;
        reorder_buffers_[group].emplace(time, sample);
        auto& maximum = reorder_max_[group];
        if (time > maximum) maximum = time;
        releaseReady(group, false);
        return;
    }
    deliver(quality_.process(sample));
}
void ProcessingService::applyDedup(AlignedSample& aligned) {
    if (config_.dedup.empty() || !aligned.time_ns) return;
    const auto& sample = aligned.sample;
    for (std::size_t i = 0; i < config_.dedup.size(); ++i) {
        const auto& rule = config_.dedup[i];
        const bool matches = (rule.source == "*" || rule.source == sample.source) &&
            (rule.parameter == "*" || rule.parameter == sample.parameter_id);
        if (!matches) continue;
        auto& last = dedup_last_[i];
        if (last && *aligned.time_ns >= *last && *aligned.time_ns - *last <= rule.window_ns) {
            aligned.quality.push_back("duplicate");
            continue;
        }
        last = *aligned.time_ns;
    }
}
void ProcessingService::deliver(AlignedSample aligned) {
    applyDedup(aligned);
    if (aligned.usable()) {
        auto& watermark = watermarks_[aligned.time_group];
        if (*aligned.time_ns < watermark) aligned.quality.push_back("late_for_group");
        else {
            // A response arriving EXACTLY at its deadline is processed before expiry.
            expire(aligned.time_group, *aligned.time_ns, false);
            watermark = *aligned.time_ns;
        }
    }
    sink_->sample(aligned);
    ++stats_.samples;
    if (aligned.usable()) ++stats_.usable; else ++stats_.rejected;
    processConsistency(aligned); processResponse(aligned);
}
void ProcessingService::releaseReady(const std::string& group, bool force) {
    const auto window_it = reorder_windows_.find(group);
    const auto buffer_it = reorder_buffers_.find(group);
    if (window_it == reorder_windows_.end() || buffer_it == reorder_buffers_.end()) return;
    auto& buffer = buffer_it->second;
    const auto maximum = reorder_max_[group];
    const auto window = window_it->second;
    const auto cutoff = (force || maximum < window) ? std::numeric_limits<std::uint64_t>::max() : maximum - window;
    while (!buffer.empty()) {
        auto it = buffer.begin();
        if (!force && it->first > cutoff) break;
        const auto raw = it->second;
        buffer.erase(it);
        deliver(quality_.process(raw));
    }
}
void ProcessingService::processConsistency(const AlignedSample& sample) {
    for (std::size_t i = 0; i < config_.consistency.size(); ++i) {
        const auto& rule = config_.consistency[i]; auto& pair = pairs_[i];
        const bool left = rule.left.matches(sample.sample), right = rule.right.matches(sample.sample);
        if (!left && !right) continue;
        auto& side = left ? pair.left : pair.right;
        const bool changed = side && !sameEpoch(*side, sample);
        if (changed || !sample.usable()) { pair.left.reset(); pair.right.reset(); }
        if (!sample.usable()) continue;
        side = sample;
        if (!pair.left || !pair.right) continue;
        const auto& a = *pair.left; const auto& b = *pair.right;
        if (a.time_group != b.time_group) continue;
        const auto now = std::max(*a.time_ns, *b.time_ns);
        if (!fresh(a, now) || !fresh(b, now)) continue;
        const auto difference = distance(*a.time_ns, *b.time_ns);
        if (difference > rule.window_ns || a.uncertainty_ns > rule.window_ns - difference ||
            b.uncertainty_ns > rule.window_ns - difference - a.uncertainty_ns) continue;
        if (a.sample.unit != b.sample.unit) { emit(event(rule.id, "unit_mismatch", a, &b)); continue; }
        const auto av = numeric(a.sample.value), bv = numeric(b.sample.value);
        if (!av || !bv) { emit(event(rule.id, "non_numeric_pair", a, &b)); continue; }
        const auto residual = *av - *bv;
        auto result = event(rule.id, !std::isfinite(residual) ? "numeric_overflow" :
            (std::abs(residual) <= rule.tolerance ? "consistent" : "inconsistent"), a, &b);
        if (std::isfinite(residual)) result.metric = residual;
        result.time_ns = now; emit(std::move(result));
    }
}
void ProcessingService::processResponse(const AlignedSample& sample) {
    for (std::size_t i = 0; i < config_.responses.size(); ++i) {
        const auto& rule = config_.responses[i]; auto& state = responses_[i];
        const bool command = rule.command.matches(sample.sample), response = rule.response.matches(sample.sample);
        if (!command && !response) continue;
        auto& previous = command ? state.command : state.response;
        const bool boundary = previous && !sameEpoch(*previous, sample);
        const auto value = numeric(sample.sample.value);
        if (boundary || !sample.usable() || !value) {
            if (state.pending) emit(event(rule.id, "response_cancelled", *state.pending, &sample));
            state = {};
            if (!sample.usable() || !value) continue;
        }
        auto& old = command ? state.command : state.response;
        const auto old_value = old ? numeric(old->sample.value) : std::nullopt;
        const auto threshold = command ? rule.command_threshold : rule.response_threshold;
        const bool rising = old && old_value && old->time_group == sample.time_group && fresh(*old, *sample.time_ns) &&
            *old_value < threshold && *value >= threshold;
        if (command && rising) {
            if (state.pending) emit(event(rule.id, "response_superseded", *state.pending, &sample));
            state.pending.reset();
            const auto baseline = state.response ? numeric(state.response->sample.value) : std::nullopt;
            if (!state.response || state.response->time_group != sample.time_group || !fresh(*state.response, *sample.time_ns) || !baseline)
                emit(event(rule.id, "response_missing_baseline", sample));
            else if (*baseline >= rule.response_threshold) emit(event(rule.id, "response_already_high", sample, &*state.response));
            else if (*sample.time_ns > UINT64_MAX - rule.max_delay_ns) emit(event(rule.id, "response_time_overflow", sample));
            else state.pending = sample;
        }
        if (response && rising && state.pending && state.pending->time_group == sample.time_group &&
            *sample.time_ns >= *state.pending->time_ns) {
            const auto delta = *sample.time_ns - *state.pending->time_ns;
            const auto uncertainty = state.pending->uncertainty_ns + sample.uncertainty_ns;
            auto result = event(rule.id, "response_observed", *state.pending, &sample);
            result.metric = static_cast<double>(delta) / 1000000.0;
            result.latency_low_ns = delta > uncertainty ? delta - uncertainty : 0;
            result.latency_high_ns = delta > UINT64_MAX - uncertainty ? UINT64_MAX : delta + uncertainty;
            if (*result.latency_high_ns > rule.max_delay_ns || delta < uncertainty) result.outcome = "response_time_ambiguous";
            emit(std::move(result)); state.pending.reset();
        }
        old = sample;
    }
}
void ProcessingService::expire(const std::string& group, std::uint64_t time, bool inclusive) {
    for (std::size_t i = 0; i < responses_.size(); ++i) {
        auto& state = responses_[i]; const auto& rule = config_.responses[i];
        if (!state.pending || state.pending->time_group != group) continue;
        const auto deadline = *state.pending->time_ns + rule.max_delay_ns;
        auto uncertainty = state.pending->uncertainty_ns;
        for (const auto& clock : config_.clocks) if (clock.source == rule.response.source && clock.group == group)
            uncertainty += clock.uncertainty_ns;
        const auto final_deadline = deadline > UINT64_MAX - uncertainty ? UINT64_MAX : deadline + uncertainty;
        if (time < final_deadline || (!inclusive && time == final_deadline)) continue;
        const bool observed = state.response && state.response->time_group == group && fresh(*state.response, deadline);
        auto result = event(rule.id, observed ? "response_timeout" : "response_observation_gap", *state.pending,
            state.response ? &*state.response : nullptr);
        result.time_ns = deadline; emit(std::move(result)); state.pending.reset();
    }
}
void ProcessingService::advance(const std::string& group, std::uint64_t watermark) {
    std::lock_guard lock(mutex_);
    if (finished_) throw std::logic_error("analysis is already finished");
    bool known = false;
    for (const auto& clock : config_.clocks) if (clock.group == group) known = true;
    if (!known || watermark < watermarks_[group]) throw std::invalid_argument("unknown group or backward watermark");
    if (reorder_windows_.count(group)) {
        auto& maximum = reorder_max_[group];
        if (watermark > maximum) maximum = watermark;
        releaseReady(group, false);
    }
    expire(group, watermark, true); watermarks_[group] = watermark;
}
void ProcessingService::finish() {
    std::lock_guard lock(mutex_);
    if (finished_) return;
    for (const auto& entry : reorder_buffers_) releaseReady(entry.first, true);
    for (std::size_t i = 0; i < responses_.size(); ++i) if (responses_[i].pending) {
        emit(event(config_.responses[i].id, "response_unresolved", *responses_[i].pending)); responses_[i].pending.reset();
    }
    sink_->flush(); finished_ = true;
}
ProcessingStats ProcessingService::stats() const { std::lock_guard lock(mutex_); return stats_; }
std::vector<CorrelationResult> ProcessingService::results() const {
    std::lock_guard lock(mutex_); return {history_.begin(), history_.end()};
}
}
