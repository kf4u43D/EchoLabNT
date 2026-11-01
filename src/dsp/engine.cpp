#include "engine.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

namespace echolab {
namespace {
constexpr float pi = 3.14159265358979323846f;
float bounded(float value, float low, float high, float fallback) noexcept {
    if (!std::isfinite(value)) value = fallback;
    return std::max(low, std::min(high, value));
}
float loopGuard(float value,float feedback) noexcept {
    const float ceiling=feedback > 1.0f ? 4.0f : 1.0e12f;
    return bounded(value,-ceiling,ceiling,0.0f);
}
}

const char* modelName(Model model) noexcept {
    switch (model) {
    case Model::Analog: return "analog";
    case Model::Tube: return "tube";
    case Model::TubeA04: return "tubea04";
    case Model::PingPong: return "pingpong";
    case Model::MultiHead: return "multihead";
    case Model::LoRes: return "8bit";
    case Model::Dynamic: return "dynamic";
    case Model::Tape: return "tape";
    case Model::Sweep: return "sweep";
    case Model::SweepUp: return "sweepup";
    case Model::AnalogMod: return "analogmod";
    case Model::DigitalMod: return "digitalmod";
    case Model::Reverse: return "reverse";
    case Model::AutoVolume: return "autovolume";
    case Model::DigitalRack: return "digitalrack";
    case Model::EchoPlatter: return "platter";
    case Model::StereoDelays: return "stereo";
    case Model::LoopSampler: return "looper";
    case Model::ExciterEcho: return "exciter";
    default: return "digital";
    }
}

int resolvedVoicing(Model model,int requested) noexcept {
    if (model == Model::TubeA04) return 1;
    if (model == Model::Tube) return 3;
    if (requested >= 1 && requested <= 3) return requested;
    switch (model) {
    case Model::Analog: case Model::AnalogMod: case Model::Tape:
    case Model::EchoPlatter: case Model::DigitalMod: case Model::ExciterEcho: return 1;
    case Model::Digital: case Model::AutoVolume: return 2;
    default: return 3;
    }
}
const char* voicingName(int resolved) noexcept {
    return resolved == 1 ? "a04" : (resolved == 2 ? "a05" : (resolved == 3 ? "b06" : "auto"));
}

std::size_t Engine::requiredSamples(float fs, float maxMs) noexcept {
    if (!std::isfinite(fs) || !std::isfinite(maxMs) || fs < 8000.0f ||
        fs > 384000.0f || maxMs <= 0.0f || maxMs > 60000.0f)
        return 0;
    const double samples = std::ceil(static_cast<double>(fs) * maxMs / 1000.0) + 4.0;
    if (samples > static_cast<double>(std::numeric_limits<std::size_t>::max() /
                                      (4 * sizeof(float))))
        return 0;
    return 4 * static_cast<std::size_t>(samples);
}

bool Engine::prepare(float fs, float maxMs, float* memory, std::size_t count) noexcept {
    const std::size_t required = requiredSamples(fs, maxMs);
    if (!required || !memory || count < required) {
        memory_ = nullptr;
        length_ = 0;
        return false;
    }
    sampleRate_ = fs;
    maximumMs_ = maxMs;
    maximumSamples_ = std::max(1.0f, static_cast<float>(static_cast<double>(fs) * maxMs / 1000.0));
    length_ = required / 2;
    memory_ = memory;
    controlSlew_ = 1.0f - std::exp(-1.0f / (0.010f * fs));
    delaySlew_ = 1.0f - std::exp(-1.0f / (0.025f * fs));
    dcPole_ = std::exp(-2.0f * pi * 15.0f / fs);
    toneLowPole_=std::exp(-2.0f*pi*20.0f/fs);
    toneHighPole_=std::exp(-2.0f*pi*0.45f);
    maximumModDepth_=std::min(maximumSamples_,fs*0.05f);
    driftIncrement_[0]=0.031f/fs;driftIncrement_[1]=0.053f/fs;driftIncrement_[2]=0.079f/fs;
    saturator_.prepare(fs);rack_.prepare(fs);a04Color_.prepare(fs);tubeColor_.prepare(fs);regenerator_.prepare(fs);
    phraseReleasePole_=std::exp(-1000.0f/(50.0f*fs));
    setParameters(target_);
    reset();
    return true;
}

void Engine::setParameters(const Parameters& p) noexcept {
    const Model previousModel=target_.model;
    const int previousVoicing=voicing_;
    const int previousLoopAction=target_.loop_action;
    target_.delay_ms = bounded(p.delay_ms, 1000.0f / sampleRate_, maximumMs_, 350.0f);
    target_.delay_ms = std::min(target_.delay_ms, maximumMs_);
    // Convert once at control cadence, with enough precision to preserve exact
    // integer delays (e.g. 250 ms at 48 kHz is exactly 12000 samples).
    delayTarget_ = std::max(1.0f, std::min(maximumSamples_,
        static_cast<float>(static_cast<double>(sampleRate_) * target_.delay_ms / 1000.0)));
    target_.feedback = bounded(p.feedback, 0.0f, 2.0f, 0.28f);
    target_.mix = bounded(p.mix, 0.0f, 1.0f, 0.39f);
    target_.tone_hz = bounded(p.tone_hz, 20.0f, 0.45f * sampleRate_, 3500.0f);
    target_.drive = bounded(p.drive, 0.0f, 1.0f, 0.4f);
    const int requestedModel=static_cast<int>(p.model);
    target_.model=requestedModel >= 0 && requestedModel < modelCount ? p.model : Model::Digital;
    target_.voicing=std::max(0,std::min(3,p.voicing));
    target_.modulation_enabled=p.modulation_enabled;
    target_.exciter_highpass_enabled=p.exciter_highpass_enabled;
    target_.wet_tone_enabled=p.wet_tone_enabled;
    target_.stereo_timing_enabled=p.stereo_timing_enabled;
    voicing_=resolvedVoicing(target_.model,target_.voicing);
    target_.bit_depth=std::max(4,std::min(24,p.bit_depth));
    target_.mod_rate_hz=bounded(p.mod_rate_hz,0.01f,20.0f,1.0f);
    target_.mod_depth_ms=bounded(p.mod_depth_ms,0.0f,std::min(50.0f,maximumMs_),6.0f);
    target_.duck_threshold_db=bounded(p.duck_threshold_db,-80.0f,0.0f,-24.0f);
    target_.duck_amount=bounded(p.duck_amount,0.0f,1.0f,0.9f);
    target_.attack_ms=bounded(p.attack_ms,0.1f,5000.0f,5.0f);
    target_.release_ms=bounded(p.release_ms,1.0f,10000.0f,250.0f);
    target_.swell_ms=bounded(p.swell_ms,1.0f,10000.0f,250.0f);
    target_.stereo_ratio=bounded(p.stereo_ratio,1.0f/24.0f,24.0f,1.5f);
    target_.loop_action=std::max(0,std::min(4,p.loop_action));
    target_.limiter_enabled=p.limiter_enabled;
    target_.limiter_ceiling=bounded(p.limiter_ceiling,0.01f,safety::maximumCeiling,safety::maximumCeiling);
    target_.limiter_release_ms=bounded(p.limiter_release_ms,1.0f,5000.0f,150.0f);
    target_.sample_rate_hz=p.sample_rate_hz == 0 ? 0 : bounded(p.sample_rate_hz,100.0f,sampleRate_,8000.0f);
    target_.exciter_amount=bounded(p.exciter_amount,0.0f,1.0f,0.1f);
    target_.exciter_tune_hz=bounded(p.exciter_tune_hz,200.0f,std::min(10000.0f,0.4f*sampleRate_),4000.0f);
    target_.tap_pattern=std::max(0,std::min(static_cast<int>(multitap::patternCount)-1,p.tap_pattern));
    target_.mod1_source=std::max(0,std::min(5,p.mod1_source));
    target_.mod1_destination=std::max(0,std::min(modulationDestinationCount-1,p.mod1_destination));
    target_.mod1_amount=bounded(p.mod1_amount,-1.0f,1.0f,0.0f);
    target_.mod2_source=std::max(0,std::min(5,p.mod2_source));
    target_.mod2_destination=std::max(0,std::min(modulationDestinationCount-1,p.mod2_destination));
    target_.mod2_amount=bounded(p.mod2_amount,-1.0f,1.0f,0.0f);
    target_.feedback_lowpass_hz=p.feedback_lowpass_hz == 0 ? 0 : bounded(p.feedback_lowpass_hz,20.0f,20000.0f,12000.0f);
    target_.drift_amount=bounded(p.drift_amount,0.0f,1.0f,0.25f);
    target_.feedback_highpass_hz=p.feedback_highpass_hz == 0 ? 0 : bounded(p.feedback_highpass_hz,20.0f,2000.0f,80.0f);
    target_.lores_rate_end_hz=bounded(p.lores_rate_end_hz,100.0f,sampleRate_,1200.0f);
    target_.lores_rate_decay_ms=bounded(p.lores_rate_decay_ms,10.0f,10000.0f,1500.0f);
    target_.lores_rate_envelope_amount=bounded(p.lores_rate_envelope_amount,0.0f,1.0f,1.0f);
    target_.digital_rack_rate_hz=p.digital_rack_rate_hz == 0 ? 0 : bounded(p.digital_rack_rate_hz,1000.0f,sampleRate_,16000.0f);
    target_.lores_cutoff_hz=p.lores_cutoff_hz == 0 ? 0 : bounded(p.lores_cutoff_hz,20.0f,20000.0f,2000.0f);
    loresCutoffPole_=target_.lores_cutoff_hz == 0 ? 0 : std::exp(-2*pi*std::min(target_.lores_cutoff_hz,.45f*sampleRate_)/sampleRate_);
    rack_.prepare(sampleRate_,voicing_ == 3 ? target_.digital_rack_rate_hz : 0);
    const float rackCutoffScale=voicing_ == 2 ? .65f : .5f;
    const float feedbackCutoff=target_.feedback_lowpass_hz*(target_.model == Model::DigitalRack ? rackCutoffScale : 1.0f);
    feedbackLowpass_.prepare(sampleRate_,feedbackCutoff);
    for (unsigned i=0;i<3;++i) {
        const float cutoff=std::min(0.2f*sampleRate_,target_.feedback_highpass_hz*(0.98f+0.02f*static_cast<float>(i)));
        feedbackHighpassPole_[i]=cutoff == 0 ? 0 : std::exp(-2.0f*pi*cutoff/sampleRate_);
    }
    const float rateDecayStep=1000.0f/(target_.lores_rate_decay_ms*sampleRate_);
    // 1-exp(-x), evaluated without cancellation for very long times at384kHz.
    // Here x<=.0125; this cubic has relative error below9e-8.
    loresRateDecaySlew_=rateDecayStep*(1.0f-rateDecayStep*(0.5f-rateDecayStep/6.0f));
    loresRateRatio_=target_.sample_rate_hz == 0 ? 1.0f : target_.sample_rate_hz/sampleRate_;
    limiter_.prepare(sampleRate_,target_.limiter_release_ms);limiter_.setCeiling(target_.limiter_ceiling);
    exciter_.prepare(sampleRate_,target_.exciter_tune_hz);
    exciterA04_.prepare(sampleRate_,target_.exciter_tune_hz);exciterA05_.prepare(sampleRate_,target_.exciter_tune_hz);
    toneTarget_ = std::exp(-2.0f * pi * target_.tone_hz / sampleRate_);
    sweepLowPole_=std::exp(-2.0f*pi*std::max(20.0f,target_.tone_hz*0.125f)/sampleRate_);
    quantizerScale_=static_cast<float>(std::uint32_t(1) << (target_.bit_depth-1));
    modDepthTarget_=static_cast<float>(static_cast<double>(sampleRate_)*target_.mod_depth_ms/1000.0);
    duckThresholdTarget_=std::exp(target_.duck_threshold_db*0.1151292546497022842f);
    detectorAttackPole_=std::exp(-1000.0f/(target_.attack_ms*sampleRate_));
    detectorReleasePole_=std::exp(-1000.0f/(target_.release_ms*sampleRate_));
    swellSlew_=1.0f-std::exp(-1000.0f/(target_.swell_ms*sampleRate_));
    segmentFrames_=std::max<std::size_t>(1,std::min(static_cast<std::size_t>(maximumSamples_),
        static_cast<std::size_t>(delayTarget_+0.5f)));
    segmentFadeFrames_=std::max<std::size_t>(1,static_cast<std::size_t>(sampleRate_*0.005f));
    if (target_.model != previousModel || previousVoicing != voicing_) {
        // Invalidate module state in bounded time; shared audio storage is not cleared.
        reverse_.reset();looper_.reset();rack_.reset();exciter_.reset();saturator_.reset();feedbackLowpass_.reset();
        loresPhase_=1.0f;loresHeld_[0]=loresHeld_[1]=0.0f;
        modPhase_=flutterPhase_=detector_=swellGain_=swellTarget_=0.0f;swellGate_=false;duckGain_=1.0f;
        for (unsigned c=0;c<2;++c) extraFilter1_[c]=extraFilter2_[c]=wetTone_[c]=0.0f;
        driftPhase_[0]=0.17f;driftPhase_[1]=0.43f;driftPhase_[2]=0.71f;
        a04Color_.reset();tubeColor_.reset();regenerator_.reset();
        exciterA04_.reset();exciterA05_.reset();
        phraseEnvelope_=loresRateEnvelope_=loresRateEnvelopeError_=sweepProgress_=sweepProgressError_=0;phraseGate_=false;
        for (unsigned c=0;c<2;++c) feedbackHighpassInput_[c]=feedbackHighpassOutput_[c]=0;
        if (previousVoicing != voicing_)
            for (unsigned c=0;c<2;++c) filter1_[c]=filter2_[c]=dcInput_[c]=dcOutput_[c]=0;
    }
    if (target_.model == Model::LoopSampler && previousLoopAction != target_.loop_action &&
        (target_.loop_action == 1 || target_.loop_action == 4)) {
        feedbackLowpass_.reset();regenerator_.reset();
        for (unsigned c=0;c<2;++c) feedbackHighpassInput_[c]=feedbackHighpassOutput_[c]=0;
    }
}

void Engine::setExternalModulation(float cv1,float cv2,float wheel) noexcept {
    externalMod_[0]=bounded(cv1,-1.0f,1.0f,0.0f);
    externalMod_[1]=bounded(cv2,-1.0f,1.0f,0.0f);
    externalMod_[2]=bounded(wheel,0.0f,1.0f,0.0f);
}

void Engine::reset() noexcept {
    if (memory_) std::fill(memory_, memory_ + 2 * length_, 0.0f);
    write_ = 0;
    delay_ = delayTarget_;
    feedback_ = target_.feedback;
    mix_ = target_.mix;
    drive_ = target_.drive;
    tonePole_ = toneTarget_;
    phase_ = 0.0f;
    modPhase_=flutterPhase_=detector_=swellGain_=swellTarget_=0.0f;swellGate_=false;duckGain_=1.0f;
    modRate_=target_.mod_rate_hz;modDepth_=modDepthTarget_;
    duckThreshold_=duckThresholdTarget_;duckAmount_=target_.duck_amount;
    driftAmount_=target_.drift_amount;driftPhase_[0]=0.17f;driftPhase_[1]=0.43f;driftPhase_[2]=0.71f;
    reverse_.reset();looper_.reset();rack_.reset();exciter_.reset();limiter_.reset();saturator_.reset();feedbackLowpass_.reset();
    loresPhase_=1.0f;loresHeld_[0]=loresHeld_[1]=0.0f;
    a04Color_.reset();tubeColor_.reset();regenerator_.reset();
    exciterA04_.reset();exciterA05_.reset();
    phraseEnvelope_=loresRateEnvelope_=loresRateEnvelopeError_=sweepProgress_=sweepProgressError_=0;phraseGate_=false;
    for (unsigned c=0;c<2;++c) feedbackHighpassInput_[c]=feedbackHighpassOutput_[c]=0;
    for (unsigned c=0;c<2;++c) extraFilter1_[c]=extraFilter2_[c]=wetTone_[c]=0.0f;
    for (unsigned c = 0; c != 2; ++c)
        filter1_[c] = filter2_[c] = dcInput_[c] = dcOutput_[c] = 0.0f;
}

float Engine::finiteSignal(float x) noexcept {
    // Preserve headroom without allowing an adversarial input to overflow state.
    return bounded(x, -1.0e12f, 1.0e12f, 0.0f);
}

float Engine::readDelay(unsigned channel, float samples) const noexcept {
    const float boundedDelay = std::max(1.0f, std::min(maximumSamples_, samples));
    const std::size_t whole = static_cast<std::size_t>(boundedDelay);
    const float fraction = boundedDelay - static_cast<float>(whole);
    const std::size_t newer = (write_ + length_ - whole) % length_;
    const std::size_t older = newer == 0 ? length_ - 1 : newer - 1;
    const float* line = memory_ + channel * length_;
    return line[newer] + fraction * (line[older] - line[newer]);
}

void Engine::processSample(float inL, float inR, float& outL, float& outR) noexcept {
    const float input[2] = {finiteSignal(inL), finiteSignal(inR)};
    if (!memory_) {
        outL = input[0];
        outR = input[1];
        limiter_.process(outL,outR,target_.limiter_enabled);
        return;
    }
    delay_ += delaySlew_ * (delayTarget_ - delay_);
    feedback_ += controlSlew_ * (target_.feedback - feedback_);
    mix_ += controlSlew_ * (target_.mix - mix_);
    drive_ += controlSlew_ * (target_.drive - drive_);
    tonePole_ += controlSlew_ * (toneTarget_ - tonePole_);
    modRate_+=controlSlew_*(target_.mod_rate_hz-modRate_);
    modDepth_+=controlSlew_*(modDepthTarget_-modDepth_);
    duckThreshold_+=controlSlew_*(duckThresholdTarget_-duckThreshold_);
    duckAmount_+=controlSlew_*(target_.duck_amount-duckAmount_);
    driftAmount_+=controlSlew_*(target_.drift_amount-driftAmount_);
    float driftWave[3];
    for (unsigned i=0;i<3;++i) {
        const float triangleDrift=4.0f*std::fabs(driftPhase_[i]-0.5f)-1.0f;
        // Cubic shaping has zero slope at the triangle corners. These bounded
        // phase recurrences need no trigonometry or coefficient work in audio.
        driftWave[i]=triangleDrift*(1.5f-0.5f*triangleDrift*triangleDrift);
        if (target_.modulation_enabled) {
            driftPhase_[i]+=driftIncrement_[i];if (driftPhase_[i] >= 1.0f) driftPhase_[i]-=1.0f;
        }
    }
    const bool moving=target_.modulation_enabled;
    const bool looper=target_.model == Model::LoopSampler;
    const float modulation=moving ? 4.0f*std::fabs(modPhase_-0.5f)-1.0f : 0.0f;
    const float flutter=moving ? 4.0f*std::fabs(flutterPhase_-0.5f)-1.0f : 0.0f;
    if (target_.model == Model::LoRes || target_.model == Model::Sweep || target_.model == Model::SweepUp) {
        // Phrase detection listens only to fresh stereo input, never repeats.
        // A 50 ms peak release and -40/-46 dBFS hysteresis prevent feedback or
        // each waveform zero crossing from restarting a one-way movement.
        const float level=std::max(std::fabs(input[0]),std::fabs(input[1]));
        phraseEnvelope_=std::max(level,phraseReleasePole_*phraseEnvelope_);
        if (!phraseGate_ && level >= 0.01f) {
            phraseGate_=true;loresRateEnvelope_=1.0f;loresRateEnvelopeError_=sweepProgress_=sweepProgressError_=0;
        }
        if (phraseGate_ && phraseEnvelope_ < 0.005f) phraseGate_=false;
    }
    float wetGain=1.0f;
    const bool envelopeRoute=(target_.mod1_source == 2 && target_.mod1_destination && target_.mod1_amount != 0) ||
        (target_.mod2_source == 2 && target_.mod2_destination && target_.mod2_amount != 0);
    if (target_.model == Model::Dynamic || target_.model == Model::AutoVolume || envelopeRoute) {
        const float level=std::max(std::fabs(input[0]),std::fabs(input[1]));
        const float detectorPole=level > detector_ ? detectorAttackPole_ : detectorReleasePole_;
        detector_=level+detectorPole*(detector_-level);
        if (target_.model == Model::AutoVolume) {
            if (!swellGate_ && detector_ >= duckThreshold_) { swellGate_=true;swellTarget_=0.0f; }
            // Quiet input rearms the next phrase but never gates the stored tail.
            // Continue the current ramp after release; a retrigger lowers its
            // target with Attack smoothing rather than cutting existing echoes.
            if (swellGate_ && detector_ < 0.5f*duckThreshold_) swellGate_=false;
            if (swellGate_ || swellTarget_ > 0.0f)
                swellTarget_+=swellSlew_*(1.0f-swellTarget_);
            if (swellTarget_ < swellGain_)
                swellGain_=swellTarget_+detectorAttackPole_*(swellGain_-swellTarget_);
            else swellGain_=swellTarget_;
        }
    }
    const float sources[6]={0.0f,modulation,moving ? bounded(detector_,0.0f,1.0f,0.0f) : 0.0f,externalMod_[0],externalMod_[1],externalMod_[2]};
    float routed[modulationDestinationCount]{};
    routed[target_.mod1_destination]+=sources[target_.mod1_source]*target_.mod1_amount;
    routed[target_.mod2_destination]+=sources[target_.mod2_source]*target_.mod2_amount;
    for (unsigned destination=1;destination<modulationDestinationCount;++destination) routed[destination]=bounded(routed[destination],-1.0f,1.0f,0.0f);
    const float routedDrift=bounded(driftAmount_+(looper?0.0f:routed[11]),0.0f,1.0f,driftAmount_);
    const float drift=target_.model == Model::Digital || voicing_ == 1 || !moving ? 0.0f : routedDrift;
    const float playbackDrift=looper ? 0.0f : drift;
    const float stereoRatio=bounded(target_.stereo_ratio*(1.0f+0.75f*routed[10]),1.0f/24.0f,24.0f,target_.stereo_ratio);
    if (target_.model == Model::Dynamic || target_.model == Model::AutoVolume) {
        const float amount=bounded(duckAmount_+routed[12],0.0f,1.0f,duckAmount_);
        const float level=std::max(std::fabs(input[0]),std::fabs(input[1]));
        const float targetGain=level > duckThreshold_ ? 1.0f-amount*(1.0f-duckThreshold_/level) : 1.0f;
        // Attack/Release act on wet gain directly. A loud preceding phrase must
        // not add an amplitude-dependent wait before the quiet tail can return.
        const float pole=targetGain < duckGain_ ? detectorAttackPole_ : detectorReleasePole_;
        duckGain_=targetGain+pole*(duckGain_-targetGain);
        if (targetGain == 1.0f && duckGain_ > 0.99999f) duckGain_=1.0f;
        wetGain=amount == 0.0f ? 1.0f : bounded(duckGain_,0.0f,1.0f,1.0f);
        if (target_.model == Model::AutoVolume) wetGain*=swellGain_;
    }
    const float delay=bounded(delay_*(1.0f+0.5f*routed[1])+sampleRate_*0.0001f*playbackDrift*driftWave[0],1.0f,maximumSamples_,delay_);
    const float feedback=bounded((feedback_+0.5f*routed[2])*(1.0f+0.003f*drift*driftWave[1]),0.0f,2.0f,feedback_);
    const float routedTonePole=routed[3] >= 0 ? tonePole_+routed[3]*(toneHighPole_-tonePole_) :
        tonePole_-routed[3]*(toneLowPole_-tonePole_);
    // p*(1-p) is bounded by -p*log(p), keeping the implied cutoff movement
    // within approximately2% without log/exp per sample, including routed Tone.
    const float tonePole=bounded(routedTonePole-0.019f*drift*driftWave[2]*routedTonePole*(1.0f-routedTonePole),toneHighPole_,toneLowPole_,routedTonePole);
    const float drive=bounded((drive_+routed[4])*(1.0f+0.01f*playbackDrift*driftWave[2]),0.0f,1.0f,drive_);
    const float baseMix=bounded(mix_+routed[5],0.0f,1.0f,mix_);
    const float mix=bounded(baseMix+0.002f*playbackDrift*driftWave[1]*4.0f*baseMix*(1.0f-baseMix),0.0f,1.0f,baseMix);
    const auto mixWet=[this,&input,mix,tonePole](unsigned channel,float wet) noexcept {
        if(target_.wet_tone_enabled) {
            wetTone_[channel]+=(1.0f-tonePole)*(wet-wetTone_[channel]);
            wet=wetTone_[channel];
        }
        return finiteSignal((1.0f-mix)*input[channel]+mix*wet);
    };
    const float modDepth=bounded((modDepth_+sampleRate_*0.020f*routed[6])*(1.0f+0.02f*playbackDrift*driftWave[0]),0.0f,maximumModDepth_,modDepth_);
    const float routedModRate=bounded(modRate_+20.0f*routed[9],0.01f,20.0f,modRate_);
    const float effectiveModRate=routedModRate*(1.0f+0.02f*playbackDrift*driftWave[1])*(target_.model == Model::AnalogMod && voicing_ != 1 ? 0.72f : 1.0f);
    const float feedbackCutoffMod=drift*driftWave[2];
    const float exciterAmount=bounded(target_.exciter_amount+routed[7],0.0f,1.0f,target_.exciter_amount);
    float feedbackCompensation=1.0f;
    if (voicing_ == 3 && (target_.model == Model::Analog || target_.model == Model::AnalogMod || target_.model == Model::Tape))
        feedbackCompensation=color::A04Color::compensationFeedbackGain(drive);
    else if (voicing_ == 3 && target_.model == Model::EchoPlatter)
        feedbackCompensation=color::A04Color::compensationFeedbackGain(drive,color::Flavor::Platter);
    else if (voicing_ == 3 && target_.model == Model::Tube)
        feedbackCompensation=color::TubeColor::compensationFeedbackGain(drive);
    else if (voicing_ == 3 && target_.model == Model::ExciterEcho)
        feedbackCompensation=exciter::State::compensationFeedbackGain(exciterAmount,drive);
    const bool digitalControl=target_.model == Model::Digital;
    const float digitalColourBlend=bounded(10.0f*(feedback-1.0f),0.0f,1.0f,0.0f);
    const auto feedbackFilter=[this,feedbackCutoffMod,feedbackCompensation,feedback,digitalControl,digitalColourBlend](unsigned channel,float value) noexcept {
        const bool exciterHighpass=voicing_ == 1 && target_.model == Model::ExciterEcho && target_.exciter_highpass_enabled;
        if ((voicing_ == 1 && !exciterHighpass) || (voicing_ == 2 && digitalControl)) return value;
        const float unfiltered=value;
        if ((voicing_ == 3 || exciterHighpass) && target_.feedback_highpass_hz != 0) {
            const float nominal=feedbackHighpassPole_[1];
            const float pole=feedbackCutoffMod >= 0 ? nominal+feedbackCutoffMod*(feedbackHighpassPole_[2]-nominal) :
                nominal-feedbackCutoffMod*(feedbackHighpassPole_[0]-nominal);
            const float highpassed=0.5f*(1.0f+pole)*(value-feedbackHighpassInput_[channel])+pole*feedbackHighpassOutput_[channel];
            feedbackHighpassInput_[channel]=value;feedbackHighpassOutput_[channel]=highpassed;value=highpassed;
        }
        if(exciterHighpass) return value;
        value=feedbackLowpass_.process(value,channel,feedbackCutoffMod);
        if (digitalControl) {
            // Keep filter histories warm below unity without changing a single
            // analytic output bit. Above100%, introduce return colour over the
            // same100..110% interval as the musical regenerator.
            if (feedback <= 1.0f) {
                regenerator_.process(0.0f,channel,feedback);
                return unfiltered;
            }
            value=unfiltered+digitalColourBlend*(value-unfiltered);
        }
        value/=feedbackCompensation;
        // Only the reinjected return is regenerated. Fresh input never traverses
        // this bounded stage; colour compensation precedes its nonlinearity.
        if (voicing_ == 3 && feedback > 1.0f) value=regenerator_.process(feedback*value,channel,feedback)/feedback;
        else regenerator_.process(0.0f,channel,feedback);
        return value;
    };
    const bool captureLoRes=loresPhase_ >= 1.0f;
    if (target_.model == Model::LoRes) {
        if (captureLoRes) loresPhase_-=1.0f;
        const float endRatio=std::min(loresRateRatio_,target_.lores_rate_end_hz/sampleRate_);
        const float envelopeRatio=endRatio+(loresRateRatio_-endRatio)*loresRateEnvelope_;
        const float envelopeAmount=voicing_ == 3 ? target_.lores_rate_envelope_amount : 0;
        const float baseRatio=loresRateRatio_+envelopeAmount*(envelopeRatio-loresRateRatio_);
        const float rateRatio=target_.sample_rate_hz == 0 ? 1.0f :
            bounded(baseRatio*(1+.9f*routed[8]),100.0f/sampleRate_,1.0f,baseRatio);
        loresPhase_+=rateRatio;
        const float step=-loresRateDecaySlew_*loresRateEnvelope_-loresRateEnvelopeError_;
        const float next=loresRateEnvelope_+step;
        loresRateEnvelopeError_=(next-loresRateEnvelope_)-step;loresRateEnvelope_=next;
        if (loresRateEnvelope_ < 1.0e-12f) loresRateEnvelope_=loresRateEnvelopeError_=0;
    }
    const float triangle = 4.0f * std::fabs(phase_ - 0.5f) - 1.0f;
    // A deterministic, provisional wow term, exclusively for the Tube model.
    const float wow = moving && (target_.model == Model::Tube || target_.model == Model::TubeA04) ? triangle * sampleRate_ * 0.00025f : 0.0f;
    float output[2];
    if (target_.model == Model::Reverse || target_.model == Model::LoopSampler) {
        float wetL=0,wetR=0;
        // A new Record latches the configured Time, including the current route.
        // It must not capture the residual glide from the previous model/preset.
        const float captureDelay=target_.model == Model::LoopSampler ?
            bounded(delayTarget_*(1.0f+0.5f*routed[1]),1.0f,maximumSamples_,delayTarget_) : delay;
        const std::size_t routedFrames=std::max<std::size_t>(1,std::min(static_cast<std::size_t>(maximumSamples_),
            static_cast<std::size_t>(captureDelay+0.5f)));
        if (target_.model == Model::Reverse)
            reverse_.process(input[0],input[1],feedback,memory_,length_,routedFrames,segmentFadeFrames_,wetL,wetR,feedbackFilter);
        else looper_.process(input[0],input[1],target_.loop_action,feedback,memory_,length_,routedFrames,wetL,wetR,feedbackFilter);
        output[0]=mixWet(0,wetL);
        output[1]=mixWet(1,wetR);
    } else if (target_.model == Model::DigitalRack || target_.model == Model::EchoPlatter || target_.model == Model::StereoDelays) {
        rack::Controls controls;
        controls.delaySamples=delay;controls.feedback=feedback;controls.tonePole=tonePole;controls.drive=drive;
        controls.modDepthSamples=target_.model == Model::EchoPlatter && !moving ? 0.0f : modDepth;
        controls.modPhase=modPhase_;
        controls.stereoRatio=stereoRatio;controls.maximumSamples=maximumSamples_;
        controls.voicing=voicing_;
        const auto kind=target_.model == Model::DigitalRack ? rack::Kind::Digital24 :
            (target_.model == Model::EchoPlatter ? rack::Kind::Platter : rack::Kind::Stereo);
        const auto frame=rack_.process(kind,input[0],input[1],controls,
            [this](unsigned channel,float tap) noexcept { return readDelay(channel,tap); },feedbackFilter);
        memory_[write_]=loopGuard(frame.writeL,feedback);memory_[length_+write_]=loopGuard(frame.writeR,feedback);
        output[0]=mixWet(0,frame.wetL);
        output[1]=mixWet(1,frame.wetR);
    } else if (target_.model == Model::PingPong) {
        // Mono excitation starts on the left; only the feedback crosses sides.
        // Read both taps before writing so the matrix has no order dependence.
        const float delayedL = readDelay(0,delay);
        const float delayedR = readDelay(1,delay);
        const float mono = 0.5f * (input[0] + input[1]);
        memory_[write_] = loopGuard(mono + feedback * feedbackFilter(1,delayedR),feedback);
        memory_[length_ + write_] = loopGuard(feedback * feedbackFilter(0,delayedL),feedback);
        output[0] = mixWet(0,delayedL);
        output[1] = mixWet(1,delayedR);
    } else for (unsigned c = 0; c != 2; ++c) {
        const float channelDelay=c && stereoTimingActive(target_) ?
            bounded(delay*stereoRatio,1.0f,maximumSamples_,delay) : delay;
        float modelDelay=channelDelay+wow;
        if (target_.model == Model::Tape)
            modelDelay+=modDepth*0.25f*(0.8f*modulation+0.2f*flutter);
        else if (target_.model == Model::AnalogMod || target_.model == Model::DigitalMod)
            modelDelay+=(c == 0 ? 1.0f : -1.0f)*modDepth*modulation;
        const float delayed = readDelay(c, modelDelay);
        float loopSignal=delayed;
        if (target_.model == Model::ExciterEcho)
            loopSignal=voicing_ == 1 ? exciterA04_.process(delayed,c,exciterAmount,drive) :
                (voicing_ == 2 ? exciterA05_.process(delayed,c,exciterAmount,drive) : exciter_.process(delayed,c,exciterAmount,drive,feedback));
        // Ducking and swell shape the wet return only. Fresh input and the
        // feedback line retain the complete echo so it can emerge after a note.
        float writeValue = input[c] + feedback * feedbackFilter(c,loopSignal);
        if (target_.model == Model::Analog) {
            // BBD-inspired candidate: two low-pass stages inside the loop.
            writeValue = voicing_ == 1 ? legacy::saturate(writeValue,drive) :
                (voicing_ == 2 ? saturator_.process(writeValue,c,drive) : a04Color_.process(writeValue,c,drive));
            filter1_[c] += (1.0f - tonePole) * (writeValue - filter1_[c]);
            filter2_[c] += (1.0f - tonePole) * (filter1_[c] - filter2_[c]);
            writeValue = filter2_[c];
        } else if (target_.model == Model::Tube || target_.model == Model::TubeA04) {
            // Two asymmetric, sagging gain stages provide the original tube
            // candidate; the return division above preserves sub-unity decay.
            if (voicing_ == 3) writeValue=tubeColor_.process(writeValue,c,drive);
            else {
                const float raw=writeValue;
                writeValue=voicing_ == 1 ? legacy::tube(raw,drive) : saturator_.process(raw,c,drive)+.12f*drive*raw*raw/(1+raw*raw);
                const float highpassed=writeValue-dcInput_[c]+dcPole_*dcOutput_[c];
                dcInput_[c]=writeValue;dcOutput_[c]=highpassed;writeValue=highpassed;
            }
            filter1_[c] += (1.0f - tonePole) * (writeValue - filter1_[c]);
            writeValue = filter1_[c];
        } else if (target_.model == Model::MultiHead) {
            // Candidate tape-head topology: one shared line per channel, three
            // output taps, and only the longest tap in the colored feedback.
            writeValue = voicing_ == 1 ? legacy::saturate(writeValue,drive) : saturator_.process(writeValue,c,drive);
            filter1_[c] += (1.0f-tonePole) * (writeValue-filter1_[c]);
            writeValue = filter1_[c];
        } else if (target_.model == Model::LoRes) {
            if (captureLoRes) {
                const float clipped=std::max(-1.0f,std::min(1.0f-1.0f/quantizerScale_,writeValue));
                const float scaled=clipped*quantizerScale_;
                // Truncate toward zero to avoid a rounding-induced final-LSB cycle.
                const std::int32_t code=static_cast<std::int32_t>(scaled);
                loresHeld_[c]=static_cast<float>(code)/quantizerScale_;
            }
            writeValue=loresHeld_[c];
        } else if (target_.model == Model::Tape || target_.model == Model::AnalogMod) {
            writeValue=voicing_ == 1 ? legacy::saturate(writeValue,drive) :
                (voicing_ == 2 ? saturator_.process(writeValue,c,drive) : a04Color_.process(writeValue,c,drive));
            extraFilter1_[c]+=(1.0f-tonePole)*(writeValue-extraFilter1_[c]);
            extraFilter2_[c]+=(1.0f-tonePole)*(extraFilter1_[c]-extraFilter2_[c]);
            writeValue=extraFilter2_[c];
        } else if (target_.model == Model::Sweep || target_.model == Model::SweepUp) {
            const float basePosition=voicing_ == 3 ? sweepProgress_ : .5f+.5f*modulation;
            const float position=!moving ? 0 : (target_.model == Model::SweepUp ? 1.0f-basePosition : basePosition);
            const float pole=tonePole+(sweepLowPole_-tonePole)*position;
            writeValue=voicing_ == 1 ? legacy::saturate(writeValue,drive) : saturator_.process(writeValue,c,drive);
            extraFilter1_[c]+=(1.0f-pole)*(writeValue-extraFilter1_[c]);
            // SweepUp removes the low-pass component at the same moving cutoff.
            writeValue=target_.model == Model::SweepUp ? writeValue-extraFilter1_[c] : extraFilter1_[c];
        }
        memory_[c * length_ + write_] = loopGuard(writeValue,feedback);
        float wet=delayed;
        if (target_.model == Model::MultiHead) {
            if (target_.tap_pattern == 0)
                wet=(readDelay(c,delay/3.0f)+readDelay(c,(2.0f*delay)/3.0f)+delayed)/3.0f;
            else {
                wet=0.0f;
                const auto& pattern=multitap::pattern(static_cast<unsigned>(target_.tap_pattern));
                for (unsigned head=0;head<pattern.count;++head)
                    wet+=pattern.taps[head].gain*readDelay(c,delay*pattern.taps[head].offset);
            }
        }
        if (target_.model == Model::LoRes && voicing_ == 3 && target_.lores_cutoff_hz != 0) {
            filter1_[c]+=(1-loresCutoffPole_)*(wet-filter1_[c]);wet=filter1_[c];
        }
        output[c] = mixWet(c,wetGain*wet);
    }
    outL = output[0];
    outR = output[1];
    limiter_.process(outL,outR,target_.limiter_enabled);
    if (moving && (target_.model == Model::Sweep || target_.model == Model::SweepUp)) {
        // Compensated summation preserves very slow ramps even at384 kHz.
        const float step=effectiveModRate/sampleRate_-sweepProgressError_;
        const float next=sweepProgress_+step;
        sweepProgressError_=(next-sweepProgress_)-step;
        sweepProgress_=std::min(1.0f,next);
    }
    if (++write_ == length_) write_ = 0;
    if (moving) {
        phase_ += 0.27f / sampleRate_;
        if (phase_ >= 1.0f) phase_ -= 1.0f;
        modPhase_+=effectiveModRate/sampleRate_;if (modPhase_ >= 1.0f) modPhase_-=1.0f;
        flutterPhase_+=7.0f*effectiveModRate/sampleRate_;if (flutterPhase_ >= 1.0f) flutterPhase_-=1.0f;
    }
}

void Engine::processBlock(const float* inL, const float* inR, float* outL,
                          float* outR, std::size_t frames) noexcept {
    for (std::size_t i = 0; i != frames; ++i) {
        float left, right;
        processSample(inL ? inL[i] : 0.0f, inR ? inR[i] : 0.0f, left, right);
        if (outL) outL[i] = left;
        if (outR) outR[i] = right;
    }
}

bool Engine::processBlock(const float* inL, const float* inR, float* outL,
                          float* outR, std::size_t frames,
                          const ParameterEvent* events, std::size_t count) noexcept {
    if (count && !events) return false;
    for (std::size_t i = 0; i != count; ++i)
        if (events[i].offset > frames || (i && events[i].offset < events[i-1].offset))
            return false;
    std::size_t current = 0;
    for (std::size_t i = 0; i != frames; ++i) {
        while (current < count && events[current].offset == i)
            setParameters(events[current++].parameters);
        float left, right;
        processSample(inL ? inL[i] : 0.0f, inR ? inR[i] : 0.0f, left, right);
        if (outL) outL[i] = left;
        if (outR) outR[i] = right;
    }
    while (current < count) setParameters(events[current++].parameters);
    return true;
}

} // namespace echolab
