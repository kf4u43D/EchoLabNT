#include "wav.h"
#include "sha256.h"
#include "profile_timing.h"
#include "dsp/engine.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#ifndef ECHOLAB_BUILD_ID
#define ECHOLAB_BUILD_ID "unversioned"
#endif
#ifndef ECHOLAB_SOURCE_SHA256
#define ECHOLAB_SOURCE_SHA256 "unknown"
#endif
#ifndef ECHOLAB_COMPILER
#define ECHOLAB_COMPILER "unknown"
#endif
#ifndef ECHOLAB_CONFIGURATION
#define ECHOLAB_CONFIGURATION "unknown"
#endif
#ifndef ECHOLAB_SANITIZER
#define ECHOLAB_SANITIZER "none"
#endif

namespace {
using echolab::Parameters;
std::string trim(const std::string& text) {
    const auto first = text.find_first_not_of(" \t\r\n");
    return first == std::string::npos ? "" : text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1);
}
double number(const std::string& text) {
    std::size_t used = 0;
    double result;
    try { result = std::stod(text, &used); }
    catch (...) { throw std::runtime_error("Invalid number: " + text); }
    if (used != text.size() || !std::isfinite(result)) throw std::runtime_error("Invalid finite number: " + text);
    return result;
}
std::size_t integer(const std::string& text) {
    if (text.empty() || text.find_first_not_of("0123456789") != std::string::npos)
        throw std::runtime_error("Invalid unsigned integer: " + text);
    std::size_t used = 0;
    const auto result = std::stoull(text,&used);
    if (used != text.size() || result > std::numeric_limits<std::size_t>::max())
        throw std::runtime_error("Integer is too large: " + text);
    return static_cast<std::size_t>(result);
}
int noteDivision(const std::string& value) {
    const char* longNames[]={"1/1","1/2","1/4","1/8","1/16","1/4 dotted","1/8 dotted","1/4 triplet","1/8 triplet","1/16 triplet"};
    for(int division=0;division<10;++division)
        if(value==echolab::host::divisionName(division) || value==longNames[division]) return division;
    if(!value.empty() && value.find_first_not_of("0123456789")==std::string::npos) {
        const auto division=integer(value);
        if(division<10) return static_cast<int>(division);
    }
    throw std::runtime_error("division must be 1/1|1/2|1/4|1/8|1/16|1/4D|1/8D|1/4T|1/8T|1/16T or 0..9");
}
void assign(Parameters& p, const std::string& key, const std::string& value,echolab::host::ProfileTiming* timing=nullptr) {
    if(timing) {
        if(key=="sync_mode") {
            if(value=="internal") timing->internal=true;
            else if(value=="free" || value=="none" || value=="off") timing->internal=false;
            else throw std::runtime_error("sync_mode must be internal or free");
            timing->explicitSync=true;return;
        }
        if(key=="tempo_bpm") {
            const double bpm=number(value);
            if(bpm<30 || bpm>300) throw std::runtime_error("tempo_bpm must be 30..300");
            timing->tempoBpm=bpm;return;
        }
        if(key=="division") { timing->division=noteDivision(value);return; }
        if(key=="right_division") { timing->rightDivision=value=="ratio" ? -1 : noteDivision(value);return; }
    }
    if(key=="mod1_source" || key=="mod2_source" || key=="mod1_destination" || key=="mod2_destination") {
        const char* sources[]={"none","lfo","envelope","cv1","cv2","wheel"};
        const char* destinations[]={"none","time","feedback","tone","drive","mix","depth","exciter","sample_rate",
                                    "mod_rate","stereo_ratio","drift","duck_amount"};
        const bool source=key.find("source")!=std::string::npos;
        const int count=source?6:echolab::modulationDestinationCount;
        int selected=-1;
        for(int i=0;i<count;++i) if(value==(source?sources[i]:destinations[i])) selected=i;
        if(!source && value=="fe") selected=8;
        if(selected<0) {const auto numeric=integer(value);if(numeric>=static_cast<unsigned>(count)) throw std::runtime_error("Invalid modulation selection: "+key);selected=static_cast<int>(numeric);}
        if(key=="mod1_source")p.mod1_source=selected; else if(key=="mod2_source")p.mod2_source=selected;
        else if(key=="mod1_destination")p.mod1_destination=selected;else p.mod2_destination=selected;
        return;
    }
    if (key == "voicing") {
        const char* names[]={"auto","a04","a05","b06"};
        for(int i=0;i<4;++i) if(value==names[i]) {p.voicing=i;return;}
        const auto selected=integer(value);
        if(selected>3)throw std::runtime_error("voicing must be auto|a04|a05|b06 or 0..3");
        p.voicing=static_cast<int>(selected);return;
    }
    if (key == "model") {
        const std::string canonical = value == "lores" ? "8bit" : (value == "ducking" ? "dynamic" : value);
        for (int i=0; i<echolab::modelCount; ++i)
            if (canonical == echolab::modelName(static_cast<echolab::Model>(i))) {
                p.model = static_cast<echolab::Model>(i); return;
            }
        throw std::runtime_error("Unknown model: " + value + "; see --help");
    }
    if (key == "loop_action") {
        const char* names[] = {"stop", "record", "play", "overdub", "clear"};
        for (int i=0;i<5;++i) if (value == names[i]) { p.loop_action=i; return; }
        const auto v=integer(value);
        if(v>4) throw std::runtime_error("loop_action must be stop|record|play|overdub|clear or 0..4");
        p.loop_action=static_cast<int>(v); return;
    }
    const float parsed = static_cast<float>(number(value));
    if (!std::isfinite(parsed)) throw std::runtime_error("Parameter exceeds float range: " + key);
    if (key == "delay_ms") { if (parsed <= 0 || parsed > 60000) throw std::runtime_error("delay_ms must be >0 and <=60000"); p.delay_ms = parsed; if(timing) timing->setManualDelay(parsed); }
    else if (key == "feedback") { if (parsed < 0 || parsed > 2) throw std::runtime_error("feedback must be 0..2 (200 percent)"); p.feedback = parsed; }
    else if (key == "mix") { if (parsed < 0 || parsed > 1) throw std::runtime_error("mix must be 0..1"); p.mix = parsed; }
    else if (key == "tone_hz") { if (parsed < 20 || parsed > 172800) throw std::runtime_error("tone_hz is outside 20..172800"); p.tone_hz = parsed; }
    else if (key == "drive") { if (parsed < 0 || parsed > 1) throw std::runtime_error("drive must be 0..1"); p.drive = parsed; }
    else if (key == "bit_depth") { if (parsed < 4 || parsed > 24 || std::floor(parsed)!=parsed) throw std::runtime_error("bit_depth must be an integer 4..24"); p.bit_depth=static_cast<int>(parsed); }
    else if (key == "mod_rate_hz") { if (parsed < .01f || parsed > 20) throw std::runtime_error("mod_rate_hz must be .01..20"); p.mod_rate_hz=parsed; }
    else if (key == "mod_depth_ms") { if (parsed < 0 || parsed > 50) throw std::runtime_error("mod_depth_ms must be 0..50"); p.mod_depth_ms=parsed; }
    else if (key == "duck_threshold_db") { if (parsed < -80 || parsed > 0) throw std::runtime_error("duck_threshold_db must be -80..0"); p.duck_threshold_db=parsed; }
    else if (key == "duck_amount") { if (parsed < 0 || parsed > 1) throw std::runtime_error("duck_amount must be 0..1"); p.duck_amount=parsed; }
    else if (key == "attack_ms") { if (parsed < .1f || parsed > 5000) throw std::runtime_error("attack_ms must be .1..5000"); p.attack_ms=parsed; }
    else if (key == "release_ms" || key == "swell_ms") {
        if (parsed < 1 || parsed > 10000) throw std::runtime_error(key+" must be 1..10000");
        if(key=="release_ms") p.release_ms=parsed; else p.swell_ms=parsed;
    }
    else if (key == "stereo_ratio") { if (parsed < .25f || parsed > 4) throw std::runtime_error("stereo_ratio must be .25..4"); p.stereo_ratio=parsed; if(timing) timing->manualStereoRatio=parsed; }
    else if (key == "limiter" || key == "limiter_enabled") { if(parsed!=0 && parsed!=1)throw std::runtime_error("limiter must be 0 or 1");p.limiter_enabled=parsed!=0; }
    else if (key == "limiter_ceiling") {if(parsed<.01f || parsed>1)throw std::runtime_error("limiter_ceiling must be .01..1");p.limiter_ceiling=parsed;}
    else if (key == "limiter_release_ms") {if(parsed<1 || parsed>2000)throw std::runtime_error("limiter_release_ms must be 1..2000");p.limiter_release_ms=parsed;}
    else if (key == "sample_rate_hz") {if(parsed<0 || (parsed>0 && parsed<100) || parsed>384000)throw std::runtime_error("sample_rate_hz must be 0 or 100..384000; zero=full Fs");p.sample_rate_hz=parsed;}
    else if (key == "digital_rack_rate_hz") {if(parsed<0 || (parsed>0 && parsed<1000) || parsed>384000)throw std::runtime_error("digital_rack_rate_hz must be 0 or 1000..384000; zero=full Fs");p.digital_rack_rate_hz=parsed;}
    else if (key == "exciter_amount") {if(parsed<0 || parsed>1)throw std::runtime_error("exciter_amount must be 0..1");p.exciter_amount=parsed;}
    else if (key == "exciter_tune_hz") {if(parsed<200 || parsed>10000)throw std::runtime_error("exciter_tune_hz must be 200..10000");p.exciter_tune_hz=parsed;}
    else if (key == "feedback_lowpass_hz") {if(parsed<0 || (parsed>0 && parsed<20) || parsed>20000)throw std::runtime_error("feedback_lowpass_hz must be 0 or 20..20000");p.feedback_lowpass_hz=parsed;}
    else if (key == "drift_amount") {if(parsed<0 || parsed>1)throw std::runtime_error("drift_amount must be 0..1");p.drift_amount=parsed;}
    else if (key == "feedback_highpass_hz") {if(parsed<0 || (parsed>0 && parsed<20) || parsed>2000)throw std::runtime_error("feedback_highpass_hz must be 0 or 20..2000");p.feedback_highpass_hz=parsed;}
    else if (key == "lores_rate_end_hz") {if(parsed<100 || parsed>384000)throw std::runtime_error("lores_rate_end_hz must be 100..384000");p.lores_rate_end_hz=parsed;}
    else if (key == "lores_rate_decay_ms") {if(parsed<10 || parsed>10000)throw std::runtime_error("lores_rate_decay_ms must be 10..10000");p.lores_rate_decay_ms=parsed;}
    else if (key == "lores_rate_envelope_amount") {if(parsed<0 || parsed>1)throw std::runtime_error("lores_rate_envelope_amount must be 0..1");p.lores_rate_envelope_amount=parsed;}
    else if (key == "lores_cutoff_hz") {if(parsed<0 || (parsed>0 && parsed<20) || parsed>20000)throw std::runtime_error("lores_cutoff_hz must be 0 or 20..20000");p.lores_cutoff_hz=parsed;}
    else if (key == "exciter_highpass_enabled" || key == "wet_tone_enabled") {
        if(parsed!=0 && parsed!=1) throw std::runtime_error(key+" must be 0 or 1");
        if(key=="exciter_highpass_enabled") p.exciter_highpass_enabled=parsed!=0;else p.wet_tone_enabled=parsed!=0;
    }
    else if (key == "modulation_enabled") {if(parsed!=0 && parsed!=1)throw std::runtime_error("modulation_enabled must be 0 or 1");p.modulation_enabled=parsed!=0;}
    else if (key == "stereo_timing_enabled") {if(parsed!=0 && parsed!=1)throw std::runtime_error("stereo_timing_enabled must be 0 or 1");p.stereo_timing_enabled=parsed!=0;}
    else if (key == "tap_pattern") {if(parsed<0 || parsed>=echolab::multitap::patternCount || std::floor(parsed)!=parsed)throw std::runtime_error("tap_pattern must be an integer 0.."+std::to_string(echolab::multitap::patternCount-1));p.tap_pattern=static_cast<int>(parsed);}
    else if (key == "mod1_amount" || key == "mod2_amount") {if(parsed<-1 || parsed>1)throw std::runtime_error("modulation amount must be -1..1");if(key=="mod1_amount")p.mod1_amount=parsed;else p.mod2_amount=parsed;}
    else throw std::runtime_error("Unknown parameter: " + key);
}
void readProfile(const std::string& path, Parameters& p,echolab::host::ProfileTiming& timing) {
    std::ifstream input(path);
    if (!input) throw std::runtime_error("Cannot open profile: " + path);
    std::string line;
    std::size_t row = 0;
    while (std::getline(input,line)) {
        ++row;
        line = trim(line);
        if (line.empty() || line[0] == '#') continue;
        const auto split = line.find('=');
        if (split == std::string::npos) throw std::runtime_error("Profile row " + std::to_string(row) + " requires key=value");
        assign(p,trim(line.substr(0,split)),trim(line.substr(split+1)),&timing);
    }
}
struct Event { std::size_t sample; std::string parameter, value; };
bool externalKey(const std::string& key) {return key=="cv1" || key=="cv2" || key=="wheel";}
float externalValue(const std::string& key,const std::string& value) {
    const float v=static_cast<float>(number(value));
    if(!std::isfinite(v) || v>(1.0f) || v<(key=="wheel"?0.0f:-1.0f))throw std::runtime_error("External modulation must be in normalized range");
    return v;
}
std::vector<Event> readEvents(const std::string& path, const Parameters& initial,const echolab::host::ProfileTiming& initialTiming,float maxDelayMs,unsigned sampleRate) {
    std::ifstream input(path);
    if (!input) throw std::runtime_error("Cannot open event CSV: " + path);
    std::vector<Event> events;
    std::string line;
    Parameters check = initial;
    auto timing=initialTiming;
    std::size_t row = 0;
    while (std::getline(input,line)) {
        ++row; line = trim(line);
        if (line.empty() || line[0] == '#') continue;
        if (line == "sample,param,value") continue;
        const auto first = line.find(','), second = first == std::string::npos ? first : line.find(',',first+1);
        if (first == std::string::npos || second == std::string::npos || line.find(',',second+1) != std::string::npos)
            throw std::runtime_error("CSV row " + std::to_string(row) + " requires sample,param,value");
        Event e{integer(trim(line.substr(0,first))),trim(line.substr(first+1,second-first-1)),trim(line.substr(second+1))};
        if (!events.empty() && e.sample < events.back().sample) throw std::runtime_error("Events must be sorted by sample (equal times preserve CSV order)");
        if(externalKey(e.parameter))externalValue(e.parameter,e.value);
        else { assign(check,e.parameter,e.value,&timing);timing.resolve(check,maxDelayMs,sampleRate); }
        events.push_back(std::move(e));
    }
    return events;
}
std::string json(const std::string& value) {
    std::string result = "\"";
    for (unsigned char c : value) {
        if (c == '\\' || c == '"') { result += '\\'; result += static_cast<char>(c); }
        else if (c < 32) {
            const char* hex = "0123456789abcdef";
            result += "\\u00"; result += hex[c >> 4]; result += hex[c & 15];
        } else result += static_cast<char>(c);
    }
    return result + '"';
}
bool samePath(const std::string& a, const std::string& b) {
    namespace fs = std::filesystem;
    if (fs::exists(a) && fs::exists(b)) return fs::equivalent(a,b);
    return fs::absolute(a).lexically_normal() == fs::absolute(b).lexically_normal();
}
void help() {
    std::cout << "EchoLabNT offline renderer (provisional project algorithms)\n"
        "Usage: echolabnt_render --input excitation.wav --output render.wav [options]\n"
        "  --model digital|analog|tube|pingpong|multihead|8bit|dynamic|tape|sweep\n"
        "          analogmod|digitalmod|reverse|autovolume|digitalrack|platter|stereo|looper|exciter|sweepup|tubea04\n"
        "          aliases: lores=8bit, ducking=dynamic; default digital\n"
        "  --sync-mode internal|free     default internal: 120 BPM, quarter note = 500 ms\n"
        "  --tempo-bpm N                 30..300, default 120\n"
        "  --division NOTE               default 1/4; 1/1|1/2|1/4|1/8|1/16|1/4D|1/8D|1/4T|1/8T|1/16T\n"
        "  --right-division NOTE         Stereo right note, or ratio (default)\n"
        "  --delay-ms N                  explicitly selects Free; manual fallback 350 ms, capacity bounded\n"
        "  --feedback N                  0..2 (200 percent), default .28\n"
        "  --mix N                       linear wet mix 0..1 (default 0.39)\n"
        "  --tone-hz N                   default 3500; effective maximum 0.45*Fs\n"
        "  --drive N                     0..1, default .4, colored models\n"
        "  --bit-depth N                 4..24, default 8 (8bit)\n"
        "  --mod-rate-hz N               .01..20, default 1 (A05/B06 Analog Mod x.72)\n"
        "  --mod-depth-ms N              0..50, default 6\n"
        "  --duck-threshold-db N          -80..0 dBFS, default -24\n"
        "  --duck-amount N               0..1, default .9; wet only\n"
        "  --attack-ms N                 .1..5000, default 5\n"
        "  --release-ms N                1..10000, default 250\n"
        "  --swell-ms N                  1..10000, default 250\n"
        "  --stereo-ratio N              .25..4, default 1.5 (R/L delay)\n"
        "  --loop-action NAME            stop|record|play|overdub|clear; default record\n"
        "  --limiter 0|1                 musical gain stage; -1 dB hard ceiling always active\n"
        "  --limiter-ceiling N           .01..1, clamped to -1 dB; release default 150 ms\n"
        "  --sample-rate-hz N            8bit held sample rate, default 8000; 0=full Fs\n"
        "  --digital-rack-rate-hz N      Rack internal sample rate, default 16000; 0=full Fs\n"
        "  --exciter-highpass-enabled 0|1 300 Hz Exciter high-pass, default off\n"
        "  --wet-tone-enabled 0|1        wet Tone filtering, default off\n"
        "  --exciter-amount N            0..1, default .1; tune default 4000 Hz\n"
        "  --voicing NAME                auto|a04|a05|b06; auto selects preferred version\n"
        "  --modulation-enabled 0|1      internal LFO/wow/drift; CV/wheel stay active\n"
        "  --feedback-lowpass-hz N       default 12000; 0=bypass; Digital Rack x.5\n"
        "  --feedback-highpass-hz N      0 or 20..2000, default 80; 0=bypass\n"
        "  --drift-amount N              subtle slow drift 0..1, default .25\n"
        "  --lores-rate-end-hz N         final internal Fs, default 1200\n"
        "  --lores-rate-decay-ms N       rate-envelope decay, default 1500\n"
        "  --lores-rate-envelope-amount N 0..1, default 1; onset starts descending Fs\n"
        "  --lores-cutoff-hz N           wet low-pass, default 2000; 0=bypass\n"
        "  --tap-pattern N               0.." << echolab::multitap::patternCount-1 << ", multitap rhythms\n"
        "  --mod1-source NAME            none|lfo|envelope|cv1|cv2|wheel (also mod2)\n"
        "  --mod1-destination NAME       none|time|feedback|tone|drive|mix|depth|exciter|sample_rate (fe)\n"
        "                                mod_rate|stereo_ratio|drift|duck_amount (also mod2)\n"
        "  --stereo-timing-enabled N     0|1; independent L/R on Analog, Tube, Tape and Analog Mod\n"
        "  --mod1-amount N               -1..1 (also mod2 destination/amount)\n"
        "  --cv1-input file.wav          normalized CV, +/-1 maps to +/-5V; also --cv2-input\n"
        "Events may set normalized cv1, cv2, wheel as well as effect parameters.\n"
        "  --max-delay-ms N              memory capacity, default 2500, maximum 60000\n"
        "  --tail N                      append N seconds of silence (default 2)\n"
        "  --block N                     host chunk size (default 64)\n"
        "  --profile file                key=value parameter file, CLI overrides\n"
        "  --events file.csv             absolute sample,param,value; before sample\n"
        "  --metadata file.json          default <output>.json\n"
        "  --diagnostic                  print effective configuration\n"
        "  --help                        show this help\n"
        "Reads mono/stereo unsigned PCM8, PCM16/24/32 or float32 RIFF WAV. Mono duplicates to stereo.\n"
        "Writes stereo float32 at input Fs, without normalization. No reference fidelity claim.\n";
}
}

