/*
 * This file is part of libsidplayfp, a SID player engine.
 *
 *  Copyright (C) 2025-2026 Leandro Nini
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 */

// Based on cRSID lightweight RealSID by Hermit (Mihaly Horvath)

#include "SID.h"

#include "sl_defs.h"
#include "sl_constants.h"
#include "filt_tables.h"
#include "cw_tables.h"

#include <algorithm>
#include <iterator>
#include <cstdint>

//TODO:  MODIZER changes start / YOYOFR
extern "C" int sid_v4;
extern "C" {
#include "../../../../../src/ModizerVoicesData.h"
}
//TODO:  MODIZER changes end / YOYOFR

namespace SIDLite
{

int SID::clock(unsigned int cycles, short* buf)
{
    int i = 0;

    //TODO:  MODIZER changes start / YOYOFR
    // Oscilloscope slot of this chip: 4 per chip (3 voices + digi), chips
    // numbered in order of first clock.
    int sid_idx=0;
    while (sid_idx<MAXSID_CHIPS) {
        if (m_sid_chipId[sid_idx]==NULL) {
            m_sid_chipId[sid_idx]=(void*)this;
            break;
        }
        if (m_sid_chipId[sid_idx]==(void*)this) break;
        sid_idx++;
    }
    sid_idx=(sid_idx%m_sid_chipNb)*4;
    const int64_t smplIncr=1<<MODIZER_OSCILLO_OFFSET_FIXEDPOINT;
    // Playback speed (patch from Modizer): more chip cycles per output sample.
    if (mdz_ratio_fp_inc!=0) cycles=cycles*mdz_ratio_fp_inc/65536;
    //TODO:  MODIZER changes end / YOYOFR

    while (cycles > 0)
    {
        short output;
        if (generateSample(cycles, output))
        {
            buf[i] = output;
            i++;

            //TODO:  MODIZER changes start / YOYOFR
            const int v[4] = { wavgen.ChannelOutput[0]>>6, wavgen.ChannelOutput[1]>>6,
                               wavgen.ChannelOutput[2]>>6, sid_v4>>10 };
            for (int j=0;j<4;j++) {
                m_voice_buff[sid_idx+j][(m_voice_current_ptr[sid_idx+j]>>MODIZER_OSCILLO_OFFSET_FIXEDPOINT)&(SOUND_BUFFER_SIZE_SAMPLE*8-1)]=LIMIT8(v[j]);
                m_voice_current_ptr[sid_idx+j]+=smplIncr;
            }
            //TODO:  MODIZER changes end / YOYOFR
        }
    }

    //YOYOFR
    for (int j=0;j<3;j++) {
        if (wavgen.ChannelFreq[j]) {
            vgm_last_note[sid_idx+j]=wavgen.ChannelFreq[j];
            vgm_last_vol[sid_idx+j]=wavgen.ChannelEnv[j];
        }
    }
    //YOYOFR

    return i;
}

inline bool SID::generateSample(unsigned int &cycles, short &output)
{
    // Cycle-based part of emulations:

    while (SampleCycleCnt <= s.SampleClockRatio)
    {
        // no more cycles, can't produce output
        if (cycles == 0)
            return false;

        unsigned char InstructionCycles = std::min(7u, cycles);
        SampleCycleCnt += InstructionCycles << 4;
        cycles -= InstructionCycles;

        adsr.clock(InstructionCycles);
    }

    SampleCycleCnt -= s.SampleClockRatio;

    // Samplerate-based part of emulations:

    wg_output_t wg_out = wavgen.clock(&adsr);
    int sample = filter.clock(wg_out.first, wg_out.second);

    // saturation logic on overflow
    if (sample > INT16_MAX)
        sample = INT16_MAX;
    else if (sample < INT16_MIN)
        sample = INT16_MIN;
    output = static_cast<short>(sample);

    return true;
}

void SID::write(int addr, int value)
{
    regs[addr] = value;
}

int SID::read(int addr) const
{
    switch (addr)
    {
        case 0x1B:
            return wavgen.getOsc3();
        case 0x1C:
            return wavgen.getEnv3();
        default:
            return 0;
    }
}

SID::SID() :
    adsr(regs),
    filter(&s, regs),
    wavgen(&s, regs)
{
    setChipModel(model_t::MOS8580);
    reset();
}

void SID::reset()
{
    SampleCycleCnt = 0;

    std::fill(std::begin(regs), std::end(regs), 0);

    adsr.reset();
    filter.reset();
    wavgen.reset();
}

bool SID::setSamplingParameters(unsigned int clockFrequency, unsigned short samplingFrequency)
{
    if ((samplingFrequency < 8000) || (samplingFrequency > 48000))
        return false;

    filter.rebuildCutoffTables(samplingFrequency);

    // shifting (multiplication) enhances SampleClockRatio precision
    s.SampleClockRatio = (clockFrequency << CRSID_CLOCK_FRACTIONAL_BITS) / samplingFrequency;
    return true;
}

void SID::setChipModel(model_t model)
{
    s.sid8580 = model == model_t::MOS8580;
}

void SID::setRealSIDmode(bool mode)
{
    s.RealSIDmode = mode;
}

}
