// Single translation unit, intentionally shares the exact host DSP implementation.
#include <distingnt/api.h>
#include <new>
#include <cstddef>
#include <cstdint>
#include <atomic>
#include "clock_sync.h"
#include "rhythm_labels.h"
#include "../dsp/output_limiter.h"
#include "../dsp/engine.h"
#include "../dsp/engine.cpp"

#ifndef ECHOLAB_BUILD_ID
#define ECHOLAB_BUILD_ID "development"
#endif

namespace echolab_nt {
enum Parameter {
    InputL, InputR, OutputL, OutputModeL, OutputR, OutputModeR,
    Model, Time, Feedback, Mix, Tone, Drive, Bypass,
    BitDepth, ModRate, ModDepth, DuckThreshold, DuckAmount, Attack, Release, Swell, StereoRatio, LoopAction,
    LimiterOn, LimiterCeiling, LimiterReleaseMs, SampleRateHz, ExciterAmount, ExciterTuneHz, TapPattern,
    Mod1Source, Mod1Destination, Mod1Amount, Mod2Source, Mod2Destination, Mod2Amount,
    CV1Bus, CV2Bus, ClockBus, ClockPpqn, SyncMode, TempoBpm, Division,
    FeedbackLPHz, Drift,
    FeedbackHPHz, LoResEndHz, LoResDecayMs, LoResEnvelopeAmount, RackSampleRateHz,
    LoResCutoffHz, Voicing, ModulationEnabled, Version, RightDivision,
    FactoryPreset, ExciterHighpassOn, WetToneOn, StereoTiming,
    ParameterCount
};
#include "factory_presets.h"
static_assert(ParameterCount<=64,"pending parameter writes use a bounded bit mask");
static_assert(std::atomic<std::uint32_t>::is_always_lock_free,"the UI boundary snapshot must not lock in audio callbacks");
static constexpr std::uint64_t initialParameterMask=UINT64_MAX>>(64-ParameterCount);
static constexpr std::uint32_t uiSnapshotPresent=std::uint32_t(1)<<31;
static const char* const modelNames[] = {
    "Digital control", "Analog", "Tube B",
    "Ping Pong", "Multi-Head",
    "8bit", "Dynamic / Ducking",
    "Tape", "Sweep Down", "Analog Mod",
    "Digital Mod", "Reverse", "Auto-Volume",
    "Digital Rack", "Echo Platter", "Stereo Delays",
    "Loop Sampler", "Exciter Echo", "Sweep Up", "Tube A"
};
static const char* const bypassNames[] = {"Active", "Dry", "Tails"};
static const char* const loopNames[] = {"Stop", "Record", "Play", "Overdub", "Clear"};
static const char* const onOffNames[] = {"Off", "On"};
static const char* const sourceNames[] = {"None", "LFO", "Envelope", "CV 1", "CV 2", "Mod wheel"};
static const char* const destinationNames[] = {"None", "Time", "Feedback", "Tone", "Drive", "Mix", "Depth", "Exciter", "8bit rate",
                                              "Mod rate", "Stereo ratio", "Drift", "Duck amount"};
static const char* const voicingNames[] = {"Auto", "A04", "A05", "B06"};
static const char* const versionNames[] = {"A", "B"};
static const char* const syncNames[] = {"Free", "MIDI clock", "CV clock", "Internal BPM"};
static const char* const ppqnNames[] = {"1", "2", "4", "24"};
static const char* const divisionNames[] = {"1/1", "1/2", "1/4", "1/8", "1/16", "1/4 dotted", "1/8 dotted", "1/4 triplet", "1/8 triplet", "1/16 triplet"};
static const char* const rightDivisionNames[] = {"Ratio", "1/1", "1/2", "1/4", "1/8", "1/16", "1/4 dotted", "1/8 dotted", "1/4 triplet", "1/8 triplet", "1/16 triplet"};
// Parameter IDs are preset schema v1. Append future parameters; do not reorder.
static constexpr _NT_parameter baseParameters[] = {
    NT_PARAMETER_AUDIO_INPUT("Input L", 0, 1)
    NT_PARAMETER_AUDIO_INPUT("Input R", 0, 2)
    NT_PARAMETER_AUDIO_OUTPUT_WITH_MODE("Output L", 0, 13)
    NT_PARAMETER_AUDIO_OUTPUT_WITH_MODE("Output R", 0, 14)
    {"Model", 0, echolab::modelCount-1, 0, kNT_unitEnum, 0, modelNames},
    {"Time", 1, 8000, 350, kNT_unitMs, 0, nullptr},
    {"Feedback", 0, 2000, 280, kNT_unitPercent, kNT_scaling10, nullptr},
    {"Mix", 0, 1000, 390, kNT_unitPercent, kNT_scaling10, nullptr},
    {"Tone (extension)", 100, 20000, 3500, kNT_unitHz, 0, nullptr},
    {"Drive (extension)", 0, 1000, 400, kNT_unitPercent, kNT_scaling10, nullptr},
    {"Internal bypass", 0, 2, 0, kNT_unitEnum, 0, bypassNames},
    {"Resolution (bits)", 4, 24, 8, kNT_unitNone, 0, nullptr},
    {"Mod rate", 1, 2000, 100, kNT_unitHz, kNT_scaling100, nullptr},
    {"Mod depth", 0, 5000, 600, kNT_unitMs, kNT_scaling100, nullptr},
    {"Duck threshold", -800, 0, -240, kNT_unitDb, kNT_scaling10, nullptr},
    {"Duck amount", 0, 1000, 900, kNT_unitPercent, kNT_scaling10, nullptr},
    {"Attack", 1, 5000, 5, kNT_unitMs, 0, nullptr},
    {"Release", 1, 10000, 250, kNT_unitMs, 0, nullptr},
    {"Swell", 1, 10000, 250, kNT_unitMs, 0, nullptr},
    {"R/L delay ratio", 25, 400, 150, kNT_unitNone, kNT_scaling100, nullptr},
    {"Loop action", 0, 4, 1, kNT_unitEnum, 0, loopNames},
    {"Musical limiter", 0, 1, 1, kNT_unitEnum, 0, onOffNames},
    {"Limit ceiling", 10, 891, 891, kNT_unitPercent, kNT_scaling10, nullptr},
    {"Limit release", 1, 2000, 150, kNT_unitMs, 0, nullptr},
    {"Sample rate", 0, 32000, 8000, kNT_unitHasStrings, 0, nullptr},
    {"Exciter amount", 0, 1000, 100, kNT_unitPercent, kNT_scaling10, nullptr},
    {"Exciter tune", 200, 10000, 4000, kNT_unitHz, 0, nullptr},
    {"Tap pattern", 0, echolab::multitap::patternCount-1, 0, kNT_unitHasStrings, 0, nullptr},
    {"Mod 1 source", 0, 5, 0, kNT_unitHasStrings, 0, nullptr},
    {"Mod 1 target", 0, 12, 0, kNT_unitHasStrings, 0, nullptr},
    {"Mod 1 amount", -1000, 1000, 0, kNT_unitPercent, kNT_scaling10, nullptr},
    {"Mod 2 source", 0, 5, 0, kNT_unitHasStrings, 0, nullptr},
    {"Mod 2 target", 0, 12, 0, kNT_unitHasStrings, 0, nullptr},
    {"Mod 2 amount", -1000, 1000, 0, kNT_unitPercent, kNT_scaling10, nullptr},
    NT_PARAMETER_CV_INPUT("Mod CV 1", 0, 0)
    NT_PARAMETER_CV_INPUT("Mod CV 2", 0, 0)
    NT_PARAMETER_CV_INPUT("Clock CV", 0, 0)
    {"Clock pulses/qtr", 0, 3, 2, kNT_unitEnum, 0, ppqnNames},
    {"Sync", 0, 3, 3, kNT_unitEnum, 0, syncNames},
    {"Tempo", 30, 300, 120, kNT_unitBPM, 0, nullptr},
    {"Division", 0, 9, 2, kNT_unitEnum, 0, divisionNames},
    {"Feedback low-pass", 0, 20000, 12000, kNT_unitHasStrings, 0, nullptr},
    {"Slow drift", 0, 1000, 250, kNT_unitPercent, kNT_scaling10, nullptr},
    {"Feedback high-pass", 0, 2000, 80, kNT_unitHasStrings, 0, nullptr},
    {"Rate envelope end", 100, 32000, 1200, kNT_unitHz, 0, nullptr},
    {"Rate decay", 10, 10000, 1500, kNT_unitMs, 0, nullptr},
    {"Rate envelope", 0, 1000, 1000, kNT_unitPercent, kNT_scaling10, nullptr},
    {"Rack sample rate", 0, 32000, 16000, kNT_unitHasStrings, 0, nullptr},
    {"8bit low-pass", 0, 20000, 2000, kNT_unitHasStrings, 0, nullptr},
    {"Saved voicing", 0, 3, 0, kNT_unitHasStrings, 0, nullptr},
    {"Internal modulation", 0, 1, 1, kNT_unitEnum, 0, onOffNames},
    {"Version", 0, 1, 0, kNT_unitHasStrings, 0, nullptr},
    {"Right division", 0, 10, 0, kNT_unitEnum, 0, rightDivisionNames},
    {"Preset", 0, preset_bank::slotCount, 0, kNT_unitEnum, 0, preset_bank::names},
    {"Exciter return HP", 0, 1, 0, kNT_unitEnum, 0, onOffNames},
    {"Wet tone", 0, 1, 0, kNT_unitEnum, 0, onOffNames},
    {"Stereo timing", 0, 1, 0, kNT_unitEnum, 0, onOffNames},
};
static_assert(ARRAY_SIZE(baseParameters)==ParameterCount,"every stable parameter has a definition");
struct ParameterDefinitions { _NT_parameter values[ParameterCount]; };
static constexpr ParameterDefinitions factoryParameterDefinitions() {
    ParameterDefinitions definitions{};
    for(int parameter=0;parameter<ParameterCount;++parameter) {
        definitions.values[parameter]=baseParameters[parameter];
        definitions.values[parameter].def=preset_bank::defaultValue(parameter,baseParameters[parameter].def);
    }
    // Auto resolves to the recipe's A04 character on Analog. Keep its historical
    // zero default so old files lacking Voicing51 retain each model's own voice.
    definitions.values[Voicing].def=0;
    definitions.values[FactoryPreset].def=static_cast<int16_t>(preset_bank::defaultSlot);
    return definitions;
}
// Defaults are compiled into the host's definitions. Existing saved NT values
// still belong to the host and are never replaced by an implicit recipe load.
static constexpr auto factoryParameters=factoryParameterDefinitions();
static constexpr const _NT_parameter (&parameters)[ParameterCount]=factoryParameters.values;
static const uint8_t modelParams[] = {FactoryPreset, Model, ModulationEnabled};
static const uint8_t mainParams[] = {Time, Feedback, Mix};
static const uint8_t syncedMainParams[] = {Division, Feedback, Mix};
static const uint8_t routingParams[] = {InputL, InputR, OutputL, OutputModeL, OutputR, OutputModeR, Bypass};
static const uint8_t modulationParams[] = {ModRate, ModDepth};
static const uint8_t rateParams[] = {ModRate};
static const uint8_t mappedEnvelopeParams[] = {Attack, Release};
static const uint8_t resolutionParams[] = {BitDepth, SampleRateHz, LoResCutoffHz};
static const uint8_t legacyResolutionParams[] = {BitDepth, SampleRateHz};
static const uint8_t rateEnvelopeParams[] = {LoResEndHz, LoResDecayMs, LoResEnvelopeAmount};
static const uint8_t rackRateParams[] = {RackSampleRateHz};
static const uint8_t duckParams[] = {DuckThreshold, DuckAmount, Attack, Release};
static const uint8_t envelopeParams[] = {DuckThreshold, DuckAmount, Attack, Release, Swell};
static const uint8_t stereoParams[] = {StereoRatio};
static const uint8_t syncedStereoParams[] = {Division, RightDivision};
static const uint8_t syncedStereoRatioParams[] = {Division, RightDivision, StereoRatio};
static const uint8_t colouredStereoOffParams[] = {StereoTiming};
static const uint8_t colouredStereoParams[] = {StereoTiming, StereoRatio};
static const uint8_t colouredSyncedStereoParams[] = {StereoTiming, Division, RightDivision};
static const uint8_t colouredSyncedStereoRatioParams[] = {StereoTiming, Division, RightDivision, StereoRatio};
static const uint8_t loopParams[] = {LoopAction};
static const uint8_t tapParams[] = {TapPattern};
static const uint8_t syncedTapParams[] = {Division, TapPattern};
static const uint8_t exciterParams[] = {ExciterAmount, ExciterTuneHz};
static const uint8_t limitParams[] = {LimiterOn, LimiterCeiling, LimiterReleaseMs};
static const uint8_t syncParams[] = {SyncMode, TempoBpm, Division, ClockBus, ClockPpqn};
static const uint8_t map1Params[] = {Mod1Source, Mod1Destination, Mod1Amount};
static const uint8_t map2Params[] = {Mod2Source, Mod2Destination, Mod2Amount};
static const uint8_t cvParams[] = {CV1Bus, CV2Bus};
static const uint8_t feedbackColourParams[] = {FeedbackLPHz, FeedbackHPHz, Drift};
static const uint8_t a05ColourParams[] = {FeedbackLPHz, Drift};
static const uint8_t regenerationParams[] = {FeedbackLPHz, FeedbackHPHz};
static const _NT_specification specifications[] = {{"Max delay", 1, 8, 3, kNT_typeSeconds}};

struct Algorithm : _NT_algorithm {
    echolab::Engine engine;
    echolab::Parameters controls;
    ClockSync clock;
    echolab::safety::Limiter outputLimiter;
    _NT_parameterPage pageStorage[16]{};
    uint8_t colorParameters[6]{};
    _NT_parameterPages pageDefinitions{};
    int visibleModel = -1;
    int visibleVoicing = -1;
    bool visibleSyncedTime = false;
    bool visibleRightRatio = true;
    bool visibleWetTone = false;
    bool visibleExciterHighpass = false;
    bool visibleMappedLfo = false;
    bool visibleMappedEnvelope = false;
    bool visibleStereoTiming = false;
    int observedVersion = 0;
    int observedDestinations[2]{};
    int pendingVoicing = -1;
    int observedPreset = 0;
    std::uint64_t initialParameters = 0;
    std::uint64_t pendingWrites = 0;
    int16_t desiredValues[ParameterCount]{};
    int16_t sentValues[ParameterCount]{};
    bool applyingPreset = false;
    std::uint32_t appliedClockRevision = 0;
    float preparedLimiterRelease = 0.0f;
    float wheel = 0.0f;
    float maxDelayMs = 3000.0f;
    bool ready = false;
    bool initialControls = true;
    bool initialNormalizationPending = true;
    std::atomic<std::uint32_t> initialUiSnapshot{0};
    Algorithm() : _NT_algorithm{} {}
};

static int value(const Algorithm* self, int id);
static bool hasVersion(int model) {
    if(model==2 || model==19) return false;
    return echolab::resolvedVoicing(static_cast<echolab::Model>(model),0)!=3;
}
static bool nativeTone(int model) {
    return model==1 || model==2 || model==4 || model==7 || model==8 || model==9 || model==14 || model==18 || model==19;
}
static bool colouredStereoModel(int model) {
    return model!=15 && echolab::supportsStereoTiming(static_cast<echolab::Model>(model));
}
static bool hasStereoTiming(const Algorithm* self) {
    const int model=value(self,Model);
    return model==15 || (colouredStereoModel(model) && value(self,StereoTiming)!=0);
}
static bool meaningfulDestination(const Algorithm* self,int destination) {
    const int model=value(self,Model);
    switch(destination) {
    case 0: case 1: case 2: case 5: return true;
    case 3: return nativeTone(model) || value(self,WetToneOn)!=0;
    case 4: return nativeTone(model) || model==17;
    case 6: return value(self,ModulationEnabled)!=0 && (model==7 || model==9 || model==10 || model==14);
    case 7: return model==17;
    case 8: return model==5 && value(self,SampleRateHz)!=0;
    case 9: return value(self,ModulationEnabled)!=0 &&
        (model==7 || model==8 || model==9 || model==10 || model==14 || model==18 ||
         value(self,Mod1Source)==1 || value(self,Mod2Source)==1);
    case 10: return hasStereoTiming(self);
    case 11: return value(self,ModulationEnabled)!=0 && model!=0 && model!=16 &&
        echolab::resolvedVoicing(static_cast<echolab::Model>(model),self->controls.voicing)!=1;
    case 12: return model==6 || model==12;
    default: return false;
    }
}
static int selectableDestination(const Algorithm* self,int requested,int previous) {
    if(meaningfulDestination(self,requested)) return requested;
    const int direction=requested>previous ? 1 : -1;
    for(int target=requested+direction;target>=0 && target<=12;target+=direction)
        if(meaningfulDestination(self,target)) return target;
    return 0;
}

static void updatePages(Algorithm* self, int model, bool notify) {
    const int voice=echolab::resolvedVoicing(static_cast<echolab::Model>(model),self->controls.voicing);
    const bool syncedTime=self->clock.locked();
    const bool rightRatio=value(self,RightDivision)==0;
    const bool wetTone=value(self,WetToneOn)!=0;
    const bool exciterHighpass=value(self,ExciterHighpassOn)!=0;
    const bool mappedLfo=value(self,Mod1Source)==1 || value(self,Mod2Source)==1;
    const bool mappedEnvelope=value(self,Mod1Source)==2 || value(self,Mod2Source)==2;
    const bool stereoTiming=hasStereoTiming(self);
    if(self->visibleModel==model && self->visibleVoicing==voice && self->visibleSyncedTime==syncedTime &&
       (!stereoTiming || self->visibleRightRatio==rightRatio) && self->visibleStereoTiming==stereoTiming && self->visibleWetTone==wetTone &&
       self->visibleExciterHighpass==exciterHighpass && self->visibleMappedLfo==mappedLfo &&
       self->visibleMappedEnvelope==mappedEnvelope) return;
    unsigned count=0;
    const auto add=[&](const char* name,const uint8_t* ids,std::size_t size,uint8_t group) {
        self->pageStorage[count++]={name,static_cast<uint8_t>(size),group,{0,0},ids};
    };
    add("Delay",syncedTime ? syncedMainParams : mainParams,ARRAY_SIZE(mainParams),2);
    add("Model",modelParams,ARRAY_SIZE(modelParams),1);
    unsigned colorCount=0;
    const bool modelTone=nativeTone(model);
    if(hasVersion(model)) self->colorParameters[colorCount++]=Version;
    self->colorParameters[colorCount++]=WetToneOn;
    if(modelTone || wetTone) self->colorParameters[colorCount++]=Tone;
    if(modelTone || model==17) self->colorParameters[colorCount++]=Drive;
    if(model==17 && voice==1) {
        self->colorParameters[colorCount++]=ExciterHighpassOn;
        if(exciterHighpass) self->colorParameters[colorCount++]=FeedbackHPHz;
    }
    add("Color",self->colorParameters,colorCount,3);
    if(model==7 || model==9 || model==10 || model==14)
        add("Modulation",modulationParams,ARRAY_SIZE(modulationParams),4);
    if(model==8 || model==18) add("Sweep",rateParams,ARRAY_SIZE(rateParams),4);
    if(mappedLfo && model!=7 && model!=9 && model!=10 && model!=14 && model!=8 && model!=18)
        add("Mapped LFO",rateParams,ARRAY_SIZE(rateParams),4);
    if(mappedEnvelope && model!=6 && model!=12)
        add("Mapped envelope",mappedEnvelopeParams,ARRAY_SIZE(mappedEnvelopeParams),15);
    if(model==4) {
        if(syncedTime) add("Heads",syncedTapParams,ARRAY_SIZE(syncedTapParams),5);
        else add("Heads",tapParams,ARRAY_SIZE(tapParams),5);
    }
    if(model==5) {
        if(voice==3) {
            add("8bit",resolutionParams,ARRAY_SIZE(resolutionParams),5);
            add("Rate envelope",rateEnvelopeParams,ARRAY_SIZE(rateEnvelopeParams),13);
        } else add("8bit",legacyResolutionParams,ARRAY_SIZE(legacyResolutionParams),5);
    }
    if(model==6) add("Ducking",duckParams,ARRAY_SIZE(duckParams),5);
    if(model==12) add("Swell",envelopeParams,ARRAY_SIZE(envelopeParams),5);
    if(model==13 && voice==3) add("Rack",rackRateParams,ARRAY_SIZE(rackRateParams),14);
    if(model==15) {
        if(!syncedTime) add("Stereo",stereoParams,ARRAY_SIZE(stereoParams),5);
        else if(rightRatio) add("Stereo",syncedStereoRatioParams,ARRAY_SIZE(syncedStereoRatioParams),5);
        else add("Stereo",syncedStereoParams,ARRAY_SIZE(syncedStereoParams),5);
    } else if(colouredStereoModel(model)) {
        if(!stereoTiming) add("Stereo",colouredStereoOffParams,ARRAY_SIZE(colouredStereoOffParams),5);
        else if(!syncedTime) add("Stereo",colouredStereoParams,ARRAY_SIZE(colouredStereoParams),5);
        else if(rightRatio) add("Stereo",colouredSyncedStereoRatioParams,ARRAY_SIZE(colouredSyncedStereoRatioParams),5);
        else add("Stereo",colouredSyncedStereoParams,ARRAY_SIZE(colouredSyncedStereoParams),5);
    }
    if(model==16) add("Looper",loopParams,ARRAY_SIZE(loopParams),5);
    if(model==17) add("Exciter",exciterParams,ARRAY_SIZE(exciterParams),5);
    if(voice==3) {
        if(model!=0) add(model==16 ? "Overdub color" : "Feedback color",feedbackColourParams,ARRAY_SIZE(feedbackColourParams),12);
        else add("Regen >100%",regenerationParams,ARRAY_SIZE(regenerationParams),12);
    } else if(voice==2 && model!=0)
        add(model==16 ? "Overdub color" : "Feedback color",a05ColourParams,ARRAY_SIZE(a05ColourParams),12);
    add("Clock",syncParams,ARRAY_SIZE(syncParams),6);
    add("Mod map 1",map1Params,ARRAY_SIZE(map1Params),7);
    add("Mod map 2",map2Params,ARRAY_SIZE(map2Params),8);
    add("CV inputs",cvParams,ARRAY_SIZE(cvParams),9);
    add("Limiter",limitParams,ARRAY_SIZE(limitParams),10);
    add("Routing",routingParams,ARRAY_SIZE(routingParams),11);
    self->pageDefinitions={count,self->pageStorage};
    self->parameterPages=&self->pageDefinitions;
    self->visibleModel=model;
    self->visibleVoicing=voice;
    self->visibleSyncedTime=syncedTime;
    self->visibleRightRatio=rightRatio;
    self->visibleWetTone=wetTone;
    self->visibleExciterHighpass=exciterHighpass;
    self->visibleMappedLfo=mappedLfo;
    self->visibleMappedEnvelope=mappedEnvelope;
    self->visibleStereoTiming=stereoTiming;
    if(notify) { const auto index=NT_algorithmIndex(self);if(index>=0) NT_updateParameterPages(static_cast<uint32_t>(index)); }
}

static int maxSeconds(const int32_t* specs) {
    const int v = specs ? specs[0] : 3;
    return v < 1 ? 1 : (v > 8 ? 8 : v);
}
static float sampleRate() {
    // Fail closed through Engine::prepare if an invalid host rate is supplied.
    return static_cast<float>(NT_globals.sampleRate);
}
template<class T> static T* aligned(uint8_t* address) {
    const uintptr_t value = reinterpret_cast<uintptr_t>(address);
    return reinterpret_cast<T*>((value + alignof(T) - 1) & ~(uintptr_t(alignof(T) - 1)));
}
void calculateRequirements(_NT_algorithmRequirements& req, const int32_t* specs) {
    req = {};
    req.numParameters = ParameterCount;
    req.sram = static_cast<uint32_t>(sizeof(Algorithm) + alignof(Algorithm) - 1);
    const size_t samples = echolab::Engine::requiredSamples(sampleRate(), maxSeconds(specs) * 1000.0f);
    // Bound arithmetic even for an invalid host sample rate; supported rates fit easily.
    if (samples && samples <= (UINT32_MAX - alignof(float) + 1) / sizeof(float))
        req.dram = static_cast<uint32_t>(samples * sizeof(float) + alignof(float) - 1);
}
_NT_algorithm* construct(const _NT_algorithmMemoryPtrs& ptrs, const _NT_algorithmRequirements& req,
                         const int32_t* specs) {
    auto* self = new (aligned<Algorithm>(ptrs.sram)) Algorithm();
    self->parameters = parameters;
    updatePages(self,parameters[Model].def,false);
    self->maxDelayMs = maxSeconds(specs) * 1000.0f;
    const size_t count = echolab::Engine::requiredSamples(sampleRate(), self->maxDelayMs);
    const uintptr_t padding = ptrs.dram ? reinterpret_cast<uintptr_t>(aligned<float>(ptrs.dram)) - reinterpret_cast<uintptr_t>(ptrs.dram) : 0;
    if (ptrs.dram && req.dram >= padding && count <= (req.dram - padding) / sizeof(float))
        self->ready = self->engine.prepare(sampleRate(), self->maxDelayMs, aligned<float>(ptrs.dram), count);
    // No reads from v here: its ownership and initialization belong to the host.
    return self;
}
static int rawValue(const Algorithm* self, int id) {
    const int v = self->v ? self->v[id] : parameters[id].def;
    return v < parameters[id].min ? parameters[id].min : (v > parameters[id].max ? parameters[id].max : v);
}
static int value(const Algorithm* self,int id) {
    return self->pendingWrites&(std::uint64_t(1)<<id) ? self->desiredValues[id] : rawValue(self,id);
}
static void setHostParameter(Algorithm* self,int parameter,int setting) {
    const auto index=NT_algorithmIndex(self);
    if(index>=0) NT_setParameterFromAudio(static_cast<uint32_t>(index),
                                         static_cast<uint32_t>(parameter)+NT_parameterOffset(),static_cast<int16_t>(setting));
}
static bool stageParameter(Algorithm* self,int parameter,int setting) {
    const auto bit=std::uint64_t(1)<<parameter;
    const int bounded=std::max<int>(parameters[parameter].min,std::min<int>(parameters[parameter].max,setting));
    self->desiredValues[parameter]=static_cast<int16_t>(bounded);
    if(self->pendingWrites&bit) return false;
    // An unchanged host value may produce no callback. Do not reserve a write
    // awaiting an acknowledgement that can never arrive: a later recipe must
    // still be able to persist its new value after a manual Model edit.
    if(rawValue(self,parameter)==bounded) {
        if(parameter==Voicing && self->pendingVoicing==bounded) self->pendingVoicing=-1;
        return false;
    }
    self->pendingWrites|=bit;
    self->sentValues[parameter]=static_cast<int16_t>(bounded);
    return true;
}
static void requestParameter(Algorithm* self,int parameter,int setting) {
    if(stageParameter(self,parameter,setting)) setHostParameter(self,parameter,self->sentValues[parameter]);
}
static float stereoRatio(const Algorithm* self,float leftMs) {
    const int right=value(self,RightDivision);
    if(!self->clock.locked() || right==0) return value(self,StereoRatio)*.01f;
    const float rightMs=std::min(self->maxDelayMs,self->clock.lastQuarterMs()*
                               static_cast<float>(ClockSync::quarterMultiplier(right-1)));
    return rightMs/leftMs;
}
static void updateControls(Algorithm* self) {
    self->controls.model = static_cast<echolab::Model>(value(self, Model));
    static const int ppqn[]={1,2,4,24};
    self->clock.configure(sampleRate(),value(self,SyncMode),ppqn[value(self,ClockPpqn)],
                          static_cast<float>(value(self,TempoBpm)),value(self,Division));
    self->appliedClockRevision=self->clock.revision();
    self->controls.delay_ms = self->clock.delayMs(static_cast<float>(value(self, Time)));
    if (self->controls.delay_ms > self->maxDelayMs) self->controls.delay_ms = self->maxDelayMs;
    self->controls.feedback = value(self, Feedback) * 0.001f;
    self->controls.mix = value(self, Mix) * 0.001f;
    self->controls.tone_hz = static_cast<float>(value(self, Tone));
    self->controls.drive = value(self, Drive) * 0.001f;
    self->controls.bit_depth = value(self, BitDepth);
    self->controls.mod_rate_hz = value(self, ModRate) * .01f;
    self->controls.mod_depth_ms = value(self, ModDepth) * .01f;
    self->controls.duck_threshold_db = value(self, DuckThreshold) * .1f;
    self->controls.duck_amount = value(self, DuckAmount) * .001f;
    self->controls.attack_ms = static_cast<float>(value(self, Attack));
    self->controls.release_ms = static_cast<float>(value(self, Release));
    self->controls.swell_ms = static_cast<float>(value(self, Swell));
    self->controls.stereo_timing_enabled = value(self,StereoTiming)!=0;
    self->controls.stereo_ratio = stereoRatio(self,self->controls.delay_ms);
    self->controls.loop_action = value(self, LoopAction);
    self->controls.limiter_enabled = value(self,LimiterOn)!=0;
    self->controls.limiter_ceiling = value(self,LimiterCeiling)*.001f;
    self->controls.limiter_release_ms = static_cast<float>(value(self,LimiterReleaseMs));
    const int rate=value(self,SampleRateHz);
    self->controls.sample_rate_hz = rate ? static_cast<float>(std::max(500,rate)) : 0.0f;
    self->controls.exciter_amount = value(self,ExciterAmount)*.001f;
    self->controls.exciter_tune_hz = static_cast<float>(value(self,ExciterTuneHz));
    self->controls.tap_pattern = value(self,TapPattern);
    self->controls.mod1_source = value(self,Mod1Source);
    self->controls.mod1_destination = value(self,Mod1Destination);
    self->controls.mod1_amount = value(self,Mod1Amount)*.001f;
    self->controls.mod2_source = value(self,Mod2Source);
    self->controls.mod2_destination = value(self,Mod2Destination);
    self->controls.mod2_amount = value(self,Mod2Amount)*.001f;
    const int lowpass=value(self,FeedbackLPHz);
    self->controls.feedback_lowpass_hz=lowpass ? static_cast<float>(std::max(20,lowpass)) : 0.0f;
    self->controls.drift_amount=value(self,Drift)*.001f;
    const int highpass=value(self,FeedbackHPHz);
    self->controls.feedback_highpass_hz=highpass ? static_cast<float>(std::max(20,highpass)) : 0.0f;
    self->controls.lores_rate_end_hz=static_cast<float>(value(self,LoResEndHz));
    self->controls.lores_rate_decay_ms=static_cast<float>(value(self,LoResDecayMs));
    self->controls.lores_rate_envelope_amount=value(self,LoResEnvelopeAmount)*.001f;
    const int rackRate=value(self,RackSampleRateHz);
    self->controls.digital_rack_rate_hz=rackRate ? static_cast<float>(std::max(1000,rackRate)) : 0.0f;
    const int cutoff=value(self,LoResCutoffHz);
    self->controls.lores_cutoff_hz=cutoff ? static_cast<float>(std::max(20,cutoff)) : 0.0f;
    const int savedVoicing=value(self,Voicing);
    const int productVersion=self->initialControls ? value(self,Version) : self->observedVersion;
    self->controls.voicing=self->pendingVoicing>=0 ? self->pendingVoicing :
        (productVersion==1 ? 3 : savedVoicing);
    self->controls.modulation_enabled=value(self,ModulationEnabled)!=0;
    self->controls.exciter_highpass_enabled=value(self,ExciterHighpassOn)!=0;
    self->controls.wet_tone_enabled=value(self,WetToneOn)!=0;
    if(self->preparedLimiterRelease!=self->controls.limiter_release_ms) {
        self->outputLimiter.prepare(sampleRate(),self->controls.limiter_release_ms);
        self->preparedLimiterRelease=self->controls.limiter_release_ms;
    }
    self->outputLimiter.setCeiling(self->controls.limiter_ceiling);
    self->engine.setParameters(self->controls);
}
static bool activePreset(int slot) {
    return slot>=1 && slot<=static_cast<int>(preset_bank::slotCount) &&
        preset_bank::presets[slot-1].values && preset_bank::presets[slot-1].count!=0;
}
static bool matchesDefaultRecipeSound(const Algorithm* self) {
    const auto& preset=preset_bank::presets[preset_bank::defaultSlot-1];
    for(unsigned i=0;i<preset.count;++i) {
        const auto& setting=preset.values[i];
        if(setting.parameter==SyncMode || setting.parameter==TempoBpm || setting.parameter==ClockPpqn) continue;
        if(setting.parameter==Voicing) {
            const auto model=static_cast<echolab::Model>(value(self,Model));
            if(echolab::resolvedVoicing(model,value(self,Voicing))!=echolab::resolvedVoicing(model,setting.value)) return false;
            continue;
        }
        if(value(self,setting.parameter)!=setting.value) return false;
    }
    return true;
}
static void finishInitialControls(Algorithm* self,int changingParameter=-1) {
    if(!self->initialControls) return;
    updateControls(self);
    updatePages(self,value(self,Model),true);
    // On UI activation preserve the selector observation captured before the
    // current changed selector, so this first live choice can be applied.
    if(changingParameter!=Version) self->observedVersion=value(self,Version);
    if(changingParameter!=FactoryPreset) self->observedPreset=value(self,FactoryPreset);
    self->initialControls=false;
    self->initialUiSnapshot.store(0,std::memory_order_relaxed);
}
static void normalizeInitialControls(Algorithm* self) {
    if(!self->initialNormalizationPending) return;
    self->initialNormalizationPending=false;
    // Gate completion itself must not issue setters while loaded callbacks
    // may still be arriving. Preserve the original first-render migration
    // boundary and inspect the latest live controls, not an earlier snapshot.
    if(self->observedPreset==static_cast<int>(preset_bank::defaultSlot) && !matchesDefaultRecipeSound(self)) {
        // Older NT files may lack Preset55 and inherit its new default4.
        // Correct that label only; the host's saved sound is authoritative.
        self->observedPreset=0;
        requestParameter(self,FactoryPreset,0);
    }
    if(hasVersion(value(self,Model)) && self->observedVersion==0 && self->controls.voicing==3) {
        self->observedVersion=1;
        requestParameter(self,Version,1);
    }
}
static int selectablePreset(int requested,int previous) {
    if(requested==0 || activePreset(requested)) return requested;
    const int direction=requested>previous ? 1 : -1;
    for(int slot=requested+direction;slot>=1 && slot<=static_cast<int>(preset_bank::slotCount);slot+=direction)
        if(activePreset(slot)) return slot;
    return 0;
}
static void applyPreset(Algorithm* self,int selected) {
    if(!activePreset(selected)) return;
    const auto& preset=preset_bank::presets[selected-1];
    std::uint64_t send=0;
    self->applyingPreset=true;
    // Publish all desired values before the first host callback. The audio and
    // pages then see one complete recipe while the host persists its fields.
    for(unsigned i=0;i<preset.count;++i) {
        const auto& setting=preset.values[i];
        if(setting.parameter<0 || setting.parameter>=ParameterCount || setting.parameter==FactoryPreset ||
           setting.parameter==SyncMode || setting.parameter==TempoBpm || setting.parameter==ClockPpqn) continue;
        if(stageParameter(self,setting.parameter,setting.value)) send|=std::uint64_t(1)<<setting.parameter;
    }
    if(value(self,SyncMode)==0) {
        // A musical recipe must activate its note values. Keep an existing
        // internal/external clock; Free moves to the user's internal tempo.
        if(stageParameter(self,SyncMode,3)) send|=std::uint64_t(1)<<SyncMode;
    }
    self->observedPreset=selected;
    self->observedVersion=value(self,Version);
    self->observedDestinations[0]=value(self,Mod1Destination);
    self->observedDestinations[1]=value(self,Mod2Destination);
    self->pendingVoicing=-1;
    updateControls(self);updatePages(self,value(self,Model),true);
    for(int p=0;p<ParameterCount;++p) if(send&(std::uint64_t(1)<<p)) setHostParameter(self,p,self->sentValues[p]);
    self->applyingPreset=false;
}
static bool keepsPresetName(int p) {
    return p<=OutputModeR || p==Bypass || p==CV1Bus || p==CV2Bus || p==ClockBus || p==ClockPpqn || p==SyncMode || p==TempoBpm;
}
void parameterChanged(_NT_algorithm* base, int p) {
    // Coefficients cached in the core; no clearing a delay or allocating here.
    auto* self=static_cast<Algorithm*>(base);
    if(p<0 || p>=ParameterCount) return;
    const std::uint32_t uiSnapshot=self->initialControls ? self->initialUiSnapshot.load(std::memory_order_relaxed) : 0;
    const bool changedUiSelector=(uiSnapshot&uiSnapshotPresent) &&
        ((p==FactoryPreset && rawValue(self,p)!=static_cast<int>(uiSnapshot&0xffff)) ||
         (p==Version && rawValue(self,p)!=static_cast<int>((uiSnapshot>>16)&1)) ||
         (p==Mod1Destination && rawValue(self,p)!=static_cast<int>((uiSnapshot>>17)&15)) ||
         (p==Mod2Destination && rawValue(self,p)!=static_cast<int>((uiSnapshot>>21)&15)));
    // Non-selector or identical notifications can still belong to loading.
    // Keep the first UI snapshot intact until a selector actually differs.
    if(changedUiSelector) {
        self->observedPreset=static_cast<int>(uiSnapshot&0xffff);
        self->observedVersion=static_cast<int>((uiSnapshot>>16)&1);
        self->observedDestinations[0]=static_cast<int>((uiSnapshot>>17)&15);
        self->observedDestinations[1]=static_cast<int>((uiSnapshot>>21)&15);
        finishInitialControls(self,p);
    }
    if(self->initialControls && self->initialParameters==0) {
        // Snapshot selectors even when the host notifies only a subset of the
        // loaded parameters. Nothing here applies a stored factory recipe.
        self->observedPreset=rawValue(self,FactoryPreset);
        self->observedVersion=rawValue(self,Version);
    }
    const auto bit=std::uint64_t(1)<<p;
    const int raw=rawValue(self,p);
    if(self->pendingWrites&bit) {
        if(raw==self->sentValues[p]) {
            if(raw!=self->desiredValues[p]) {
                // A previous recipe or a later manual edit superseded this
                // queued write. Persist the newest value, retaining its sound.
                self->sentValues[p]=self->desiredValues[p];
                setHostParameter(self,p,self->sentValues[p]);
            } else self->pendingWrites&=~bit;
            if(p==Voicing && raw==self->pendingVoicing) {
                self->pendingVoicing=-1;
                // A native edit may have superseded this temporary Version
                // override. Its acknowledgement must also release the cached
                // DSP/page override, not only settle the host-owned value.
                updateControls(self);
                updatePages(self,value(self,Model),true);
            }
            return;
        }
        // An actual edit arrived while an older host write was outstanding.
        // Keep that edit in the desired cache until its stale acknowledgement
        // can be corrected through the host, without touching v directly.
        self->desiredValues[p]=static_cast<int16_t>(raw);
    }
    if(p==FactoryPreset) {
        if(self->initialControls) self->observedPreset=raw;
        else if(raw!=self->observedPreset) {
            const int selected=selectablePreset(raw,self->observedPreset);
            self->observedPreset=selected;
            if(selected!=raw) requestParameter(self,FactoryPreset,selected);
            if(selected!=0) { applyPreset(self,selected);return; }
        }
    } else if(!self->initialControls && !self->applyingPreset && !keepsPresetName(p) && self->observedPreset!=0) {
        self->observedPreset=0;
        requestParameter(self,FactoryPreset,0);
    }
    if(p==Version) {
        const int selected=value(self,Version);
        if(!self->initialControls && selected!=self->observedVersion) {
            self->observedVersion=selected;
            // Keep old saved A04/A05 choices until the user explicitly edits
            // Version. The host owns v; persist the new selection through its API.
            self->pendingVoicing=selected ? 3 : 0;
            requestParameter(self,Voicing,self->pendingVoicing);
        } else if(self->initialControls) self->observedVersion=selected;
    }
    if(p==Mod1Destination || p==Mod2Destination) {
        const int slot=p==Mod1Destination ? 0 : 1;
        const int requested=value(self,p);
        const int selected=self->initialControls || self->applyingPreset || requested==self->observedDestinations[slot] ? requested :
            selectableDestination(self,requested,self->observedDestinations[slot]);
        self->observedDestinations[slot]=selected;
        if(selected!=requested) requestParameter(self,p,selected);
    }
    if(p==Voicing && value(self,Voicing)==self->pendingVoicing) self->pendingVoicing=-1;
    if(p==ClockBus) self->clock.clearCv();
    updateControls(self);
    updatePages(self,value(self,Model),true);
    if(self->initialControls) {
        self->initialParameters|=bit;
        // A complete initial callback pass is enough to accept later edits;
        // a bypassed or not-yet-rendered instance must not wait for step().
        if(self->initialParameters==initialParameterMask) finishInitialControls(self);
    }
}
static void updateClockTime(Algorithm* self) {
    if(self->appliedClockRevision==self->clock.revision()) return;
    self->appliedClockRevision=self->clock.revision();
    const float ms=std::min(self->maxDelayMs,self->clock.delayMs(static_cast<float>(value(self,Time))));
    const float ratio=stereoRatio(self,ms);
    if(ms!=self->controls.delay_ms || ratio!=self->controls.stereo_ratio) {
        self->controls.delay_ms=ms;self->controls.stereo_ratio=ratio;self->engine.setParameters(self->controls);
    }
    updatePages(self,value(self,Model),true);
}
void midiRealtime(_NT_algorithm* base,uint8_t byte) { static_cast<Algorithm*>(base)->clock.midiRealtime(byte); }
void midiMessage(_NT_algorithm* base,uint8_t status,uint8_t data1,uint8_t data2) {
    if((status&0xf0)==0xb0 && data1==1)
        static_cast<Algorithm*>(base)->wheel=std::min(127,int(data2))/127.0f;
}
static bool delayLimited(const Algorithm* self) {
    const float requested=self->clock.delayMs(static_cast<float>(value(self,Time)));
    if(requested>self->maxDelayMs) return true;
    if(!hasStereoTiming(self)) return false;
    const int right=value(self,RightDivision);
    const float rightMs=self->clock.locked() && right!=0 ? self->clock.lastQuarterMs()*
        static_cast<float>(ClockSync::quarterMultiplier(right-1)) : requested*value(self,StereoRatio)*.01f;
    return rightMs>self->maxDelayMs;
}
bool draw(_NT_algorithm* base) {
    auto* self=static_cast<Algorithm*>(base);
    // Capture side-effect selectors once, before a possible UI edit. A late
    // initialization notification with the same value is not a live selection.
    // Publish the whole snapshot atomically; never update controls/setters here.
    const std::uint32_t snapshot=uiSnapshotPresent | static_cast<std::uint32_t>(rawValue(self,FactoryPreset)) |
        (static_cast<std::uint32_t>(rawValue(self,Version))<<16) |
        (static_cast<std::uint32_t>(rawValue(self,Mod1Destination))<<17) |
        (static_cast<std::uint32_t>(rawValue(self,Mod2Destination))<<21);
    std::uint32_t unset=0;
    self->initialUiSnapshot.compare_exchange_strong(unset,snapshot,std::memory_order_relaxed,std::memory_order_relaxed);
    const char* status="Free Time";
    if(self->clock.mode()==1) status=self->clock.locked() ? "MIDI sync" : "MIDI waiting: manual Time";
    else if(self->clock.mode()==2) status=self->clock.locked() ? "CV sync" : "CV waiting: manual Time";
    else if(self->clock.mode()==3) status="Internal BPM sync";
    if(delayLimited(self)) {
        if(!self->clock.locked()) status="Manual Time: max delay";
        else if(self->clock.mode()==1) status="MIDI sync: max delay";
        else if(self->clock.mode()==2) status="CV sync: max delay";
        else status="Internal BPM: max delay";
    }
    NT_drawText(0,63,status,8,kNT_textLeft,kNT_textTiny);
    return false;
}
int parameterString(_NT_algorithm* base,int p,int v,char* buffer) {
    if(p==Mod1Source || p==Mod2Source || p==Mod1Destination || p==Mod2Destination) {
        const bool source=p==Mod1Source || p==Mod2Source;
        const int choice=std::max(0,std::min(source ? 5 : 12,v));
        const char* name=source ? sourceNames[choice] : destinationNames[choice];
        int length=0;
        while(*name && length<kNT_parameterStringSize-1) buffer[length++]=*name++;
        const auto* self=static_cast<Algorithm*>(base);
        const bool inactive=self && (source ?
            (((choice==1 || choice==2) && value(self,ModulationEnabled)==0) ||
             (choice==3 && value(self,CV1Bus)==0) || (choice==4 && value(self,CV2Bus)==0)) :
            !meaningfulDestination(self,choice));
        if(inactive) {
            const char* suffix=" (off)";
            while(*suffix && length<kNT_parameterStringSize-1) buffer[length++]=*suffix++;
        }
        buffer[length]=0;return length;
    }
    if(p==TapPattern) {
        const auto* self=static_cast<Algorithm*>(base);
        const bool synced=self && self->clock.locked();
        const bool limited=synced && self->clock.delayMs(static_cast<float>(value(self,Time)))>self->maxDelayMs;
        int length=static_cast<int>(formatTapPattern(v,synced && !limited,self ? value(self,Division) : 2,
                                                   buffer,kNT_parameterStringSize));
        if(limited) {
            const char* suffix=" (max)";
            while(*suffix && length<kNT_parameterStringSize-1) buffer[length++]=*suffix++;
            buffer[length]=0;
        }
        return length;
    }
    if(p==Version) {
        int version=std::max(0,std::min(1,v));
        if(base && (v==value(static_cast<Algorithm*>(base),Version) || v==rawValue(static_cast<Algorithm*>(base),Version))) {
            const auto* self=static_cast<Algorithm*>(base);
            version=self->initialControls ? (self->controls.voicing==3 ? 1 : version) : self->observedVersion;
        }
        buffer[0]=versionNames[version][0];buffer[1]=0;return 1;
    }
    if(p==Voicing) {
        const auto model=static_cast<echolab::Model>(base ? value(static_cast<Algorithm*>(base),Model) : 0);
        const int requested=std::max(0,std::min(3,v));
        const int resolved=echolab::resolvedVoicing(model,requested);
        const bool fixed=model==echolab::Model::Tube || model==echolab::Model::TubeA04;
        const char* prefix=fixed ? "Fixed: " : (requested==0 ? "Auto: " : (requested!=resolved ? "Fixed: " : ""));
        int length=0;
        while(*prefix) buffer[length++]=*prefix++;
        const char* name=voicingNames[std::max(1,std::min(3,resolved))];
        for(int i=0;i<3;++i) buffer[length++]=name[i];
        buffer[length]=0;return length;
    }
    if(p!=SampleRateHz && p!=RackSampleRateHz && p!=FeedbackLPHz && p!=FeedbackHPHz && p!=LoResCutoffHz) return 0;
    if((p==SampleRateHz || p==RackSampleRateHz) && v==0) { const char text[]="Full Fs";for(unsigned i=0;i<sizeof(text);++i) buffer[i]=text[i];return sizeof(text)-1; }
    if(v==0) { const char text[]="Off";for(unsigned i=0;i<sizeof(text);++i) buffer[i]=text[i];return sizeof(text)-1; }
    int rate=std::max(500,v),length=0;
    if(p==RackSampleRateHz) rate=std::max(1000,v);
    if(p==FeedbackLPHz || p==FeedbackHPHz || p==LoResCutoffHz) {
        const float fs=sampleRate()>=8000 && sampleRate()<=384000 ? sampleRate() : 48000.0f;
        float cutoff=static_cast<float>(std::max(20,std::min(p==FeedbackHPHz ? 2000 : 20000,v)));
        if(p==FeedbackLPHz && base && value(static_cast<Algorithm*>(base),Model)==13) {
            const auto* self=static_cast<Algorithm*>(base);
            const int voice=echolab::resolvedVoicing(echolab::Model::DigitalRack,self->controls.voicing);
            if(voice==2) cutoff*=.65f;
            else if(voice==3) cutoff*=.5f;
        }
        cutoff=std::min(cutoff,(p==FeedbackHPHz ? .2f : .45f)*fs);
        rate=static_cast<int>(cutoff+.5f);
    }
    char digits[6];int count=0;
    do { digits[count++]=static_cast<char>('0'+rate%10);rate/=10; } while(rate && count<6);
    while(count) buffer[length++]=digits[--count];
    buffer[length++]=' ';buffer[length++]='H';buffer[length++]='z';buffer[length]=0;
    return length;
}
static int bus(const Algorithm* self, int id) {
    const int b = self->v ? self->v[id] : parameters[id].def;
    return b >= 1 && b <= kNT_lastBus ? b : 0;
}
static float cleanBus(float v) {
    if (!std::isfinite(v)) return 0.0f;
    return v < -8.0e12f ? -8.0e12f : (v > 8.0e12f ? 8.0e12f : v);
}
void step(_NT_algorithm* base, float* busFrames, int numFramesBy4) {
    auto* self = static_cast<Algorithm*>(base);
    if (!busFrames || numFramesBy4 <= 0 || static_cast<uint32_t>(numFramesBy4) > NT_globals.maxFramesPerStep / 4) return;
    const size_t n = static_cast<size_t>(numFramesBy4) * 4;
    if(self->initialControls) finishInitialControls(self);
    normalizeInitialControls(self);
    const int il = bus(self, InputL), ir = bus(self, InputR);
    const int ol = bus(self, OutputL), oright = bus(self, OutputR);
    const int cv1Bus=bus(self,CV1Bus),cv2Bus=bus(self,CV2Bus),clockBus=bus(self,ClockBus);
    const bool replaceL = value(self, OutputModeL) != 0, replaceR = value(self, OutputModeR) != 0;
    const int bypass = value(self, Bypass);
    // SDK sampleStreamer maps normalized WAV samples to bus units using *8.
    // This is a provisional scale; physical calibration remains outstanding.
    constexpr float reference = 8.0f;
    for (size_t i = 0; i < n; ++i) {
        // Snapshot both input samples before any writes: all bus aliases are supported.
        const float left = il ? cleanBus(busFrames[(il - 1) * n + i]) : 0.0f;
        const float right = ir ? cleanBus(busFrames[(ir - 1) * n + i]) : 0.0f;
        const float cv1=cv1Bus ? cleanBus(busFrames[(cv1Bus-1)*n+i]) : 0.0f;
        const float cv2=cv2Bus ? cleanBus(busFrames[(cv2Bus-1)*n+i]) : 0.0f;
        const float clock=clockBus ? cleanBus(busFrames[(clockBus-1)*n+i]) : 0.0f;
        const float oldL=ol ? cleanBus(busFrames[(ol-1)*n+i]) : 0.0f;
        const float oldR=oright ? cleanBus(busFrames[(oright-1)*n+i]) : 0.0f;
        self->clock.cvSample(clock);updateClockTime(self);
        self->engine.setExternalModulation(cv1*.2f,cv2*.2f,self->wheel);
        float resultL = bypass ? 0.0f : left / reference;
        float resultR = bypass ? 0.0f : right / reference;
        if (self->ready) self->engine.processSample(bypass ? 0.0f : left / reference,
                                                   bypass ? 0.0f : right / reference, resultL, resultR);
        resultL *= reference;
        resultR *= reference;
        if (bypass == 1) { resultL = left; resultR = right; }
        else if (bypass == 2) { resultL += left; resultR += right; }
        const bool mono=ol && ol==oright;
        float targetL=ol ? (mono ? .5f*(resultL+resultR) : resultL)+(replaceL ? 0.0f : oldL) : 0.0f;
        float targetR=oright && !mono ? resultR+(replaceR ? 0.0f : oldR) : 0.0f;
        targetL/=reference;targetR/=reference;
        const bool needsPostMusical=bypass!=0 || !self->ready ||
            (ol && !replaceL) || (oright && !mono && !replaceR);
        if(!self->controls.limiter_enabled || needsPostMusical)
            self->outputLimiter.process(targetL,targetR,self->controls.limiter_enabled);
        else {
            // Active/Replace already passed the core's musical stage. Only the
            // linked guard follows fold-down; keep this post envelope recovering.
            self->outputLimiter.releaseIdle();
            self->outputLimiter.guard(targetL,targetR);
        }
        targetL*=reference;targetR*=reference;
        if(ol) busFrames[(ol-1)*n+i]=cleanBus(targetL);
        if(oright && !mono) busFrames[(oright-1)*n+i]=cleanBus(targetR);
        self->clock.advanceSample();
    }
}

static constexpr _NT_factory factory = [] {
    _NT_factory f = {};
    f.guid = NT_MULTICHAR('E', 'L', 'n', 't');
    f.name = "EchoLabNT";
    f.description = ECHOLAB_BUILD_ID ": Stereo delays, musical presets and CV/MIDI control";
    f.numSpecifications = 1;
    f.specifications = specifications;
    f.calculateRequirements = calculateRequirements;
    f.construct = construct;
    f.parameterChanged = parameterChanged;
    f.step = step;
    f.draw = draw;
    f.midiRealtime = midiRealtime;
    f.midiMessage = midiMessage;
    f.parameterString = parameterString;
    f.tags = kNT_tagEffect | kNT_tagDelay;
    return f;
}();
} // namespace echolab_nt

// Firmware 1.18.0 uses API 13. Keep the standard .text entry from its official
// examples; .text.nt_dram and the corresponding SDK macro were added later.
uintptr_t pluginEntry(_NT_selector selector, uint32_t data) {
    switch (selector) {
    case kNT_selector_version: return kNT_apiVersion13;
    case kNT_selector_numFactories: return 1;
    case kNT_selector_factoryInfo: return data == 0 ? reinterpret_cast<uintptr_t>(&echolab_nt::factory) : 0;
    }
    return 0;
}
