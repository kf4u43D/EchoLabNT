#pragma once

#include <algorithm>
#include "dsp/engine.h"
#include "../nt/clock_sync.h"

namespace echolab { namespace host {

inline const char* divisionName(int division) noexcept {
    static constexpr const char* names[]={"1/1","1/2","1/4","1/8","1/16","1/4D","1/8D","1/4T","1/8T","1/16T"};
    return names[std::max(0,std::min(9,division))];
}

// Host configuration resolves musical controls into the shared DSP parameters.
// A legacy delay value selects Free within each profile/CLI override layer,
// unless that layer explicitly chooses its synchronization mode.
struct ProfileTiming {
    bool internal=true;
    bool explicitSync=false;
    double tempoBpm=120.0;
    int division=2;
    int rightDivision=-1; // Ratio compatibility mode.
    float manualDelayMs=350.0f;
    float manualStereoRatio=1.5f;

    void beginLayer() noexcept { explicitSync=false; }
    void setManualDelay(float value) noexcept {
        manualDelayMs=value;
        if(!explicitSync) internal=false;
    }
    double requestedLeftMs() const noexcept {
        return internal ? 60000.0/tempoBpm*echolab_nt::ClockSync::quarterMultiplier(division) : manualDelayMs;
    }
    double requestedRightMs() const noexcept {
        return internal && rightDivision>=0 ? 60000.0/tempoBpm*echolab_nt::ClockSync::quarterMultiplier(rightDivision) :
            requestedLeftMs()*manualStereoRatio;
    }
    void resolve(Parameters& parameters,float maxDelayMs,unsigned sampleRate) const noexcept {
        const double minimum=1000.0/sampleRate;
        const double left=std::max(minimum,std::min(static_cast<double>(maxDelayMs),requestedLeftMs()));
        parameters.delay_ms=static_cast<float>(left);
        if(internal && stereoTimingActive(parameters)) {
            const double right=std::max(minimum,std::min(static_cast<double>(maxDelayMs),requestedRightMs()));
            parameters.stereo_ratio=static_cast<float>(right/left);
        } else parameters.stereo_ratio=manualStereoRatio;
    }
};

} } // namespace echolab::host