int main(int argc, char** argv) {
    try {
        std::vector<std::pair<std::string,std::string>> options;
        bool diagnostic = false;
        for (int i = 1; i < argc; ++i) {
            const std::string key = argv[i];
            if (key == "--help" || key == "-h") { help(); return 0; }
            if (key == "--diagnostic") { diagnostic = true; continue; }
            if (i+1 == argc) throw std::runtime_error("Missing value for " + key);
            for (const auto& option : options)
                if (option.first == key) throw std::runtime_error("Duplicate option: " + key);
            options.emplace_back(key,argv[++i]);
        }
        Parameters parameters;
        echolab::host::ProfileTiming timing;
        std::string inputPath, outputPath, metadataPath, profilePath, eventsPath,cv1Path,cv2Path;
        for (const auto& option : options)
            if (option.first == "--profile") { profilePath = option.second; readProfile(profilePath,parameters,timing); }
        timing.beginLayer();
        float maxDelayMs = 2500;
        double tailSeconds = 2;
        std::size_t block = 64;
        for (const auto& [key,value] : options) {
            if (key == "--input") inputPath = value;
            else if (key == "--output") outputPath = value;
            else if (key == "--metadata") metadataPath = value;
            else if (key == "--events") eventsPath = value;
            else if (key == "--cv1-input") cv1Path=value;
            else if (key == "--cv2-input") cv2Path=value;
            else if (key == "--profile") {}
            else if (key == "--max-delay-ms") maxDelayMs = static_cast<float>(number(value));
            else if (key == "--tail") tailSeconds = number(value);
            else if (key == "--block") block = integer(value);
            else if (key == "--delay-ms") assign(parameters,"delay_ms",value,&timing);
            else if (key == "--tone-hz") assign(parameters,"tone_hz",value,&timing);
            else if (key == "--model" || key == "--feedback" || key == "--mix" || key == "--drive") assign(parameters,key.substr(2),value,&timing);
            else if (key.rfind("--",0)==0) {
                std::string parameter=key.substr(2);
                std::replace(parameter.begin(),parameter.end(),'-','_');
                assign(parameters,parameter,value,&timing);
            } else throw std::runtime_error("Unknown option: " + key);
        }
        if (inputPath.empty() || outputPath.empty()) throw std::runtime_error("--input and --output are required; see --help");
        if (metadataPath.empty()) metadataPath = outputPath + ".json";
        if (!block || block > 1048576 || tailSeconds < 0 || tailSeconds > 3600)
            throw std::runtime_error("block must be 1..1048576 and tail must be 0..3600 seconds");
        for (const auto& source : {inputPath,profilePath,eventsPath,cv1Path,cv2Path})
            if (!source.empty() && (samePath(source,outputPath) || samePath(source,metadataPath)))
                throw std::runtime_error("Output/metadata must not overwrite an input source");
        if (samePath(outputPath,metadataPath)) throw std::runtime_error("WAV and metadata paths must differ");
        const auto input = echolab::host::readWave(inputPath);
        echolab::host::Wave cv1Wave,cv2Wave;
        if(!cv1Path.empty())cv1Wave=echolab::host::readWave(cv1Path);
        if(!cv2Path.empty())cv2Wave=echolab::host::readWave(cv2Path);
        if((!cv1Path.empty() && cv1Wave.sampleRate!=input.sampleRate) || (!cv2Path.empty() && cv2Wave.sampleRate!=input.sampleRate))throw std::runtime_error("CV WAV sample rate must equal input Fs; no implicit resampling");
        const auto inputSha = echolab::host::sha256File(inputPath);
        const auto profileSha = profilePath.empty() ? "" : echolab::host::sha256File(profilePath);
        const auto eventsSha = eventsPath.empty() ? "" : echolab::host::sha256File(eventsPath);
        const auto cv1Sha=cv1Path.empty()?"":echolab::host::sha256File(cv1Path);
        const auto cv2Sha=cv2Path.empty()?"":echolab::host::sha256File(cv2Path);
        const auto required = echolab::Engine::requiredSamples(static_cast<float>(input.sampleRate),maxDelayMs);
        if (!required) throw std::runtime_error("Invalid --max-delay-ms / sample rate combination");
        std::vector<float> memory(required);
        echolab::Engine engine;
        if (!engine.prepare(static_cast<float>(input.sampleRate),maxDelayMs,memory.data(),memory.size()))
            throw std::runtime_error("DSP preparation failed");
        timing.resolve(parameters,maxDelayMs,input.sampleRate);
        engine.setParameters(parameters);
        engine.reset();
        parameters = engine.parameters();
        const Parameters initial = parameters;
        const auto initialTiming=timing;
        const auto events = eventsPath.empty() ? std::vector<Event>{} : readEvents(eventsPath,parameters,timing,maxDelayMs,input.sampleRate);
        const auto tailFrames = static_cast<std::size_t>(std::llround(tailSeconds * input.sampleRate));
        const auto frames = input.frames() + tailFrames;
        if (frames > (0xffffffffULL - 50) / 8) throw std::runtime_error("Rendered WAV would exceed RIFF size limit");
        if (!events.empty() && events.back().sample > frames) throw std::runtime_error("An event is beyond the rendered output (input + tail)");
        std::vector<float> left(frames,0), right(frames,0), outLeft(frames), outRight(frames);
        for (std::size_t i = 0; i != input.frames(); ++i) {
            left[i] = input.samples[i * input.channels];
            right[i] = input.samples[i * input.channels + (input.channels == 2 ? 1 : 0)];
        }
        std::size_t event = 0;
        float cv1=0,cv2=0,wheel=0;
        auto applyEvent=[&](const Event& e) {
            if(externalKey(e.parameter)) {
                const float v=externalValue(e.parameter,e.value);
                if(e.parameter=="cv1")cv1=v;else if(e.parameter=="cv2")cv2=v;else wheel=v;
                engine.setExternalModulation(cv1,cv2,wheel);
            } else {assign(parameters,e.parameter,e.value,&timing);timing.resolve(parameters,maxDelayMs,input.sampleRate);engine.setParameters(parameters);}
        };
        for (std::size_t begin = 0; begin < frames;) {
            const std::size_t end = std::min(frames,begin + block);
            while (begin < end) {
                while (event < events.size() && events[event].sample == begin) {
                    applyEvent(events[event]);
                    ++event;
                }
                const auto stop = event < events.size() ? std::min(end,events[event].sample) : end;
                if(cv1Path.empty() && cv2Path.empty())engine.processBlock(left.data()+begin,right.data()+begin,outLeft.data()+begin,outRight.data()+begin,stop-begin);
                else for(size_t i=begin;i<stop;++i) {
                    const float a=cv1Path.empty()?cv1:(i<cv1Wave.frames()?cv1Wave.samples[i*cv1Wave.channels]:0);
                    const float b=cv2Path.empty()?cv2:(i<cv2Wave.frames()?cv2Wave.samples[i*cv2Wave.channels]:0);
                    engine.setExternalModulation(a,b,wheel);engine.processSample(left[i],right[i],outLeft[i],outRight[i]);
                }
                begin = stop;
            }
        }
        // End-of-stream events are valid state updates with no audible sample.
        while (event < events.size()) {
            applyEvent(events[event]); ++event;
        }
        echolab::host::Wave output;
        output.channels = 2; output.sampleRate = input.sampleRate; output.samples.resize(frames * 2);
        for (std::size_t i = 0; i != frames; ++i) { output.samples[2*i] = outLeft[i]; output.samples[2*i+1] = outRight[i]; }
        echolab::host::writeWave(outputPath,output);
        const auto outputSha = echolab::host::sha256File(outputPath);
        std::ofstream metadata(metadataPath,std::ios::trunc);
        if (!metadata) throw std::runtime_error("Cannot create metadata: " + metadataPath);
        metadata << std::setprecision(9)
            << "{\n  \"schema_version\": 1,\n  \"build_id\": " << json(ECHOLAB_BUILD_ID)
            << ",\n  \"source_sha256\": " << json(ECHOLAB_SOURCE_SHA256)
            << ",\n  \"compiler\": " << json(ECHOLAB_COMPILER)
            << ",\n  \"configuration\": " << json(ECHOLAB_CONFIGURATION)
            << ",\n  \"sanitizer\": " << json(ECHOLAB_SANITIZER)
            << ",\n  \"engine\": \"echolab-cpp17\",\n  \"status\": \"provisional\",\n"
            << "  \"fidelity\": \"not_qualified\",\n  \"input\": " << json(inputPath)
            << ",\n  \"output\": " << json(outputPath) << ",\n  \"profile\": " << (profilePath.empty() ? "null" : json(profilePath))
            << ",\n  \"input_sha256\": " << json(inputSha) << ",\n  \"output_sha256\": " << json(outputSha)
            << ",\n  \"profile_sha256\": " << (profilePath.empty() ? "null" : json(profileSha))
            << ",\n  \"events_sha256\": " << (eventsPath.empty() ? "null" : json(eventsSha))
            << ",\n  \"cv1_input\": " << (cv1Path.empty()?"null":json(cv1Path)) << ",\n  \"cv1_sha256\": " << (cv1Path.empty()?"null":json(cv1Sha))
            << ",\n  \"cv2_input\": " << (cv2Path.empty()?"null":json(cv2Path)) << ",\n  \"cv2_sha256\": " << (cv2Path.empty()?"null":json(cv2Sha))
            << ",\n  \"events\": " << (eventsPath.empty() ? "null" : json(eventsPath))
            << ",\n  \"event_count\": " << events.size() << ",\n  \"sample_rate_hz\": " << input.sampleRate
            << ",\n  \"input_channels\": " << input.channels << ",\n  \"output_channels\": 2,\n  \"input_frames\": " << input.frames()
            << ",\n  \"output_frames\": " << frames << ",\n  \"tail_frames\": " << tailFrames
            << ",\n  \"block_frames\": " << block << ",\n  \"normalized\": false,\n  \"output_encoding\": \"float32\",\n"
            << "  \"mono_policy\": \"duplicate_to_stereo\",\n  \"max_delay_ms\": " << maxDelayMs
            << ",\n  \"delay_memory_bytes\": " << memory.size() * sizeof(float)
            << ",\n  \"rhythm\": {\"sync_mode\": " << json(initialTiming.internal ? "internal" : "free")
            << ", \"tempo_bpm\": " << initialTiming.tempoBpm
            << ", \"division\": " << json(echolab::host::divisionName(initialTiming.division))
            << ", \"right_division\": " << json(initialTiming.rightDivision<0 ? "ratio" : echolab::host::divisionName(initialTiming.rightDivision))
            << ", \"requested_left_ms\": " << initialTiming.requestedLeftMs()
            << ", \"effective_left_ms\": " << initial.delay_ms
            << ", \"requested_right_ms\": " << (echolab::stereoTimingActive(initial) ? initialTiming.requestedRightMs() : initialTiming.requestedLeftMs())
            << ", \"effective_right_ms\": " << (echolab::stereoTimingActive(initial) ? std::min(maxDelayMs,initial.delay_ms*initial.stereo_ratio) : initial.delay_ms)
            << ", \"left_clipped\": " << (std::fabs(initialTiming.requestedLeftMs()-initial.delay_ms)>.001 ? "true" : "false")
            << ", \"right_clipped\": " << (echolab::stereoTimingActive(initial) && std::fabs(initialTiming.requestedRightMs()-std::min(maxDelayMs,initial.delay_ms*initial.stereo_ratio))>.001 ? "true" : "false") << "}"
            << ",\n  \"resolved_voicing\": " << json(echolab::voicingName(echolab::resolvedVoicing(initial.model,initial.voicing)))
            << ",\n  \"parameters\": {\"model\": " << json(echolab::modelName(initial.model))
            << ", \"delay_ms\": " << initial.delay_ms << ", \"feedback\": " << initial.feedback
            << ", \"mix\": " << initial.mix << ", \"tone_hz\": " << initial.tone_hz << ", \"drive\": " << initial.drive << "},\n"
            << "  \"controls\": {\"bit_depth\": " << initial.bit_depth << ", \"mod_rate_hz\": " << initial.mod_rate_hz
            << ", \"mod_depth_ms\": " << initial.mod_depth_ms << ", \"duck_threshold_db\": " << initial.duck_threshold_db
            << ", \"duck_amount\": " << initial.duck_amount << ", \"attack_ms\": " << initial.attack_ms
            << ", \"release_ms\": " << initial.release_ms << ", \"swell_ms\": " << initial.swell_ms
            << ", \"stereo_ratio\": " << initial.stereo_ratio << ", \"loop_action\": " << initial.loop_action
            << ", \"stereo_timing_enabled\": " << (initial.stereo_timing_enabled ? "true" : "false")
            << ", \"limiter_enabled\": " << (initial.limiter_enabled?"true":"false") << ", \"limiter_ceiling\": " << initial.limiter_ceiling
            << ", \"limiter_release_ms\": " << initial.limiter_release_ms << ", \"sample_rate_hz\": " << initial.sample_rate_hz
            << ", \"digital_rack_rate_hz\": " << initial.digital_rack_rate_hz
            << ", \"exciter_amount\": " << initial.exciter_amount << ", \"exciter_tune_hz\": " << initial.exciter_tune_hz
            << ", \"tap_pattern\": " << initial.tap_pattern << ", \"mod1_source\": " << initial.mod1_source
            << ", \"mod1_destination\": " << initial.mod1_destination << ", \"mod1_amount\": " << initial.mod1_amount
            << ", \"mod2_source\": " << initial.mod2_source << ", \"mod2_destination\": " << initial.mod2_destination
            << ", \"mod2_amount\": " << initial.mod2_amount
            << ", \"feedback_lowpass_hz\": " << initial.feedback_lowpass_hz
            << ", \"drift_amount\": " << initial.drift_amount
            << ", \"feedback_highpass_hz\": " << initial.feedback_highpass_hz
            << ", \"lores_rate_end_hz\": " << initial.lores_rate_end_hz
            << ", \"lores_rate_decay_ms\": " << initial.lores_rate_decay_ms
            << ", \"lores_rate_envelope_amount\": " << initial.lores_rate_envelope_amount
            << ", \"lores_cutoff_hz\": " << initial.lores_cutoff_hz
            << ", \"exciter_highpass_enabled\": " << (initial.exciter_highpass_enabled ? "true" : "false")
            << ", \"wet_tone_enabled\": " << (initial.wet_tone_enabled ? "true" : "false")
            << ", \"voicing\": " << initial.voicing
            << ", \"modulation_enabled\": " << (initial.modulation_enabled?"true":"false") << "}\n}\n";
        if (!metadata) throw std::runtime_error("Failed writing metadata");
        std::cout << "Rendered " << frames << " stereo frames at " << input.sampleRate << " Hz -> " << outputPath << '\n';
        if (diagnostic) std::cout << "Build " << ECHOLAB_BUILD_ID << "; " << echolab::modelName(initial.model)
            << "; buffer " << memory.size() * sizeof(float) << " bytes; events " << events.size()
            << "; normalization disabled; fidelity not qualified\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "echolabnt_render: " << error.what() << '\n';
        return 1;
    }
}
