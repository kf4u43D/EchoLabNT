#include "dsp/engine.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <new>
#include <vector>

namespace {
bool audioGuard = false;
std::size_t audioAllocations = 0;
int failures = 0;
constexpr echolab::Model allModels[] = {echolab::Model::Digital,echolab::Model::Analog,
    echolab::Model::Tube,echolab::Model::PingPong,echolab::Model::MultiHead,
    echolab::Model::LoRes,echolab::Model::Dynamic,echolab::Model::Tape,echolab::Model::Sweep,
    echolab::Model::AnalogMod,echolab::Model::DigitalMod,echolab::Model::Reverse,
    echolab::Model::AutoVolume,echolab::Model::DigitalRack,echolab::Model::EchoPlatter,
    echolab::Model::StereoDelays,echolab::Model::LoopSampler,echolab::Model::ExciterEcho,echolab::Model::SweepUp,echolab::Model::TubeA04};
echolab::Parameters neutralParameters() {
    echolab::Parameters p;
    p.limiter_enabled=false;p.drive=0;p.tone_hz=4500;p.mod_rate_hz=.7f;p.mod_depth_ms=2;p.sample_rate_hz=0;
    p.feedback_lowpass_hz=0;p.feedback_highpass_hz=0;p.drift_amount=0;p.lores_rate_envelope_amount=0;p.digital_rack_rate_hz=0;
    p.voicing=3;p.lores_cutoff_hz=0;
    return p;
}
void check(bool passed, const char* message) {
    if (!passed) { std::cerr << "FAIL: " << message << '\n'; ++failures; }
}
void near(float actual, float expected, float tolerance, const char* message) {
    check(std::fabs(actual - expected) <= tolerance,message);
}
struct Fixture {
    std::vector<float> memory;
    echolab::Engine engine;
    Fixture(float fs = 8000, float maximum = 20) : memory(echolab::Engine::requiredSamples(fs,maximum)) {
        check(engine.prepare(fs,maximum,memory.data(),memory.size()),"prepare succeeds with exact memory");
        engine.setParameters(neutralParameters());engine.reset();
    }
    void parameters(float delay, float feedback = 0, float mix = 1, echolab::Model model = echolab::Model::Digital) {
        auto p=neutralParameters();
        p.delay_ms = delay; p.feedback = feedback; p.mix = mix; p.model = model;
        engine.setParameters(p); engine.reset();
    }
};
void impulseAndInterpolation() {
    Fixture f(8000,2);
    f.parameters(1,0.5f);
    for (unsigned i = 0; i != 600; ++i) {
        float l,r;
        f.engine.processSample(i == 0 ? 0.5f : 0.0f,i == 2 ? 0.25f : 0.0f,l,r);
        const float expectedL = i && i % 8 == 0 ? 0.5f*std::pow(0.5f,static_cast<float>(i / 8 - 1)) : 0;
        const float expectedR = i > 2 && (i-2) % 8 == 0 ? 0.25f * std::pow(0.5f,static_cast<float>((i-2) / 8 - 1)) : 0;
        near(l,expectedL,2e-6f,"digital impulse timing/gain through repeated buffer wraps");
        near(r,expectedR,2e-6f,"stereo independent delay timing/gain");
    }
    f.parameters(1.0625f);
    for (unsigned i = 0; i != 32; ++i) {
        float l,r; f.engine.processSample(i == 0 ? 0.5f : 0.0f,0,l,r);
        near(l,i == 8 || i == 9 ? 0.25f : 0,2e-6f,"linear fractional-delay impulse weights");
    }
    f.parameters(2000);
    near(f.engine.parameters().delay_ms,2,0,"requested delay clamps to reserved capacity");
    for (unsigned i = 0; i != 40; ++i) {
        float l,r; f.engine.processSample(i == 0 ? 0.5f : 0.0f,0,l,r);
        near(l,i == 16 ? 0.5f : 0,2e-6f,"maximum delay reaches final legal tap");
    }
}
void colouredStereoTiming() {
    const echolab::Model models[]={echolab::Model::Analog,echolab::Model::AnalogMod,
        echolab::Model::Tube,echolab::Model::TubeA04,echolab::Model::Tape};
    for (auto model:models) for (bool enabled:{false,true}) {
        Fixture f(8000,6);
        auto p=neutralParameters();p.model=model;p.delay_ms=3;p.feedback=0;p.mix=1;
        p.modulation_enabled=false;p.stereo_timing_enabled=enabled;p.stereo_ratio=2.0f/3.0f;
        f.engine.setParameters(p);f.engine.reset();
        const unsigned rightStart=enabled?16:24;
        for(unsigned sample=0;sample<32;++sample) {
            float left,right;
            f.engine.processSample(sample==0?.125f:0,sample==0?.125f:0,left,right);
            if(sample<24) near(left,0,0,"coloured left echo waits for its configured time");
            if(sample<rightStart) near(right,0,0,"coloured right echo waits for its independent time");
            if(sample==24) check(std::fabs(left)>1e-8f,"coloured left first arrival is audible");
            if(sample==rightStart) check(std::fabs(right)>1e-8f,"coloured right first arrival follows stereo timing toggle");
        }
        p.stereo_timing_enabled=true;p.delay_ms=3;p.stereo_ratio=24;
        f.engine.setParameters(p);f.engine.reset();
        for(unsigned sample=0;sample<=48;++sample) {
            float left,right;f.engine.processSample(0,sample==0?.125f:0,left,right);
            near(left,0,0,"independent coloured feedback does not copy right excitation to left");
            if(sample<48) near(right,0,0,"right time clamps independently to allocated capacity");
            if(sample==48) check(std::fabs(right)>1e-8f,"right clipped tap reaches final legal delay");
        }
    }
    // Off preserves the existing coloured sound even if a saved ratio differs.
    Fixture a(8000,20),b(8000,20);
    auto p=neutralParameters();p.model=echolab::Model::Tape;p.modulation_enabled=true;p.delay_ms=8;p.feedback=.3f;
    p.stereo_ratio=.5f;a.engine.setParameters(p);a.engine.reset();
    p.stereo_ratio=2;b.engine.setParameters(p);b.engine.reset();
    for(unsigned sample=0;sample<1000;++sample) {
        float al,ar,bl,br;const float input=sample%47==0?.2f:0;
        a.engine.processSample(input,-input,al,ar);b.engine.processSample(input,-input,bl,br);
        near(al,bl,0,"disabled coloured stereo timing retains legacy left output");
        near(ar,br,0,"disabled coloured stereo timing ignores stored ratio");
    }
}
void extendedModulationRoutes() {
    for(int destination=9;destination<echolab::modulationDestinationCount;++destination) {
        Fixture mapped(8000,30),direct(8000,30);
        auto p=neutralParameters();p.model=echolab::Model::AnalogMod;p.delay_ms=10;p.feedback=.25f;
        p.mix=1;p.mod_rate_hz=1;p.mod_depth_ms=1;p.modulation_enabled=true;p.drift_amount=0;
        if(destination==10) {p.model=echolab::Model::Analog;p.stereo_timing_enabled=true;p.stereo_ratio=1.5f;}
        if(destination==11) {p.model=echolab::Model::Tape;p.drift_amount=.2f;}
        if(destination==12) {p.model=echolab::Model::Dynamic;p.duck_amount=.4f;p.duck_threshold_db=-35;}
        p.mod1_source=3;p.mod1_destination=destination;p.mod1_amount=.2f;
        mapped.engine.setParameters(p);mapped.engine.reset();mapped.engine.setExternalModulation(.5f,0,0);
        p.mod1_destination=0;
        const float route=.5f*.2f;
        if(destination==9) p.mod_rate_hz+=20*route;
        if(destination==10) p.stereo_ratio*=1+.75f*route;
        if(destination==11) p.drift_amount+=route;
        if(destination==12) p.duck_amount+=route;
        direct.engine.setParameters(p);direct.engine.reset();
        for(unsigned sample=0;sample<4000;++sample) {
            const float input=sample%71<10?.125f:0;
            float ml,mr,dl,dr;
            mapped.engine.processSample(input,-input,ml,mr);direct.engine.processSample(input,-input,dl,dr);
            near(ml,dl,2e-6f,"new CV destination matches equivalent direct left control");
            near(mr,dr,2e-6f,"new CV destination matches equivalent direct right control");
        }
        // Exercise both route sums and endpoints without allocations or nonfinites.
        p.mod1_destination=destination;p.mod2_source=4;p.mod2_destination=destination;
        p.mod1_amount=p.mod2_amount=1;
        mapped.engine.setParameters(p);mapped.engine.reset();audioGuard=true;
        bool finite=true;
        for(unsigned sample=0;sample<1000;++sample) {
            const float cv=sample%2?1.0f:-1.0f;mapped.engine.setExternalModulation(cv,cv,0);
            float left,right;mapped.engine.processSample(.1f,-.1f,left,right);
            finite=finite && std::isfinite(left) && std::isfinite(right);
        }
        audioGuard=false;check(finite,"new CV route endpoints remain finite");
    }
}
void savedDriftRouteIsInactiveOnLooper() {
    Fixture mapped(8000,30),plain(8000,30);
    auto p=neutralParameters();p.model=echolab::Model::LoopSampler;p.loop_action=1;p.delay_ms=10;
    p.feedback=.7f;p.drive=.3f;p.modulation_enabled=true;p.drift_amount=.25f;p.mix=1;
    p.mod1_source=3;p.mod1_destination=11;p.mod1_amount=1;
    mapped.engine.setParameters(p);mapped.engine.reset();mapped.engine.setExternalModulation(1,0,0);
    p.mod1_destination=0;plain.engine.setParameters(p);plain.engine.reset();
    for(unsigned sample=0;sample<1000;++sample) {
        if(sample==100) {
            p.loop_action=3;plain.engine.setParameters(p);
            p.mod1_destination=11;mapped.engine.setParameters(p);
        }
        const float input=sample%19<3?.125f:0;
        float ml,mr,pl,pr;mapped.engine.processSample(input,-input,ml,mr);plain.engine.processSample(input,-input,pl,pr);
        near(ml,pl,0,"saved inactive Drift route preserves looper left record/overdub");
        near(mr,pr,0,"saved inactive Drift route preserves looper right record/overdub");
    }
}
void exact48kLongDelay() {
    Fixture f(48000,250);
    f.parameters(250,0.55f);
    float expected=.5f;
    for (unsigned i=0;i<120001;++i) {
        float l,r;
        f.engine.processSample(i == 0 ? 0.5f : 0.0f,0,l,r);
        if (i && i % 12000 == 0) {
            near(l,expected,0,"48k/250ms first and later repeats preserve exact analytical loop gain");
            expected *= 0.55f;
        } else near(l,0,0,"48k/250ms integer delay has no unintended fractional-delay smearing");
        near(r,0,0,"48k impulse remains in its input channel");
    }
}
void stereoMusicalRatios() {
    struct Case { unsigned leftFrames; float ratio; unsigned rightFrames; };
    const Case cases[]={{12,2.0f/3.0f,8},{24,1.0f/24.0f,1},{1,24,24},
                        {8,24,24},{8,1.0f/24.0f,1}};
    for (const auto& test : cases) {
        Fixture f(8000,3);
        auto p=neutralParameters();p.model=echolab::Model::StereoDelays;
        p.delay_ms=test.leftFrames*.125f;p.stereo_ratio=test.ratio;
        p.feedback=.5f;p.mix=1;p.modulation_enabled=false;
        f.engine.setParameters(p);f.engine.reset();
        near(f.engine.parameters().stereo_ratio,test.ratio,0,"musical stereo ratios retain exact triplet and full note-range ratios");
        for (unsigned n=0;n<=240;++n) {
            float l,r;f.engine.processSample(n == 0 ? .25f : 0,n == 0 ? -.125f : 0,l,r);
            const float expectedL=n && n%test.leftFrames == 0 ? .25f*std::pow(.5f,static_cast<float>(n/test.leftFrames-1)) : 0;
            const float expectedR=n && n%test.rightFrames == 0 ? -.125f*std::pow(.5f,static_cast<float>(n/test.rightFrames-1)) : 0;
            near(l,expectedL,1e-7f,"Stereo left echo train retains its own note duration across right-ratio and memory limits");
            near(r,expectedR,1e-7f,"Stereo right echo train follows exact musical ratios with independent one-sample and capacity clamps");
        }
    }
}
void stereoThreeAgainstTwoMusicalTiming() {
    constexpr unsigned fs=48000;
    for(unsigned bpm : {120u,100u}) {
        const unsigned left=fs*60/bpm,right=left*2/3,cycle=left*2;
        Fixture f(fs,700);
        auto p=neutralParameters();p.model=echolab::Model::StereoDelays;
        p.delay_ms=60000.0f/bpm;p.stereo_ratio=2.0f/3.0f;p.feedback=.5f;p.mix=1;p.modulation_enabled=false;
        f.engine.setParameters(p);f.engine.reset();
        unsigned leftPulses=0,rightPulses=0;
        for(unsigned n=0;n<=cycle*3;++n) {
            float l,r;f.engine.processSample(n == 0 ? .2f : 0,n == 0 ? -.1f : 0,l,r);
            const float expectedL=n && n%left == 0 ? .2f*std::pow(.5f,static_cast<float>(n/left-1)) : 0;
            const float expectedR=n && n%right == 0 ? -.1f*std::pow(.5f,static_cast<float>(n/right-1)) : 0;
            near(l,expectedL,0,"musical Stereo keeps left-quarter repeats at their own beat and feedback count");
            near(r,expectedR,0,"musical Stereo keeps right-quarter-triplet repeats separate at120 and100BPM");
            leftPulses+=l!=0;rightPulses+=r!=0;
        }
        check(leftPulses==6 && rightPulses==9,"three complete Stereo cycles contain two left and three right pulses each");
        f.engine.reset();
        for(unsigned n=0;n<=cycle;++n) {
            float l,r;f.engine.processSample(n == 0 ? .2f : 0,0,l,r);
            near(r,0,0,"independent Stereo feedback does not invent right excitation when only left input is connected");
        }
    }
}
void pingPongTopology() {
    for (const auto input : {0,1,2,3}) {
        Fixture f(8000,2);
        f.parameters(1,0.5f,1,echolab::Model::PingPong);
        // Both mono excitation and unequal/antiphase stereo are explicit policies.
        const float left = input == 0 || input == 1 || input == 3 ? 0.5f : 0.0f;
        const float right = input == 0 ? 0.5f : (input == 2 ? -0.25f : (input == 3 ? -0.5f : 0.0f));
        float gain = 0.5f * (left+right);
        for (unsigned n=0;n<320;++n) {
            float l,r;
            f.engine.processSample(n == 0 ? left : 0,n == 0 ? right : 0,l,r);
            float expectedL=0,expectedR=0;
            if (n && n % 8 == 0) {
                if ((n/8) % 2) expectedL=gain; else expectedR=gain;
                gain *= 0.5f;
            }
            near(l,expectedL,0,"PingPong mono-fold excitation alternates left on odd repeats");
            near(r,expectedR,0,"PingPong crossfeedback alternates right on even repeats");
        }
        f.parameters(1,0.5f,0.25f,echolab::Model::PingPong);
        float l,r;
        f.engine.processSample(1,-0.5f,l,r);
        near(l,0.75f,0,"PingPong dry left preserves its stereo input");
        near(r,-0.375f,0,"PingPong dry right preserves its stereo input");
    }
    check(static_cast<int>(echolab::Model::Digital) == 0 && static_cast<int>(echolab::Model::Analog) == 1 &&
          static_cast<int>(echolab::Model::Tube) == 2 && static_cast<int>(echolab::Model::PingPong) == 3 &&
          static_cast<int>(echolab::Model::MultiHead) == 4,"published model IDs remain stable");
}
double multiHeadImpulse(int n,double pole,double feedback,int length=24) {
    if (n < 0) return 0;
    // Closed-form expansion of H(z)=A(z)/(1-f*z^-24*A(z)), A=(1-p)/(1-p*z^-1).
    // This independent response has no circular buffer or sample recurrence.
    double sum=0;
    for (int loop=0;loop*length<=n;++loop) {
        const int remaining=n-loop*length;
        double choose=1;
        for (int j=1;j<=loop;++j) choose *= static_cast<double>(remaining+j)/j;
        sum += std::pow(feedback,loop) * std::pow(1-pole,loop+1) * choose * std::pow(pole,remaining);
    }
    return sum;
}
void multiHeadTopology() {
    Fixture f(8000,3);
    auto p=f.engine.parameters();
    p.model=echolab::Model::MultiHead;p.delay_ms=3;p.mix=1;p.feedback=0.6f;p.drive=0;p.tone_hz=1000;
    const double pole=std::exp(-2*3.14159265358979323846*1000/8000);
    for (float feedback : {0.0f,0.6f}) {
        p.feedback=feedback;f.engine.setParameters(p);f.engine.reset();
        for (int n=0;n<480;++n) {
            float l,r;
            f.engine.processSample(n == 0 ? 1.0f : 0,n == 2 ? -0.5f : 0,l,r);
            const double expected=(multiHeadImpulse(n-8,pole,feedback)+multiHeadImpulse(n-16,pole,feedback)+multiHeadImpulse(n-24,pole,feedback))/3;
            const double expectedRight=-0.5*(multiHeadImpulse(n-10,pole,feedback)+multiHeadImpulse(n-18,pole,feedback)+multiHeadImpulse(n-26,pole,feedback))/3;
            near(l,static_cast<float>(expected),2e-7f,"MultiHead three tap times/weights and longest-tap-only feedback match closed-form response");
            near(r,static_cast<float>(expectedRight),2e-7f,"MultiHead retains independent stereo lines and filter states");
        }
    }
    // At a one-sample overall delay, all three taps clamp to that same legal tap.
    p.delay_ms=0.01f;p.feedback=0.6f;f.engine.setParameters(p);f.engine.reset();
    for (int n=0;n<80;++n) {
        float l,r;f.engine.processSample(n == 0 ? 1.0f : 0,0,l,r);
        const double expected=n == 0 ? 0 : (1-pole)*std::pow(pole+0.6f*(1-pole),n-1);
        near(l,static_cast<float>(expected),2e-7f,"MultiHead collapsed minimum taps remain causal with bounded memory");
    }
    p.delay_ms=3;p.feedback=0;p.drive=1;f.engine.setParameters(p);f.engine.reset();
    float steadyL=0,steadyR=0;
    for (int n=0;n<800;++n) f.engine.processSample(.25f,.5f,steadyL,steadyR);
    check(steadyL>0 && steadyR>steadyL && steadyR<1.95f*steadyL,"MultiHead unity-gain soft saturation remains level dependent without preamplification");
}
void lowResolutionQuantization() {
    check(static_cast<int>(echolab::Model::LoRes) == 5 &&
          std::strcmp(echolab::modelName(echolab::Model::LoRes),"8bit") == 0,
          "8bit is the canonical name of published model ID5");
    Fixture f;
    f.parameters(1,0.5f,1,echolab::Model::LoRes);
    auto p=f.engine.parameters();p.bit_depth=8;f.engine.setParameters(p);f.engine.reset();
    for (unsigned n=0;n<128;++n) {
        float l,r;f.engine.processSample(n == 0 ? 0.02f : 0,n == 0 ? -0.02f : 0,l,r);
        float expected=0;
        if (n == 8) expected=2.0f/128.0f;else if (n == 16) expected=1.0f/128.0f;
        near(l,expected,0,"8-bit quantization is inside the feedback loop and sub-unity feedback loses its final LSB");
        near(r,-expected,0,"8-bit quantization is symmetric for signed input");
        near(l*128.0f,std::round(l*128.0f),0,"integer-delay 8-bit repeats remain on the quantizer grid");
    }
    p.feedback=0;f.engine.setParameters(p);f.engine.reset();
    for (unsigned n=0;n<=8;++n) {
        float l,r;f.engine.processSample(n == 0 ? 2.0f : 0,n == 0 ? -2.0f : 0,l,r);
        if (n == 8) {
            near(l,(127.0f/128)*echolab::safety::maximumCeiling,0,"linked headroom guard preserves quantizer stereo ratio");
            near(r,-echolab::safety::maximumCeiling,0,"headroom guard remains active when musical limiting is off");
            near(f.memory[0],127.0f/128,0,"8-bit positive stored endpoint clips to 127/128");
            near(f.memory[f.memory.size()/2],-1,0,"8-bit negative stored endpoint clips to -1");
        }
    }
    for (int bits : {4,8,16,24}) {
        p.bit_depth=bits;p.feedback=0.98f;f.engine.setParameters(p);f.engine.reset();
        for (unsigned n=0;n<160;++n) { float l,r;f.engine.processSample(0,0,l,r);check(l == 0 && r == 0,"quantizer silence is exactly zero at each supported depth"); }
    }
}
void fractionalSampleRateReduction() {
    Fixture f(8000,2);
    auto p=neutralParameters();p.model=echolab::Model::LoRes;p.delay_ms=.125f;p.mix=1;p.feedback=0;p.bit_depth=8;
    for (unsigned requested : {0U,2000U,3000U,8000U}) {
        p.sample_rate_hz=static_cast<float>(requested);f.engine.setParameters(p);f.engine.reset();
        const unsigned rate=requested ? requested : 8000;
        for (unsigned n=0;n<64;++n) {
            float l,r;const float input=static_cast<float>(n)/128.0f;
            f.engine.processSample(input,-input,l,r);
            unsigned captured=0;
            if (n) {
                const unsigned tick=(n-1)*rate/8000;
                captured=(tick*8000+rate-1)/rate;
            }
            const float expected=static_cast<float>(captured)/128.0f;
            near(l,expected,0,"LoRes uses fractional-rate zero-order-held quantized writes");
            near(r,-expected,0,"LoRes rate clock is shared coherently by both channels");
        }
    }
}
void decliningLoResPhraseEnvelope() {
    Fixture swept(8000,2),fixed(8000,2);
    auto p=neutralParameters();p.model=echolab::Model::LoRes;p.delay_ms=.125f;p.feedback=0;p.mix=1;
    p.bit_depth=24;p.sample_rate_hz=4000;p.lores_rate_end_hz=500;p.lores_rate_decay_ms=100;p.lores_rate_envelope_amount=1;
    swept.engine.setParameters(p);swept.engine.reset();p.lores_rate_envelope_amount=0;
    fixed.engine.setParameters(p);fixed.engine.reset();
    unsigned early=0,late=0,fixedEarly=0,fixedLate=0;
    float last=0,lastFixed=0;
    for (unsigned n=0;n<1000;++n) {
        const float input=.2f+static_cast<float>(n)*.0001f;
        float l,r,fl,fr;swept.engine.processSample(input,-input,l,r);fixed.engine.processSample(input,-input,fl,fr);
        if (n>=1 && n<81) {early+=l != last;fixedEarly+=fl != lastFixed;}
        if (n>=801 && n<881) {late+=l != last;fixedLate+=fl != lastFixed;}
        near(r,-l,0,"LoRes declining rate keeps the stereo hold clock coherent");
        last=l;lastFixed=fl;
    }
    check(early>=36 && early<=40 && late>=17 && late<=20,"LoRes phrase envelope decreases Fe exponentially from4000Hz toward500Hz over100ms");
    check(fixedEarly == 40 && fixedLate == 40,"zero LoRes envelope amount restores constant sample-rate reduction");
    // Silence rearms onset detection while the rate continues to its endpoint.
    for (unsigned n=0;n<5000;++n) {float l,r;swept.engine.processSample(0,0,l,r);}
    unsigned restarted=0;last=0;
    for (unsigned n=0;n<81;++n) {
        float l,r;swept.engine.processSample(.2f+static_cast<float>(n)*.0001f,0,l,r);
        if (n) restarted+=l != last;last=l;
    }
    check(restarted>=36 && restarted<=41,"a fresh phrase retriggers the high LoRes start rate after silence");
    Fixture fullA(8000,2),fullB(8000,2);p.sample_rate_hz=0;p.lores_rate_envelope_amount=1;
    fullA.engine.setParameters(p);fullA.engine.reset();p.lores_rate_envelope_amount=0;
    fullB.engine.setParameters(p);fullB.engine.reset();
    for (unsigned n=0;n<500;++n) {
        const float input=.3f*std::sin(.13f*n);float a,b,c,d;
        fullA.engine.processSample(input,-input,a,b);fullB.engine.processSample(input,-input,c,d);
        check(a == c && b == d,"full-Fs sentinel bypasses LoRes rate envelopes exactly");
    }
    // At the most demanding Fs/time combination a float pole near1 loses
    // precision. Count audible hold updates against the closed-form integral,
    // rather than duplicating the engine's envelope recurrence.
    Fixture longDecay(384000,2);p=neutralParameters();p.model=echolab::Model::LoRes;
    p.delay_ms=1000.0f/384000;p.mix=1;p.feedback=0;p.bit_depth=24;p.sample_rate_hz=8000;
    p.lores_rate_end_hz=1200;p.lores_rate_decay_ms=10000;p.lores_rate_envelope_amount=1;
    longDecay.engine.setParameters(p);longDecay.engine.reset();
    constexpr unsigned frames=384000;unsigned captures=0;last=0;
    for (unsigned n=0;n<=frames;++n) {
        float l,r;longDecay.engine.processSample(.2f+static_cast<float>(n)*1e-7f,0,l,r);
        captures+=l != last;last=l;
    }
    const double q=std::exp(-1.0/(10*384000));
    const double sum=(1200.0/384000)*(frames-1)+(6800.0/384000)*(1-std::pow(q,frames-1))/(1-q);
    check(std::fabs(static_cast<double>(captures)-(1+std::floor(sum)))<=2,
          "LoRes10second decay at384kHz follows the analytical clock integral without pole-rounding slowdown");
}
void oneWaySweepDirections() {
    Fixture down(8000,2),up(8000,2);
    auto p=neutralParameters();p.model=echolab::Model::Sweep;p.delay_ms=.125f;p.feedback=0;p.mix=1;
    p.tone_hz=1600;p.mod_rate_hz=1;
    down.engine.setParameters(p);down.engine.reset();p.model=echolab::Model::SweepUp;
    up.engine.setParameters(p);up.engine.reset();
    const float highPole=std::exp(-2*3.14159265358979323846f*1600/8000);
    const float lowPole=std::exp(-2*3.14159265358979323846f*200/8000);
    float previousDown=0,previousUp=0,previousInput=0,lastDownPole=0,lastUpPole=1;
    for (unsigned n=0;n<10002;++n) {
        const float input=n%2 ? -.2f : .2f;
        float dl,dr,ul,ur;down.engine.processSample(input,0,dl,dr);up.engine.processSample(input,0,ul,ur);
        if (n == 1 || n == 2001 || n == 4001 || n == 6001 || n == 8001 || n == 10001) {
            const float dp=(dl-previousInput)/(previousDown-previousInput);
            const float upLowpass=previousInput-ul;
            const float upp=(upLowpass-previousInput)/(previousUp-previousInput);
            check(dp>=lastDownPole-2e-6f && upp<=lastUpPole+2e-6f,"SweepDown cutoff falls while SweepUp cutoff rises without a return sweep");
            near(dp+upp,highPole+lowPole,3e-6f,"Sweep Up and Down are mirrored one-way pole trajectories");
            if (n>=8001) {
                near(dp,lowPole,2e-6f,"SweepDown holds its low endpoint after one cycle");
                near(upp,highPole,2e-6f,"SweepUp holds its high endpoint after one cycle");
            }
            lastDownPole=dp;lastUpPole=upp;
        }
        previousDown=dl;previousUp=previousInput-ul;previousInput=input;
    }
    for (unsigned n=0;n<4000;++n) {float a,b;down.engine.processSample(0,0,a,b);up.engine.processSample(0,0,a,b);}
    float d0,r,u0;down.engine.processSample(.2f,0,d0,r);up.engine.processSample(.2f,0,u0,r);
    down.engine.processSample(-.2f,0,d0,r);up.engine.processSample(-.2f,0,u0,r);
    near(d0,.2f*(1-highPole),2e-6f,"fresh input restarts SweepDown at its high low-pass cutoff");
    near(u0,.2f*lowPole,2e-6f,"fresh input restarts SweepUp at its low high-pass cutoff");
    check(static_cast<int>(echolab::Model::ExciterEcho) == 17 && static_cast<int>(echolab::Model::SweepUp) == 18 &&
          static_cast<int>(echolab::Model::TubeA04) == 19 && echolab::modelCount == 20,
          "SweepUp appends model18 without renumbering published algorithms");
}
void sweepFilterSpectra() {
    constexpr double pi=3.14159265358979323846;
    for (float fs : {8000.0f,48000.0f,96000.0f}) for (int voicing : {1,2,3}) {
        double gains[2][2]{};
        for (unsigned model=0;model<2;++model) for (unsigned band=0;band<2;++band) {
            Fixture f(fs,2);
            auto p=neutralParameters();p.model=model ? echolab::Model::SweepUp : echolab::Model::Sweep;
            p.delay_ms=1000/fs;p.feedback=0;p.mix=1;p.tone_hz=1000;p.voicing=voicing;p.modulation_enabled=false;
            f.engine.setParameters(p);f.engine.reset();
            const double frequency=band ? .35*fs : 50;
            const unsigned warmup=static_cast<unsigned>(fs/10),frames=static_cast<unsigned>(fs/5);
            double inputEnergy=0,outputEnergy=0;
            for (unsigned n=0;n<warmup+frames;++n) {
                const float input=.1f*static_cast<float>(std::sin(2*pi*frequency*n/fs));
                float l,r;f.engine.processSample(input,-input,l,r);
                near(r,-l,1e-7f,"Sweep filters retain independent symmetric stereo channels");
                if (n>=warmup) {inputEnergy+=input*input;outputEnergy+=l*l;}
            }
            gains[model][band]=std::sqrt(outputEnergy/inputEnergy);
        }
        check(gains[0][0]>.94 && gains[0][1]<.45,"SweepDown passes bass and attenuates treble at every voicing and Fs");
        check(gains[1][0]<.06 && gains[1][1]>.55,"SweepUp attenuates bass and passes treble at every voicing and Fs");
        Fixture dc(fs,2);auto p=neutralParameters();p.model=echolab::Model::SweepUp;p.delay_ms=1000/fs;
        p.feedback=.45f;p.mix=1;p.tone_hz=1000;p.voicing=voicing;p.modulation_enabled=false;
        dc.engine.setParameters(p);dc.engine.reset();
        float l=0,r=0;
        for (unsigned n=0;n<static_cast<unsigned>(fs/10);++n) dc.engine.processSample(.2f,-.1f,l,r);
        check(std::fabs(l)<1e-5f && std::fabs(r)<1e-5f,"SweepUp removes steady DC with feedback rather than leaving a low-pass response");
    }
}
void feedbackAboveUnityAndLimiter() {
    Fixture f(8000,2);
    auto p=neutralParameters();p.delay_ms=.125f;p.feedback=1.5f;p.mix=1;
    f.engine.setParameters(p);f.engine.reset();
    float peak=0;
    for (unsigned n=0;n<4000;++n) {
        float l,r;f.engine.processSample(n == 0 ? .1f : 0,0,l,r);
        peak=std::max(peak,std::fabs(l));
        check(std::isfinite(l) && std::fabs(l) <= echolab::safety::maximumCeiling,"musical regeneration remains finite under mandatory output headroom");
    }
    check(peak > .12f,"super-unity regeneration grows from the original excitation");
    check(f.engine.parameters().feedback == 1.5f,"feedback above unity is retained rather than silently clamped below1");
    for (float sample:f.memory) check(std::fabs(sample) <= 1.35f,"softly regenerated delay memory avoids the old four-unit hard guard on a normalized excitation");
    p.limiter_enabled=true;p.limiter_ceiling=.7f;f.engine.setParameters(p);f.engine.reset();
    for (unsigned n=0;n<120;++n) {
        float l,r;f.engine.processSample(n == 0 ? .1f : 0,0,l,r);
        check(std::isfinite(l) && std::fabs(l) <= .7f && std::fabs(r) <= .7f,"output brickwall remains independent of the growing feedback loop");
    }
    echolab::Engine fallback;
    float l,r;fallback.processSample(4,2,l,r);
    near(l,echolab::safety::maximumCeiling,2e-7f,"unprepared fallback obeys mandatory output headroom");
    near(r,.5f*echolab::safety::maximumCeiling,2e-7f,"output limiter links stereo gain and preserves channel ratio");
    const echolab::Parameters defaults;
    check(defaults.limiter_enabled && defaults.feedback == .28f && defaults.mix == .39f && defaults.drive == .4f && defaults.tone_hz == 3500 &&
          defaults.mod_depth_ms == 6 && defaults.mod_rate_hz == 1 && defaults.sample_rate_hz == 8000 &&
          defaults.feedback_lowpass_hz == 12000 && defaults.feedback_highpass_hz == 80 && defaults.drift_amount == .25f && defaults.limiter_release_ms == 150 &&
          defaults.lores_rate_end_hz == 1200 && defaults.lores_rate_decay_ms == 1500 && defaults.lores_rate_envelope_amount == 1 && defaults.swell_ms == 250 && defaults.digital_rack_rate_hz == 16000 &&
          defaults.lores_cutoff_hz == 2000 && defaults.voicing == 0 && defaults.modulation_enabled && defaults.exciter_amount == .1f && defaults.exciter_tune_hz == 4000 &&
          !defaults.exciter_highpass_enabled && !defaults.wet_tone_enabled,
          "production defaults combine coloured first pass, band-limited decay, slower LoRes and fixed output headroom");
    for (const auto model:allModels) {
        Fixture guarded(8000,3);p=neutralParameters();p.model=model;p.delay_ms=3;p.feedback=2;p.mix=1;p.limiter_enabled=true;
        guarded.engine.setParameters(p);guarded.engine.reset();
        for (unsigned n=0;n<240;++n) {
            if (model == echolab::Model::LoopSampler && n == 24) {p.loop_action=3;guarded.engine.setParameters(p);}
            guarded.engine.processSample(n%3 == 0 ? 20.0f : -10.0f,8,l,r);
            check(std::fabs(l) <= .95f && std::fabs(r) <= .95f,"all models, including record/overdub and reverse, obey the enabled brickwall");
        }
        for (float sample:guarded.memory) check(std::fabs(sample) <= 4,"every model guards all shared audio-buffer writes above unity feedback");
    }
}
void exciterInFeedbackOnly() {
    Fixture excited,plain;
    auto p=neutralParameters();p.model=echolab::Model::ExciterEcho;p.delay_ms=1;p.feedback=.8f;p.mix=1;
    p.drive=.7f;p.exciter_amount=.8f;p.exciter_tune_hz=1200;
    excited.engine.setParameters(p);excited.engine.reset();
    p.model=echolab::Model::Digital;plain.engine.setParameters(p);plain.engine.reset();
    bool laterChanged=false;
    for (unsigned n=0;n<240;++n) {
        const float input=n == 0 ? .2f : 0;
        float el,er,dl,dr;excited.engine.processSample(input,-input,el,er);plain.engine.processSample(input,-input,dl,dr);
        if (n<16) check(el == dl && er == dr,"ExciterEcho leaves dry excitation and the first delay pass unchanged");
        else laterChanged=laterChanged || std::fabs(el-dl)>1e-5f || std::fabs(er-dr)>1e-5f;
    }
    check(laterChanged,"ExciterEcho changes later repeats through its original harmonic loop processor");
    p.model=echolab::Model::ExciterEcho;p.exciter_amount=0;excited.engine.setParameters(p);excited.engine.reset();
    p.model=echolab::Model::Digital;plain.engine.setParameters(p);plain.engine.reset();
    for (unsigned n=0;n<240;++n) {
        const float input=.1f*std::sin(static_cast<float>(n)*.13f);
        float el,er,dl,dr;excited.engine.processSample(input,-input,el,er);plain.engine.processSample(input,-input,dl,dr);
        check(el == dl && er == dr,"zero exciter amount is an exact identity inside the shared feedback path");
    }
}
void optionalExciterHighpass() {
    check(!echolab::Parameters{}.exciter_highpass_enabled,"legacy Exciter presets leave the new return high-pass disabled");
    constexpr unsigned fs=48000,delay=12000,burst=9600;
    double ratios[2]{};unsigned band=0;
    for(double frequency : {50.0,2000.0}) {
        Fixture filtered(fs,250),unfiltered(fs,250),legacy(fs,250);
        auto p=neutralParameters();p.model=echolab::Model::ExciterEcho;p.voicing=1;p.delay_ms=250;p.feedback=.5f;p.mix=1;
        p.exciter_amount=0;p.feedback_highpass_hz=300;p.feedback_lowpass_hz=1000;p.modulation_enabled=false;
        unfiltered.engine.setParameters(p);unfiltered.engine.reset();
        p.exciter_highpass_enabled=true;filtered.engine.setParameters(p);filtered.engine.reset();
        p.exciter_highpass_enabled=false;p.feedback_highpass_hz=0;legacy.engine.setParameters(p);legacy.engine.reset();
        double filteredEnergy=0,plainEnergy=0;
        for(unsigned n=0;n<delay*2+burst;++n) {
            const float input=n<burst ? .1f*static_cast<float>(std::sin(2*3.14159265358979323846*frequency*n/fs)) : 0;
            float a,b,c,d,e,g;filtered.engine.processSample(input,-input,a,b);
            unfiltered.engine.processSample(input,-input,c,d);legacy.engine.processSample(input,-input,e,g);
            check(c==e && d==g,"disabled A04 Exciter high-pass preserves the historical response exactly");
            if(n<delay*2) check(a==c && b==d,"optional Exciter return high-pass leaves the complete first echo unchanged");
            if(n>=delay*2+burst/2) {filteredEnergy+=a*a;plainEnergy+=c*c;}
        }
        ratios[band++]=std::sqrt(filteredEnergy/plainEnergy);
    }
    check(ratios[0]>.1 && ratios[0]<.25 && ratios[1]>.9,
          "A04 Exciter optional300Hz return high-pass rejects bass while retaining treble without enabling a low-pass");
    Fixture dc(fs,10);auto p=neutralParameters();p.model=echolab::Model::ExciterEcho;p.voicing=1;
    p.delay_ms=10;p.feedback=.5f;p.mix=1;p.exciter_amount=0;p.feedback_highpass_hz=300;p.exciter_highpass_enabled=true;
    dc.engine.setParameters(p);dc.engine.reset();float l=0,r=0;
    for(unsigned n=0;n<1440;++n) dc.engine.processSample(n<480 ? .1f : 0,n<480 ? -.05f : 0,l,r);
    check(std::fabs(l)<1e-7f && std::fabs(r)<1e-7f,"optional Exciter high-pass removes a held DC segment from the second return");
    for(auto model : allModels) if(model!=echolab::Model::ExciterEcho) {
        Fixture enabled,disabled;p=neutralParameters();p.model=model;p.voicing=1;p.delay_ms=1;p.mix=1;p.feedback=.4f;p.feedback_highpass_hz=300;
        disabled.engine.setParameters(p);disabled.engine.reset();p.exciter_highpass_enabled=true;
        enabled.engine.setParameters(p);enabled.engine.reset();
        for(unsigned n=0;n<100;++n) {
            float a,b,c,d;const float input=.1f*std::sin(n*.13f);
            enabled.engine.processSample(input,-input,a,b);disabled.engine.processSample(input,-input,c,d);
            check(a==c && b==d,"Exciter high-pass opt-in has no effect on any other model");
        }
    }
}
void selectableHeadPattern() {
    Fixture f;
    auto p=neutralParameters();p.model=echolab::Model::MultiHead;p.delay_ms=4;p.mix=1;p.feedback=.6f;p.tone_hz=1000;p.tap_pattern=1;
    f.engine.setParameters(p);f.engine.reset();
    const double pole=std::exp(-2*3.14159265358979323846*1000/8000);
    for (int n=0;n<400;++n) {
        float l,r;f.engine.processSample(n == 0 ? 1.0f : 0,0,l,r);
        double expected=0;
        for (int head=1;head<=4;++head) expected+=multiHeadImpulse(n-8*head,pole,.6f,32)*.25;
        near(l,static_cast<float>(expected),2e-7f,"selectable four-head pattern preserves normalized gains and longest-head-only feedback");
        near(r,0,0,"head patterns preserve independent stereo lines");
    }
}
void polyrhythmPulseGrids() {
    static_assert(echolab::multitap::patternCount==11 && echolab::multitap::tapCapacity==8,
                  "polyrhythms append three patterns using eight static tap slots");
    for(unsigned index=0;index<echolab::multitap::patternCount;++index) {
        const auto& pattern=echolab::multitap::pattern(index);
        float previous=0,total=0;
        check(pattern.count>0 && pattern.count<=echolab::multitap::tapCapacity,"tap count stays within fixed storage");
        for(unsigned head=0;head<pattern.count;++head) {
            check(pattern.taps[head].offset>previous && pattern.taps[head].offset<=1 && pattern.taps[head].gain>0,
                  "each pattern has ordered, distinct, positive onsets inside one cycle");
            previous=pattern.taps[head].offset;total+=pattern.taps[head].gain;
        }
        near(total,1,2e-7f,"all tap patterns retain unity total wet gain including simultaneous pulse accents");
    }
    struct Rhythm { int id,first,second; };
    const Rhythm rhythms[]={{8,3,2},{9,4,3},{10,5,4}};
    constexpr int cycle=120;
    for(float fs : {8000.0f,48000.0f}) for(const auto& rhythm : rhythms) for(float feedback : {0.0f,.6f}) {
        Fixture f(fs,cycle*1000/fs);
        auto p=neutralParameters();p.model=echolab::Model::MultiHead;p.tap_pattern=rhythm.id;
        p.delay_ms=cycle*1000/fs;p.mix=1;p.feedback=feedback;p.tone_hz=1000;
        f.engine.setParameters(p);f.engine.reset();
        const double pole=std::exp(-2*3.14159265358979323846*1000/fs);
        const auto response=[&](int sample) {
            double sum=0;
            // Two independently generated uniform pulse trains; their common
            // endpoint is intentionally counted twice. No pattern table reads.
            for(int grid : {rhythm.first,rhythm.second}) for(int pulse=1;pulse<=grid;++pulse)
                sum+=multiHeadImpulse(sample-cycle*pulse/grid,pole,feedback,cycle)/(rhythm.first+rhythm.second);
            return sum;
        };
        audioGuard=true;
        for(int n=0;n<cycle*5;++n) {
            float l,r;f.engine.processSample(n == 0 ? .2f : 0,n == 3 ? -.1f : 0,l,r);
            near(l,static_cast<float>(.2*response(n)),1e-7f,"polyrhythm merges both complete pulse grids and repeats their common cycle");
            near(r,static_cast<float>(-.1*response(n-3)),1e-7f,"polyrhythm keeps independent stereo timing and coincident-pulse gain");
        }
        audioGuard=false;
    }
}
void optionalWetTone() {
    check(!echolab::Parameters{}.wet_tone_enabled,"legacy presets leave the optional wet-tone filter bypassed");
    constexpr double pi=3.14159265358979323846;
    for(float fs : {8000.0f,48000.0f,96000.0f}) {
        Fixture filtered(fs,20),legacy(fs,20);
        auto p=neutralParameters();p.delay_ms=8000/fs;p.feedback=0;p.mix=.25f;p.tone_hz=20000;p.modulation_enabled=false;
        legacy.engine.setParameters(p);legacy.engine.reset();p.wet_tone_enabled=true;
        filtered.engine.setParameters(p);filtered.engine.reset();
        const double pole=std::exp(-2*pi*std::min(20000.0f,.45f*fs)/fs);
        for(int n=0;n<128;++n) {
            float a,b,c,d;filtered.engine.processSample(n == 0 ? .2f : 0,n == 2 ? -.1f : 0,a,b);
            legacy.engine.processSample(n == 0 ? .2f : 0,n == 2 ? -.1f : 0,c,d);
            const double wetL=n>=8 ? .05*(1-pole)*std::pow(pole,n-8) : 0;
            const double wetR=n>=10 ? -.025*(1-pole)*std::pow(pole,n-10) : 0;
            near(a,static_cast<float>((n == 0 ? .15 : 0)+wetL),1e-7f,"optional wet LP matches the analytical impulse at each Fs without filtering dry input");
            near(b,static_cast<float>((n == 2 ? -.075 : 0)+wetR),1e-7f,"optional wet LP has independent stereo states and sample-rate-bounded cutoff");
            near(c,n == 0 ? .15f : (n == 8 ? .05f : 0),1e-7f,"disabled wet tone retains the original analytical digital response");
            near(d,n == 2 ? -.075f : (n == 10 ? -.025f : 0),1e-7f,"disabled wet tone retains original independent right timing");
        }
        p.mix=0;filtered.engine.setParameters(p);filtered.engine.reset();
        for(unsigned n=0;n<128;++n) {
            const float input=.2f*std::sin(n*.17f);float l,r;
            filtered.engine.processSample(input,-input,l,r);
            check(l==input && r==-input,"zero Mix keeps dry audio bit-exact with wet filtering enabled");
        }
        p.mix=1;p.tone_hz=1000;filtered.engine.setParameters(p);filtered.engine.reset();
        float l=0,r=0;
        for(unsigned n=0;n<static_cast<unsigned>(fs/10);++n) filtered.engine.processSample(.2f,-.1f,l,r);
        near(l,.2f,2e-7f,"wet-tone low-pass retains unity DC gain");
        near(r,-.1f,2e-7f,"wet-tone DC gain is independent in both channels");
    }
    // Independently convolve each model's unfiltered wet render with the
    // closed-form one-pole impulse, then add its untouched dry signal.
    for(auto model : allModels) {
        Fixture filtered,plain;
        auto p=neutralParameters();p.model=model;p.delay_ms=1;p.feedback=.4f;p.mix=1;p.tone_hz=1000;p.modulation_enabled=false;
        plain.engine.setParameters(p);plain.engine.reset();p.mix=.25f;p.wet_tone_enabled=true;
        filtered.engine.setParameters(p);filtered.engine.reset();
        constexpr unsigned frames=96;float wetL[frames]{},wetR[frames]{};
        const double pole=std::exp(-2*pi*1000/8000);
        for(unsigned n=0;n<frames;++n) {
            const float input=n == 0 ? .2f : (n == 11 ? -.1f : 0);float l,r;
            plain.engine.processSample(input,-.5f*input,wetL[n],wetR[n]);
            filtered.engine.processSample(input,-.5f*input,l,r);
            double expectedL=.75*input,expectedR=-.375*input;
            for(unsigned k=0;k<=n;++k) {
                const double coefficient=.25*(1-pole)*std::pow(pole,n-k);
                expectedL+=coefficient*wetL[k];expectedR+=coefficient*wetR[k];
            }
            near(l,static_cast<float>(expectedL),1e-7f,"every model applies optional Tone only to its wet output before mixing");
            near(r,static_cast<float>(expectedR),1e-7f,"wet-tone placement preserves stereo dry gain across every model branch");
        }
        check(filtered.memory==plain.memory,"wet-tone output filtering never changes delay, feedback or looper capture memory");
    }
    Fixture reset;
    auto p=neutralParameters();p.delay_ms=1;p.feedback=0;p.mix=1;p.tone_hz=1000;p.wet_tone_enabled=true;p.modulation_enabled=false;
    reset.engine.setParameters(p);reset.engine.reset();float l=0,r=0;
    for(unsigned n=0;n<200;++n) reset.engine.processSample(.2f,0,l,r);
    const float first=.2f*(1-static_cast<float>(std::exp(-2*pi*1000/8000)));
    p.model=echolab::Model::DigitalMod;reset.engine.setParameters(p);reset.engine.processSample(0,0,l,r);
    near(l,first,1e-7f,"changing model clears optional wet-tone history without clearing shared audio memory");
    p.voicing=1;reset.engine.setParameters(p);reset.engine.processSample(0,0,l,r);
    near(l,first,1e-7f,"changing voicing clears optional wet-tone history");
    reset.engine.reset();reset.engine.processSample(0,0,l,r);
    check(l==0 && r==0,"reset clears optional wet-tone history and audio memory");
}
void liveLooperRecordUsesConfiguredTime() {
    for (float cv : {0.0f,1.0f}) {
        // Preparation starts at350ms. A live host changes Time before the first
        // Record callback without calling reset, just as the NT adapter does.
        Fixture f(8000,500);
        // Establish a fully wet path before the live Time change so this timing
        // fixture does not depend on the factory Mix or its control smoothing.
        auto p=neutralParameters();p.mix=1;f.engine.setParameters(p);f.engine.reset();
        p.model=echolab::Model::LoopSampler;p.delay_ms=2;p.feedback=0;
        p.mod1_source=3;p.mod1_destination=1;p.mod1_amount=1;
        f.engine.setParameters(p);f.engine.setExternalModulation(cv,0,0);
        const unsigned period=cv == 0 ? 16 : 24;
        for (unsigned n=0;n<=period*3;++n) {
            float l,r;f.engine.processSample(n == 0 ? .2f : 0,0,l,r);
            if (n) near(l,n%period == 0 ? .2f : 0,0,"live initial looper Record latches configured Time plus current CV rather than the old delay glide");
            near(r,0,0,"live looper duration change preserves the independent silent channel");
        }
    }
}
void feedbackLowpassPlacementAndSlope() {
    constexpr float fs=48000;
    constexpr unsigned time=48;
    for (const auto model:{echolab::Model::StereoDelays,echolab::Model::DigitalRack}) {
        Fixture filtered(fs,10),bypass(fs,10);
        auto p=neutralParameters();p.model=model;p.delay_ms=1;p.stereo_ratio=1;p.feedback=.5f;p.mix=1;
        p.feedback_lowpass_hz=12000;filtered.engine.setParameters(p);filtered.engine.reset();
        p.feedback_lowpass_hz=0;bypass.engine.setParameters(p);bypass.engine.reset();
        const float cutoff=model == echolab::Model::DigitalRack ? 6000.0f : 12000.0f;
        const float pole=std::exp(-2*3.14159265358979323846f*cutoff/fs);
        bool darker=false;
        for (unsigned n=0;n<time*3;++n) {
            float l,r,a,b;
            filtered.engine.processSample(n == 0 ? .5f : 0,n == 0 ? -.25f : 0,l,r);
            bypass.engine.processSample(n == 0 ? .5f : 0,n == 0 ? -.25f : 0,a,b);
            if (n<time*2) check(l == a && r == b,"feedback lowpass preserves first echo and startup rather than filtering input or wet output");
            if (n>=time*2) {
                const float expected=.25f*(1-pole)*std::pow(pole,static_cast<float>(n-time*2));
                near(l,expected,3e-7f,"feedback lowpass second echo matches the one-pole impulse with model-specific DigitalRack cutoff");
                near(r,-.5f*expected,3e-7f,"feedback filter has independent left/right state and unity relative gain");
                darker=darker || std::fabs(l-a)>1e-3f;
            }
        }
        check(darker,"feedback filtering acts on later repeats rather than being an inactive control");
    }
    double gains[2]{};
    unsigned index=0;
    for (double frequency:{4000.0,8000.0}) {
        echolab::color::FeedbackLowpass filter;filter.prepare(192000,1000);filter.reset();
        double inEnergy=0,outEnergy=0;
        for (unsigned n=0;n<38400;++n) {
            const float input=.1f*static_cast<float>(std::sin(2*3.14159265358979323846*frequency*n/192000));
            const float output=filter.process(input,0);
            if (n>=19200) {inEnergy+=static_cast<double>(input)*input;outEnergy+=static_cast<double>(output)*output;}
        }
        gains[index++]=std::sqrt(outEnergy/inEnergy);
    }
    const double octaveLoss=20*std::log10(gains[0]/gains[1]);
    check(octaveLoss>5.5 && octaveLoss<6.1,"prepared first-order feedback filter approaches6dB per octave above cutoff below Nyquist");
    Fixture control;
    auto p=neutralParameters();p.feedback_lowpass_hz=20000;control.engine.setParameters(p);
    check(control.engine.parameters().feedback_lowpass_hz == 20000,"nominal feedback cutoff remains explicit when active Fs limits its effective value");
}
void feedbackHighpassPlacementAndSlope() {
    constexpr float fs=48000;
    constexpr unsigned time=4800;
    Fixture filtered(fs,110),bypass(fs,110);
    auto p=neutralParameters();p.model=echolab::Model::StereoDelays;p.delay_ms=100;p.stereo_ratio=1;p.feedback=.5f;p.mix=1;
    p.feedback_highpass_hz=2000;filtered.engine.setParameters(p);filtered.engine.reset();
    p.feedback_highpass_hz=0;bypass.engine.setParameters(p);bypass.engine.reset();
    const double pole=std::exp(-2*3.14159265358979323846*2000/fs);
    const double first=.5*(1+pole);
    double real[2]{},imag[2]{};
    for (unsigned n=0;n<3*time;++n) {
        const float input=n == 0 ? .5f : 0;
        float l,r,a,b;filtered.engine.processSample(input,-.5f*input,l,r);bypass.engine.processSample(input,-.5f*input,a,b);
        if (n<2*time) check(l == a && r == b,"feedback HP leaves dry and first repeat exactly unchanged");
        else {
            const unsigned k=n-2*time;
            const double h=k == 0 ? first : first*(pole-1)*std::pow(pole,static_cast<double>(k-1));
            near(l,static_cast<float>(.25*h),2e-7f,"feedback-only HP has the independent normalized one-pole impulse response");
            near(r,static_cast<float>(-.125*h),1e-7f,"feedback HP preserves independent stereo histories");
            for (unsigned i=0;i<2;++i) {
                const double phase=2*3.14159265358979323846*(i == 0 ? 100 : 200)*k/fs;
                real[i]+=l*std::cos(phase);imag[i]-=l*std::sin(phase);
            }
        }
    }
    const double low=std::sqrt(real[0]*real[0]+imag[0]*imag[0]);
    const double high=std::sqrt(real[1]*real[1]+imag[1]*imag[1]);
    const double octaveRise=20*std::log10(high/low);
    check(octaveRise>5.8 && octaveRise<6.1,"feedback highpass rises approximately6dB per octave below its cutoff");
    Fixture dc(fs,110);p.feedback_highpass_hz=80;dc.engine.setParameters(p);dc.engine.reset();
    float tail=0;
    for (unsigned n=0;n<10*time;++n) {float l,r;dc.engine.processSample(n<time ? .2f : 0,0,l,r);if (n>9*time) tail=std::max(tail,std::fabs(l));}
    check(tail<.002f,"feedback highpass removes a held DC excitation from later repeats");
    for (float fb:{.5f,1.0f}) {
        Fixture a,b;p=neutralParameters();p.model=echolab::Model::Digital;p.delay_ms=1;p.feedback=fb;p.mix=1;
        a.engine.setParameters(p);a.engine.reset();p.feedback_highpass_hz=2000;p.feedback_lowpass_hz=1000;
        b.engine.setParameters(p);b.engine.reset();
        for (unsigned n=0;n<2000;++n) {
            float al,ar,bl,br;const float input=n == 0 ? .2f : 0;
            a.engine.processSample(input,-input,al,ar);b.engine.processSample(input,-input,bl,br);
            check(al == bl && ar == br,"Digital remains an exact analytic control through100% feedback regardless of return-colour settings");
        }
    }
    Fixture coloured(8000,2),plain(8000,2);p=neutralParameters();p.model=echolab::Model::Digital;
    p.delay_ms=.125f;p.feedback=1;p.mix=1;p.feedback_highpass_hz=2000;p.feedback_lowpass_hz=1000;
    coloured.engine.setParameters(p);coloured.engine.reset();p.feedback_highpass_hz=p.feedback_lowpass_hz=0;
    plain.engine.setParameters(p);plain.engine.reset();
    for (unsigned n=0;n<1000;++n) {
        float a,b,c,d;const float input=.001f*std::sin(.3f*n);
        coloured.engine.processSample(input,0,a,b);plain.engine.processSample(input,0,c,d);
        near(a,c,0,"background Digital filter updates preserve the exact unity-feedback reference");
    }
    auto cp=coloured.engine.parameters(),pp=plain.engine.parameters();cp.feedback=pp.feedback=1.0001f;
    coloured.engine.setParameters(cp);plain.engine.setParameters(pp);
    float maximumDifference=0;
    for (unsigned n=0;n<100;++n) {
        float a,b,c,d;coloured.engine.processSample(0,0,a,b);plain.engine.processSample(0,0,c,d);
        maximumDifference=std::max(maximumDifference,std::fabs(a-c));
    }
    check(maximumDifference<.0002f,"Digital return filters enter continuously when feedback crosses100% instead of switching fully on");
}
void rackRateIsModelSpecific() {
    for (const auto model:allModels) {
        if (model == echolab::Model::DigitalRack) continue;
        Fixture a(48000,20),b(48000,20);auto p=neutralParameters();p.model=model;p.delay_ms=2;p.feedback=.6f;p.mix=.5f;
        p.sample_rate_hz=8000;p.lores_rate_envelope_amount=1;p.digital_rack_rate_hz=0;
        a.engine.setParameters(p);a.engine.reset();p.digital_rack_rate_hz=16000;
        b.engine.setParameters(p);b.engine.reset();
        for (unsigned n=0;n<2000;++n) {
            const float input=.2f*std::sin(.17f*n);float al,ar,bl,br;
            a.engine.processSample(input,-.5f*input,al,ar);b.engine.processSample(input,-.5f*input,bl,br);
            check(al == bl && ar == br,"Rack Fe control has no audible or state effect on any other model, including LoRes");
        }
    }
    for (float fs:{8000.0f,48000.0f,384000.0f}) {
        Fixture f(fs,2);auto p=neutralParameters();p.model=echolab::Model::DigitalRack;
        for (float rate:{0.0f,1.0f,16000.0f,999999.0f,std::numeric_limits<float>::quiet_NaN()}) {
            p.digital_rack_rate_hz=rate;f.engine.setParameters(p);
            const float effective=f.engine.parameters().digital_rack_rate_hz;
            check(std::isfinite(effective) && (effective == 0 || (effective>=1000 && effective<=fs)),
                  "Rack Fe sanitization preserves FullFs sentinel and bounds finite active rates to the host Fs");
        }
    }
}
void selectedVoicingsAndLegacyFeedback() {
    for (const auto model:allModels) {
        const int selected=echolab::resolvedVoicing(model,0);
        const bool old=model == echolab::Model::Analog || model == echolab::Model::AnalogMod || model == echolab::Model::Tape ||
            model == echolab::Model::EchoPlatter || model == echolab::Model::DigitalMod || model == echolab::Model::ExciterEcho || model == echolab::Model::TubeA04;
        const int expected=old ? 1 : ((model == echolab::Model::Digital || model == echolab::Model::AutoVolume) ? 2 : 3);
        check(selected == expected,"Auto voicing resolves to the explicit user-selected version for each published model");
    }
    check(echolab::resolvedVoicing(echolab::Model::TubeA04,3) == 1,"TubeA04 model remains the named historical version under any global selector");
    check(echolab::resolvedVoicing(echolab::Model::Tube,2) == 3,"TubeB06 remains the named modern version without an implicit third Tube variant");
    check(std::strcmp(echolab::voicingName(1),"a04") == 0 && std::strcmp(echolab::voicingName(2),"a05") == 0 && std::strcmp(echolab::voicingName(3),"b06") == 0,
          "resolved voicing names are stable lowercase provenance values");
    Fixture historical(8000,2),unfiltered(8000,2);auto p=neutralParameters();p.voicing=1;p.model=echolab::Model::Analog;
    p.delay_ms=1;p.feedback=.5f;p.drive=.4f;p.tone_hz=1000;p.mix=1;
    unfiltered.engine.setParameters(p);unfiltered.engine.reset();p.feedback_lowpass_hz=12000;p.feedback_highpass_hz=80;p.drift_amount=1;
    historical.engine.setParameters(p);historical.engine.reset();
    const double pole=std::exp(-2*3.14159265358979323846*1000/8000),alpha=1-pole;
    const auto transfer=[](double x) {const double z=(1+7*.4)*x;return z/(1+.4*std::fabs(z));};
    const double impulse=transfer(.05);
    for (unsigned n=0;n<200;++n) {
        float a,b,c,d;historical.engine.processSample(n == 0 ? .05f : 0,0,a,b);unfiltered.engine.processSample(n == 0 ? .05f : 0,0,c,d);
        near(a,c,0,"A04 ignores later feedback HP/LP/drift additions while retaining native tone");
        if (n>=8 && n<16) near(a,static_cast<float>(impulse*alpha*alpha*(n-7)*std::pow(pole,n-8)),2e-7f,
            "A04 Analog first repeat follows the archived drive-weighted rational transfer and native two-pole impulse response");
        if (n == 16) {
            const double expected=impulse*alpha*alpha*9*std::pow(pole,8)+alpha*alpha*transfer(.5*impulse*alpha*alpha);
            near(a,static_cast<float>(expected),2e-7f,"A04 second repeat retains its original preamp in the loop without B06 compensation or2x latency");
        }
    }
    Fixture digital(8000,2);p=neutralParameters();p.voicing=2;p.delay_ms=.125f;p.feedback=1.5f;p.mix=1;
    p.feedback_highpass_hz=2000;p.feedback_lowpass_hz=1000;digital.engine.setParameters(p);digital.engine.reset();
    float expected=.1f;
    for (unsigned n=0;n<100;++n) {
        float l,r;digital.engine.processSample(n == 0 ? .1f : 0,0,l,r);
        if (n) {near(l,std::min(expected,echolab::safety::maximumCeiling),2e-7f,"A05 Digital restores the old emergency loop guard rather than the B06 AGC or return filters");expected=std::min(4.0f,expected*1.5f);}
    }
    check(std::find(digital.memory.begin(),digital.memory.end(),4.0f) != digital.memory.end(),"selected A05 super-unity feedback reaches its documented four-unit emergency bound internally");
}
void optionalModulationAndLoResRateRoute() {
    Fixture routed(8000,2),reference(8000,2);auto p=neutralParameters();p.model=echolab::Model::LoRes;p.delay_ms=.125f;
    p.sample_rate_hz=2000;p.feedback=0;p.mix=1;p.bit_depth=24;p.modulation_enabled=false;
    p.mod1_source=3;p.mod1_destination=8;p.mod1_amount=1;
    routed.engine.setParameters(p);routed.engine.setExternalModulation(1,0,0);routed.engine.reset();
    p.mod1_source=p.mod1_destination=0;p.sample_rate_hz=3800;reference.engine.setParameters(p);reference.engine.reset();
    for (unsigned n=0;n<2000;++n) {
        const float input=.2f*std::sin(.17f*n);float a,b,c,d;
        routed.engine.processSample(input,-input,a,b);reference.engine.processSample(input,-input,c,d);
        check(a == c && b == d,"CV Fe route remains active with internal modulation disabled and applies the documented plus90percent range");
    }
    p.sample_rate_hz=0;p.mod1_source=3;p.mod1_destination=8;
    routed.engine.setParameters(p);routed.engine.reset();p.mod1_source=p.mod1_destination=0;
    reference.engine.setParameters(p);reference.engine.reset();
    for (unsigned n=0;n<300;++n) {
        float a,b,c,d;const float input=.2f*std::sin(.11f*n);
        routed.engine.processSample(input,0,a,b);reference.engine.processSample(input,0,c,d);
        near(a,c,0,"FullFs LoRes sentinel bypasses Fe mapping strictly");
    }
    p=neutralParameters();p.delay_ms=2;p.feedback=.5f;p.mix=.5f;p.modulation_enabled=false;
    p.mod1_source=1;p.mod1_destination=1;p.mod1_amount=1;p.mod2_source=2;p.mod2_destination=5;p.mod2_amount=1;
    routed.engine.setParameters(p);routed.engine.reset();p.modulation_enabled=true;p.mod1_source=p.mod2_source=0;
    reference.engine.setParameters(p);reference.engine.reset();
    for (unsigned n=0;n<800;++n) {
        float a,b,c,d;const float input=.2f*std::sin(.13f*n);
        routed.engine.processSample(input,-input,a,b);reference.engine.processSample(input,-input,c,d);
        check(a == c && b == d,"modulation Off suppresses both internal LFO and envelope mapping sources");
    }
    p=neutralParameters();p.delay_ms=1;p.feedback=.5f;p.mix=.5f;p.modulation_enabled=false;
    p.mod1_source=3;p.mod1_destination=1;p.mod1_amount=1;p.mod2_source=5;p.mod2_destination=5;p.mod2_amount=1;
    routed.engine.setParameters(p);routed.engine.setExternalModulation(1,0,.25f);routed.engine.reset();
    p.modulation_enabled=true;p.mod1_source=p.mod2_source=0;p.delay_ms=1.5f;p.mix=.75f;
    reference.engine.setParameters(p);reference.engine.reset();
    for (unsigned n=0;n<800;++n) {
        float a,b,c,d;const float input=.2f*std::sin(.17f*n);
        routed.engine.processSample(input,-input,a,b);reference.engine.processSample(input,-input,c,d);
        check(a == c && b == d,"CV and MIDI wheel still reach Time and Mix when internal modulation is Off");
    }
    for (const auto model:{echolab::Model::Dynamic,echolab::Model::AutoVolume,echolab::Model::LoRes}) {
        p=neutralParameters();p.model=model;p.delay_ms=1;p.feedback=.5f;p.mix=1;p.sample_rate_hz=4000;p.lores_rate_envelope_amount=1;
        p.modulation_enabled=false;routed.engine.setParameters(p);routed.engine.reset();p.modulation_enabled=true;
        reference.engine.setParameters(p);reference.engine.reset();
        for (unsigned n=0;n<1500;++n) {
            float a,b,c,d;const float input=n<500 ? .3f : 0;
            routed.engine.processSample(input,0,a,b);reference.engine.processSample(input,0,c,d);
            check(a == c && b == d,"modulation switch leaves duck, swell and declining LoRes functional envelopes running");
        }
    }
    p=neutralParameters();p.model=echolab::Model::Tube;p.delay_ms=1;p.feedback=0;p.mix=1;p.drive=0;p.tone_hz=1000;p.modulation_enabled=false;
    routed.engine.setParameters(p);routed.engine.reset();
    const double pole=std::exp(-2*3.14159265358979323846*1000/8000);
    for (unsigned n=0;n<120;++n) {
        float l,r;routed.engine.processSample(n == 0 ? .2f : 0,0,l,r);
        near(l,n>=8 ? static_cast<float>(.2*(1-pole)*std::pow(pole,n-8)) : 0,1e-7f,"disabling modulation also removes Tube fixed wow and restores a fixed-time single-pole impulse");
    }
    p=neutralParameters();p.model=echolab::Model::LoRes;p.delay_ms=.125f;p.feedback=0;p.mix=1;p.bit_depth=24;p.sample_rate_hz=0;p.lores_cutoff_hz=2000;
    routed.engine.setParameters(p);routed.engine.reset();p.lores_cutoff_hz=0;reference.engine.setParameters(p);reference.engine.reset();
    const double cutoffPole=std::exp(-2*3.14159265358979323846*2000/8000);
    for (unsigned n=0;n<60;++n) {
        float a,b,c,d;routed.engine.processSample(n == 0 ? .25f : 0,0,a,b);reference.engine.processSample(n == 0 ? .25f : 0,0,c,d);
        const float expected=n ? static_cast<float>(.25*(1-cutoffPole)*std::pow(cutoffPole,n-1)) : 0;
        near(a,expected,1e-7f,"LoRes2k cutoff acts on wet reconstruction after the shared quantized feedback memory");
        if (n>1) near(c,0,0,"LoRes cutoff does not contaminate its bypassed counterpart");
    }
}
void platterModulationOffRemovesDepthOffset() {
    for(float fs : {8000.0f,48000.0f}) for(int voicing : {1,2,3}) {
        Fixture zeroDepth(fs,20),depthAndCv(fs,20);
        auto p=neutralParameters();p.model=echolab::Model::EchoPlatter;p.voicing=voicing;
        p.delay_ms=10;p.feedback=.5f;p.mix=1;p.tone_hz=1000;p.modulation_enabled=false;p.mod_depth_ms=0;
        zeroDepth.engine.setParameters(p);zeroDepth.engine.reset();
        p.mod_depth_ms=1;p.mod1_source=3;p.mod1_destination=6;p.mod1_amount=.5f;
        depthAndCv.engine.setParameters(p);depthAndCv.engine.setExternalModulation(.8f,0,0);depthAndCv.engine.reset();
        for(unsigned n=0;n<static_cast<unsigned>(fs*.05f);++n) {
            const float input=n == 0 ? .2f : (n == 37 ? -.08f : 0);float a,b,c,d;
            zeroDepth.engine.processSample(input,-.5f*input,a,b);depthAndCv.engine.processSample(input,-.5f*input,c,d);
            check(a==c && b==d,"Platter Internal Modulation Off removes base and routed Depth instead of retaining a frozen wow offset");
        }
        p.modulation_enabled=true;p.mod_depth_ms=0;p.mod1_source=p.mod1_destination=0;
        zeroDepth.engine.setParameters(p);zeroDepth.engine.reset();p.mod_depth_ms=1;
        depthAndCv.engine.setParameters(p);depthAndCv.engine.reset();
        bool changed=false;
        for(unsigned n=0;n<static_cast<unsigned>(fs*.05f);++n) {
            const float input=n == 0 ? .2f : 0;float a,b,c,d;
            zeroDepth.engine.processSample(input,-.5f*input,a,b);depthAndCv.engine.processSample(input,-.5f*input,c,d);
            changed=changed || std::fabs(a-c)>1e-4f || std::fabs(b-d)>1e-4f;
        }
        check(changed,"Platter Depth remains audible with Internal Modulation On in every voicing and Fs");
    }
}
void colourCompensationAndSlowDrift() {
    Fixture coloured(48000,50),linear(48000,50);
    auto p=neutralParameters();p.model=echolab::Model::Analog;p.delay_ms=2;p.feedback=0;p.mix=1;p.tone_hz=3500;p.drive=.4f;
    coloured.engine.setParameters(p);coloured.engine.reset();p.drive=0;linear.engine.setParameters(p);linear.engine.reset();
    double colourEnergy=0,linearEnergy=0;
    for (unsigned n=0;n<24000;++n) {
        const float input=1e-4f*std::sin(2*3.14159265358979323846f*100*n/48000);
        float a,b,c,d;coloured.engine.processSample(input,-input,a,b);linear.engine.processSample(input,-input,c,d);
        if (n>12000) {colourEnergy+=static_cast<double>(a)*a;linearEnergy+=static_cast<double>(c)*c;}
    }
    const double smallGain=std::sqrt(colourEnergy/linearEnergy);
    check(smallGain>3.75 && smallGain<3.85,"A04 first-pass preamplifier colour returns atDrive.4 without changing the configured feedback gain");
    for (const auto model:{echolab::Model::Analog,echolab::Model::Tube,echolab::Model::Tape,
        echolab::Model::Sweep,echolab::Model::AnalogMod,echolab::Model::MultiHead,echolab::Model::EchoPlatter,echolab::Model::ExciterEcho}) {
        Fixture f(8000,50);p=neutralParameters();p.model=model;p.delay_ms=10;p.mix=1;p.feedback=.75f;p.drive=.4f;
        p.feedback_lowpass_hz=12000;f.engine.setParameters(p);f.engine.reset();
        float tailPeak=0;
        for (unsigned n=0;n<24000;++n) {
            const float input=n<800 ? .3f*std::sin(2*3.14159265358979323846f*500*n/8000) : 0;
            float l,r;f.engine.processSample(input,input,l,r);
            if (n>20000) tailPeak=std::max(tailPeak,std::max(std::fabs(l),std::fabs(r)));
        }
        check(tailPeak<1e-5f,"compensated coloured loops decay at75% feedback instead of self-growing from their first-pass preamp");
    }
    Fixture drifted(8000,200);p=neutralParameters();p.model=echolab::Model::StereoDelays;
    p.delay_ms=100;p.stereo_ratio=1;p.mix=1;p.feedback=0;p.drift_amount=1;
    drifted.engine.setParameters(p);drifted.engine.reset();
    double previous=0,minimum=1,maximum=-1;
    double moment=0,weight=0;unsigned observed=0;
    for (unsigned n=0;n<320000;++n) {
        float l,r;drifted.engine.processSample(n%8000 == 0 ? .25f : 0,0,l,r);
        const int relative=static_cast<int>(n%8000)-800;
        if (relative>=-3 && relative<=3) {moment+=relative*static_cast<double>(l);weight+=l;}
        if (relative == 3) {
            check(weight>.24 && weight<.26,"microdelay drift retains the impulse energy at low level");
            const double milliseconds=(moment/weight)/8.0;
            check(std::fabs(milliseconds)<=.101,"full-depth microdelay drift remains within0.1ms");
            if (observed) check(std::fabs(milliseconds-previous)<.021,"microdelay drift changes slowly rather than acting as a fast chorus");
            previous=milliseconds;minimum=std::min(minimum,milliseconds);maximum=std::max(maximum,milliseconds);
            ++observed;moment=weight=0;
        }
    }
    check(observed == 40 && maximum-minimum>.15,"slow deterministic drift completes a measurable bounded cycle");
    Fixture slow(8000,200);p=neutralParameters();p.model=echolab::Model::AnalogMod;
    p.delay_ms=100;p.mix=1;p.feedback=0;p.tone_hz=3600;p.mod_rate_hz=.25f;p.mod_depth_ms=2;
    slow.engine.setParameters(p);slow.engine.reset();
    const double pole=std::exp(-2*3.14159265358979323846*.45);
    const double groupDelay=2*pole/(1-pole);
    for (unsigned n=0;n<=40000;++n) {
        float l,r;slow.engine.processSample(static_cast<float>(n)*1e-6f,static_cast<float>(n)*1e-6f,l,r);
        if (n>=8000 && n%8000 == 0) {
            const double phase=std::fmod(.18*n/8000,1.0);
            const double motion=4*std::fabs(phase-.5)-1;
            const double actual=n-static_cast<double>(l)/1e-6-groupDelay;
            check(std::fabs(actual-(800+16*motion))<.2,"AnalogMod default phase advances at0.18Hz with the configured0.25Hz nominal rate");
        }
    }
    Fixture digitalA,digitalB;p=neutralParameters();p.model=echolab::Model::Digital;p.delay_ms=2;p.feedback=.5f;p.mix=1;
    digitalA.engine.setParameters(p);digitalA.engine.reset();p.feedback_lowpass_hz=12000;p.drift_amount=1;
    digitalB.engine.setParameters(p);digitalB.engine.reset();
    for (unsigned n=0;n<2000;++n) {
        float a,b,c,d;const float input=.2f*std::sin(.13f*n);
        digitalA.engine.processSample(input,-input,a,b);digitalB.engine.processSample(input,-input,c,d);
        check(a == c && b == d,"Digital control stays an exact analytical reference when analogue refinements are enabled elsewhere");
    }
}
void externalModulationRouting() {
    Fixture routed,reference;
    auto p=neutralParameters();p.delay_ms=2;p.mix=1;p.feedback=.5f;
    p.mod1_source=3;p.mod1_destination=1;p.mod1_amount=1;
    routed.engine.setParameters(p);routed.engine.setExternalModulation(1,0,0);routed.engine.reset();
    p.mod1_source=0;p.mod1_destination=0;p.delay_ms=3;reference.engine.setParameters(p);reference.engine.reset();
    for (unsigned n=0;n<300;++n) {
        float a,b,c,d;const float input=n == 0 ? .3f : 0;
        routed.engine.processSample(input,-input,a,b);reference.engine.processSample(input,-input,c,d);
        check(a == c && b == d,"bipolar CV time route applies the documented50% base-delay range");
    }
    for (const auto model : {echolab::Model::Reverse,echolab::Model::LoopSampler}) {
        p=neutralParameters();p.model=model;p.delay_ms=2;p.feedback=0;p.mix=1;
        p.mod1_source=3;p.mod1_destination=1;p.mod1_amount=1;
        routed.engine.setParameters(p);routed.engine.setExternalModulation(1,0,0);routed.engine.reset();
        p.mod1_source=p.mod1_destination=0;p.delay_ms=3;
        reference.engine.setParameters(p);reference.engine.reset();
        bool audible=false;
        for (unsigned n=0;n<160;++n) {
            const float input=.2f*std::sin(static_cast<float>(n)*.13f);
            float a,b,c,d;routed.engine.processSample(input,-input,a,b);reference.engine.processSample(input,-input,c,d);
            check(a == c && b == d,"Time routing reaches Reverse capture boundaries and the next looper Record length");
            audible=audible || a != 0 || b != 0;
        }
        check(audible,"segmented Time modulation continues completed audible chunks instead of invalidating each sample");
    }
    p=neutralParameters();p.delay_ms=1;p.mix=1;p.feedback=.25f;p.mod1_source=3;p.mod1_destination=2;p.mod1_amount=1;
    routed.engine.setParameters(p);routed.engine.setExternalModulation(.5f,0,0);routed.engine.reset();
    p.mod1_source=0;p.mod1_destination=0;p.feedback=.5f;reference.engine.setParameters(p);reference.engine.reset();
    for (unsigned n=0;n<100;++n) {
        float a,b,c,d;routed.engine.processSample(n == 0 ? .3f : 0,0,a,b);reference.engine.processSample(n == 0 ? .3f : 0,0,c,d);
        check(a == c && b == d,"CV feedback route applies the documented half-unit depth");
    }
    p=neutralParameters();p.delay_ms=1;p.mix=0;p.mod1_source=5;p.mod1_destination=5;p.mod1_amount=1;
    routed.engine.setParameters(p);routed.engine.setExternalModulation(0,0,.5f);routed.engine.reset();
    p.mod1_source=0;p.mod1_destination=0;p.mix=.5f;reference.engine.setParameters(p);reference.engine.reset();
    for (unsigned n=0;n<40;++n) {
        float a,b,c,d;const float in=n%5 == 0 ? .4f : -.1f;
        routed.engine.processSample(in,-in,a,b);reference.engine.processSample(in,-in,c,d);
        check(a == c && b == d,"unipolar MIDI-wheel modulation reaches the shared mix destination");
    }
    p=neutralParameters();p.delay_ms=1;p.feedback=0;p.mix=0;p.mod1_source=2;p.mod1_destination=5;p.mod1_amount=1;
    routed.engine.setParameters(p);routed.engine.reset();
    float l=0,r=0;
    for (unsigned n=0;n<=200;++n) routed.engine.processSample(n<200 ? .5f : 0,0,l,r);
    check(l > .2f && l < .3f,"envelope modulation source runs outside ducking models and controls the selected destination");
    p.mod1_source=3;p.mod1_destination=2;p.mod2_source=4;p.mod2_destination=2;p.mod1_amount=p.mod2_amount=1;
    p.mix=1;routed.engine.setParameters(p);routed.engine.setExternalModulation(1,-1,0);routed.engine.reset();
    p.mod1_source=p.mod1_destination=p.mod2_source=p.mod2_destination=0;reference.engine.setParameters(p);reference.engine.reset();
    for (unsigned n=0;n<80;++n) {
        float a,b,c,d;routed.engine.processSample(n == 0 ? .2f : 0,0,a,b);reference.engine.processSample(n == 0 ? .2f : 0,0,c,d);
        check(a == c && b == d,"two modulation slots sum coherently on a common destination");
    }
    routed.engine.setExternalModulation(std::numeric_limits<float>::quiet_NaN(),std::numeric_limits<float>::infinity(),-1);
    routed.engine.processSample(0,0,l,r);check(std::isfinite(l) && std::isfinite(r),"external modulation sanitizes non-finite CV and out-of-range wheel controls");
}
void dynamicDuckingAndRecovery() {
    Fixture duck(8000,20),plain(8000,20);
    auto p=neutralParameters();
    p.model=echolab::Model::Dynamic;p.delay_ms=10;p.feedback=0.8f;p.mix=1;
    p.attack_ms=1;p.release_ms=20;p.duck_threshold_db=-36;p.duck_amount=0.9f;
    duck.engine.setParameters(p);duck.engine.reset();
    p.model=echolab::Model::Digital;plain.engine.setParameters(p);plain.engine.reset();
    bool attenuated=false,recovered=false;
    for (unsigned n=0;n<4800;++n) {
        const float inL=n == 0 ? 0.5f : 0.0f;
        const float inR=n < 1600 ? 0.15f : 0.0f;
        float dl,dr,pl,pr;
        duck.engine.processSample(inL,inR,dl,dr);plain.engine.processSample(inL,inR,pl,pr);
        if (n == 800) { check(pl > 0 && dl/pl < 0.3f,"stereo detector ducks an existing left tail during sustained right input");attenuated=true; }
        if (n == 4000) { check(pl > 0 && dl == pl && dr == pr,"ducking settles to the unchanged feedback tail after gain release");recovered=true; }
    }
    check(attenuated && recovered,"ducking test observes both active and recovered wet states");
    p.model=echolab::Model::Dynamic;p.mix=0;duck.engine.setParameters(p);duck.engine.reset();
    for (unsigned n=0;n<200;++n) { float l,r;duck.engine.processSample(.7f,-.4f,l,r);check(l == .7f && r == -.4f,"Dynamic attenuation affects wet only and preserves dry stereo exactly"); }
    p.mix=1;p.duck_amount=0;duck.engine.setParameters(p);duck.engine.reset();
    p.model=echolab::Model::Digital;plain.engine.setParameters(p);plain.engine.reset();
    for (unsigned n=0;n<400;++n) { float dl,dr,pl,pr;duck.engine.processSample(.3f,-.5f,dl,dr);plain.engine.processSample(.3f,-.5f,pl,pr);check(dl == pl && dr == pr,"zero duck amount exactly bypasses gain reduction without changing the loop"); }
}
void automaticVolumeSwell() {
    Fixture f;
    auto p=f.engine.parameters();p.model=echolab::Model::AutoVolume;p.delay_ms=1;p.feedback=0;p.mix=1;
    p.attack_ms=.1f;p.release_ms=10;p.swell_ms=50;p.duck_threshold_db=-40;p.duck_amount=0;
    f.engine.setParameters(p);f.engine.reset();
    float early=0,late=0,retrigger=0;
    for (unsigned n=0;n<2400;++n) {
        const float input=n < 1200 || n >= 2200 ? .5f : 0.0f;
        float l,r;f.engine.processSample(input,input,l,r);
        if (n == 8) early=l;
        if (n == 1000) late=l;
        if (n == 2240) retrigger=l;
    }
    const float expectedEarly=.5f*(1.0f-std::exp(-9.0f/(.050f*8000.0f)));
    near(early,expectedEarly,2e-6f,"AutoVolume applies the prepared swell time constant to its wet return");
    check(early > 0 && early < .02f && late > .45f,"AutoVolume starts softly and rises toward the full wet return");
    const float expectedRetrigger=.5f*(1.0f-std::exp(-41.0f/(.050f*8000.0f)));
    near(retrigger,expectedRetrigger,3e-6f,"AutoVolume rearms the next phrase without resetting the stored delay");
    p.mix=0;f.engine.setParameters(p);f.engine.reset();
    float l,r;f.engine.processSample(.5f,-.25f,l,r);
    check(l == .5f && r == -.25f,"AutoVolume leaves the dry input unchanged");
}

void factoryEnvelopeTailRecovery() {
    constexpr unsigned fs=48000;
    struct Recipe { echolab::Model model; float delay,feedback,mix,tone,attack,release,threshold,swell; int voicing; bool moving; };
    const Recipe recipes[]={
        {echolab::Model::Dynamic,500,.28f,.39f,4800,5,200,-30,250,3,true},
        {echolab::Model::AutoVolume,125,.22f,1,4400,2,500,-40,600,2,true},
        {echolab::Model::AutoVolume,500,.28f,1,4200,2,500,-40,1200,2,false},
    };
    for(const auto& recipe:recipes) {
        Fixture duck(fs,1500),oracle(fs,1500);
        echolab::Parameters p;
        p.model=recipe.model;p.delay_ms=recipe.delay;p.feedback=recipe.feedback;p.mix=recipe.mix;
        p.tone_hz=recipe.tone;p.drive=0;p.attack_ms=recipe.attack;p.release_ms=recipe.release;
        p.duck_threshold_db=recipe.threshold;p.duck_amount=.9f;p.swell_ms=recipe.swell;
        p.voicing=recipe.voicing;p.modulation_enabled=recipe.moving;p.wet_tone_enabled=true;
        p.drift_amount=recipe.moving?.25f:0;p.mod_depth_ms=recipe.moving?6.0f:0;
        duck.engine.setParameters(p);duck.engine.reset();
        p.duck_amount=0;oracle.engine.setParameters(p);oracle.engine.reset();
        double wetPower[4]{},oraclePower[4]{};
        const float starts[]={.7f,1.1f,1.3f,1.8f};
        const float stops[]={.8f,1.2f,1.4f,1.9f};
        for(unsigned n=0;n<2*fs;++n) {
            const float time=static_cast<float>(n)/fs;
            const float inputL=n<2*fs/5?.35f*std::sin(6.2831853071795864769f*220*time):0;
            // The right input holds down a real left-channel tail, then falls
            // to -48 dBFS, below each factory threshold, instead of exact zero.
            const float inputR=(n<fs?.7f:.004f)*std::sin(6.2831853071795864769f*440*time);
            float dl,dr,ol,orr;
            duck.engine.processSample(inputL,inputR,dl,dr);
            oracle.engine.processSample(inputL,inputR,ol,orr);
            check(std::isfinite(dl) && std::isfinite(dr),"factory envelope outputs remain finite");
            for(unsigned window=0;window<4;++window)
                if(n>=static_cast<unsigned>(starts[window]*fs) && n<static_cast<unsigned>(stops[window]*fs)) {
                    wetPower[window]+=static_cast<double>(dl)*dl;
                    oraclePower[window]+=static_cast<double>(ol)*ol;
                }
        }
        float ratio[4]{};
        for(unsigned window=0;window<4;++window) {
            check(oraclePower[window]>0,"factory tail gain is compared only with a nonzero oracle return");
            ratio[window]=static_cast<float>(std::sqrt(wetPower[window]/oraclePower[window]));
            check(ratio[window]<=1.02f,"wet gain recovery does not boost the stored tail above its oracle");
        }
        check(ratio[0]<.3f,"Ducked Voice, Bloom and Swell attenuate an existing tail while either input plays");
        check(ratio[2]>.45f && ratio[2]>2*ratio[0],"factory wet tails rise within 300-400 ms after input falls below threshold");
        check(ratio[3]>.8f,"factory tails approach their unducked oracle despite a below-threshold noise floor");
        if(recipe.model==echolab::Model::Dynamic)
            check(ratio[1]>.5f && ratio[2]>.8f,"200 ms Dynamic release acts on gain without a preceding level-decay wait");
    }
}

void swellPreservesStoredTailAndSmoothRetrigger() {
    constexpr unsigned fs=48000;
    Fixture swell(fs,2000),oracle(fs,2000);
    echolab::Parameters p;
    p.model=echolab::Model::AutoVolume;p.delay_ms=500;p.feedback=.28f;p.mix=1;
    p.tone_hz=4200;p.drive=0;p.voicing=2;p.attack_ms=2;p.release_ms=500;
    p.duck_threshold_db=-40;p.duck_amount=0;p.swell_ms=1200;p.wet_tone_enabled=true;
    p.modulation_enabled=false;p.mod_depth_ms=0;p.drift_amount=0;
    swell.engine.setParameters(p);swell.engine.reset();
    auto full=p;full.swell_ms=1;oracle.engine.setParameters(full);oracle.engine.reset();
    double tailPower=0,fullPower=0;
    for(unsigned n=0;n<2*fs;++n) {
        if(n==5*fs/4) {p.swell_ms=1;swell.engine.setParameters(p);}
        const float input=n<fs/10?.25f*std::sin(6.2831853071795864769f*220*static_cast<float>(n)/fs):0;
        float sl,sr,ol,orr;swell.engine.processSample(input,input,sl,sr);oracle.engine.processSample(input,input,ol,orr);
        if(n<fs/2) near(sl,0,0,"a short Swell phrase has no dry attack before its actual 500 ms delay");
        if(n>=3*fs/2 && n<8*fs/5) {tailPower+=static_cast<double>(sl)*sl;fullPower+=static_cast<double>(ol)*ol;}
    }
    check(fullPower>1e-8 && std::sqrt(tailPower/fullPower)>.999,"Swell keeps the complete short-phrase feedback tail independently of its earlier envelope gain");

    p.swell_ms=50;p.release_ms=10;p.wet_tone_enabled=false;
    swell.engine.setParameters(p);swell.engine.reset();
    float previous=0;float largestDrop=0;
    for(unsigned n=0;n<8*fs/5;++n) {
        const float input=n<fs || n>=7*fs/5?.4f:0;
        float sl,sr;swell.engine.processSample(input,input,sl,sr);
        if(n>=7*fs/5 && n<7*fs/5+fs/100)
            largestDrop=std::max(largestDrop,previous-sl);
        previous=sl;
    }
    check(largestDrop<.01f,"retriggered Swell lowers an audible old return with Attack smoothing rather than an abrupt gate");
}
void sharedMemoryCanariesAndModelChanges() {
    const auto required=echolab::Engine::requiredSamples(8000,3);
    std::vector<float> guarded(required+2,1234.5f);
    echolab::Engine e;
    check(e.prepare(8000,3,guarded.data()+1,required),"two-bank engine prepares within a guarded exact allocation");
    for (auto model : allModels) {
        auto p=e.parameters();p.model=model;p.delay_ms=3;p.feedback=.9f;p.mix=1;p.loop_action=1;
        audioGuard=true;e.setParameters(p);
        // setParameters must only invalidate state, not clear potentially large audio storage.
        for (unsigned n=0;n<300;++n) { float l,r;e.processSample(n%11 == 0 ? .3f : 0,n%7 == 0 ? -.2f : 0,l,r); }
        audioGuard=false;
        check(guarded.front() == 1234.5f && guarded.back() == 1234.5f,"all model entries and shared-bank buffer wraps preserve memory canaries");
    }
    e.reset();
    check(guarded.front() == 1234.5f && guarded.back() == 1234.5f,"reset clears exactly caller-provided audio storage");
}
void memoryAndReset() {
    check(echolab::Engine::requiredSamples(48000,2500) == 480016,"memory formula includes stereo interpolation margin and two shared reverse banks");
    check(echolab::Engine::requiredSamples(0,1) == 0,"invalid sample rate rejected");
    check(echolab::Engine::requiredSamples(48000,std::numeric_limits<float>::infinity()) == 0,"nonfinite memory specification rejected");
    std::vector<float> memory(40,123);
    echolab::Engine invalid;
    check(!invalid.prepare(8000,2,memory.data(),39),"insufficient capacity rejected before clear");
    check(std::all_of(memory.begin(),memory.end(),[](float x){ return x == 123; }),"failed prepare preserves caller memory");
    float l,r; invalid.processSample(0.25f,-0.5f,l,r);
    check(l == 0.25f && r == -0.5f,"unprepared engine finite dry passthrough");
    Fixture a,b;
    a.parameters(1); b.parameters(1);
    a.engine.processSample(1,1,l,r);
    for (unsigned i = 0; i != 100; ++i) {
        b.engine.processSample(0,0,l,r);
        check(l == 0 && r == 0,"instances do not share persistent state");
    }
    a.engine.reset();
    for (unsigned i = 0; i != 100; ++i) {
        a.engine.processSample(0,0,l,r);
        check(l == 0 && r == 0,"reset clears delay and filters");
    }
}
std::vector<echolab::ParameterEvent> timeline(std::size_t frames) {
    std::vector<echolab::ParameterEvent> events;
    echolab::Parameters p;
    p.delay_ms = 2; p.mix = 0.7f; p.feedback = 0.7f;
    std::vector<std::size_t> offsets={0,1,23,24,25,64,65};
    for (std::size_t i=0;i<echolab::modelCount;++i) offsets.push_back(128+i*127);
    offsets.push_back(frames-1);offsets.push_back(frames);
    for (std::size_t offset : offsets) {
        p.delay_ms += 0.35f; p.mix = 1.0f - p.mix; p.drive += 0.07f;
        p.model = static_cast<echolab::Model>(events.size() % echolab::modelCount);
        p.bit_depth=4+static_cast<int>(events.size()%21);
        p.mod_rate_hz=0.1f+0.2f*static_cast<float>(events.size());
        p.mod_depth_ms=0.25f+0.1f*static_cast<float>(events.size());
        p.duck_threshold_db=-60.0f+static_cast<float>(events.size());
        p.duck_amount=0.95f;p.attack_ms=0.1f+static_cast<float>(events.size());
        p.release_ms=10.0f+10.0f*static_cast<float>(events.size());
        p.swell_ms=10.0f+20.0f*static_cast<float>(events.size());
        p.stereo_ratio=1.0f+0.03f*static_cast<float>(events.size());
        p.loop_action=static_cast<int>(events.size()%5);
        p.limiter_enabled=events.size()%2 == 0;p.limiter_ceiling=.7f;p.limiter_release_ms=20;
        p.sample_rate_hz=1000.0f+100.0f*static_cast<float>(events.size());
        p.exciter_amount=.8f;p.exciter_tune_hz=1200;
        p.tap_pattern=static_cast<int>(events.size()%echolab::multitap::patternCount);
        p.mod1_source=1+static_cast<int>(events.size()%5);p.mod1_destination=1+static_cast<int>(events.size()%8);p.mod1_amount=.7f;
        p.mod2_source=1+static_cast<int>((events.size()+1)%5);p.mod2_destination=1+static_cast<int>((events.size()+2)%8);p.mod2_amount=-.4f;
        p.feedback_lowpass_hz=events.size()%3 == 0 ? 0.0f : 4000.0f+200.0f*static_cast<float>(events.size());
        p.drift_amount=.5f;
        p.feedback_highpass_hz=events.size()%3 == 0 ? 0.0f : 40.0f+20.0f*static_cast<float>(events.size());
        p.lores_rate_end_hz=500;p.lores_rate_decay_ms=100+50.0f*static_cast<float>(events.size());p.lores_rate_envelope_amount=.8f;
        p.digital_rack_rate_hz=1000+200.0f*static_cast<float>(events.size());
        p.voicing=static_cast<int>(events.size()%4);p.modulation_enabled=events.size()%3 != 0;
        p.lores_cutoff_hz=events.size()%2 == 0 ? 0.0f : 2000.0f;
        events.push_back({offset,p});
    }
    return events;
}
void eventAndBlockInvariance() {
    constexpr std::size_t count = 4096;
    std::vector<float> inL(count),inR(count),expectedL(count),expectedR(count),actualL(count),actualR(count);
    for (std::size_t i = 0; i != count; ++i) { inL[i] = 0.2f * std::sin(0.37f * static_cast<float>(i)); inR[i] = i % 53 == 0 ? -0.7f : 0; }
    const auto events = timeline(count);
    Fixture baseline;
    check(baseline.engine.processBlock(inL.data(),inR.data(),expectedL.data(),expectedR.data(),count,events.data(),events.size()),"valid full timeline accepted");
    for (std::size_t block : {1U,17U,24U,31U,64U,511U,4096U}) {
        Fixture f;
        std::size_t event = 0;
        std::vector<echolab::ParameterEvent> local;
        local.reserve(events.size());
        for (std::size_t begin = 0; begin < count; begin += block) {
            const auto length = std::min(block,count-begin);
            local.clear();
            while (event < events.size() && events[event].offset <= begin+length) {
                auto e = events[event++]; e.offset -= begin; local.push_back(e);
            }
            audioGuard=true;
            const bool accepted=f.engine.processBlock(inL.data()+begin,inR.data()+begin,actualL.data()+begin,actualR.data()+begin,length,local.data(),local.size());
            audioGuard=false;
            check(accepted,"split timeline accepted");
        }
        check(std::memcmp(expectedL.data(),actualL.data(),count*sizeof(float)) == 0 &&
              std::memcmp(expectedR.data(),actualR.data(),count*sizeof(float)) == 0,"sample events independent of 1/17/24/31/64/511/full block partition");
    }
    Fixture f;
    for (const auto model : allModels) {
        f.parameters(1,0.5f,0.4f,model);
        f.engine.processBlock(inL.data(),inR.data(),expectedL.data(),expectedR.data(),count);
        f.engine.reset(); actualL = inL; actualR = inR;
        f.engine.processBlock(actualL.data(),actualR.data(),actualL.data(),actualR.data(),count);
        check(actualL == expectedL && actualR == expectedR,"all models support in-place block buffers");
        f.engine.reset(); actualL = inL; actualR = inR;
        f.engine.processBlock(actualL.data(),actualR.data(),actualR.data(),actualL.data(),count);
        check(actualR == expectedL && actualL == expectedR,"all models read both inputs before cross-channel output aliases");
    }
    const echolab::ParameterEvent bad[] = {{2,{}},{1,{}}};
    float sentinelL = 123,sentinelR = 456;
    check(!f.engine.processBlock(nullptr,nullptr,&sentinelL,&sentinelR,1,bad,2),"unsorted/out-of-block timeline rejected");
    check(sentinelL == 123 && sentinelR == 456,"invalid timeline does not touch output");
}
void ratesExtremesAndAllocations() {
    for (float fs : {8000.0f,44100.0f,48000.0f,96000.0f,192000.0f,384000.0f}) {
        Fixture f(fs,30);
        for (auto model : allModels) {
            echolab::Parameters p;
            p.model=model; p.feedback=2.0f; p.mix=1; p.drive=1; p.delay_ms=0.01f; p.tone_hz=fs;
            p.mod1_source=3;p.mod1_destination=2;p.mod1_amount=1;
            p.mod2_source=2;p.mod2_destination=3;p.mod2_amount=-.8f;
            p.sample_rate_hz=fs*.373f;
            f.engine.setParameters(p); f.engine.reset();
            bool finite = true;
            audioGuard = true;
            for (unsigned i = 0; i != 20000; ++i) {
                if (i == 200) { p.delay_ms=29; f.engine.setParameters(p); }
                float l,r;
                f.engine.setExternalModulation(i%2 ? 1.0f : -1.0f,0,.5f);
                const float input = i % 997 == 0 ? std::numeric_limits<float>::infinity() : (i % 3 == 0 ? 1.0e20f : -2.0f);
                f.engine.processSample(input,-input,l,r);
                finite = finite && std::isfinite(l) && std::isfinite(r) && std::fabs(l) <= p.limiter_ceiling && std::fabs(r) <= p.limiter_ceiling;
            }
            f.engine.reset();
            audioGuard = false;
            check(finite,"all models remain finite across Fs, feedback, drive, input and delay extremes");
        }
        auto p = f.engine.parameters();
        p.delay_ms=p.feedback=p.mix=p.tone_hz=p.drive=std::numeric_limits<float>::quiet_NaN();
        p.mod_rate_hz=p.mod_depth_ms=p.duck_threshold_db=p.duck_amount=p.attack_ms=p.release_ms=
            p.swell_ms=p.stereo_ratio=std::numeric_limits<float>::quiet_NaN();
        p.limiter_ceiling=p.limiter_release_ms=p.sample_rate_hz=p.exciter_amount=p.exciter_tune_hz=
            p.mod1_amount=p.mod2_amount=std::numeric_limits<float>::quiet_NaN();
        p.lores_cutoff_hz=p.digital_rack_rate_hz=p.feedback_lowpass_hz=p.feedback_highpass_hz=p.drift_amount=p.lores_rate_end_hz=
            p.lores_rate_decay_ms=p.lores_rate_envelope_amount=std::numeric_limits<float>::quiet_NaN();
        f.engine.setParameters(p); f.engine.reset();
        check(std::isfinite(f.engine.parameters().delay_ms) && std::isfinite(f.engine.parameters().feedback),"nonfinite controls sanitized");
        check(f.engine.parameters().feedback == .28f && f.engine.parameters().mix == .39f,
              "nonfinite feedback and mix recover the current production defaults");
        check(f.engine.parameters().tone_hz <= 0.45f * fs,"nonfinite tone fallback respects the active sample-rate limit");
    }
    check(audioAllocations == 0,"no heap allocation in control/reset/sample/block/event audio paths");
    std::vector<std::vector<float>> renders;
    for (auto model : allModels) {
        Fixture f; f.parameters(2,0.8f,1,model);
        auto p = f.engine.parameters(); p.drive=0.3f; p.tone_hz=1200; f.engine.setParameters(p); f.engine.reset();
        std::vector<float> result(2000);
        const auto left=[](std::size_t i) { return 0.7f*std::sin(0.07f*static_cast<float>(i))+0.05f; };
        const auto right=[](std::size_t i) { return 0.6f*std::cos(0.13f*static_cast<float>(i))-0.02f; };
        for (std::size_t i=0;i<result.size()/2;++i) f.engine.processSample(left(i),right(i),result[2*i],result[2*i+1]);
        f.engine.reset();
        for (std::size_t i=0;i<result.size()/2;++i) { float l,r; f.engine.processSample(left(i),right(i),l,r); check(l == result[2*i] && r == result[2*i+1],"reset reproduces stereo filter, envelope, segmented and modulation states exactly"); }
        renders.push_back(std::move(result));
    }
    for (std::size_t a=0;a<renders.size();++a) for (std::size_t b=a+1;b<renders.size();++b)
        check(renders[a] != renders[b],"all model pairs produce distinct output on sustained asymmetric stereo input");
}
}

