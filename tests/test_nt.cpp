#define ECHOLAB_NT_HOST_TEST
#include "../src/nt/echo_lab.cpp"
#include <vector>
#include <iostream>
#include <cmath>
#include <stdexcept>
#include <limits>
#include <cstring>

extern "C" const _NT_globals NT_globals = {48000, 128, nullptr, 0, 0, 0};
static unsigned pageNotifications=0;
static const char* lastDrawText=nullptr;
static std::vector<_NT_algorithm*> hostAlgorithms;
struct ParameterWrite { _NT_algorithm* algorithm;int parameter,value; };
static std::vector<ParameterWrite> queuedWrites,parameterWrites;
static bool deferParameterWrites=false;
static bool skipUnchangedParameterWrites=false;
static unsigned hostParameterErrors=0;
static constexpr uint32_t hostParameterOffset=7;
extern "C" int32_t NT_algorithmIndex(const _NT_algorithm* algorithm) {
    for(std::size_t i=0;i<hostAlgorithms.size();++i) if(hostAlgorithms[i]==algorithm) return static_cast<int32_t>(i);
    return -1;
}
extern "C" uint32_t NT_parameterOffset() { return hostParameterOffset; }
static void applyParameterWrite(const ParameterWrite& write) {
    if(skipUnchangedParameterWrites && write.algorithm->v[write.parameter]==write.value) return;
    const_cast<int16_t*>(write.algorithm->v)[write.parameter]=static_cast<int16_t>(write.value);
    echolab_nt::parameterChanged(write.algorithm,write.parameter);
}
extern "C" void NT_setParameterFromAudio(uint32_t algorithm,uint32_t parameter,int16_t value) {
    if(algorithm>=hostAlgorithms.size() || !hostAlgorithms[algorithm] || !hostAlgorithms[algorithm]->v ||
       parameter<hostParameterOffset || parameter-hostParameterOffset>=echolab_nt::ParameterCount) {
        ++hostParameterErrors;return;
    }
    const ParameterWrite write{hostAlgorithms[algorithm],static_cast<int>(parameter-hostParameterOffset),value};
    parameterWrites.push_back(write);
    if(deferParameterWrites) queuedWrites.push_back(write);else applyParameterWrite(write);
}
static void flushParameterWrites(bool reversed=false) {
    for(int pass=0;!queuedWrites.empty() && pass<16;++pass) {
        const auto writes=queuedWrites;queuedWrites.clear();
        if(reversed) for(auto write=writes.rbegin();write!=writes.rend();++write) applyParameterWrite(*write);
        else for(const auto& write:writes) applyParameterWrite(write);
    }
    if(!queuedWrites.empty()) throw std::runtime_error("host setter callbacks did not settle");
}
extern "C" void NT_updateParameterPages(uint32_t) { ++pageNotifications; }
extern "C" void NT_drawText(int,int,const char* text,int,_NT_textAlignment,_NT_textSize) { lastDrawText=text; }
using namespace echolab_nt;

static void check(bool ok, const char* text) { if (!ok) throw std::runtime_error(text); }
static void near(float a, float b, const char* text) { check(std::fabs(a-b) < 2e-6f, text); }
static void near(float a, float b, float tolerance, const char* text) { check(std::isfinite(a) && std::fabs(a-b)<=tolerance,text); }
struct Instance {
    _NT_algorithmRequirements req{};
    std::vector<uint8_t> sram, dram;
    std::vector<int16_t> values;
    _NT_algorithm* self;
    explicit Instance(int32_t seconds = 1, bool limiterEnabled = false,int legacyVoice=0,int productVersion=0,bool reverseCallbacks=false,
                      bool freeClock=true,int loadedPreset=0,bool productDefaults=false,bool completeInitialCallbacks=true) {
        calculateRequirements(req, &seconds);
        sram.resize(req.sram + 2, 0xad); dram.resize(req.dram + 2, 0xad);
        _NT_algorithmMemoryPtrs ptrs{sram.data()+1, dram.data()+1, nullptr, nullptr};
        self = construct(ptrs, req, &seconds);
        hostAlgorithms.push_back(self);
        for (int p=0;p<ParameterCount;++p) values.push_back((productDefaults ? parameters[p] : baseParameters[p]).def);
        if(!productDefaults) {
            // Explicit legacy Digital/Custom fixtures exercise independent DSP
            // contracts; product default behavior has its own complete test.
            values[LimiterOn]=limiterEnabled ? 1 : 0;
            values[Voicing]=static_cast<int16_t>(legacyVoice);values[Version]=static_cast<int16_t>(productVersion);
            values[SyncMode]=freeClock ? 0 : parameters[SyncMode].def;
            values[FactoryPreset]=static_cast<int16_t>(loadedPreset);
        }
        self->v = values.data();
        if(completeInitialCallbacks) initialise(reverseCallbacks);
    }
    ~Instance() { const auto index=NT_algorithmIndex(self);if(index>=0) hostAlgorithms[static_cast<std::size_t>(index)]=nullptr; }
    void initialise(bool reversed=false) {
        const auto* state=static_cast<const Algorithm*>(self);
        for(int i=0;i<ParameterCount;++i) {
            const int p=reversed ? ParameterCount-1-i : i;
            if(!(state->initialParameters&(std::uint64_t(1)<<p))) parameterChanged(self,p);
        }
    }
    void set(int p, int v) { values[p] = static_cast<int16_t>(v); parameterChanged(self, p); }
};
static void routing() {
    for (int mode = 0; mode <= 1; ++mode) {
        Instance instance;
        instance.set(Bypass, 1);
        instance.set(InputL, 1); instance.set(InputR, 2);
        // Cross aliases: reading R must happen before writing L to R's bus.
        instance.set(OutputL, 2); instance.set(OutputR, 1);
        instance.set(OutputModeL, mode); instance.set(OutputModeR, mode);
        for (int n : {4, 20, 128}) {
            std::vector<float> b(kNT_lastBus*n, 0.0f);
            for (int i=0;i<n;++i) { b[i]=.01f*float(i+1); b[n+i]=-.01f*float(i+2); }
            step(instance.self, b.data(), n/4);
            for (int i=0;i<n;++i) {
                near(b[i], mode ? -.01f*float(i+2) : -.01f, "cross alias R output");
                near(b[n+i], mode ? .01f*float(i+1) : -.01f, "cross alias L output");
            }
        }
    }
    Instance mono;
    mono.set(Bypass, 1); mono.set(OutputL, kNT_lastBus); mono.set(OutputR, kNT_lastBus);
    mono.set(OutputModeL, 1); mono.set(OutputModeR, 0);
    std::vector<float> b(kNT_lastBus*4, 7.0f);
    for (int i=0;i<4;++i) {b[i]=2; b[4+i]=4;}
    step(mono.self,b.data(),1);
    for (int i=0;i<4;++i) near(b[(kNT_lastBus-1)*4+i],3,"same bus fold-down L mode wins");
    mono.set(InputL, 0); mono.set(InputR, 0);
    step(mono.self,b.data(),1);
    near(b[(kNT_lastBus-1)*4],0,"None inputs silent");
    mono.set(OutputL, 0); mono.set(OutputR, 0);
    const auto snapshot = b;
    step(mono.self,b.data(),1);
    check(b == snapshot,"None outputs untouched");
    mono.set(OutputL, -1); mono.set(OutputR, 100);
    step(mono.self,b.data(),1);
    check(b == snapshot,"invalid buses guarded");
    step(mono.self,b.data(),0); step(mono.self,b.data(),33);
    check(b == snapshot,"invalid block rejected");
}
static void sharedCoreAndInstances(int model) {
    Instance instance;
    instance.set(Model, model);
    near(static_cast<Algorithm*>(instance.self)->engine.parameters().feedback, .28f,
         "the explicit analytic fixture retains its 28 percent feedback setting");
    near(static_cast<Algorithm*>(instance.self)->engine.parameters().mix, .39f,
         "the explicit analytic fixture retains its 39 percent mix setting");
    instance.set(OutputModeL, 1); instance.set(OutputModeR, 1);
    instance.set(Time, 10); instance.set(Mix, 800); instance.set(Feedback, 650);
    instance.set(Tone, 1800); instance.set(Drive, 350);
    check(static_cast<Algorithm*>(instance.self)->engine.parameters().model == static_cast<echolab::Model>(model),
          "every NT model selector reaches the intended core model");
    echolab::Engine expected;
    std::vector<float> memory(echolab::Engine::requiredSamples(48000,1000));
    check(expected.prepare(48000,1000,memory.data(),memory.size()),"reference prepare");
    expected.setParameters(static_cast<Algorithm*>(instance.self)->controls);
    for (int block=0;block<150;++block) {
        if(block==50 || block==100) {
            instance.set(ModulationEnabled,block==100 ? 1 : 0);
            expected.setParameters(static_cast<Algorithm*>(instance.self)->controls);
        }
        const int n = block%2 ? 20 : 128;
        std::vector<float> buses(kNT_lastBus*n,0.0f);
        for (int i=0;i<n;++i) {
            const float l=std::sin(float(block*128+i)*.017f)*.4f;
            const float r=std::cos(float(block*128+i)*.023f)*.3f;
            buses[i]=l*8; buses[n+i]=r*8;
        }
        auto input=buses;
        step(instance.self,buses.data(),n/4);
        for (int i=0;i<n;++i) {
            float l,r; expected.processSample(input[i]/8,input[n+i]/8,l,r);
            near(buses[12*n+i]/8,l,"shared core left"); near(buses[13*n+i]/8,r,"shared core right");
        }
    }
    Instance silent;
    silent.set(Model, model);
    silent.set(Mix,1000); silent.set(OutputModeL,1); silent.set(OutputModeR,1);
    std::vector<float> b(kNT_lastBus*128,0.0f);
    for (int i=0;i<30;++i) step(silent.self,b.data(),32);
    for (float v:b) near(v,0,"second instance isolated");
    check(instance.dram.front()==0xad && instance.sram.front()==0xad &&
          instance.dram.back()==0xad && instance.sram.back()==0xad,"unaligned allocation boundary canaries");
}

static void configureNewModel(Instance& instance, int model, int time = 1) {
    instance.set(Model, model); instance.set(Time, time);
    instance.set(Mix, 1000); instance.set(Feedback, 500);
    instance.set(Tone, 1700); instance.set(Drive, 200);
    instance.set(FeedbackLPHz,0);instance.set(FeedbackHPHz,0);instance.set(Drift,0);
    instance.set(OutputModeL, 1); instance.set(OutputModeR, 1);
    // Fixture setup, outside step: establish an exact delay and wet mix.
    static_cast<Algorithm*>(instance.self)->engine.reset();
}

static void statefulModelAliases() {
    // Compare stateful routing to separate-bus rendering, including cross-channel
    // feedback. Merely passing dry samples cannot detect corrupted delay writes.
    for (int model : {3, 4, 13}) for (int route : {0, 1, 2}) for (int mode : {0, 1}) {
        Instance separate, aliased;
        configureNewModel(separate, model); configureNewModel(aliased, model);
        const int outputL = route == 0 ? 1 : 2;
        const int outputR = route == 1 ? 1 : 2;
        aliased.set(OutputL, outputL); aliased.set(OutputR, outputR);
        aliased.set(OutputModeL, mode); aliased.set(OutputModeR, route == 2 ? 1-mode : mode);
        size_t cursor = 0;
        float delayedPeak = 0.0f;
        for (int block = 0; block < 24; ++block) {
            const int n = block % 3 == 0 ? 128 : (block % 3 == 1 ? 20 : 4);
            std::vector<float> reference(kNT_lastBus*n, 0.0f);
            for (int i=0;i<n;++i) {
                const size_t frame = cursor + static_cast<size_t>(i);
                reference[i] = frame == 0 ? 4.0f : (frame == 17 ? 2.0f : 0.0f);
                reference[n+i] = frame == 3 ? -2.0f : (frame == 37 ? 1.0f : 0.0f);
            }
            auto actual = reference;
            const auto original = reference;
            step(separate.self,reference.data(),n/4);
            step(aliased.self,actual.data(),n/4);
            for (int i=0;i<n;++i) {
                const float l = reference[12*n+i], r = reference[13*n+i];
                if (outputL == outputR) {
                    const float expected = 0.5f*(l+r) + (mode ? 0.0f : original[(outputL-1)*n+i]);
                    near(actual[(outputL-1)*n+i],expected,"stateful same-bus mono alias/Add/Replace");
                } else {
                    near(actual[(outputL-1)*n+i],l+(mode ? 0.0f : original[(outputL-1)*n+i]),
                         "stateful left alias/Add/Replace");
                    near(actual[(outputR-1)*n+i],r+(mode ? 0.0f : original[(outputR-1)*n+i]),
                         "stateful right alias/Add/Replace");
                }
                if (cursor+static_cast<size_t>(i) >= 48)
                    delayedPeak = std::max(delayedPeak,std::max(std::fabs(l),std::fabs(r)));
            }
            cursor += static_cast<size_t>(n);
        }
        check(delayedPeak > 0.01f,"alias fixture exercises a stored delay tail");
    }
}

