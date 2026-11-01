#include "dsp/output_limiter.h"
#include "dsp/loop_exciter.h"
#include "dsp/multitap_patterns.h"
#include "dsp/analog_color.h"
#include "dsp/a04_color.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>

// Every source here is synthetic_fixture. These are signal/structure tests,
// never comparisons to an Aphex, BBE or Line 6 capture.
namespace {
int failures = 0;
constexpr double pi = 3.1415926535897932384626433832795;
void check(bool condition, const char* message) {
    if (!condition) { std::cerr << "FAIL: " << message << '\n'; ++failures; }
}
void near(float actual, float expected, float tolerance, const char* message) {
    check(std::fabs(actual - expected) <= tolerance, message);
}
void limiterAttackLinkRelease() {
    echolab::safety::Limiter limiter;
    limiter.prepare(48000,80); limiter.setCeiling(0.95f); limiter.reset();
    float l=0.25f,r=-0.125f;
    limiter.process(l,r);
    near(l,0.25f,0,"limiter below ceiling is exact identity at reset");
    near(r,-0.125f,0,"limiter below ceiling preserves right channel");
    l=2; r=-0.5f; limiter.process(l,r);
    check(std::fabs(l)<=limiter.ceiling() && std::fabs(r)<=limiter.ceiling(),"limiter catches overload with linked gain");
    near(r,-0.25f*l,2e-7f,"limiter preserves stereo ratio");
    limiter.setCeiling(0.1f);l=0.5f;r=-0.25f;limiter.process(l,r);
    check(std::fabs(l)<=0.1f && std::fabs(r)<=0.1f,"lower ceiling applies immediately");
    limiter.setCeiling(0);l=1;r=-1;limiter.process(l,r);
    check(std::fabs(l)<=limiter.ceiling() && std::fabs(r)<=limiter.ceiling(),"out-of-range ceiling uses the current bounded guard contract");
    limiter.reset();limiter.setCeiling(0.95f);
    l=std::numeric_limits<float>::quiet_NaN();r=std::numeric_limits<float>::infinity();
    limiter.process(l,r);near(l,0,0,"limiter NaN becomes zero");near(r,0,0,"limiter infinity becomes zero");
}
void limiterBoundsAndFeedback() {
    echolab::safety::Limiter limiter;
    limiter.prepare(384000,1);limiter.setCeiling(0.95f);
    const std::array<float,9> extremes{{0,1,-1,100,-100,1e12f,-1e12f,
        std::numeric_limits<float>::max(),-std::numeric_limits<float>::max()}};
    for (float a:extremes) for (float b:extremes) {
        float l=a,r=b;limiter.process(l,r);
        check(std::isfinite(l)&&std::isfinite(r)&&std::fabs(l)<=0.95f&&std::fabs(r)<=0.95f,
              "limiter guarantees finite sample ceiling even for maximum float");
    }
    limiter.prepare(std::numeric_limits<float>::quiet_NaN(),-100);
    limiter.setCeiling(std::numeric_limits<float>::quiet_NaN());
    check(limiter.ceiling()>0 && limiter.ceiling()<=1,"invalid limiter ceiling has finite fallback");
    // Independent stress fixture: strongly driven feedback at and above unity,
    // with the same explicit +/-4 feedback protection used by the common core.
    for (const float feedback:{1.0f,1.5f}) {
        echolab::exciter::State exciter;
        exciter.prepare(48000,3000);exciter.reset();limiter.reset();
        std::array<float,17> ring{};
        unsigned position=0;
        for (unsigned n=0;n<100000;++n) {
            const float wet=ring[position];
            float value=(n%997==0 ? 4.0f : 0.0f)+feedback*exciter.process(wet,0,1,1);
            value=value>4?4:(value< -4?-4:value);ring[position]=value;
            if (++position==ring.size()) position=0;
            float l=wet,r=-0.5f*wet;limiter.process(l,r);
            check(std::isfinite(value)&&std::isfinite(l)&&std::isfinite(r)&&
                  std::fabs(l)<=0.95f&&std::fabs(r)<=0.95f,"100% and 150% feedback stress stays within sample ceiling");
        }
    }
}
void exciterIdentityResetAndChannels() {
    echolab::exciter::State exciter;
    exciter.prepare(48000,3000);exciter.reset();
    const std::array<float,7> source{{0,-0.0f,0.125f,-0.25f,0.999f,-1,4}};
    for (float value:source) {
        const float result=exciter.process(value,0,0,1);
        check(std::memcmp(&value,&result,sizeof(float))==0,"amount zero is bit-exact identity");
        near(exciter.process(value,1,1,0),value,0,"drive zero is exact identity");
        near(exciter.process(value,2,1,1),value,0,"invalid channel cannot access state");
    }
    for (unsigned n=0;n<1000;++n) {
        exciter.process(n%2?0.5f:-0.5f,0,1,1);
        near(exciter.process(0,1,1,1),0,0,"exciter channels have independent state");
    }
    exciter.reset();near(exciter.process(0,0,1,1),0,0,"exciter reset removes retained sidechain");
    near(exciter.process(std::numeric_limits<float>::infinity(),0,1,1),0,0,"exciter rejects nonfinite input");
    echolab::exciter::State a,b;
    a.prepare(48000,3000);b.prepare(48000,3000);
    const float once=a.process(0.1f,0,1,1),twice=b.process(0.2f,0,1,1);
    check(std::fabs(twice-2*once)>1e-5f,"exciter sidechain remains nonlinear without input gain boost");
}
void exciterHarmonicsAndDc() {
    echolab::exciter::State state;
    state.prepare(48000,3000);state.reset();
    constexpr unsigned warm=48000,frames=4800;
    double sum=0,re2=0,im2=0,re3=0,im3=0;
    for (unsigned n=0;n<warm+frames;++n) {
        const double phase=2*pi*1000*static_cast<double>(n)/48000;
        const float input=0.5f*static_cast<float>(std::sin(phase));
        const float output=state.process(input,0,0.45f,0.4f);
        check(std::isfinite(output),"exciter coherent sine remains finite");
        if (n>=warm) {
            const double difference=static_cast<double>(output)-input;
            sum+=difference;
            re2+=difference*std::cos(2*phase);im2-=difference*std::sin(2*phase);
            re3+=difference*std::cos(3*phase);im3-=difference*std::sin(3*phase);
        }
    }
    const double second=2*std::sqrt(re2*re2+im2*im2)/frames;
    const double third=2*std::sqrt(re3*re3+im3*im3)/frames;
    check(third>2e-4 && second>2e-4,"A04-inspired exciter adds audible-strength even and odd harmonic components");
    check(std::fabs(sum/frames)<1e-5,"exciter generated sidechain has negligible steady DC");
    state.reset();float output=0;
    for (unsigned n=0;n<96000;++n) output=state.process(0.5f,0,1,1);
    near(output,0.5f,1e-6f,"constant input does not acquire a persistent DC enhancement");
    std::cout << "synthetic_fixture exciter h2=" << second << " h3=" << third << " addedDC=" << sum/frames << '\n';
}
void saturationUnityAndSmoothness() {
    using echolab::color::Saturator;
    Saturator state;
    state.prepare(48000);
    for (float d:{0.01f,0.2f,0.4f,1.0f}) {
        float previous=-2;
        for (unsigned n=0;n<=10000;++n) {
            const float x=-2.0f+4.0f*static_cast<float>(n)/10000.0f;
            const float y=Saturator::transfer(x,d);
            check(y>=previous-1e-6f,"soft static saturation remains monotone");
            check(std::fabs(y)<=std::fabs(x)+1e-6f,"soft static saturation never boosts absolute level");
            near(y,-Saturator::transfer(-x,d),1e-7f,"saturation odd symmetry prevents DC from a symmetric signal");
            previous=y;
        }
        near(Saturator::transfer(1e-4f,d)/1e-4f,1,1e-6f,"small-signal saturation slope is unity");
    }
    const float source[]={0.0f,-0.0f,0.1f,-0.25f,1.0f,-4.0f};
    for (float x:source) {const float y=state.process(x,0,0);check(std::memcmp(&x,&y,sizeof(float))==0,"Drive zero bypass is bit exact");}
    state.reset();near(state.process(0,0,1),0,0,"saturation reset clears history");
    near(state.process(0,1,1),0,0,"saturation right history independent");
    const float limit=1.0f/(0.25f+1.75f*0.7f),epsilon=1e-4f;
    const float left=(Saturator::transfer(limit,0.7f)-Saturator::transfer(limit-epsilon,0.7f))/epsilon;
    const float right=(Saturator::transfer(limit+epsilon,0.7f)-Saturator::transfer(limit,0.7f))/epsilon;
    check(std::fabs(left-right)<0.002f,"soft-clip join has continuous first derivative");
}
void saturationAliasComparison() {
    echolab::color::Saturator saturation;saturation.prepare(48000);
    constexpr unsigned frames=4800;
    constexpr double frequencies[]={10000,18000,2000,22000,6000};
    double real[2][5]{},imaginary[2][5]{};
    for (unsigned n=0;n<frames+48;++n) {
        const float x=0.8f*static_cast<float>(std::sin(2*pi*10000*n/48000));
        const float output[2]={echolab::color::Saturator::transfer(x,0.7f),saturation.process(x,0,0.7f)};
        if (n<48) continue;
        for (unsigned mode=0;mode<2;++mode) for (unsigned bin=0;bin<5;++bin) {
            const double phase=2*pi*frequencies[bin]*n/48000;
            real[mode][bin]+=output[mode]*std::cos(phase);imaginary[mode][bin]-=output[mode]*std::sin(phase);
        }
    }
    double ratios[2]{};
    for (unsigned mode=0;mode<2;++mode) {
        double aliasEnergy=0;
        for (unsigned bin=1;bin<5;++bin) aliasEnergy+=real[mode][bin]*real[mode][bin]+imaginary[mode][bin]*imaginary[mode][bin];
        ratios[mode]=std::sqrt(aliasEnergy/(real[mode][0]*real[mode][0]+imaginary[mode][0]*imaginary[mode][0]));
    }
    check(ratios[1]<0.8*ratios[0],"2x interpolation/decimation reduces measured folded harmonics versus naive same curve");
    std::cout << "synthetic_fixture 10kHz alias/fund naive=" << ratios[0] << " two_x=" << ratios[1]
              << " change_dB=" << 20*std::log10(ratios[1]/ratios[0]) << '\n';
}
void feedbackLowpassUnityAndDrift() {
    echolab::color::FeedbackLowpass filter;
    filter.prepare(48000,0);filter.reset();
    for (const float input:{0.0f,-0.0f,0.125f,-0.5f,4.0f}) {
        const float output=filter.process(input,0,1);
        check(std::memcmp(&input,&output,sizeof(float))==0,"feedback cutoff zero is bit-exact bypass");
    }
    for (const float modulation:{-1.0f,0.0f,1.0f}) {
        filter.prepare(48000,12000);filter.reset();
        const float expected=1-std::exp(-static_cast<float>(2*pi)*12000*(1+.02f*modulation)/48000);
        near(filter.process(1,0,modulation),expected,1e-6f,"prepared lowpass drift interpolates endpoint poles");
    }
    for (const float fs:{8000.0f,44100.0f,48000.0f,96000.0f}) {
        filter.prepare(fs,12000);filter.reset();float value=0;
        for (unsigned n=0;n<10000;++n) value=filter.process(0.25f,0);
        near(value,0.25f,1e-6f,"feedback lowpass DC gain remains unity at each Fs");
        near(filter.process(0,1),0,0,"feedback filter channels independent");
    }
}
void exciterSmallSignalAndDecay() {
    for (const double frequency:{100.0,1000.0,8000.0,15000.0}) {
        echolab::exciter::State state;state.prepare(48000,3000);
        double re=0,im=0;
        for (unsigned n=0;n<52800;++n) {
            const double phase=2*pi*frequency*n/48000;
            const float output=state.process(1e-4f*static_cast<float>(std::sin(phase)),0,0.45f,0.4f,.75f);
            if(n>=48000) {re+=output*std::cos(phase);im-=output*std::sin(phase);}
        }
        const double gain=2*std::sqrt(re*re+im*im)/(4800*1e-4);
        check(gain<=1.25 && gain>=.85,"exciter high-band compensation keeps small-signal tilt modest");
        if (frequency==100) check(std::fabs(gain-1)<.001,"exciter retains bass instead of globally dividing by high-band gain");
        std::cout << "synthetic_fixture exciter small_signal f=" << frequency << " gain=" << gain << '\n';
    }
    for (const float cutoff:{0.0f,12000.0f}) for (const double frequency:{500.0,2000.0,8000.0}) {
        echolab::exciter::State state;state.prepare(48000,3000);
        echolab::color::FeedbackLowpass lowpass;lowpass.prepare(48000,cutoff);
        std::array<float,2400> ring{},control{};unsigned position=0;float peak=0,tailPeak=0;
        double coloredEnergy=0,controlEnergy=0,cross=0;
        for (unsigned n=0;n<192000;++n) {
            const float wet=ring[position];
            const float input=n<2400 ? 0.8f*static_cast<float>(std::sin(2*pi*frequency*n/48000)) : 0;
            const float reference=control[position];
            const float value=input+.75f*lowpass.process(state.process(wet,0,.45f,.4f,.75f),0);
            check(std::isfinite(value),"default exciter feedback remains finite without limiter or feedback clipping");
            ring[position]=value;control[position]=input+.75f*reference;
            if (n>=2400 && n<48000) {coloredEnergy+=wet*wet;controlEnergy+=reference*reference;cross+=wet*reference;}
            if (++position==ring.size()) position=0;
            peak=std::fmax(peak,std::fabs(value));if(n>=189600)tailPeak=std::fmax(tailPeak,std::fabs(value));
        }
        check(peak<=.8001f && tailPeak<1e-6f,"default 75% feedback exciter decays without runaway");
        const double scale=std::sqrt(coloredEnergy/controlEnergy);
        const double relativeDifference=std::sqrt(std::fmax(0.0,coloredEnergy+scale*scale*controlEnergy-2*scale*cross)/coloredEnergy);
        check(relativeDifference>.02,"exciter changes the tail after RMS matching, rather than only its level");
        std::cout << "synthetic_fixture exciter RMS_matched_tail_difference=" << relativeDifference << '\n';
        std::cout << "synthetic_fixture exciter fb75 f=" << frequency << " LP=" << cutoff << " peak=" << peak << " final50ms=" << tailPeak << '\n';
    }
}

void a04CompensationAndDecay() {
    using namespace echolab::color;
    for (auto flavor:{A04ColorFlavor::Analog,A04ColorFlavor::Platter}) {
        A04Color state;state.prepare(48000);
        for (float d:{0.0f,.01f,.1f,.4f,1.0f}) {
            const float g=A04Color::compensationFeedbackGain(d,flavor);
            near(A04Color::transfer(1e-7f,d,flavor)/1e-7f,g,2e-5f,"A04 compensation equals small-signal gain");
            float previous=-100;
            for (unsigned n=0;n<=10000;++n) {
                const float x=-4+8.0f*n/10000;
                const float y=A04Color::transfer(x,d,flavor);
                check(y>=previous-1e-6f && std::fabs(y)<=g*std::fabs(x)+1e-6f,"A04 is monotone with bounded compensated slope");
                previous=y;
            }
        }
        const float g=A04Color::compensationFeedbackGain(.4f,flavor);
        near(A04Color::transfer(.2f,.4f,flavor),g*.2f/(1+g*.2f),1e-7f,"A04 normal Drive retains the archived rational curve");
        for(float x:{0.0f,-0.0f,.5f,-1.0f,4.0f}) {
            const float y=state.process(x,0,0,flavor);
            check(std::memcmp(&x,&y,sizeof x)==0,"A04 Drive0 remains bit exact");
        }
        for (double frequency:{500.0,2000.0,8000.0}) {
            state.reset();std::array<float,2400> ring{};unsigned cursor=0;float peak=0,tail=0;
            for(unsigned n=0;n<192000;++n) {
                const float input=n<2400 ? .8f*static_cast<float>(std::sin(2*pi*frequency*n/48000)) : 0;
                const float y=state.process(input+.75f*ring[cursor]/g,0,.4f,flavor);
                ring[cursor]=y;if(++cursor==ring.size())cursor=0;
                peak=std::fmax(peak,std::fabs(y));if(n>=189600)tail=std::fmax(tail,std::fabs(y));
                check(std::isfinite(y),"A04 compensated loop remains finite without limiter");
            }
            check(peak<=1.00001f && tail<1e-6f,"A04 compensated 75% loop decays, with strong first pass and no emergency clip");
            std::cout<<"synthetic_fixture A04 g="<<g<<" f="<<frequency<<" peak="<<peak<<" final50ms="<<tail<<'\n';
        }
    }
}
void a04AliasComparison() {
    using echolab::color::A04Color;
    A04Color state;state.prepare(48000);
    const double bins[]={10000,18000,2000,22000,6000};
    double re[2][5]{},im[2][5]{};
    for(unsigned n=0;n<4848;++n) {
        const float x=.8f*static_cast<float>(std::sin(2*pi*10000*n/48000));
        const float y[]={A04Color::transfer(x,.4f),state.process(x,0,.4f)};
        if(n<48)continue;
        for(unsigned mode=0;mode<2;++mode)for(unsigned bin=0;bin<5;++bin) {
            const double phase=2*pi*bins[bin]*n/48000;
            re[mode][bin]+=y[mode]*std::cos(phase);im[mode][bin]-=y[mode]*std::sin(phase);
        }
    }
    double ratio[2]{};
    for(unsigned mode=0;mode<2;++mode) {
        double energy=0;
        for(unsigned bin=1;bin<5;++bin)energy+=re[mode][bin]*re[mode][bin]+im[mode][bin]*im[mode][bin];
        ratio[mode]=std::sqrt(energy/(re[mode][0]*re[mode][0]+im[mode][0]*im[mode][0]));
    }
    check(ratio[1]<.8*ratio[0],"A04 2x measurably reduces folded harmonics versus its naive transfer");
    std::cout<<"synthetic_fixture A04 alias_ratio naive="<<ratio[0]<<" two_x="<<ratio[1]<<" change_dB="<<20*std::log10(ratio[1]/ratio[0])<<'\n';
}
void tubeHarmonicsSagAndRecovery() {
    echolab::color::TubeColor tube;tube.prepare(48000);
    for(float x:{0.0f,-0.0f,.5f,-1.0f,4.0f}) {
        const float y=tube.process(x,0,0);check(std::memcmp(&x,&y,sizeof x)==0,"Tube Drive0 exact bypass");
    }
    tube.reset();double re[3]{},im[3]{},dc=0;
    for(unsigned n=0;n<52800;++n) {
        const double phase=2*pi*1000*n/48000;
        const float y=tube.process(.5f*static_cast<float>(std::sin(phase)),0,.4f);
        near(tube.process(0,1,.4f),0,0,"Tube channel states remain independent");
        if(n>=48000) {
            dc+=y;
            for(unsigned h=0;h<3;++h){re[h]+=y*std::cos((h+1)*phase);im[h]-=y*std::sin((h+1)*phase);}
        }
    }
    double harmonic[3]{};
    for(unsigned h=0;h<3;++h)harmonic[h]=2*std::sqrt(re[h]*re[h]+im[h]*im[h])/4800;
    check(harmonic[1]>.005 && harmonic[2]>.005,"two biased tube stages produce substantial even and odd harmonics");
    check(std::fabs(dc/4800)<1e-5,"Tube interstage/output coupling removes steady DC");
    tube.reset();
    double quiet=0,early=0,recovered=0;
    for(unsigned n=0;n<240000;++n) {
        const bool loud=n>=48000 && n<72000;
        const float x=(loud ? 1.0f : .05f)*static_cast<float>(std::sin(2*pi*1000*n/48000));
        const float y=tube.process(x,0,.4f);
        if(n>=43200 && n<48000)quiet+=y*y;
        if(n>=75000 && n<79800)early+=y*y;
        if(n>=235200)recovered+=y*y;
    }
    quiet=std::sqrt(quiet/4800);early=std::sqrt(early/4800);recovered=std::sqrt(recovered/4800);
    check(early<.98*quiet,"loud burst leaves a measurable short-term sag gain reduction");
    check(std::fabs(recovered/quiet-1)<.01,"Tube gain recovers after the loud burst");
    std::cout<<"synthetic_fixture tube H2="<<harmonic[1]<<" H3="<<harmonic[2]<<" DC="<<dc/4800<<" sagRMS quiet="<<quiet<<" early="<<early<<" recovered="<<recovered<<'\n';
    tube.reset();std::array<float,2400> ring{};unsigned cursor=0;float tail=0,peak=0;
    const float g=echolab::color::TubeColor::compensationFeedbackGain(.4f);
    for(unsigned n=0;n<192000;++n) {
        const float x=n<2400 ? .8f*static_cast<float>(std::sin(2*pi*1000*n/48000)) : 0;
        const float y=tube.process(x+.75f*ring[cursor]/g,0,.4f);
        ring[cursor]=y;if(++cursor==ring.size())cursor=0;
        peak=std::fmax(peak,std::fabs(y));if(n>=189600)tail=std::fmax(tail,std::fabs(y));
        check(std::isfinite(y),"Tube compensated feedback is finite");
    }
    check(tail<1e-6f && peak<2,"Tube compensated 75% feedback decays without limiter or hard clip");
    std::cout<<"synthetic_fixture tube FB75 peak="<<peak<<" final50ms="<<tail<<'\n';
}
void musicalRegeneration() {
    using echolab::color::Regenerator;
    Regenerator state;state.prepare(48000);
    for(float fb:{0.0f,.95f,1.0f})for(float x:{0.0f,-0.0f,.2f,-1.5f,8.0f}) {
        const float y=state.process(x,0,fb);check(std::memcmp(&x,&y,sizeof x)==0,"Regenerator FB<=1 is a complete exact bypass");
    }
    float previous=-2;
    for(unsigned n=0;n<=20000;++n) {
        const float x=-5+10.0f*n/20000;
        const float y=Regenerator::transfer(x);
        check(y>=previous-1e-6f && std::fabs(y)<=1.250001f,"Regenerator soft curve is monotone and bounded");
        if(std::fabs(x)<=.95f)near(y,x,0,"Regenerator curve retains a linear low-level range");
        previous=y;
    }
    state.reset();std::array<float,2400> ring{};unsigned cursor=0,emergencyHits=0,flat=0,tailFrames=0;
    double energy=0,differenceEnergy=0;float peak=0,tailPeak=0,last=0;
    for(unsigned n=0;n<192000;++n) {
        const float x=n<4800 ? .8f*static_cast<float>(std::sin(2*pi*1000*n/48000)) : 0;
        const float feedback=state.process(1.5f*ring[cursor],0,1.5f);
        check(std::isfinite(feedback)&&std::fabs(feedback)<=1.250001f,"150% regenerated return remains within its musical cap");
        const float write=x+feedback;
        if(std::fabs(write)>=4)++emergencyHits;
        ring[cursor]=write;if(++cursor==ring.size())cursor=0;peak=std::fmax(peak,std::fabs(write));
        if(n>=96000) {
            energy+=write*write;differenceEnergy+=(write-last)*(write-last);
            tailPeak=std::fmax(tailPeak,std::fabs(write));if(std::fabs(write)>1.249f)++flat;++tailFrames;
        }
        last=write;
    }
    const double rms=std::sqrt(energy/tailFrames),crest=tailPeak/rms;
    check(emergencyHits==0 && peak<2.051f,"150% feedback never approaches the emergency +/-4 write guard");
    check(crest>1.3 && flat<tailFrames/100,"musical regenerated tail is not mostly flat ceiling plateaux");
    std::cout<<"synthetic_fixture regeneration FB150 peak="<<peak<<" emergency_hits="<<emergencyHits<<" crest="<<crest<<" cap_fraction="<<static_cast<double>(flat)/tailFrames<<" roughness="<<std::sqrt(differenceEnergy/energy)<<'\n';
    double amplitudes[2]{};
    for(unsigned i=0;i<2;++i) {
        const double f=i ? 12000 : 1000;state.reset();double re=0,im=0;
        for(unsigned n=0;n<52800;++n) {
            const double phase=2*pi*f*n/48000;
            const float y=state.process(1e-4f*static_cast<float>(std::sin(phase)),0,1.5f);
            if(n>=48000){re+=y*std::cos(phase);im-=y*std::sin(phase);}
        }
        amplitudes[i]=2*std::sqrt(re*re+im*im)/4800;
    }
    check(amplitudes[1]<.5*amplitudes[0],"regeneration bandlimiting reduces 12kHz by more than6dB relative to1kHz");
    std::cout<<"synthetic_fixture regeneration 12k_vs_1k_dB="<<20*std::log10(amplitudes[1]/amplitudes[0])<<'\n';
}

void patterns() {
    using namespace echolab::multitap;
    static_assert(patternCount==11 && tapCapacity==8,"eight legacy patterns and three static polyrhythm grids are published");
    static_assert(tapCount(0)==3 && tapCount(1)==4 && tapCount(7)==1 && tapCount(8)==4 && tapCount(9)==6 && tapCount(10)==8,
                  "legacy pattern sizes remain stable and polyrhythms merge both complete grids");
    for (unsigned patternIndex=0;patternIndex<patternCount;++patternIndex) {
        const auto& p=pattern(patternIndex);
        check(p.name && p.name[0] && p.count>=1 && p.count<=tapCapacity,"pattern has bounded name and tap count");
        float sum=0,previous=0;
        for (unsigned head=0;head<p.count;++head) {
            const auto t=tap(patternIndex,head);
            check(t.offset>previous && t.offset<=1 && t.gain>=0 && t.gain<=1,"tap order, positive offsets and bounded gains");
            sum+=t.gain;previous=t.offset;
        }
        near(sum,1,1e-7f,"pattern total gain is normalized to one");
        near(tap(patternIndex,p.count-1).offset,1,0,"longest feedback tap remains at T");
        near(tap(patternIndex,100).gain,0,0,"invalid tap is inactive");
    }
    check(&pattern(999)==&pattern(0),"invalid pattern uses Legacy fallback");
    near(tap(0,0).offset,1.0f/3,0,"Legacy first head remains T/3");
    near(tap(0,1).offset,2.0f/3,0,"Legacy second head remains 2T/3");
}
} // namespace

int main() {
    limiterAttackLinkRelease();limiterBoundsAndFeedback();saturationUnityAndSmoothness();saturationAliasComparison();
    a04CompensationAndDecay();a04AliasComparison();tubeHarmonicsSagAndRecovery();musicalRegeneration();
    feedbackLowpassUnityAndDrift();exciterIdentityResetAndChannels();exciterHarmonicsAndDc();exciterSmallSignalAndDecay();patterns();
    if (failures) { std::cerr << failures << " color utility synthetic_fixture failures\n";return 1; }
    std::cout << "color utilities synthetic_fixture: limiter/exciter/patterns passed\n";
    return 0;
}
