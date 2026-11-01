#pragma once

#include <cstddef>
#include "segment_models.h"
#include "rack_models.h"
#include "output_limiter.h"
#include "loop_exciter.h"
#include "multitap_patterns.h"
#include "analog_color.h"
#include "a04_color.h"
#include "legacy_voicings.h"

namespace echolab {

// These are project algorithms. Effect models are provisional structures, not
// measured reproductions of Line 6 products. Published IDs remain stable.
enum class Model {
    Digital = 0, Analog = 1, Tube = 2, PingPong = 3, MultiHead = 4,
    LoRes = 5, Dynamic = 6, Tape = 7, Sweep = 8, AnalogMod = 9,
    DigitalMod = 10, Reverse = 11, AutoVolume = 12, DigitalRack = 13,
    EchoPlatter = 14, StereoDelays = 15, LoopSampler = 16, ExciterEcho = 17,
    SweepUp = 18, TubeA04 = 19
};
constexpr int modelCount = 20;
constexpr int modulationDestinationCount = 13; // Published route IDs 0..12.

struct Parameters {
    float delay_ms = 350.0f;
    float feedback = 0.28f;
    float mix = 0.39f;
    float tone_hz = 3500.0f;
    float drive = 0.4f;
    Model model = Model::Digital;
    int bit_depth = 8;
    float mod_rate_hz = 1.0f;
    float mod_depth_ms = 6.0f;
    float duck_threshold_db = -24.0f;
    float duck_amount = 0.9f;
    float attack_ms = 5.0f;
    float release_ms = 250.0f;
    float swell_ms = 250.0f;
    float stereo_ratio = 1.5f;
    int loop_action = 1; // 0 stop, 1 record, 2 play, 3 overdub, 4 clear.
    bool limiter_enabled = true;
    float limiter_ceiling = safety::maximumCeiling;
    float limiter_release_ms = 150.0f;
    float sample_rate_hz = 8000.0f; // 0 disables LoRes sample-rate reduction.
    float exciter_amount = 0.1f;
    float exciter_tune_hz = 4000.0f;
    int tap_pattern = 0;
    int mod1_source = 0;
    int mod1_destination = 0;
    float mod1_amount = 0.0f;
    int mod2_source = 0;
    int mod2_destination = 0;
    float mod2_amount = 0.0f;
    float feedback_lowpass_hz = 12000.0f; // Nominal; 0 diagnostic bypass.
    float drift_amount = 0.25f; // Very slow, bounded microvariation, 0 disables.
    float feedback_highpass_hz = 80.0f; // Return only; 0 diagnostic bypass.
    float lores_rate_end_hz = 1200.0f;
    float lores_rate_decay_ms = 1500.0f;
    float lores_rate_envelope_amount = 1.0f; // 0 fixes Fe at sample_rate_hz.
    float digital_rack_rate_hz = 16000.0f; // Rack converter Fe; 0=full host Fs.
    float lores_cutoff_hz = 2000.0f; // Wet reconstruction only, 0=bypass.
    int voicing = 0; // 0=model selection, 1=A04, 2=A05, 3=B06.
    bool modulation_enabled = true; // CV/wheel and functional envelopes remain.
    bool exciter_highpass_enabled = false; // Opt-in A04 Exciter return HP; legacy presets stay unchanged.
    bool wet_tone_enabled = false; // Optional output LP, separate from model/feedback filters.
    bool stereo_timing_enabled = false; // Opt-in independent coloured L/R delay times.
};

inline bool supportsStereoTiming(Model model) noexcept {
    return model == Model::StereoDelays || model == Model::Analog || model == Model::AnalogMod ||
        model == Model::Tube || model == Model::TubeA04 || model == Model::Tape;
}
inline bool stereoTimingActive(const Parameters& p) noexcept {
    return p.model == Model::StereoDelays || (p.stereo_timing_enabled && supportsStereoTiming(p.model));
}

struct ParameterEvent {
    std::size_t offset = 0; // Applied before this sample; frames means after block.
    Parameters parameters;
};

class Engine {
public:
    // Counts float samples, not bytes. Zero means invalid/overflowing request.
    // Two channels * two banks * (ceil(Fs*Tmax/1000) + 4 interpolation margin).
    // Reverse shares these banks with the other models; no per-model buffer.
    static std::size_t requiredSamples(float sampleRate, float maxDelayMs) noexcept;
    bool prepare(float sampleRate, float maxDelayMs, float* memory,
                 std::size_t count) noexcept;
    void setParameters(const Parameters& parameters) noexcept;
    // CV inputs are bipolar [-1,1], wheel is unipolar [0,1]; invalid values=0.
    // Cached controls have no coefficient computation or allocation.
    void setExternalModulation(float cv1, float cv2, float wheel) noexcept;
    const Parameters& parameters() const noexcept { return target_; }
    void reset() noexcept;
    bool prepared() const noexcept { return memory_ != nullptr; }
    void processSample(float inL, float inR, float& outL, float& outR) noexcept;
    // Null input channels supply silence; null output channels discard output.
    // Input/output channel buffers may alias at matching sample positions.
    void processBlock(const float* inL, const float* inR, float* outL,
                      float* outR, std::size_t frames) noexcept;
    // Rejects an invalid timeline before changing audio or engine state.
    bool processBlock(const float* inL, const float* inR, float* outL,
                      float* outR, std::size_t frames,
                      const ParameterEvent* events, std::size_t eventCount) noexcept;

private:
    float readDelay(unsigned channel, float samples) const noexcept;
    static float finiteSignal(float value) noexcept;
    float* memory_ = nullptr;
    std::size_t length_ = 0;
    std::size_t write_ = 0;
    float sampleRate_ = 48000.0f;
    float maximumMs_ = 2500.0f;
    float maximumSamples_ = 120000.0f;
    Parameters target_{};
    float delayTarget_ = 16800.0f;
    float delay_ = 16800.0f;
    float feedback_ = 0.28f;
    float mix_ = 0.39f;
    float drive_ = 0.0f;
    float tonePole_ = 0.0f;
    float toneTarget_ = 0.0f;
    float controlSlew_ = 0.0f;
    float delaySlew_ = 0.0f;
    float dcPole_ = 0.0f;
    float phase_ = 0.0f;
    float filter1_[2]{};
    float filter2_[2]{};
    float wetTone_[2]{};
    float dcInput_[2]{};
    float dcOutput_[2]{};
    float modPhase_ = 0.0f;
    float flutterPhase_ = 0.0f;
    float modRate_ = 0.7f;
    float modDepth_ = 96.0f;
    float modDepthTarget_ = 96.0f;
    float quantizerScale_ = 128.0f;
    float detector_ = 0.0f;
    float detectorAttackPole_ = 0.0f;
    float detectorReleasePole_ = 0.0f;
    float duckThreshold_ = 0.063095734f;
    float duckThresholdTarget_ = 0.063095734f;
    float duckAmount_ = 0.9f;
    float duckGain_ = 1.0f;
    float swellSlew_ = 0.0f;
    float swellGain_ = 0.0f;
    float swellTarget_ = 0.0f;
    float sweepLowPole_ = 0.0f;
    float extraFilter1_[2]{};
    float extraFilter2_[2]{};
    bool swellGate_ = false;
    std::size_t segmentFrames_ = 1;
    std::size_t segmentFadeFrames_ = 1;
    segments::ReverseState reverse_{};
    segments::LoopState looper_{};
    rack::State rack_{};
    safety::Limiter limiter_{};
    exciter::State exciter_{};
    float externalMod_[3]{};
    float toneLowPole_ = 0.997385442f;
    float toneHighPole_ = 0.059164511f;
    float maximumModDepth_ = 2400.0f;
    float loresPhase_ = 1.0f;
    float loresRateRatio_ = 1.0f/6.0f;
    float loresHeld_[2]{};
    color::Saturator saturator_{};
    color::FeedbackLowpass feedbackLowpass_{};
    float driftAmount_ = 0.25f;
    float driftPhase_[3] = {0.17f,0.43f,0.71f};
    float driftIncrement_[3]{};
    float feedbackHighpassPole_[3]{};
    float feedbackHighpassInput_[2]{};
    float feedbackHighpassOutput_[2]{};
    float loresRateEnvelope_ = 0.0f;
    float loresRateDecaySlew_ = 0.0f;
    float loresRateEnvelopeError_ = 0.0f;
    float phraseEnvelope_ = 0.0f;
    float phraseReleasePole_ = 0.0f;
    bool phraseGate_ = false;
    float sweepProgress_ = 0.0f;
    float sweepProgressError_ = 0.0f;
    color::A04Color a04Color_{};
    color::TubeColor tubeColor_{};
    color::Regenerator regenerator_{};
    legacy::ExciterA04 exciterA04_{};
    legacy::ExciterA05 exciterA05_{};
    int voicing_ = 3;
    float loresCutoffPole_ = 0.0f;
};

const char* modelName(Model model) noexcept;
int resolvedVoicing(Model model,int requested) noexcept;
const char* voicingName(int resolved) noexcept;

} // namespace echolab