static void analyticalNewModelRouting() {
    // synthetic_fixture: both inputs carry an identical half-scale impulse. The
    // provisional mono-excitation ping-pong emits L@T, R@2T, L@3T, etc.
    Instance pingpong;
    configureNewModel(pingpong,3);
    for (int block=0;block<88;++block) {
        std::vector<float> b(kNT_lastBus*4,0.0f);
        if (block==0) { b[0]=4; b[4]=4; }
        step(pingpong.self,b.data(),1);
        for (int i=0;i<4;++i) {
            const int frame=block*4+i;
            const int repeat=frame/48;
            const float gain=frame>0 && frame%48==0 ? 4.0f*std::pow(0.5f,float(repeat-1)) : 0.0f;
            near(b[12*4+i],repeat%2==1 ? gain : 0.0f,"ping-pong independent left repeat time/gain");
            near(b[13*4+i],repeat%2==0 ? gain : 0.0f,"ping-pong independent right repeat time/gain");
        }
    }
    // synthetic_fixture: with feedback/drive zero, each of the three equally
    // weighted taps carries a shifted one-pole impulse, independently of R.
    Instance multihead;
    configureNewModel(multihead,4,3);
    multihead.set(Feedback,0); multihead.set(Drive,0); multihead.set(Tone,4500);
    static_cast<Algorithm*>(multihead.self)->engine.reset();
    constexpr double pi=3.14159265358979323846;
    const double pole=std::exp(-2.0*pi*4500.0/48000.0);
    for (int block=0;block<60;++block) {
        std::vector<float> b(kNT_lastBus*4,0.0f);
        if (block==0) b[0]=8;
        step(multihead.self,b.data(),1);
        for (int i=0;i<4;++i) {
            const int frame=block*4+i;
            double expected=0;
            for (int tap : {48,96,144}) if(frame>=tap)
                expected += 8.0*(1.0-pole)*std::pow(pole,frame-tap)/3.0;
            near(b[12*4+i],static_cast<float>(expected),"multi-head independent tap timing/filter/gain");
            near(b[13*4+i],0,"multi-head does not cross-feed unexcited R");
        }
    }
}
static void bypassAndNonfinite() {
    for (int model=0;model<echolab::modelCount;++model) for (int mode : {0, 1, 2}) {
        Instance i;
        i.set(Model, model);
        i.set(Bypass, mode); i.set(OutputModeL, 1); i.set(OutputModeR, 1);
        std::vector<float> b(kNT_lastBus*4,0.0f);
        b[0]=std::numeric_limits<float>::infinity();
        b[4]=std::numeric_limits<float>::quiet_NaN();
        step(i.self,b.data(),1);
        check(std::isfinite(b[12*4]) && std::isfinite(b[13*4]),"nonfinite NT input sanitized");
    }
    Instance fallback;
    auto* a=static_cast<Algorithm*>(fallback.self);
    a->ready=false;
    fallback.set(Bypass,2); fallback.set(OutputModeL,1); fallback.set(OutputModeR,1);
    std::vector<float> b(kNT_lastBus*4,0.0f);
    b[0]=2; b[4]=4;
    step(fallback.self,b.data(),1);
    near(b[12*4],2,"unready tails preserves single dry L");
    near(b[13*4],4,"unready tails preserves single dry R");
    // A pre-existing delay tail is audible in Tails and hidden in Dry.
    Instance tails, dry;
    for (Instance* i : {&tails,&dry}) {
        i->set(Time,1); i->set(Mix,1000); i->set(Feedback,0);
        auto* state=static_cast<Algorithm*>(i->self);
        state->engine.reset(); // Test setup only, outside audio callback.
        i->set(OutputModeL,1); i->set(OutputModeR,1);
    }
    b.assign(kNT_lastBus*4,0.0f); b[0]=4;
    auto other=b;
    step(tails.self,b.data(),1); step(dry.self,other.data(),1);
    tails.set(Bypass,2); dry.set(Bypass,1);
    float peak=0;
    for(int n=0;n<20;++n) {
        b.assign(kNT_lastBus*4,0.0f); other=b;
        step(tails.self,b.data(),1); step(dry.self,other.data(),1);
        for(int k=0;k<4;++k) {peak=std::max(peak,std::fabs(b[12*4+k])); near(other[12*4+k],0,"Dry hides tail");}
    }
    check(peak>3.9f,"Tails emits existing wet tail");
}
static void appendedControls() {
    Instance i;
    i.set(Model,5); i.set(BitDepth,12); i.set(ModRate,125); i.set(ModDepth,375);
    i.set(DuckThreshold,-300); i.set(DuckAmount,1000); i.set(Attack,12); i.set(Release,200);
    i.set(Swell,450); i.set(StereoRatio,175); i.set(LoopAction,2);
    i.set(FeedbackLPHz,8900);i.set(Drift,375);
    i.set(FeedbackHPHz,125);i.set(LoResEndHz,900);i.set(LoResDecayMs,2300);i.set(LoResEnvelopeAmount,650);
    i.set(RackSampleRateHz,12000);
    i.set(LoResCutoffHz,1350);i.set(Voicing,3);i.set(ModulationEnabled,0);i.set(Mod1Destination,8);
    i.set(ExciterHighpassOn,1);i.set(WetToneOn,1);
    const auto& p=static_cast<Algorithm*>(i.self)->engine.parameters();
    check(p.bit_depth==12 && p.loop_action==2,"integer controls reach shared core");
    near(p.mod_rate_hz,1.25f,"modulation rate hundredths"); near(p.mod_depth_ms,3.75f,"modulation depth hundredths");
    near(p.duck_threshold_db,-30,"duck threshold tenths dB"); near(p.duck_amount,1,"duck amount percent");
    near(p.attack_ms,12,"attack ms"); near(p.release_ms,200,"release ms"); near(p.swell_ms,450,"swell ms");
    near(p.stereo_ratio,1.75f,"stereo ratio hundredths");
    near(p.feedback_lowpass_hz,8900,"feedback low-pass Hz reaches core");
    near(p.drift_amount,.375f,"slow drift uses permille amount");
    near(p.feedback_highpass_hz,125,"feedback high-pass Hz reaches core");
    near(p.lores_rate_end_hz,900,"Lo Res endpoint Hz reaches core");
    near(p.lores_rate_decay_ms,2300,"Lo Res rate decay uses milliseconds");
    near(p.lores_rate_envelope_amount,.65f,"Lo Res envelope amount uses permille");
    near(p.digital_rack_rate_hz,12000,"Rack sample rate has its own Hz field");
    near(p.lores_cutoff_hz,1350,"Lo Res wet low-pass has its own Hz field");
    check(p.voicing==3 && !p.modulation_enabled && p.mod1_destination==8,"voicing, modulation toggle and appended Fe destination reach core");
    check(p.exciter_highpass_enabled && p.wet_tone_enabled,"optional Exciter return high-pass and wet Tone flags reach the shared core");
}
static bool pageHas(const _NT_algorithm* self,int parameter) {
    for(uint32_t page=0;page<self->parameterPages->numPages;++page) {
        const auto& p=self->parameterPages->pages[page];
        for(unsigned i=0;i<p.numParams;++i) if(p.params[i]==parameter) return true;
    }
    return false;
}
static void dynamicPagesAndControls() {
    Instance i;
    check(std::strcmp(i.self->parameterPages->pages[0].name,"Delay")==0 &&
          i.self->parameterPages->pages[0].params[0]==Time,"Delay is the first page and starts with manual Time");
    check(!pageHas(i.self,BitDepth) && !pageHas(i.self,LoopAction),"Digital pages hide unrelated model controls");
    check(!pageHas(i.self,FeedbackLPHz) && !pageHas(i.self,FeedbackHPHz) && !pageHas(i.self,Drift),
          "Digital Auto A05 hides B06 regeneration filters it does not use");
    check(!pageHas(i.self,Voicing) && pageHas(i.self,Version) && pageHas(i.self,ModulationEnabled),
          "the product Version selector replaces the hidden legacy voicing control");
    bool versionOnColor=false;
    for(uint32_t page=0;page<i.self->parameterPages->numPages;++page) {
        const auto& p=i.self->parameterPages->pages[page];
        for(unsigned id=0;id<p.numParams;++id) if(p.params[id]==Version) versionOnColor=std::strcmp(p.name,"Color")==0;
    }
    check(versionOnColor && std::strcmp(parameters[Version].name,"Version")==0,"Version is located on the Color page");
    check(!pageHas(i.self,LoResEndHz) && !pageHas(i.self,LoResDecayMs),"rate envelope is absent from Digital pages");
    const auto before=pageNotifications;
    i.set(Model,5);
    check(pageNotifications==before+1 && pageHas(i.self,BitDepth) && pageHas(i.self,SampleRateHz),
          "Lo Res model publishes its own parameter pages through the host callback");
    check(pageHas(i.self,LoResEndHz) && pageHas(i.self,LoResDecayMs) && pageHas(i.self,LoResEnvelopeAmount),
          "Lo Res exposes its sample-rate decay envelope");
    i.set(Mix,500);check(pageNotifications==before+1,"ordinary control changes do not rebuild pages");
    i.set(Model,16);check(pageHas(i.self,LoopAction) && !pageHas(i.self,BitDepth),"Looper has its action page");
    check(pageHas(i.self,FeedbackLPHz) && pageHas(i.self,Drift),"Looper exposes its Overdub feedback colour");
    i.set(Model,17);check(pageHas(i.self,ExciterAmount) && pageHas(i.self,ExciterTuneHz),"Exciter has its own controls");
    i.set(Model,18);check(pageHas(i.self,Tone) && pageHas(i.self,Drive) && pageHas(i.self,ModRate) && !pageHas(i.self,ModDepth),
                          "Sweep Up exposes colour and one-shot sweep duration controls");
    const int automaticVoices[]={2,1,3,3,3,3,3,1,3,1,1,3,2,3,1,3,3,1,3,1};
    for(int voice=0;voice<=3;++voice) for(int model=0;model<echolab::modelCount;++model) {
        i.set(Voicing,voice);
        i.set(Model,model);
        const int resolved=model==19 ? 1 : (model==2 ? 3 : (voice ? voice : automaticVoices[model]));
        check(echolab::resolvedVoicing(static_cast<echolab::Model>(model),voice)==resolved,"published automatic voice selection and fixed Tube A04");
        check(!pageHas(i.self,Voicing) && pageHas(i.self,Version)==hasVersion(model),
              "legacy voicing stays hidden and Version appears where A/B have distinct native sounds");
        check(pageHas(i.self,FeedbackLPHz)==(resolved==3 || (resolved==2 && model!=0)) &&
              pageHas(i.self,FeedbackHPHz)==(resolved==3) && pageHas(i.self,Drift)==(resolved!=1 && model!=0),
              "feedback colour pages follow the actual model scope");
        check(pageHas(i.self,LoResEnvelopeAmount)==(model==5 && resolved==3) && pageHas(i.self,LoResCutoffHz)==(model==5 && resolved==3),
              "Lo Res envelope and wet cutoff are scoped to its B06 voice");
        check(pageHas(i.self,RackSampleRateHz)==(model==13 && resolved==3),"Rack internal rate is scoped to its B06 voice");
        check(i.self->parameterPages->numPages<=ARRAY_SIZE(static_cast<Algorithm*>(i.self)->pageStorage),"dynamic page storage capacity");
        for(uint32_t page=0;page<i.self->parameterPages->numPages;++page) {
            const auto& p=i.self->parameterPages->pages[page];
            for(unsigned id=0;id<p.numParams;++id) check(p.params[id]<ParameterCount,"all page indices are valid and stable");
        }
    }
    i.set(Feedback,2000);i.set(SampleRateHz,0);i.set(ExciterAmount,750);i.set(ExciterTuneHz,6000);i.set(TapPattern,7);
    i.set(Mod1Source,3);i.set(Mod1Destination,5);i.set(Mod1Amount,-500);
    i.set(Mod2Source,5);i.set(Mod2Destination,4);i.set(Mod2Amount,250);
    const auto& p=static_cast<Algorithm*>(i.self)->engine.parameters();
    near(p.feedback,2.0f,"200 percent feedback reaches the core");near(p.sample_rate_hz,0,"zero sample rate requests full Fs");
    near(p.exciter_amount,.75f,"Exciter amount permille");near(p.exciter_tune_hz,6000,"Exciter tune Hz");
    check(p.tap_pattern==7 && p.mod1_source==3 && p.mod1_destination==5 && p.mod2_source==5,"tap and modulation route IDs");
    near(p.mod1_amount,-.5f,"negative modulation amount");near(p.mod2_amount,.25f,"positive modulation amount");
    char text[kNT_parameterStringSize]{};
    i.set(Model,1);i.set(Voicing,0);
    check(parameterString(i.self,Voicing,0,text)>0 && std::strcmp(text,"Auto: A04")==0,"automatic voice display names the effective per-model sound");
    const auto beforeVoice=pageNotifications;
    i.set(Voicing,3);
    check(pageNotifications==beforeVoice+1 && pageHas(i.self,FeedbackHPHz),"changing effective voice refreshes model pages without changing the model");
    i.set(Model,19);
    check(parameterString(i.self,Voicing,3,text)>0 && std::strcmp(text,"Fixed: A04")==0,"Tube A04 exposes its fixed voice when an override requests another");
    i.set(Model,2);
    check(parameterString(i.self,Voicing,1,text)>0 && std::strcmp(text,"Fixed: B06")==0,"Tube B06 remains fixed when an override requests A04");
    check(parameterString(i.self,SampleRateHz,0,text)>0 && std::strcmp(text,"Full Fs")==0,"Full Fs is explicit in parameter display");
    check(parameterString(i.self,RackSampleRateHz,0,text)>0 && std::strcmp(text,"Full Fs")==0,"Rack rate bypass displays Full Fs");
    check(parameterString(i.self,RackSampleRateHz,16000,text)>0 && std::strcmp(text,"16000 Hz")==0,"Rack displays its actual internal rate");
    check(parameterString(i.self,RackSampleRateHz,1,text)>0 && std::strcmp(text,"1000 Hz")==0,"Rack subminimum preset rate displays its effective lower bound");
    check(parameterString(i.self,FeedbackLPHz,0,text)>0 && std::strcmp(text,"Off")==0,"feedback low-pass diagnostic bypass is explicit");
    check(parameterString(i.self,FeedbackHPHz,0,text)>0 && std::strcmp(text,"Off")==0,"feedback high-pass bypass is explicit");
    check(parameterString(i.self,FeedbackHPHz,80,text)>0 && std::strcmp(text,"80 Hz")==0,"feedback high-pass displays its effective frequency");
    check(parameterString(i.self,LoResCutoffHz,0,text)>0 && std::strcmp(text,"Off")==0,"Lo Res wet low-pass bypass is explicit");
    check(parameterString(i.self,LoResCutoffHz,2000,text)>0 && std::strcmp(text,"2000 Hz")==0,"Lo Res wet low-pass frequency is explicit");
    i.set(Model,13);
    check(parameterString(i.self,FeedbackLPHz,12000,text)>0 && std::strcmp(text,"6000 Hz")==0,
          "Digital Rack displays its actual lower feedback cutoff");
    i.set(Voicing,2);
    check(parameterString(i.self,FeedbackLPHz,12000,text)>0 && std::strcmp(text,"7800 Hz")==0 && !pageHas(i.self,RackSampleRateHz),
          "Rack A05 keeps its historical cutoff and full-Fs path");
    i.set(Voicing,3);
    i.set(Model,1);
    check(parameterString(i.self,FeedbackLPHz,12000,text)>0 && std::strcmp(text,"12000 Hz")==0,
          "other models display nominal feedback cutoff below the sample-rate limit");
    draw(i.self);check(lastDrawText && std::strcmp(lastDrawText,"Free Time")==0,"draw reports clock state");
}
static void finalBusLimiter() {
    for(bool enabled : {false,true}) for(int bypass : {0,1,2}) for(bool same : {false,true}) for(int mode : {0,1}) {
        Instance i(1,enabled);i.set(Bypass,bypass);i.set(Mix,0);
        i.set(OutputL,13);i.set(OutputR,same ? 13 : 14);
        i.set(OutputModeL,mode);i.set(OutputModeR,mode);
        i.set(LimiterCeiling,500);
        std::vector<float> b(kNT_lastBus*4,0.0f);
        for(int k=0;k<4;++k) { b[k]=80;b[4+k]=-20;b[12*4+k]=16;b[13*4+k]=8; }
        step(i.self,b.data(),1);
        for(int k=0;k<4;++k) {
            check(std::fabs(b[12*4+k])<=4.0f+1e-6f,"permanent guard covers Limiter Off/On, Active/Dry/Tails, Add/Replace/fold-down");
            if(!same) check(std::fabs(b[13*4+k])<=4.0f+1e-6f,"permanent guard bounds the right target bus");
        }
    }
    Instance linked(1,true);linked.set(Bypass,1);linked.set(OutputModeL,1);linked.set(OutputModeR,1);linked.set(LimiterCeiling,500);
    std::vector<float> b(kNT_lastBus*4,0.0f);b[0]=16;b[4]=4;
    step(linked.self,b.data(),1);near(b[12*4],4,"limiter instantaneous left attack");near(b[13*4],1,"postmix stereo limiter uses linked gain");
    linked.set(LimiterOn,0);b.assign(kNT_lastBus*4,0.0f);b[0]=16;b[4]=4;
    step(linked.self,b.data(),1);near(b[12*4],4,"Off still guards Dry");near(b[13*4],1,"Off guard preserves stereo image");
    linked.set(LimiterCeiling,950);
    auto* state=static_cast<Algorithm*>(linked.self);
    near(state->controls.limiter_ceiling,.891f,1e-7f,"loaded old ceiling950 is clamped to the NT maximum");
    check(state->engine.parameters().limiter_ceiling<=echolab::safety::maximumCeiling,
          "loaded old ceiling cannot evade system headroom");
}
static void singleMusicalStage() {
    for(int route : {0,1,2}) {
        Instance instance(1,true);
        instance.set(Model,0);instance.set(Mix,0);
        instance.set(OutputModeL,1);instance.set(OutputModeR,route==1 ? 0 : 1);
        if(route==1) instance.set(OutputR,13); // Its ignored R Add must not re-limit L.
        if(route==2) { instance.set(OutputR,0);instance.set(OutputModeR,0); }
        auto* state=static_cast<Algorithm*>(instance.self);
        state->engine.reset();
        echolab::Engine reference;
        std::vector<float> memory(echolab::Engine::requiredSamples(48000,1000));
        check(reference.prepare(48000,1000,memory.data(),memory.size()),"single-stage reference prepares");
        reference.setParameters(state->controls);reference.reset();
        std::vector<float> buses(kNT_lastBus*4,0.0f);
        for(int k=0;k<4;++k) { buses[k]=6.4f;buses[4+k]=3.2f; }
        step(instance.self,buses.data(),1);
        for(int k=0;k<4;++k) {
            float l=0,r=0;reference.processSample(.8f,.4f,l,r);
            check(l<.8f && l>.7f,"fixture reaches the progressive knee");
            near(buses[12*4+k],8*(route==1 ? .5f*(l+r) : l),
                 "Active Replace has no second musical reduction");
            if(route==0) near(buses[13*4+k],8*r,"stereo Replace retains one linked musical stage");
        }
        near(state->outputLimiter.gain(),1,0,"unused postmix musical envelope remains at unity");
    }
}
static void clockAndCvAliasing() {
    Instance i;i.set(Bypass,1);i.set(OutputModeL,1);i.set(OutputModeR,1);i.set(ClockBus,13);i.set(SyncMode,2);i.set(ClockPpqn,2);
    i.set(CV1Bus,13);i.set(CV2Bus,14);
    auto* state=static_cast<Algorithm*>(i.self);
    // Clock/CV on output buses must be sampled before Dry overwrites them.
    for(int block=0;block<1501;++block) {
        std::vector<float> b(kNT_lastBus*4,0.0f);
        b[12*4]=block%1500==0 ? 5.0f : 0.0f;
        step(i.self,b.data(),1);
    }
    check(state->clock.locked(),"CV clock input aliases a written output bus without losing edges");
    near(state->controls.delay_ms,500,1e-4f,"CV4PPQN updates shared delay controls");
    i.set(Drive,900);check(state->clock.locked(),"Drive changes preserve clock lock");
    i.set(ClockBus,15);check(!state->clock.locked(),"clock bus changes require reacquisition");
    i.set(SyncMode,3);i.set(TempoBpm,120);i.set(Division,6);near(state->controls.delay_ms,375,1e-4f,"internal BPM dotted division");
    i.set(SyncMode,1);midiRealtime(i.self,0xfa);midiRealtime(i.self,0xf8);
    for(int pulse=0;pulse<24;++pulse) {
        for(int block=0;block<250;++block) { std::vector<float> b(kNT_lastBus*4,0.0f);step(i.self,b.data(),1); }
        midiRealtime(i.self,0xf8);
    }
    std::vector<float> last(kNT_lastBus*4,0.0f);step(i.self,last.data(),1);
    near(state->controls.delay_ms,375,1e-4f,"MIDI 24PPQN timestamps use the rendered frame counter");
    midiRealtime(i.self,0xfc);step(i.self,last.data(),1);near(state->controls.delay_ms,350,0,"MIDI Stop falls back to manual Time");
    midiMessage(i.self,0xb7,1,127);near(state->wheel,1,0,"CC1 modulation wheel accepts every MIDI channel");
    midiMessage(i.self,0xb1,2,0);near(state->wheel,1,0,"other CCs leave the wheel unchanged");
}
static void timingPageFollowsClock() {
    Instance i(3);
    auto* state=static_cast<Algorithm*>(i.self);
    const auto firstTimingParameter=[&]() { return i.self->parameterPages->pages[0].params[0]; };
    i.set(Time,731);
    check(firstTimingParameter()==Time && parameters[Time].unit==kNT_unitMs,
          "Free mode exposes the editable millisecond Time on the first page");
    const auto beforeInternal=pageNotifications;
    i.set(SyncMode,3);
    check(firstTimingParameter()==Division && parameters[Division].unit==kNT_unitEnum &&
          parameters[Division].enumStrings==divisionNames && pageNotifications==beforeInternal+1,
          "Internal BPM replaces manual Time with the real editable musical Division");
    near(state->controls.delay_ms,500,1e-5f,"internal tempo initially uses quarter notes");
    i.set(Division,6);
    near(state->controls.delay_ms,375,1e-5f,"editing the first-page division changes the synchronized delay");
    check(i.values[Time]==731,"musical editing preserves the saved manual Time");
    i.set(TempoBpm,60);
    near(state->controls.delay_ms,750,1e-5f,"tempo edits retain the chosen musical division");
    i.set(SyncMode,1);
    check(firstTimingParameter()==Time,"an external clock that has not locked exposes its actual manual fallback");
    near(state->controls.delay_ms,731,0,"MIDI wait uses the saved manual delay");
    std::vector<float> buses(kNT_lastBus*4,0.0f);
    midiRealtime(i.self,0xfa);midiRealtime(i.self,0xf8);
    for(int block=0;block<250;++block) step(i.self,buses.data(),1);
    const auto beforeLock=pageNotifications;
    midiRealtime(i.self,0xf8);step(i.self,buses.data(),1);
    check(state->clock.locked() && firstTimingParameter()==Division && pageNotifications==beforeLock+1,
          "external lock refreshes the first page once and exposes musical editing");
    near(state->controls.delay_ms,375,1e-5f,"locked MIDI uses the same selected dotted division");
    i.set(Division,3);
    near(state->controls.delay_ms,250,1e-5f,"the synchronized first-page edit reaches the clock calculation");
    const auto whileLocked=pageNotifications;
    for(int block=0;block<100;++block) step(i.self,buses.data(),1);
    check(pageNotifications==whileLocked,"stable clock rendering does not repeatedly rebuild pages");
    for(int block=0;block<24001;++block) step(i.self,buses.data(),1);
    check(!state->clock.locked() && firstTimingParameter()==Time && pageNotifications==whileLocked+1,
          "clock timeout restores the editable manual Time and refreshes pages once");
    near(state->controls.delay_ms,731,0,"timeout restores the saved milliseconds rather than the last synchronized delay");
    midiRealtime(i.self,0xf8);
    for(int block=0;block<250;++block) step(i.self,buses.data(),1);
    midiRealtime(i.self,0xf8);step(i.self,buses.data(),1);
    check(firstTimingParameter()==Division,"a returning MIDI clock restores musical editing");
    midiRealtime(i.self,0xfc);step(i.self,buses.data(),1);
    check(firstTimingParameter()==Time,"MIDI Stop exposes manual fallback on the first page");
    i.set(SyncMode,2);i.set(ClockBus,3);i.set(ClockPpqn,0);
    buses[8]=5;step(i.self,buses.data(),1);buses[8]=0;
    for(int block=0;block<5999;++block) step(i.self,buses.data(),1);
    buses[8]=5;step(i.self,buses.data(),1);
    check(state->clock.locked() && firstTimingParameter()==Division,"CV clock lock also exposes first-page musical editing");
    i.set(ClockBus,4);
    check(!state->clock.locked() && firstTimingParameter()==Time,"changing the clock bus immediately restores manual fallback");
}
static void versionMigrationAndSelection() {
    for(bool reverseCallbacks : {false,true}) for(bool deferred : {false,true}) for(int saved=0;saved<=3;++saved) {
        deferParameterWrites=deferred;parameterWrites.clear();queuedWrites.clear();
        Instance i(1,false,saved,0,reverseCallbacks);
        auto* state=static_cast<Algorithm*>(i.self);
        const int expected=saved ? saved : 2; // Digital's native A sound is A05.
        check(echolab::resolvedVoicing(state->controls.model,state->controls.voicing)==expected,
              "old explicit and native voices survive either initialization callback order");
        std::vector<float> buses(kNT_lastBus*4,0.0f);step(i.self,buses.data(),1);
        check(i.values[Voicing]==saved,"first render does not rewrite an old saved voicing");
        check(echolab::resolvedVoicing(state->controls.model,state->controls.voicing)==expected,
              "first render preserves the old sound before any product Version edit");
        char text[kNT_parameterStringSize]{};
        check(parameterString(i.self,Version,i.values[Version],text)==1 && text[0]==(saved==3 ? 'B' : 'A'),
              "visible Version contains only the effective A or B during migration");
        flushParameterWrites();
        check(i.values[Version]==(saved==3 ? 1 : 0) && i.values[Voicing]==saved,
              "old B normalizes the appended selector through the host without discarding legacy51");
        if(i.values[Version]==1) { i.set(Version,0);flushParameterWrites(); }
        i.set(Version,1);
        check(state->controls.voicing==3,"explicit B changes sound immediately even with deferred host setters");
        flushParameterWrites();check(i.values[Voicing]==3,"explicit B is persisted in the compatible hidden field");
        i.set(Version,0);
        check(state->controls.voicing==0 && echolab::resolvedVoicing(state->controls.model,state->controls.voicing)==2,
              "explicit A restores the current per-model native A05 rather than forcing A04");
        flushParameterWrites();check(i.values[Voicing]==0,"explicit native A survives saving and reopening");
        for(const auto& write:parameterWrites) check(write.parameter==Version || write.parameter==Voicing,
              "migration writes only its two published fields through offset-aware setters");
    }
    deferParameterWrites=false;queuedWrites.clear();
    {
        deferParameterWrites=true;
        Instance late(1,false,3,0,true);
        auto* state=static_cast<Algorithm*>(late.self);
        std::vector<float> buses(kNT_lastBus*4,0.0f);step(late.self,buses.data(),1);
        late.set(Version,0);
        check(state->controls.voicing==0,"a user can select A before an old B normalization acknowledgement arrives");
        flushParameterWrites();
        check(late.values[Version]==0 && late.values[Voicing]==0 && state->controls.voicing==0,
              "late normalization cannot overwrite a newer explicit A selection");
        late.set(Version,1);late.set(Version,0);flushParameterWrites();
        check(state->controls.voicing==0 && late.values[Voicing]==0,"deferred B/A writes settle to the latest explicit sound");
        deferParameterWrites=false;
    }
    for(bool reverseCallbacks : {false,true}) {
        Instance savedB(1,false,1,1,reverseCallbacks);
        check(static_cast<Algorithm*>(savedB.self)->controls.voicing==3,
              "a new preset's explicit B takes precedence regardless of legacy callback order");
    }
    Instance native;
    native.set(Model,5);std::vector<float> buses(kNT_lastBus*4,0.0f);step(native.self,buses.data(),1);
    check(native.values[Version]==0 && !pageHas(native.self,Version),"native B-only models keep the model-selection default without a redundant selector");
    native.set(Model,1);
    check(echolab::resolvedVoicing(echolab::Model::Analog,static_cast<Algorithm*>(native.self)->controls.voicing)==1,
          "switching from native B-only models preserves Analog's native A sound");
}
static void synchronizedStereoNotes() {
    Instance i(3);i.set(Model,15);i.set(SyncMode,3);i.set(TempoBpm,120);i.set(Division,2);
    auto* state=static_cast<Algorithm*>(i.self);
    check(pageHas(i.self,RightDivision) && pageHas(i.self,StereoRatio),"synced Stereo initially preserves Ratio mode");
    i.set(RightDivision,8);
    near(state->controls.delay_ms,500,1e-5f,"left Stereo note follows the common quarter division");
    near(state->controls.stereo_ratio,2.0f/3.0f,1e-6f,"right quarter triplet uses its exact musical ratio");
    check(!pageHas(i.self,StereoRatio),"a selected right note hides the inactive numeric ratio");
    i.set(Division,0);i.set(RightDivision,10);
    near(state->controls.stereo_ratio,1.0f/24.0f,1e-6f,"whole-left and sixteenth-triplet-right support the smallest note ratio");
    i.set(Division,9);i.set(RightDivision,1);
    near(state->controls.stereo_ratio,24,1e-5f,"sixteenth-triplet-left and whole-right support the largest note ratio");
    near(state->engine.parameters().stereo_ratio,24,1e-5f,"the shared core accepts the full musical ratio range");
    i.set(StereoRatio,175);i.set(SyncMode,1);
    check(!pageHas(i.self,RightDivision) && pageHas(i.self,StereoRatio),"waiting for a clock exposes the actual manual stereo ratio");
    near(state->controls.stereo_ratio,1.75f,0,"external wait uses the saved ratio without erasing the selected right note");
    check(i.values[RightDivision]==1,"manual fallback retains the remembered right note");
    i.set(SyncMode,3);check(!pageHas(i.self,StereoRatio),"returning to synchronization restores remembered right-note editing");
    Instance capped(1);capped.set(Model,15);capped.set(SyncMode,3);capped.set(TempoBpm,30);
    capped.set(Division,0);capped.set(RightDivision,10);
    auto* limited=static_cast<Algorithm*>(capped.self);
    near(limited->controls.delay_ms,1000,0,"left note clamps to available delay memory");
    near(limited->controls.stereo_ratio,1.0f/3.0f,1e-6f,"right note is calculated against the clamped left duration");
    capped.set(TempoBpm,60);
    near(limited->controls.delay_ms,1000,0,"tempo change leaves an already clamped left duration unchanged");
    near(limited->controls.stereo_ratio,1.0f/6.0f,1e-6f,"right note still updates when only its duration changes");
    capped.set(RightDivision,1);near(limited->controls.stereo_ratio,1,0,"both long notes clamp independently to memory capacity");
    capped.set(ClockBus,3);capped.set(ClockPpqn,0);capped.set(SyncMode,2);capped.set(Division,2);capped.set(RightDivision,8);
    std::vector<float> buses(kNT_lastBus*4,0.0f);buses[8]=5;step(capped.self,buses.data(),1);buses[8]=0;
    for(int block=0;block<11999;++block) step(capped.self,buses.data(),1);
    buses[8]=5;step(capped.self,buses.data(),1);buses[8]=0;
    near(limited->controls.delay_ms,1000,1e-4f,"CV locks the stereo fixture to a one-second quarter");
    near(limited->controls.stereo_ratio,2.0f/3.0f,1e-6f,"external CV drives the independent right triplet");
    capped.set(Division,0);capped.set(RightDivision,10);
    near(limited->controls.stereo_ratio,1.0f/6.0f,1e-6f,"clamped whole-note left retains an independent right sixteenth triplet");
    for(int block=0;block<5999;++block) step(capped.self,buses.data(),1);
    buses[8]=5;step(capped.self,buses.data(),1);buses[8]=0;
    near(limited->controls.delay_ms,1000,0,"faster external tempo can leave the left note clamped unchanged");
    near(limited->controls.stereo_ratio,1.0f/12.0f,1e-6f,"a clock revision still updates the right note when left delay is unchanged");
    capped.set(Division,2);capped.set(RightDivision,8);
    capped.set(SyncMode,3);capped.set(TempoBpm,120);
    near(limited->controls.delay_ms,500,1e-4f,"Internal BPM ignores stale external CV tempo");
    near(limited->controls.stereo_ratio,2.0f/3.0f,1e-6f,"right note follows the same internal quarter after external sync");
    capped.set(SyncMode,2);buses[8]=5;step(capped.self,buses.data(),1);buses[8]=0;
    for(int block=0;block<5999;++block) step(capped.self,buses.data(),1);
    buses[8]=5;step(capped.self,buses.data(),1);buses[8]=0;
    for(int block=0;block<24001;++block) step(capped.self,buses.data(),1);
    check(!limited->clock.locked() && !pageHas(capped.self,RightDivision),"CV timeout restores manual Stereo controls");
    near(limited->controls.stereo_ratio,1.5f,0,"timeout restores saved manual stereo ratio");
}
static void colouredStereoTiming() {
    for(int model : {1,2,7,9,19}) {
        Instance i(3);i.set(Model,model);
        auto* state=static_cast<Algorithm*>(i.self);
        check(pageHas(i.self,StereoTiming) && !pageHas(i.self,StereoRatio) && !pageHas(i.self,RightDivision),
              "legacy coloured models expose an opt-in Stereo timing switch without changing their old timing");
        check(!state->controls.stereo_timing_enabled,"existing coloured instances retain linked timing by default");
        i.set(SyncMode,3);i.set(TempoBpm,120);i.set(Division,2);i.set(RightDivision,8);i.set(StereoTiming,1);
        check(pageHas(i.self,StereoTiming) && pageHas(i.self,RightDivision) && !pageHas(i.self,StereoRatio),
              "enabled coloured stereo uses the same musical right-note editor as Stereo Delays");
        check(state->controls.stereo_timing_enabled && state->engine.parameters().stereo_timing_enabled,
              "the appended Stereo timing switch reaches the common DSP rather than selecting a different model");
        check(state->controls.model==static_cast<echolab::Model>(model),"independent stereo timing preserves the selected analogue character");
        near(state->controls.delay_ms,500,1e-5f,"coloured left quarter follows 120 BPM");
        near(state->controls.stereo_ratio,2.0f/3.0f,1e-6f,"coloured right quarter triplet uses an exact independent rhythm");
        i.set(StereoRatio,175);i.set(SyncMode,1);
        check(pageHas(i.self,StereoRatio) && !pageHas(i.self,RightDivision),"missing external clock exposes the coloured stereo manual ratio");
        near(state->controls.stereo_ratio,1.75f,0,"coloured stereo preserves the manual fallback ratio");
        check(i.values[RightDivision]==8,"manual fallback retains the remembered right musical note");
        i.set(SyncMode,3);i.set(StereoTiming,0);
        check(!pageHas(i.self,StereoRatio) && !pageHas(i.self,RightDivision) && !state->controls.stereo_timing_enabled,
              "disabling coloured stereo restores its linked model and hides inactive right timing controls");
        i.set(StereoTiming,1);
        near(state->controls.stereo_ratio,2.0f/3.0f,1e-6f,"re-enabling coloured stereo restores the saved musical rhythm");
    }
    Instance capped(1);capped.set(Model,1);capped.set(StereoTiming,1);capped.set(SyncMode,3);
    capped.set(TempoBpm,60);capped.set(Division,4);capped.set(RightDivision,1);
    auto* state=static_cast<Algorithm*>(capped.self);
    near(state->controls.delay_ms,250,0,"a short coloured left note fits a one-second allocation");
    near(state->controls.stereo_ratio,4,0,"coloured right note independently clamps to memory without stretching the left");
    draw(capped.self);check(lastDrawText && std::strstr(lastDrawText,"max delay"),"coloured stereo reports right-only capacity clipping");
    capped.set(StereoTiming,0);draw(capped.self);
    check(lastDrawText && !std::strstr(lastDrawText,"max delay"),"inactive coloured right timing does not report a false capacity warning");
    capped.set(Model,0);capped.set(StereoTiming,1);
    check(!pageHas(capped.self,StereoTiming) && !hasStereoTiming(state),"unsupported models retain the stored switch without exposing a nonfunctional stereo feature");
    capped.set(Model,15);capped.set(StereoTiming,0);
    check(hasStereoTiming(state) && pageHas(capped.self,RightDivision) && !pageHas(capped.self,StereoTiming),
          "the original Stereo Delays model stays independently stereo regardless of the optional coloured-model switch");
}
static void modulationCvAliasing() {
    Instance separate,aliased;
    for(Instance* i : {&separate,&aliased}) {
        i->set(Model,1);i->set(OutputModeL,1);i->set(OutputModeR,1);
        i->set(Mod1Source,3);i->set(Mod1Destination,5);i->set(Mod1Amount,500);
        i->set(Mod2Source,4);i->set(Mod2Destination,4);i->set(Mod2Amount,300);
    }
    separate.set(CV1Bus,15);separate.set(CV2Bus,16);
    aliased.set(CV1Bus,13);aliased.set(CV2Bus,14);
    for(int block=0;block<160;++block) {
        const int n=block%2 ? 20 : 128;
        std::vector<float> a(kNT_lastBus*n,0.0f),b(kNT_lastBus*n,0.0f);
        for(int k=0;k<n;++k) {
            a[k]=b[k]=2.0f*std::sin(float(block*128+k)*.03f);
            a[n+k]=b[n+k]=1.5f*std::cos(float(block*128+k)*.02f);
            a[14*n+k]=b[12*n+k]=(k%3 ? 2.5f : -5.0f);
            a[15*n+k]=b[13*n+k]=(k%5 ? -2.5f : 5.0f);
        }
        step(separate.self,a.data(),n/4);step(aliased.self,b.data(),n/4);
        for(int k=0;k<n;++k) {
            near(a[12*n+k],b[12*n+k],"CV1 snapshot survives output-bus aliasing at audio rate");
            near(a[13*n+k],b[13*n+k],"CV2 snapshot survives output-bus aliasing at audio rate");
        }
    }
}
static void depthCvAndWheelRouting() {
    Instance instance;
    instance.set(Model,9);instance.set(Time,5);instance.set(Mix,1000);instance.set(Feedback,500);
    instance.set(Drive,0);instance.set(Tone,18000);instance.set(Drift,0);
    instance.set(FeedbackLPHz,0);instance.set(FeedbackHPHz,0);
    instance.set(OutputModeL,1);instance.set(OutputModeR,1);
    instance.set(CV1Bus,13); // CV must be read before its output alias is written.
    instance.set(Mod1Source,3);instance.set(Mod1Destination,6);instance.set(Mod1Amount,10);
    instance.set(Mod2Source,5);instance.set(Mod2Destination,6);instance.set(Mod2Amount,30);
    auto* state=static_cast<Algorithm*>(instance.self);
    state->engine.reset();
    echolab::Engine expected;
    std::vector<float> memory(echolab::Engine::requiredSamples(48000,1000));
    check(expected.prepare(48000,1000,memory.data(),memory.size()),"Depth routing reference prepares");
    expected.setParameters(state->controls);expected.reset();
    constexpr int n=128;
    int wheel=0;
    for(int block=0;block<96;++block) {
        if(block==24 || block==64) { wheel=block==24 ? 96 : 0;midiMessage(instance.self,0xb3,1,static_cast<uint8_t>(wheel)); }
        std::vector<float> b(kNT_lastBus*n,0.0f);
        for(int k=0;k<n;++k) {
            const float frame=float(block*n+k);
            b[k]=2.0f*std::sin(frame*.037f);b[n+k]=1.5f*std::cos(frame*.023f);
            b[12*n+k]=5.0f*std::sin(frame*.00017f);
        }
        const auto original=b;
        step(instance.self,b.data(),n/4);
        for(int k=0;k<n;++k) {
            expected.setExternalModulation(original[12*n+k]*.2f,0,wheel/127.0f);
            float l=0,r=0;expected.processSample(original[k]/8,original[n+k]/8,l,r);
            near(b[12*n+k],l*8,"CV to Depth keeps the audio-rate snapshot and shared core scaling");
            near(b[13*n+k],r*8,"wheel to Depth uses CC1 without changing its target to Drive");
        }
        near(state->controls.delay_ms,5,0,"Depth routing leaves manual Time unchanged");
        near(state->controls.drive,0,0,"Depth routing leaves Drive unchanged");
    }
}
static void digitalRackInternalRate() {
    Instance full,reduced,independent;
    for(Instance* i : {&full,&reduced,&independent}) {
        i->set(Model,13);i->set(Time,1);i->set(Mix,1000);i->set(Feedback,0);
        i->set(Drift,0);i->set(FeedbackLPHz,0);i->set(FeedbackHPHz,0);
        i->set(OutputModeL,1);i->set(OutputModeR,1);
    }
    full.set(RackSampleRateHz,0);
    reduced.set(RackSampleRateHz,16000);independent.set(RackSampleRateHz,16000);
    independent.set(SampleRateHz,500); // ID26 remains exclusive to Lo Res.
    for(Instance* i : {&full,&reduced,&independent}) static_cast<Algorithm*>(i->self)->engine.reset();
    near(static_cast<Algorithm*>(full.self)->engine.parameters().digital_rack_rate_hz,0,0,"zero Rack rate selects full Fs");
    near(static_cast<Algorithm*>(reduced.self)->engine.parameters().digital_rack_rate_hz,16000,0,"Rack16kHz reaches core unchanged");
    constexpr int n=128;
    double difference=0;
    std::size_t compared=0;
    const auto quantizedBus=[](float input) {
        constexpr float scale=8388608.0f;
        return static_cast<float>(static_cast<int32_t>((input/8)*scale))/scale*8;
    };
    for(int block=0;block<32;++block) {
        std::vector<float> a(kNT_lastBus*n,0.0f);
        for(int k=0;k<n;++k) {
            const float frame=float(block*n+k);
            a[k]=2*std::sin(frame*.91f);a[n+k]=1.5f*std::cos(frame*.71f);
        }
        auto b=a,c=a;
        step(full.self,a.data(),n/4);step(reduced.self,b.data(),n/4);step(independent.self,c.data(),n/4);
        for(int k=0;k<n;++k) {
            const int frame=block*n+k;
            const float l=frame>=48 ? quantizedBus(2*std::sin(float(frame-48)*.91f)) : 0;
            const float r=frame>=48 ? quantizedBus(1.5f*std::cos(float(frame-48)*.71f)) : 0;
            near(a[12*n+k],l,"Full Fs Rack preserves independent delayed24bit left samples");
            near(a[13*n+k],r,"Full Fs Rack preserves independent delayed24bit right samples");
            near(b[12*n+k],c[12*n+k],"Lo Res rate does not change Rack left internal clock");
            near(b[13*n+k],c[13*n+k],"Lo Res rate does not change Rack right internal clock");
            if(frame>=256) { const double error=a[12*n+k]-b[12*n+k];difference+=error*error;++compared; }
        }
    }
    check(std::sqrt(difference/compared)>.05,"Rack16kHz audibly changes high-frequency content compared with full Fs");
    reduced.set(RackSampleRateHz,1);
    near(static_cast<Algorithm*>(reduced.self)->engine.parameters().digital_rack_rate_hz,1000,0,"old subminimum Rack preset rate is bounded to1kHz");
}
static void loresFeMappings() {
    for(bool fullRate : {false,true}) for(bool enabled : {false,true}) for(int source=1;source<=5;++source) {
        // Restore the inactive Full-Fs route as saved data; live target editing
        // deliberately skips destinations that do not affect the current model.
        Instance instance(1,false,0,0,false,true,0,false,false);
        instance.set(Model,5);instance.set(Voicing,3);instance.set(Time,1);instance.set(Mix,1000);instance.set(Feedback,0);
        instance.set(FeedbackLPHz,0);instance.set(FeedbackHPHz,0);instance.set(Drift,0);instance.set(LoResCutoffHz,0);
        instance.set(LoResEnvelopeAmount,0);instance.set(SampleRateHz,fullRate ? 0 : 8000);instance.set(BitDepth,12);
        instance.set(ModulationEnabled,enabled ? 1 : 0);
        instance.set(OutputModeL,1);instance.set(OutputModeR,1);instance.set(CV1Bus,13);instance.set(CV2Bus,14);
        instance.set(Mod1Source,source);instance.set(Mod1Destination,8);instance.set(Mod1Amount,500);
        instance.set(Mod2Source,source>=3 ? (source==5 ? 4 : 5) : 0);
        instance.set(Mod2Destination,8);instance.set(Mod2Amount,250);
        instance.initialise();
        midiMessage(instance.self,0xb2,1,96);
        auto* state=static_cast<Algorithm*>(instance.self);state->engine.reset();
        echolab::Engine expected,unrouted;
        const auto count=echolab::Engine::requiredSamples(48000,1000);
        std::vector<float> memory(count),plainMemory(count);
        check(expected.prepare(48000,1000,memory.data(),memory.size()) &&
              unrouted.prepare(48000,1000,plainMemory.data(),plainMemory.size()),"Fe references prepare");
        expected.setParameters(state->controls);expected.reset();
        auto plain=state->controls;plain.mod1_source=plain.mod2_source=0;
        plain.mod1_destination=plain.mod2_destination=0;plain.mod1_amount=plain.mod2_amount=0;
        unrouted.setParameters(plain);unrouted.reset();
        constexpr int n=128;
        double difference=0;
        for(int block=0;block<12;++block) {
            std::vector<float> buses(kNT_lastBus*n,0.0f);
            for(int k=0;k<n;++k) {
                const float frame=float(block*n+k);
                buses[k]=2*std::sin(frame*.19f);buses[n+k]=1.5f*std::cos(frame*.23f);
                buses[12*n+k]=5*std::sin(frame*.003f);buses[13*n+k]=5*std::cos(frame*.002f);
            }
            const auto input=buses;step(instance.self,buses.data(),n/4);
            for(int k=0;k<n;++k) {
                expected.setExternalModulation(input[12*n+k]*.2f,input[13*n+k]*.2f,96/127.0f);
                float l=0,r=0,plainL=0,plainR=0;
                expected.processSample(input[k]/8,input[n+k]/8,l,r);
                unrouted.processSample(input[k]/8,input[n+k]/8,plainL,plainR);
                near(buses[12*n+k],l*8,"every Fe source uses normalized CV/wheel and snapshots its output alias");
                near(buses[13*n+k],r*8,"both Fe slots preserve stereo shared-core parity");
                const double error=buses[12*n+k]-plainL*8;difference+=error*error;
            }
        }
        const double rms=std::sqrt(difference/(12*n));
        if(fullRate || (!enabled && source<=2))
            check(rms<1e-7,"Full Fs ignores Fe mapping; Mod Off silences internal LFO/envelope mapping sources");
        else check(rms>.01,"Fe mapping remains audible for every enabled source and for external CV/wheel with Mod Off");
    }
}
static void firstPresetLoopLength() {
    for(bool cvTime : {false,true}) {
        Instance i(3);
        i.set(Model,16);i.set(Time,1451);i.set(Mix,1000);i.set(LoopAction,1);
        i.set(OutputModeL,1);i.set(OutputModeR,1);
        if(cvTime) { i.set(CV1Bus,3);i.set(Mod1Source,3);i.set(Mod1Destination,1);i.set(Mod1Amount,1000); }
        // Deliberately no Engine::reset after loading controls: this is the real
        // NT preset/first-block path, including the previous 350ms glide state.
        const std::size_t expected=cvTime ? 104472u : 69648u; // 1451ms *48; +CV gives1.5x.
        std::size_t firstWet=std::numeric_limits<std::size_t>::max();
        constexpr int n=128;
        std::vector<float> b(kNT_lastBus*n,0.0f);
        for(std::size_t cursor=0;cursor<expected+128;cursor+=n) {
            std::fill(b.begin(),b.end(),0.0f);
            if(cursor==0) b[0]=8.0f;
            if(cvTime) for(int k=0;k<n;++k) b[2*n+k]=5.0f;
            step(i.self,b.data(),n/4);
            for(int k=0;k<n;++k) if(cursor+static_cast<std::size_t>(k)>0 &&
                std::fabs(b[12*n+k])>1.0f && firstWet==std::numeric_limits<std::size_t>::max())
                firstWet=cursor+static_cast<std::size_t>(k);
        }
        check(firstWet==expected,"first NT Loop Record latches preset Time plus CV without capturing the previous delay glide");
    }
}
static void musicalTapPagesAndCapacity() {
    Instance instance;
    instance.set(Model,4);
    const auto headsHas=[&](int parameter) {
        for(uint32_t page=0;page<instance.self->parameterPages->numPages;++page) {
            const auto& definition=instance.self->parameterPages->pages[page];
            if(std::strcmp(definition.name,"Heads")!=0) continue;
            for(unsigned i=0;i<definition.numParams;++i) if(definition.params[i]==parameter) return true;
        }
        return false;
    };
    char text[kNT_parameterStringSize];
    auto label=[&](int pattern,const char* expected) {
        const int length=parameterString(instance.self,TapPattern,pattern,text);
        check(std::strcmp(text,expected)==0 && length==static_cast<int>(std::strlen(text)),
              "tap labels describe the actual positions in the selected span");
    };
    check(headsHas(TapPattern) && !headsHas(Division),"free Heads exposes patterns relative to manual Time");
    label(0,"Triplet: 1/3 2/3 1 span");
    instance.set(SyncMode,3);instance.set(Division,2);
    check(headsHas(TapPattern) && headsHas(Division),"synced Heads exposes its span as an editable musical division");
    label(0,"Triplet: 1/8T 1/4T 1/4");
    label(1,"Four: 1/16 1/8 1/8D 1/4");
    instance.set(Division,5);label(0,"Triplet: 1/8 1/4 1/4D");
    instance.set(Division,0);
    label(0,"Triplet: 1/3 2/3 1 span (max)");
    draw(instance.self);
    check(lastDrawText && std::strstr(lastDrawText,"max delay"),"a clipped musical span reports its memory limit instead of claiming exact note timing");
    instance.set(Model,15);instance.set(Division,4);instance.set(RightDivision,1);
    draw(instance.self);
    check(lastDrawText && std::strstr(lastDrawText,"max delay"),"right-only stereo note clipping is visible even when the left note fits");
    instance.set(RightDivision,4);
    draw(instance.self);
    check(lastDrawText && !std::strstr(lastDrawText,"max delay"),"capacity warning clears when both note durations fit");
}
static void productionClockAndOptionalColor() {
    Instance production(3,false,0,0,false,false);
    auto* state=static_cast<Algorithm*>(production.self);
    check(production.values[SyncMode]==3 && state->clock.locked() &&
          production.self->parameterPages->pages[0].params[0]==Division,
          "a new production instance starts with editable musical divisions at Internal BPM");
    near(state->controls.delay_ms,500,1e-5f,"the default 120 BPM quarter is a 500ms delay");
    check(pageHas(production.self,FactoryPreset) && pageHas(production.self,WetToneOn) && !pageHas(production.self,Tone),
          "the preset selector is usable and Digital initially hides inactive Tone");
    const auto before=pageNotifications;production.set(WetToneOn,1);
    check(pageHas(production.self,Tone) && pageNotifications==before+1 && state->controls.wet_tone_enabled,
          "enabling wet Tone exposes its cutoff and updates the shared core");
    production.set(WetToneOn,0);check(!pageHas(production.self,Tone),"disabling the optional filter hides Digital's inactive cutoff");
    production.set(Model,17);
    check(pageHas(production.self,ExciterHighpassOn) && !pageHas(production.self,FeedbackHPHz),
          "native Exciter A exposes an opt-in return high-pass while preserving its old off state");
    production.set(ExciterHighpassOn,1);production.set(FeedbackHPHz,300);
    check(pageHas(production.self,FeedbackHPHz) && state->controls.exciter_highpass_enabled,
          "the enabled Exciter return high-pass exposes its frequency");
    near(state->controls.feedback_highpass_hz,300,0,"the curated 300Hz return cutoff reaches the shared core");
    std::vector<float> buses(kNT_lastBus*128,0.0f);step(production.self,buses.data(),32);
    production.set(Version,1);
    check(!pageHas(production.self,ExciterHighpassOn),"the optional legacy return switch is hidden when the alternate version already has its return filters");
}
static void checkRecipe(const Instance& instance,const preset_bank::Preset& preset);
static void presetsBeforeFirstAudio() {
    for(bool reversed : {false,true}) for(bool deferred : {false,true}) for(bool skipped : {false,true})
        for(int slot : {4,1,2,3}) {
        deferParameterWrites=deferred;skipUnchangedParameterWrites=skipped;queuedWrites.clear();
        Instance instance(3,false,0,0,reversed,false);
        auto* state=static_cast<Algorithm*>(instance.self);
        instance.set(FactoryPreset,slot);
        if(slot==4 && state->controls.model!=echolab::Model::Analog)
            std::cerr << "Before-first-step preset reproduction: initial=" << state->initialControls
                      << " selected=" << slot << " hostModel=" << instance.values[Model]
                      << " engineModel=" << static_cast<int>(state->controls.model) << '\n';
        checkRecipe(instance,preset_bank::presets[slot-1]);
        flushParameterWrites(reversed);
        checkRecipe(instance,preset_bank::presets[slot-1]);
        check(instance.values[FactoryPreset]==slot && state->observedPreset==slot && state->pendingWrites==0,
              "the first four live recipes apply after parameter initialization without requiring an audio step");
    }
    deferParameterWrites=false;skipUnchangedParameterWrites=false;
}
static void partialInitialNotificationsAndUiEdits() {
    const int firstModels[]={0,10,13,1};
    for(bool reversed : {false,true}) for(bool deferred : {false,true}) for(bool skipped : {false,true})
        for(int notificationMode=0;notificationMode<3;++notificationMode) for(int slot=1;slot<=4;++slot) {
        deferParameterWrites=deferred;skipUnchangedParameterWrites=skipped;queuedWrites.clear();
        Instance instance(3,false,1,0,reversed,true,0,false,false);
        instance.values[Model]=0;instance.values[Feedback]=517;instance.values[Mix]=627;
        instance.values[Tone]=1700;instance.values[Drive]=123;
        const auto loaded=instance.values;
        const int partial[]={notificationMode==1 ? Model : FactoryPreset,
                             notificationMode==1 ? Mix : Voicing,
                             notificationMode==1 ? Tone : Drive};
        if(notificationMode) for(int p=0;p<3;++p) parameterChanged(instance.self,partial[reversed ? 2-p : p]);
        auto* state=static_cast<Algorithm*>(instance.self);
        check(state->initialControls && instance.values==loaded && state->pendingWrites==0,
              "an incomplete initial callback pass preserves loaded fields without applying their stored recipe label");
        const auto writesBefore=parameterWrites.size();
        const auto beforeDraw=state->controls;
        draw(instance.self);
        check(state->initialControls && (state->initialUiSnapshot.load(std::memory_order_relaxed)&uiSnapshotPresent) &&
              parameterWrites.size()==writesBefore && instance.values==loaded &&
              state->controls.model==beforeDraw.model && state->controls.feedback==beforeDraw.feedback,
              "draw only marks UI exposure; it never applies a recipe, changes controls or calls an audio setter");
        const auto firstSnapshot=state->initialUiSnapshot.load(std::memory_order_relaxed);
        instance.values[FactoryPreset]=static_cast<int16_t>(slot);
        draw(instance.self);
        check(state->initialUiSnapshot.load(std::memory_order_relaxed)==firstSnapshot,
              "a later UI frame cannot replace the selector snapshot captured before the edit");
        parameterChanged(instance.self,FactoryPreset);
        check(!state->initialControls && state->controls.model==static_cast<echolab::Model>(firstModels[slot-1]) &&
              state->visibleModel==firstModels[slot-1],
              "after UI exposure the first live recipe changes its actual model and pages before any audio step");
        checkRecipe(instance,preset_bank::presets[slot-1]);
        flushParameterWrites(reversed);checkRecipe(instance,preset_bank::presets[slot-1]);
        check(instance.values[FactoryPreset]==slot && state->pendingWrites==0,
              "partial initialization followed by a live preset persists coherently with delayed or omitted no-op notifications");
    }
    for(bool reversed : {false,true}) for(bool deferred : {false,true}) for(int loadedSlot : {0,2,4,15})
        for(bool completeCallbacks : {false,true}) {
        deferParameterWrites=deferred;skipUnchangedParameterWrites=true;queuedWrites.clear();
        Instance saved(3,false,1,0,reversed,true,loadedSlot,false,false);
        saved.values[Model]=0;saved.values[Feedback]=517;saved.values[Mix]=627;saved.values[Tone]=1700;saved.values[Drive]=123;
        const auto loaded=saved.values;
        parameterChanged(saved.self,FactoryPreset);parameterChanged(saved.self,Model);
        check(saved.values==loaded && static_cast<Algorithm*>(saved.self)->initialControls,
              "isolated saved-label and model notifications alone do not prove a live recipe selection");
        if(completeCallbacks) saved.initialise(reversed);
        else {
            std::vector<float> buses(kNT_lastBus*4,0.0f);step(saved.self,buses.data(),1);
        }
        flushParameterWrites(reversed);
        const auto* state=static_cast<const Algorithm*>(saved.self);
        check(!state->initialControls && state->controls.model==echolab::Model::Digital && state->visibleModel==0,
              "both complete callbacks and the first valid audio step adopt the actual saved model without factory autoapply");
        for(int p=0;p<ParameterCount;++p) if(p!=FactoryPreset)
            check(saved.values[p]==loaded[p],"finishing partial or complete saved initialization preserves every sound and routing value");
        check(saved.values[FactoryPreset]==(loadedSlot==4 && !completeCallbacks ? 0 : loadedSlot),
              "only the first render corrects an inherited mismatching default label; callback completion preserves saved labels");
    }
    deferParameterWrites=false;skipUnchangedParameterWrites=false;
}
static void unchangedLoadedUiSelectorNotifications() {
    for(bool reversed : {false,true}) for(bool deferred : {false,true}) for(bool skipped : {false,true})
        for(int notifications=0;notifications<3;++notifications) {
        deferParameterWrites=deferred;skipUnchangedParameterWrites=skipped;queuedWrites.clear();
        Instance saved(3,false,1,0,reversed,true,4,false,false);
        saved.values[Model]=0;saved.values[Feedback]=517;saved.values[Mix]=627;
        saved.values[Tone]=1700;saved.values[Drive]=123;
        const auto loaded=saved.values;
        if(notifications==1) parameterChanged(saved.self,Model);
        else if(notifications==2) saved.initialise(reversed);
        const auto writesBefore=parameterWrites.size();
        draw(saved.self);parameterChanged(saved.self,FactoryPreset);
        flushParameterWrites(reversed);
        const auto* state=static_cast<const Algorithm*>(saved.self);
        check(saved.values==loaded && state->controls.model==echolab::Model::Digital && state->controls.feedback==.517f &&
              state->pendingWrites==0 && parameterWrites.size()==writesBefore,
              "an unchanged saved Preset4 notification after first draw never reapplies Analog or changes its saved sound");
        check(state->initialControls==(notifications!=2) && state->initialNormalizationPending,
              "identical UI notifications keep partial loading open while a complete callback pass releases live editing");
    }
    for(bool reversed : {false,true}) for(bool deferred : {false,true}) {
        deferParameterWrites=deferred;skipUnchangedParameterWrites=true;queuedWrites.clear();
        Instance saved(3,false,2,1,reversed,true,2,false,false);
        saved.values[Mod1Destination]=8;saved.values[Mod2Destination]=7;
        saved.values[SampleRateHz]=0;
        const auto loaded=saved.values;
        draw(saved.self);parameterChanged(saved.self,Version);
        parameterChanged(saved.self,Mod1Destination);parameterChanged(saved.self,Mod2Destination);
        flushParameterWrites(reversed);
        for(int p=0;p<ParameterCount;++p) if(p!=FactoryPreset)
            check(saved.values[p]==loaded[p],
                  "unchanged Version and inactive saved target notifications after draw preserve their hidden voicing and CV destinations");
        check(static_cast<Algorithm*>(saved.self)->pendingWrites==0,"unchanged saved selector notifications settle their metadata writes");
        char text[kNT_parameterStringSize]{};
        check(parameterString(saved.self,Mod1Destination,8,text)>0 && std::strstr(text,"off"),
              "the preserved inactive 8bit route remains honestly marked off rather than becoming an audible Mix route");
    }
    deferParameterWrites=false;skipUnchangedParameterWrites=false;
}
static void unchangedModelThenPresetAfterFirstDraw() {
    for(bool deferred : {false,true}) for(bool skipped : {false,true}) {
        deferParameterWrites=deferred;skipUnchangedParameterWrites=skipped;queuedWrites.clear();
        Instance saved(3,false,1,0,false,true,4,false,false);
        saved.values[Model]=0;saved.values[Feedback]=517;saved.values[Mix]=627;saved.values[Drive]=123;
        const auto loaded=saved.values;
        const auto writesBefore=parameterWrites.size();
        draw(saved.self);
        auto* state=static_cast<Algorithm*>(saved.self);
        const auto firstSnapshot=state->initialUiSnapshot.load(std::memory_order_relaxed);
        parameterChanged(saved.self,Model);draw(saved.self);parameterChanged(saved.self,FactoryPreset);
        flushParameterWrites();
        check(saved.values==loaded && state->initialControls && state->controls.model==echolab::Model::Digital &&
              state->initialUiSnapshot.load(std::memory_order_relaxed)==firstSnapshot &&
              state->pendingWrites==0 && parameterWrites.size()==writesBefore,
              "late initial Model then identical Preset4 notifications after draw preserve saved sound and issue no queued Custom or recipe writes");
        saved.set(FactoryPreset,0);saved.set(FactoryPreset,4);
        checkRecipe(saved,preset_bank::presets[3]);flushParameterWrites();
        checkRecipe(saved,preset_bank::presets[3]);
        check(!state->initialControls && state->controls.model==echolab::Model::Analog && state->pendingWrites==0,
              "an explicit Custom then Analog choice still reapplies the recipe before the first audio step");
    }
    deferParameterWrites=false;skipUnchangedParameterWrites=false;
}
static void analogProductDefaultsAndSavedValues() {
    check(preset_bank::defaultSlot==4 && parameters[FactoryPreset].def==4 && parameters[Model].def==1,
          "a newly added NT algorithm selects Analog Bounce as its default recipe and model");
    const auto& recipe=preset_bank::presets[preset_bank::defaultSlot-1];
    for(unsigned p=0;p<recipe.count;++p) {
        const auto& setting=recipe.values[p];
        if(setting.parameter==Voicing)
            check(parameters[Voicing].def==0 && echolab::resolvedVoicing(echolab::Model::Analog,parameters[Voicing].def)==
                  echolab::resolvedVoicing(echolab::Model::Analog,setting.value),
                  "Auto keeps the canonical Analog A04 sound while preserving the legacy default for a missing Voicing field");
        else check(parameters[setting.parameter].def==setting.value,
                   "all other new-instance sound and clock defaults come from the canonical Analog Bounce recipe");
    }
    for(bool deferred : {false,true}) for(bool reversed : {false,true}) {
        deferParameterWrites=deferred;skipUnchangedParameterWrites=true;queuedWrites.clear();
        const auto writesBefore=parameterWrites.size();
        Instance fresh(3,false,0,0,reversed,false,0,true);
        std::vector<float> buses(kNT_lastBus*4,0.0f);step(fresh.self,buses.data(),1);
        auto* state=static_cast<Algorithm*>(fresh.self);
        for(unsigned p=0;p<recipe.count;++p) if(recipe.values[p].parameter!=Voicing)
            check(value(state,recipe.values[p].parameter)==recipe.values[p].value,
                  "a new NT instance exposes every non-voicing field of the Analog default together");
        near(state->controls.delay_ms,375,1e-5f,"the Analog default starts on a dotted eighth at 120 BPM");
        near(state->controls.feedback,.13f,1e-6f,"the Analog default applies the complete recipe feedback");
        near(state->controls.drive,.428f,1e-6f,"the Analog default applies the complete recipe drive");
        check(state->observedPreset==4 && fresh.values[FactoryPreset]==4 && state->visibleModel==1 &&
              state->engine.parameters().model==echolab::Model::Analog && state->controls.voicing==0 &&
              echolab::resolvedVoicing(state->controls.model,state->engine.parameters().voicing)==1 &&
              pageHas(fresh.self,Tone) && pageHas(fresh.self,Drive) && pageHas(fresh.self,StereoTiming),
              "the actual product default label, A04 character, engine and pages agree at startup");
        check(state->pendingWrites==0 && parameterWrites.size()==writesBefore,
              "a complete new-instance default requires no recursive preset writes or callback acknowledgements");
        // Old NT files without Preset55 inherit its new definition default,
        // while their existing sound fields must remain owned by the host.
        Instance legacy(3,false,0,0,reversed,true,4,false,false);
        legacy.set(Model,13);legacy.set(Time,617);legacy.set(Feedback,517);legacy.set(Tone,1700);
        const auto before=legacy.values;
        legacy.initialise(reversed);
        step(legacy.self,buses.data(),1);flushParameterWrites(reversed);
        const auto* saved=static_cast<const Algorithm*>(legacy.self);
        check(legacy.values[FactoryPreset]==0 && saved->observedPreset==0,
              "a legacy sound inheriting the new default recipe ID is identified honestly as Custom");
        for(int p=0;p<ParameterCount;++p) if(p!=FactoryPreset)
            check(legacy.values[p]==before[p],"correcting an inherited default recipe label preserves every existing saved NT value");
        check(saved->controls.model==echolab::Model::DigitalRack && saved->visibleModel==13,
              "the new Analog default never replaces a saved algorithm model or its pages");
        Instance changed(3,false,0,0,reversed,false,0,true,false);
        changed.set(Drive,123);
        const auto changedBefore=changed.values;
        changed.initialise(reversed);
        step(changed.self,buses.data(),1);flushParameterWrites(reversed);
        check(changed.values[FactoryPreset]==0 && static_cast<Algorithm*>(changed.self)->observedPreset==0,
              "a customized saved Analog default is named Custom instead of claiming the current complete recipe");
        for(int p=0;p<ParameterCount;++p) if(p!=FactoryPreset)
            check(changed.values[p]==changedBefore[p],"relabeling a customized saved default preserves its exact sound controls");
        Instance clocked(3,false,0,0,reversed,false,0,true,false);
        clocked.set(SyncMode,1);clocked.set(TempoBpm,93);clocked.set(ClockBus,5);clocked.set(ClockPpqn,1);
        clocked.set(InputL,3);clocked.set(InputR,3);clocked.set(OutputL,17);clocked.set(OutputR,18);
        clocked.set(CV1Bus,6);clocked.set(Time,617);
        const auto clockedBefore=clocked.values;
        clocked.initialise(reversed);
        step(clocked.self,buses.data(),1);flushParameterWrites(reversed);
        check(clocked.values==clockedBefore && static_cast<Algorithm*>(clocked.self)->observedPreset==4,
              "clock source, tempo, clock/CV/audio buses and manual fallback do not invalidate an otherwise complete Analog default recipe");
        for(int model : {0,7,13}) {
            Instance absentVoice(3,false,0,0,reversed,false,0,true,false);
            absentVoice.set(Model,model);
            check(absentVoice.values[Voicing]==0,"old files without Voicing51 inherit its unchanged Auto definition default");
            absentVoice.initialise(reversed);
            step(absentVoice.self,buses.data(),1);flushParameterWrites(reversed);
            const auto* old=static_cast<const Algorithm*>(absentVoice.self);
            const int expected=model==0 ? 2 : (model==7 ? 1 : 3);
            check(absentVoice.values[Voicing]==0 && old->controls.voicing==0 &&
                  echolab::resolvedVoicing(old->controls.model,old->engine.parameters().voicing)==expected,
                  "missing legacy Voicing51 retains Digital, Tape and Rack's distinct automatic characters");
        }
    }
    deferParameterWrites=false;skipUnchangedParameterWrites=false;
}
static bool recipeGlobal(int parameter) { return parameter==SyncMode || parameter==TempoBpm || parameter==ClockPpqn; }
static void checkRecipe(const Instance& instance,const preset_bank::Preset& preset) {
    const auto* state=static_cast<const Algorithm*>(instance.self);
    for(unsigned i=0;i<preset.count;++i) if(!recipeGlobal(preset.values[i].parameter))
        check(value(state,preset.values[i].parameter)==preset.values[i].value,"a recipe is exposed as one coherent set while host callbacks settle");
    check(state->controls.model==static_cast<echolab::Model>(value(state,Model)) &&
          state->controls.tap_pattern==value(state,TapPattern) &&
          state->controls.exciter_highpass_enabled==(value(state,ExciterHighpassOn)!=0) &&
          state->controls.wet_tone_enabled==(value(state,WetToneOn)!=0),"recipe flags, model and musical taps reach the shared core together");
    near(state->controls.feedback,value(state,Feedback)*.001f,1e-6f,"recipe feedback matches its visible parameter");
    near(state->controls.mix,value(state,Mix)*.001f,1e-6f,"recipe mix matches its visible parameter");
}
static void modelThenPresetWithUnchangedHostWrites() {
    // The actual host need not call parameterChanged for a write that leaves v
    // unchanged. A mock that always acknowledges those writes hid this failure.
    for(bool skipped : {false,true}) for(bool deferred : {false,true}) for(bool reversed : {false,true})
        for(int initialPreset : {-1,0,1,21}) for(int selected : {1,21,23}) {
        deferParameterWrites=deferred;skipUnchangedParameterWrites=skipped;queuedWrites.clear();
        Instance instance(3,false,0,0,reversed,false,std::max(0,initialPreset),false,initialPreset<=0);
        if(initialPreset>0) {
            // Populate an actual saved NT recipe before its first audio step;
            // loading must retain saved values instead of reapplying a recipe.
            const auto& saved=preset_bank::presets[initialPreset-1];
            for(unsigned p=0;p<saved.count;++p)
                instance.values[saved.values[p].parameter]=saved.values[p].value;
            instance.initialise(reversed);
        }
        std::vector<float> buses(kNT_lastBus*4,0.0f);step(instance.self,buses.data(),1);
        // -1 first applies the largely-default Clean recipe, reproducing the
        // no-op Model write; 0 starts from Custom without any prior recipe.
        if(initialPreset<0) { instance.set(FactoryPreset,1);flushParameterWrites(reversed); }
        instance.set(Model,16);
        auto* state=static_cast<Algorithm*>(instance.self);
        check(value(state,FactoryPreset)==0 && state->observedPreset==0 && state->controls.model==echolab::Model::LoopSampler &&
              state->visibleModel==16 && pageHas(instance.self,LoopAction),
              "a manual Model edit immediately selects Custom and publishes the correct model pages");
        const auto verifySelection=[&](int slot,bool settled) {
            const auto& recipe=preset_bank::presets[slot-1];
            checkRecipe(instance,recipe);
            const int model=value(state,Model);
            check(value(state,FactoryPreset)==slot && state->observedPreset==slot && state->visibleModel==model &&
                  state->engine.parameters().model==static_cast<echolab::Model>(model) &&
                  pageHas(instance.self,LoopAction)==(model==16) &&
                  pageHas(instance.self,StereoTiming)==colouredStereoModel(model),
                  "explicit recipe selection keeps its label, cached controls, engine and model pages coherent");
            if(settled) {
                if(state->pendingWrites!=0 || instance.values[Model]!=model)
                    std::cerr << "Model/Preset no-op callback reproduction: skip=" << skipped << " deferred=" << deferred
                              << " initial=" << initialPreset << " selected=" << slot << " hostModel=" << instance.values[Model]
                              << " engineModel=" << model << " pending=" << state->pendingWrites << '\n';
                check(state->pendingWrites==0 && instance.values[FactoryPreset]==slot && instance.values[Model]==model,
                      "manual Model then Preset settles without waiting for unchanged-write callbacks");
                for(unsigned p=0;p<recipe.count;++p) if(!recipeGlobal(recipe.values[p].parameter))
                    check(instance.values[recipe.values[p].parameter]==recipe.values[p].value,
                          "every selected recipe field reaches host-owned storage after acknowledgement");
            }
        };
        instance.set(FactoryPreset,selected);verifySelection(selected,false);
        flushParameterWrites(reversed);verifySelection(selected,true);
        // Re-select the same recipe after a new manual model, then supersede it
        // rapidly while older Custom and recipe writes may still be in flight.
        instance.set(Model,7);instance.set(FactoryPreset,selected);verifySelection(selected,false);
        instance.set(FactoryPreset,23);instance.set(Model,16);instance.set(FactoryPreset,22);instance.set(FactoryPreset,21);
        verifySelection(21,false);flushParameterWrites(reversed);verifySelection(21,true);
    }
    skipUnchangedParameterWrites=false;deferParameterWrites=false;
}
static void versionOverrideAcknowledgementRefreshesControls() {
    deferParameterWrites=true;skipUnchangedParameterWrites=true;queuedWrites.clear();
    Instance instance(3);std::vector<float> buses(kNT_lastBus*4,0.0f);step(instance.self,buses.data(),1);
    instance.set(Model,1);instance.set(Voicing,3);instance.set(Version,1);
    flushParameterWrites();
    instance.set(Version,0);instance.set(Voicing,2);flushParameterWrites();
    auto* state=static_cast<Algorithm*>(instance.self);
    if(state->controls.voicing!=2)
        std::cerr << "Voicing ack reproduction: hostVoicing=" << instance.values[Voicing]
                  << " controlsVoicing=" << state->controls.voicing << " pendingOverride=" << state->pendingVoicing << '\n';
    check(instance.values[Version]==0 && instance.values[Voicing]==2 && state->pendingWrites==0 && state->pendingVoicing<0 &&
          state->controls.voicing==2 && state->engine.parameters().voicing==2 && state->visibleVoicing==2,
          "acknowledging a superseded Version voicing override refreshes actual controls and pages to the latest saved voicing");
    deferParameterWrites=false;skipUnchangedParameterWrites=false;
}
static void factoryPresetBank() {
    for(bool deferred : {false,true}) for(unsigned active=0;active<preset_bank::count;++active) {
        const unsigned selected=preset_bank::activeSlots[active];
        deferParameterWrites=deferred;queuedWrites.clear();
        Instance instance(3,false,0,0,false,false);
        instance.set(Time,731);instance.set(InputL,3);instance.set(CV1Bus,15);
        std::vector<float> buses(kNT_lastBus*128,0.0f);step(instance.self,buses.data(),32);
        instance.set(FactoryPreset,static_cast<int>(selected));
        const auto& recipe=preset_bank::presets[selected-1];
        auto* state=static_cast<Algorithm*>(instance.self);
        checkRecipe(instance,recipe);
        check(instance.values[Time]==731 && instance.values[InputL]==3 && instance.values[CV1Bus]==15,
              "applying a sound recipe preserves manual fallback and audio/CV routing");
        check(state->observedPreset==static_cast<int>(selected),"recipe callbacks retain the chosen preset name");
        step(instance.self,buses.data(),32);checkRecipe(instance,recipe);
        if(deferred) {
            const auto writes=queuedWrites;queuedWrites.clear();
            for(const auto& write:writes) { applyParameterWrite(write);checkRecipe(instance,recipe); }
        }
        flushParameterWrites();
        check(state->pendingWrites==0 && instance.values[FactoryPreset]==static_cast<int>(selected),
              "immediate and deferred host persistence settle without losing the preset identity");
        instance.set(CV1Bus,16);instance.set(CV2Bus,17);
        check(instance.values[FactoryPreset]==static_cast<int>(selected) && state->observedPreset==static_cast<int>(selected),
              "manual CV input routing changes preserve the selected recipe name");
        for(unsigned i=0;i<recipe.count;++i) if(!recipeGlobal(recipe.values[i].parameter))
            check(instance.values[recipe.values[i].parameter]==recipe.values[i].value,"all curated recipe fields persist in host-owned preset memory");
        const auto before=state->controls;
        instance.set(FactoryPreset,0);
        near(state->controls.feedback,before.feedback,0,"Custom leaves the selected sound untouched");
        near(state->controls.drive,before.drive,0,"Custom never resets a recipe's drive");
    }
    deferParameterWrites=false;
    {
        Instance saved(3,false,1,0,true,true,2);
        auto* state=static_cast<Algorithm*>(saved.self);
        std::vector<float> buses(kNT_lastBus*128,0.0f);step(saved.self,buses.data(),32);
        check(saved.values[FactoryPreset]==2 && state->controls.model==echolab::Model::Digital && state->controls.voicing==1,
              "loading a saved preset name does not overwrite its stored model or legacy version");
        near(state->controls.feedback,.28f,1e-6f,"saved parameter values take precedence over a loaded bank label");
        saved.set(Mix,550);
        check(saved.values[FactoryPreset]==0,"manual sound editing marks the recipe Custom");
        near(state->controls.mix,.55f,1e-6f,"marking Custom preserves the actual user edit");
    }
    {
        deferParameterWrites=true;queuedWrites.clear();
        Instance edited(3,false,0,0,false,false);
        std::vector<float> buses(kNT_lastBus*128,0.0f);step(edited.self,buses.data(),32);
        edited.set(FactoryPreset,1);edited.set(Feedback,777);
        auto* state=static_cast<Algorithm*>(edited.self);
        near(state->controls.feedback,.777f,1e-6f,"a manual edit during a deferred recipe takes effect immediately");
        flushParameterWrites();
        check(edited.values[FactoryPreset]==0 && edited.values[Feedback]==777 && state->pendingWrites==0,
              "late recipe acknowledgements cannot overwrite a newer manual edit");
        near(state->controls.feedback,.777f,1e-6f,"the user's sound survives deferred recipe persistence");
        edited.set(FactoryPreset,2);edited.set(FactoryPreset,3);
        flushParameterWrites();checkRecipe(edited,preset_bank::presets[2]);
        check(edited.values[FactoryPreset]==3 && state->pendingWrites==0,
              "rapid recipe changes coalesce to the newest selection without partial intermediate sounds");
        deferParameterWrites=false;
    }
}
static void recipesPreserveLiveClock() {
    for(bool deferred : {false,true}) for(int tempo : {120,90}) {
        deferParameterWrites=deferred;queuedWrites.clear();
        Instance free(3);
        auto* freeState=static_cast<Algorithm*>(free.self);
        free.set(Time,731);free.set(TempoBpm,tempo);
        std::vector<float> freeBuses(kNT_lastBus*4,0.0f);step(free.self,freeBuses.data(),1);
        near(freeState->controls.delay_ms,731,0,"a saved Free instance retains its manual delay until a recipe is selected");
        free.set(FactoryPreset,1);
        check(freeState->clock.mode()==3 && freeState->clock.locked() && value(freeState,SyncMode)==3,
              "selecting a musical recipe from Free activates Internal BPM as part of the complete batch");
        near(freeState->controls.delay_ms,60000.0f/tempo,1e-4f,"the recipe's quarter note follows the user's retained tempo");
        flushParameterWrites();
        check(free.values[SyncMode]==3 && free.values[TempoBpm]==tempo && free.values[Time]==731,
              "Internal activation persists without changing tempo or the remembered millisecond fallback");
    }
    deferParameterWrites=false;
    Instance internal(3,false,0,0,false,false);
    std::vector<float> buses(kNT_lastBus*4,0.0f);step(internal.self,buses.data(),1);
    internal.set(TempoBpm,90);internal.set(FactoryPreset,2);
    auto* state=static_cast<Algorithm*>(internal.self);
    check(internal.values[TempoBpm]==90 && internal.values[SyncMode]==3,"a recipe keeps the user's internal tempo and clock source");
    near(state->controls.delay_ms,60000.0f/90*.75f,1e-4f,"the selected dotted note follows the retained internal tempo");
    internal.set(SyncMode,1);midiRealtime(internal.self,0xfa);midiRealtime(internal.self,0xf8);
    for(int block=0;block<250;++block) step(internal.self,buses.data(),1);
    midiRealtime(internal.self,0xf8);step(internal.self,buses.data(),1);
    check(state->clock.locked(),"MIDI fixture acquires an external clock before selecting a recipe");
    internal.set(FactoryPreset,1);
    check(state->clock.locked() && internal.values[SyncMode]==1 && internal.values[TempoBpm]==90,
          "a bank recipe preserves live MIDI clock lock and the remembered internal tempo");
    internal.set(SyncMode,2);internal.set(ClockBus,3);internal.set(ClockPpqn,0);
    buses[8]=5;step(internal.self,buses.data(),1);buses[8]=0;
    for(int block=0;block<5999;++block) step(internal.self,buses.data(),1);
    buses[8]=5;step(internal.self,buses.data(),1);buses[8]=0;
    check(state->clock.locked(),"CV fixture acquires a clock before the recipe change");
    internal.set(FactoryPreset,4);
    check(state->clock.locked() && internal.values[SyncMode]==2 && internal.values[ClockPpqn]==0 && internal.values[ClockBus]==3,
          "recipes retain the CV clock source, pulse rate, bus and lock");
}
static void stablePresetSlots() {
    check(preset_bank::count==21 && preset_bank::slotCount==24 && activePreset(18) && activePreset(19) && activePreset(20) &&
          activePreset(21) && activePreset(22) && activePreset(23) && activePreset(24),
          "the curated bank preserves old slots and appends Swell after the coloured stereo recipes without reusing retired IDs");
    for(bool deferred : {false,true}) for(int saved : {15,16,17}) {
        deferParameterWrites=deferred;queuedWrites.clear();
        Instance old(3,false,1,0,true,true,saved,false,false);
        old.set(Model,4);old.set(TapPattern,saved-7);
        old.initialise(true);
        std::vector<float> buses(kNT_lastBus*128,0.0f);step(old.self,buses.data(),32);
        auto* state=static_cast<Algorithm*>(old.self);
        check(old.values[FactoryPreset]==saved && state->controls.model==echolab::Model::MultiHead &&
              state->controls.tap_pattern==saved-7 && std::strcmp(preset_bank::names[saved],"Custom")==0,
              "saved removed recipes retain their model, tap pattern and sound and display Custom");
        check(!activePreset(saved),"retired slots remain tombstones rather than becoming a different recipe");
        old.set(FactoryPreset,16==saved ? 15 : 16);
        const int expected=saved==15 ? 18 : 14;
        check(state->observedPreset==expected && value(state,FactoryPreset)==expected,
              "explicit retired-slot selections skip to the active recipe in the direction of travel");
        flushParameterWrites();
        check(old.values[FactoryPreset]==expected && state->pendingWrites==0,"retired-slot skip persists through immediate or deferred host callbacks");
        checkRecipe(old,preset_bank::presets[expected-1]);
        old.set(FactoryPreset,20);flushParameterWrites();
        check(old.values[FactoryPreset]==20 && state->controls.model==echolab::Model::EchoPlatter,
              "the appended Platter slot is usable without stealing any older recipe ID");
    }
    deferParameterWrites=false;
}
static void swellFactoryPreset() {
    for(bool deferred : {false,true}) for(bool skipped : {false,true}) {
        deferParameterWrites=deferred;skipUnchangedParameterWrites=skipped;queuedWrites.clear();
        Instance instance(3);std::vector<float> buses(kNT_lastBus*4,0.0f);step(instance.self,buses.data(),1);
        instance.set(Model,1);instance.set(Mix,200);instance.set(Swell,100);
        instance.set(FactoryPreset,24);
        auto* state=static_cast<Algorithm*>(instance.self);
        checkRecipe(instance,preset_bank::presets[23]);
        check(state->controls.model==echolab::Model::AutoVolume && pageHas(instance.self,Swell),
              "Swell uses the actual Auto-Volume engine and exposes its functional envelope controls");
        near(state->controls.mix,1,0,"Swell replaces an old dry mix with a fully wet signal");
        near(state->controls.swell_ms,1200,0,"Swell replaces a previous rise time with its 1200ms recipe");
        flushParameterWrites();checkRecipe(instance,preset_bank::presets[23]);
        check(instance.values[FactoryPreset]==24 && instance.values[Model]==12 && instance.values[Mix]==1000 &&
              instance.values[Swell]==1200 && state->pendingWrites==0,
              "Swell's model, mix and rise persist coherently with immediate or deferred/no-op host notifications");
    }
    deferParameterWrites=false;skipUnchangedParameterWrites=false;
}
static void meaningfulModulationRouting() {
    Instance i;std::vector<float> buses(kNT_lastBus*128,0.0f);step(i.self,buses.data(),32);
    auto* state=static_cast<Algorithm*>(i.self);
    char text[kNT_parameterStringSize]{};
    i.set(Mod1Source,3);i.set(Mod1Amount,500);i.set(Mod1Destination,3);
    check(i.values[Mod1Destination]==5 && state->controls.mod1_destination==5,
          "Digital without wet Tone skips ineffective Tone and Drive targets during explicit route editing");
    check(parameterString(i.self,Mod1Destination,3,text)>0 && std::strcmp(text,"Tone (off)")==0,
          "an inactive saved Tone route is described honestly");
    i.set(WetToneOn,1);i.set(Mod1Destination,3);
    check(i.values[Mod1Destination]==3 && pageHas(i.self,Tone),"enabling Digital wet Tone makes both its knob and modulation destination available");
    i.set(Model,5);i.set(WetToneOn,0);
    check(i.values[Mod1Destination]==3 && state->controls.mod1_destination==3,
          "switching models preserves a remembered route without silently redirecting it to a different parameter");
    check(parameterString(i.self,Mod1Destination,3,text)>0 && std::strstr(text,"off"),"the remembered inactive route is marked off");
    i.set(Mod1Destination,8);check(i.values[Mod1Destination]==8,"8bit sample-rate modulation remains available at a reduced sample rate");
    i.set(SampleRateHz,0);
    check(i.values[Mod1Destination]==8 && parameterString(i.self,Mod1Destination,8,text)>0 && std::strstr(text,"off"),
          "Full Fs leaves the saved 8bit route intact while marking its bypass explicitly");
    i.set(Mod1Source,1);i.set(Mod2Source,2);i.set(ModulationEnabled,0);
    check(pageHas(i.self,ModRate) && pageHas(i.self,Attack) && pageHas(i.self,Release),
          "mapped LFO and envelope sources expose the controls that actually shape those sources on non-native models");
    check(parameterString(i.self,Mod1Source,1,text)>0 && std::strcmp(text,"LFO (off)")==0 &&
          parameterString(i.self,Mod2Source,2,text)>0 && std::strcmp(text,"Envelope (off)")==0,
          "Internal modulation Off is explicit for both gated mapping sources");
    check(parameterString(i.self,Mod1Source,3,text)>0 && std::strcmp(text,"CV 1 (off)")==0 &&
          parameterString(i.self,Mod2Source,4,text)>0 && std::strcmp(text,"CV 2 (off)")==0,
          "CV sources with no assigned input bus report their inactive state");
    i.set(CV1Bus,3);i.set(CV2Bus,4);
    check(parameterString(i.self,Mod1Source,3,text)>0 && std::strcmp(text,"CV 1")==0 &&
          parameterString(i.self,Mod2Source,4,text)>0 && std::strcmp(text,"CV 2")==0,
          "assigning CV buses makes their existing source selections available without remapping IDs");
    i.set(Model,9);i.set(ModulationEnabled,1);i.set(Mod1Destination,6);
    check(i.values[Mod1Destination]==6,"native modulation Depth is a selectable destination where it changes the delay");
    i.set(ModulationEnabled,0);
    check(parameterString(i.self,Mod1Destination,6,text)>0 && std::strcmp(text,"Depth (off)")==0,
          "Depth mapping reflects the internal modulation master switch");
    i.set(Model,17);i.set(Mod1Destination,7);
    check(i.values[Mod1Destination]==7,"Exciter amount routing is selectable on Exciter");
    i.set(Model,1);
    check(i.values[Mod1Destination]==7 && parameterString(i.self,Mod1Destination,7,text)>0 && std::strstr(text,"off"),
          "a remembered Exciter route stays inert on Analog and never changes another control");
    for(bool deferred : {false,true}) {
        deferParameterWrites=deferred;queuedWrites.clear();
        Instance pending;step(pending.self,buses.data(),32);
        pending.set(Mod1Destination,4);flushParameterWrites();
        check(pending.values[Mod1Destination]==5 && static_cast<Algorithm*>(pending.self)->pendingWrites==0,
              "contextual target skips settle safely with immediate or delayed host setters");
        auto* queued=static_cast<Algorithm*>(pending.self);
        queued->applyingPreset=true;
        requestParameter(queued,Mod1Destination,4);updateControls(queued);
        queued->applyingPreset=false;flushParameterWrites();
        check(pending.values[Mod1Destination]==4 && queued->controls.mod1_destination==4,
              "host acknowledgements of saved or batch-owned routes never redirect an inactive target to another parameter");
        pending.set(Mod1Destination,5);pending.set(Mod1Destination,4);pending.set(Mod1Destination,1);
        flushParameterWrites();
        check(pending.values[Mod1Destination]==1 && queued->controls.mod1_destination==1 &&
              queued->observedDestinations[0]==1 && queued->pendingWrites==0,
              "late destination-skip acknowledgements preserve a newer direct Time assignment and its edit direction");
        pending.set(Mod1Destination,4);pending.set(Mod1Destination,3);flushParameterWrites();
        check(pending.values[Mod1Destination]==2 && queued->controls.mod1_destination==2 && queued->observedDestinations[0]==2,
              "rapid contextual selections coalesce to the latest meaningful target");
    }
    deferParameterWrites=false;
    i.set(WetToneOn,1);i.set(ExciterHighpassOn,1);i.set(Mod1Source,1);i.set(Mod2Source,2);
    for(int model=0;model<echolab::modelCount;++model) {
        i.set(Model,model);
        check(i.self->parameterPages->numPages<=ARRAY_SIZE(state->pageStorage),
              "both contextual modulation pages fit bounded storage for every model and optional Color combination");
        for(uint32_t page=0;page<i.self->parameterPages->numPages;++page) {
            const auto& definition=i.self->parameterPages->pages[page];
            for(unsigned p=0;p<definition.numParams;++p) check(definition.params[p]<ParameterCount,
                  "contextual parameter routes retain valid published parameter IDs");
        }
    }
}
static void extendedModulationTargets() {
    Instance i;std::vector<float> buses(kNT_lastBus*128,0.0f);step(i.self,buses.data(),32);
    auto* state=static_cast<Algorithm*>(i.self);
    char text[kNT_parameterStringSize]{};
    i.set(CV1Bus,3);i.set(Mod1Source,3);i.set(Model,7);i.set(Mod1Destination,9);i.set(Mod1Amount,250);
    check(i.values[Mod1Destination]==9 && state->controls.mod1_destination==9 &&
          parameterString(i.self,Mod1Destination,9,text)>0 && std::strcmp(text,"Mod rate")==0,
          "the appended Mod rate target is selectable for Tape without changing IDs0to8");
    i.set(ModulationEnabled,0);
    check(parameterString(i.self,Mod1Destination,9,text)>0 && std::strcmp(text,"Mod rate (off)")==0,
          "rate routing reflects the internal movement master switch");
    i.set(ModulationEnabled,1);i.set(StereoTiming,0);i.set(Mod1Destination,10);
    check(i.values[Mod1Destination]==0,"unavailable coloured stereo ratio routing skips rather than advertising an inactive target");
    i.set(StereoTiming,1);i.set(SyncMode,3);i.set(RightDivision,8);i.set(Mod1Destination,10);
    check(i.values[Mod1Destination]==10 && state->controls.mod1_destination==10 &&
          parameterString(i.self,Mod1Destination,10,text)>0 && std::strcmp(text,"Stereo ratio")==0,
          "stereo ratio CV remains available around the resolved right-note ratio in sync");
    i.set(Model,1);i.set(Version,1);i.set(Mod1Destination,11);
    check(i.values[Mod1Destination]==11 && state->controls.mod1_destination==11,
          "Drift routing is exposed where the alternate analogue voice actually uses it");
    i.set(Version,0);
    check(parameterString(i.self,Mod1Destination,11,text)>0 && std::strcmp(text,"Drift (off)")==0,
          "a remembered Drift route is preserved and marked inactive for original A04 character");
    i.set(Model,6);i.set(ModulationEnabled,0);i.set(Mod1Destination,12);
    check(i.values[Mod1Destination]==12 && state->controls.mod1_destination==12 &&
          parameterString(i.self,Mod1Destination,12,text)>0 && std::strcmp(text,"Duck amount")==0,
          "functional duck amount CV remains available independently of internal modulation");
    i.set(Model,12);i.set(Mod1Destination,12);
    check(pageHas(i.self,DuckAmount) && pageHas(i.self,Attack) && pageHas(i.self,Release) && pageHas(i.self,Swell) &&
          parameterString(i.self,Mod1Destination,12,text)>0 && std::strcmp(text,"Duck amount")==0,
          "Auto-Volume exposes its tail duck amount and the functional CV target even with internal modulation off");
    i.set(Model,0);i.set(ModulationEnabled,1);i.set(Mod2Source,1);i.set(Mod1Destination,9);
    check(i.values[Mod1Destination]==9 && pageHas(i.self,ModRate),"an internally mapped LFO makes its rate both editable and routable on Digital");
    i.set(Mod2Source,0);
    check(parameterString(i.self,Mod1Destination,9,text)>0 && std::strcmp(text,"Mod rate (off)")==0,
          "removing the last LFO source marks an otherwise ineffective Digital rate route off");
    i.set(WetToneOn,1);i.set(ExciterHighpassOn,1);i.set(Mod1Source,1);i.set(Mod2Source,2);
    for(int model=0;model<echolab::modelCount;++model) for(int voice=0;voice<=3;++voice)
        for(int stereo=0;stereo<=1;++stereo) for(int sync : {0,3}) {
            i.set(Model,model);i.set(Voicing,voice);i.set(StereoTiming,stereo);i.set(SyncMode,sync);
            check(i.self->parameterPages->numPages<=ARRAY_SIZE(state->pageStorage),"all expanded mapping and coloured stereo page combinations fit bounded storage");
            for(uint32_t page=0;page<i.self->parameterPages->numPages;++page) {
                const auto& definition=i.self->parameterPages->pages[page];
                for(unsigned p=0;p<definition.numParams;++p) check(definition.params[p]<ParameterCount,"expanded pages contain only registered stable parameter IDs");
            }
        }
}
static void stereoWeaveMonoRouting() {
    for(bool bothInputs : {false,true}) {
        Instance instance(3,false,0,0,false,false);
        instance.set(InputL,1);instance.set(InputR,bothInputs ? 1 : 0);
        instance.set(OutputModeL,1);instance.set(OutputModeR,1);
        constexpr int n=128;
        std::vector<float> buses(kNT_lastBus*n,0.0f);step(instance.self,buses.data(),n/4);
        instance.set(FactoryPreset,10);
        auto* state=static_cast<Algorithm*>(instance.self);
        check(instance.values[InputL]==1 && instance.values[InputR]==(bothInputs ? 1 : 0),"the stereo recipe preserves the user's actual audio routing");
        check(instance.values[Division]==2 && instance.values[RightDivision]==8,
              "Stereo Weave routes quarter-note left and quarter-note-triplet right independently");
        near(state->controls.delay_ms,500,1e-4f,"Stereo Weave left quarter follows 120 BPM");
        near(state->controls.stereo_ratio,2.0f/3.0f,1e-6f,"Stereo Weave uses the exact three-to-two timing ratio");
        state->engine.reset();
        int firstL=-1,firstR=-1;
        for(int frame=0;frame<26000;frame+=n) {
            std::fill(buses.begin(),buses.end(),0.0f);if(frame==0) buses[0]=4;
            step(instance.self,buses.data(),n/4);
            for(int k=0;k<n;++k) if(frame+k>0) {
                if(firstL<0 && std::fabs(buses[12*n+k])>1e-6f) firstL=frame+k;
                if(firstR<0 && std::fabs(buses[13*n+k])>1e-6f) firstR=frame+k;
            }
        }
        check(firstL==24000,"a mono signal feeding the left stereo engine produces its quarter-note repeat");
        check(firstR==(bothInputs ? 16000 : -1),
              "feeding both NT inputs from the mono bus produces the right triplet; None intentionally remains silent");
    }
}
int main() {
    try {
        check(pluginEntry(kNT_selector_version,0)==13,"firmware 1.18 API 13 compatibility");
        check(pluginEntry(kNT_selector_numFactories,0)==1,"factory count");
        check(pluginEntry(kNT_selector_factoryInfo,1)==0,"invalid factory index");
        const auto* f = reinterpret_cast<const _NT_factory*>(pluginEntry(kNT_selector_factoryInfo,0));
        check(f && f->guid == NT_MULTICHAR('E','L','n','t'),"stable GUID");
        check(std::strcmp(f->name,"EchoLabNT")==0,"exact displayed project name");
        check(std::strstr(f->description,ECHOLAB_BUILD_ID) && !std::strstr(f->description,"provisional") &&
              !std::strstr(f->description,"original"),"the concise factory description retains its build ID without model provenance suffixes");
        for(const char* label:modelNames)
            check(!std::strstr(label,"(provisional)") && !std::strstr(label,"(original)"),"displayed model names omit provenance suffixes");
        static_assert(Model==6 && Time==7 && Bypass==12 && BitDepth==13 && LoopAction==22 && LimiterOn==23 && Division==42 && FeedbackLPHz==43 && Drift==44 && FeedbackHPHz==45 && LoResEndHz==46 && LoResDecayMs==47 && LoResEnvelopeAmount==48 && RackSampleRateHz==49 && LoResCutoffHz==50 && Voicing==51 && ModulationEnabled==52 && Version==53 && RightDivision==54 && FactoryPreset==55 && ExciterHighpassOn==56 && WetToneOn==57 && StereoTiming==58 && ParameterCount==59,"published parameter IDs remain stable; controls appended");
        check(parameters[Model].max==19 && parameters[Model].def==1 && ARRAY_SIZE(modelNames)==20,
              "20 selectors without changing existing model IDs");
        check(baseParameters[Feedback].def==280 && echolab::Parameters{}.feedback==.28f &&
              baseParameters[Mix].def==390 && echolab::Parameters{}.mix==.39f && baseParameters[ModRate].def==100 && baseParameters[ModDepth].def==600 && baseParameters[ExciterAmount].def==100 &&
              baseParameters[ExciterTuneHz].def==4000 && baseParameters[Swell].def==250 && baseParameters[LimiterReleaseMs].def==150 &&
              parameters[LimiterCeiling].max==891 && parameters[LimiterCeiling].def==891 &&
              baseParameters[FeedbackLPHz].def==12000 && baseParameters[Drift].def==250 && baseParameters[FeedbackHPHz].def==80 &&
              baseParameters[LoResEndHz].def==1200 && baseParameters[LoResDecayMs].def==1500 && baseParameters[LoResEnvelopeAmount].def==1000 && baseParameters[RackSampleRateHz].def==16000 &&
              baseParameters[LoResCutoffHz].def==2000 && baseParameters[Voicing].def==0 && baseParameters[ModulationEnabled].def==1 &&
              parameters[Version].min==0 && parameters[Version].max==1 && baseParameters[Version].def==0 && baseParameters[RightDivision].def==0 &&
              baseParameters[SyncMode].def==3 && baseParameters[TempoBpm].def==120 && baseParameters[Division].def==2 &&
              baseParameters[FactoryPreset].def==0 && parameters[FactoryPreset].max==static_cast<int>(preset_bank::slotCount) &&
              baseParameters[ExciterHighpassOn].def==0 && baseParameters[WetToneOn].def==0 && baseParameters[StereoTiming].def==0 && parameters[TapPattern].max==10,
              "legacy analytic fixture values and the unchanged -1dB NT maximum");
        check(std::strcmp(modelNames[8],"Sweep Down")==0 && std::strcmp(modelNames[18],"Sweep Up")==0,
              "Sweep Down keeps ID8 and Sweep Up appends ID18");
        check(std::strcmp(modelNames[19],"Tube A")==0 && parameters[Mod1Destination].max==12 && parameters[Mod2Destination].max==12 &&
              std::strcmp(destinationNames[8],"8bit rate")==0 && std::strcmp(destinationNames[9],"Mod rate")==0 &&
              std::strcmp(destinationNames[10],"Stereo ratio")==0 && std::strcmp(destinationNames[11],"Drift")==0 &&
              std::strcmp(destinationNames[12],"Duck amount")==0 && std::strcmp(modelNames[5],"8bit")==0,
              "product labels use A/B and 8bit without changing published selectors");
        check(std::strcmp(modelNames[0],"Digital control")==0 &&
              std::strcmp(modelNames[1],"Analog")==0 &&
              std::strcmp(modelNames[2],"Tube B")==0 &&
              std::strcmp(modelNames[3],"Ping Pong")==0 &&
              std::strcmp(modelNames[4],"Multi-Head")==0,"stable and concise model labels");
        int32_t a=1,b=8;
        _NT_algorithmRequirements low{},high{};
        calculateRequirements(low,&a); calculateRequirements(high,&b);
        check(high.dram>low.dram && low.numParameters==ParameterCount,"bounded spec memory");
        presetsBeforeFirstAudio();partialInitialNotificationsAndUiEdits();unchangedLoadedUiSelectorNotifications();unchangedModelThenPresetAfterFirstDraw();analogProductDefaultsAndSavedValues();modelThenPresetWithUnchangedHostWrites();versionOverrideAcknowledgementRefreshesControls();routing();
        for(int model=0;model<echolab::modelCount;++model) sharedCoreAndInstances(model);
        statefulModelAliases(); analyticalNewModelRouting(); bypassAndNonfinite(); appendedControls();
        dynamicPagesAndControls();musicalTapPagesAndCapacity();finalBusLimiter();singleMusicalStage();clockAndCvAliasing();timingPageFollowsClock();versionMigrationAndSelection();synchronizedStereoNotes();colouredStereoTiming();modulationCvAliasing();depthCvAndWheelRouting();
        digitalRackInternalRate();loresFeMappings();firstPresetLoopLength();productionClockAndOptionalColor();factoryPresetBank();recipesPreserveLiveClock();stablePresetSlots();swellFactoryPreset();meaningfulModulationRouting();extendedModulationTargets();stereoWeaveMonoRouting();
        check(hostParameterErrors==0,"host parameter writes use valid algorithms, ranges and the required parameter offset");
        std::cout << "NT adapter: complete catalog, appended controls, analytical ping-pong/multi-head, stateful bus aliases, modes, None, bounds, instance isolation, core parity passed\n";
        return 0;
    } catch(const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