void* operator new(std::size_t count) {
    if (audioGuard) ++audioAllocations;
    if (void* ptr = std::malloc(count ? count : 1)) return ptr;
    throw std::bad_alloc();
}
void* operator new[](std::size_t count) { return ::operator new(count); }
void operator delete(void* ptr) noexcept { std::free(ptr); }
void operator delete[](void* ptr) noexcept { std::free(ptr); }
void operator delete(void* ptr,std::size_t) noexcept { std::free(ptr); }
void operator delete[](void* ptr,std::size_t) noexcept { std::free(ptr); }

int main() {
    impulseAndInterpolation(); exact48kLongDelay(); stereoMusicalRatios(); stereoThreeAgainstTwoMusicalTiming(); colouredStereoTiming(); extendedModulationRoutes(); savedDriftRouteIsInactiveOnLooper(); pingPongTopology(); multiHeadTopology();
    lowResolutionQuantization();dynamicDuckingAndRecovery();automaticVolumeSwell();factoryEnvelopeTailRecovery();
    swellPreservesStoredTailAndSmoothRetrigger();sharedMemoryCanariesAndModelChanges();
    fractionalSampleRateReduction();decliningLoResPhraseEnvelope();oneWaySweepDirections();sweepFilterSpectra();feedbackAboveUnityAndLimiter();exciterInFeedbackOnly();optionalExciterHighpass();selectableHeadPattern();polyrhythmPulseGrids();optionalWetTone();liveLooperRecordUsesConfiguredTime();externalModulationRouting();
    feedbackLowpassPlacementAndSlope();feedbackHighpassPlacementAndSlope();rackRateIsModelSpecific();selectedVoicingsAndLegacyFeedback();optionalModulationAndLoResRateRoute();platterModulationOffRemovesDepthOffset();colourCompensationAndSlowDrift();
    memoryAndReset(); eventAndBlockInvariance(); ratesExtremesAndAllocations();
    if (failures) { std::cerr << failures << " DSP checks failed\n"; return 1; }
    std::cout << "DSP analytical, memory, stereo, events, block invariance, Fs, reset, finite-state and allocation checks passed\n";
    return 0;
}
