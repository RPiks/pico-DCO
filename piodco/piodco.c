///////////////////////////////////////////////////////////////////////////////
//
//  Roman Piksaykin [piksaykin@gmail.com], R2BDY
//  https://www.qrz.com/db/r2bdy
//
///////////////////////////////////////////////////////////////////////////////
//
//
//  piodco.c - Digital controlled radio freq oscillator based on PIO.
// 
//
//  DESCRIPTION
//
//      The oscillator provides precise generation of any frequency ranging
//  from 1.1 to 9.4 MHz with tenth's of millihertz resolution (please note that
//  this is relative resolution owing to the fact that the absolute accuracy of 
//  onboard crystal of pi pico is limited).
//      The DCO uses phase locked loop principle programmed in C.
//      The DCO does *NOT* use any floating point operations - all time-critical
//  instructions run in 1 CPU cycle.
//      Currently the upper freq. limit is about 9.8 MHz and it is achieved only
//  using pi pico overclocking to 270MHz.
//      Owing to the meager frequency step, it is possible to use 3, 5, or 7th
//  harmonics of generated frequency. Such solution completely cover all HF and
//  low band up to about 66 MHz.
//      This is an experimental project of amateur radio class and it is devised
//  by me on the free will base in order to experiment with QRP narrowband
//  digital modes.
//      I gracefully appreciate any thoughts or comments on that matter.
//
//  PLATFORM
//      Raspberry Pi pico.
//
//  REVISION HISTORY
// 
//      Rev 0.1   05 Nov 2023   Initial release
//      Rev 0.2   18 Nov 2023
//
//  PROJECT PAGE
//      https://github.com/RPiks/pico-hf-oscillator
//
//  LICENCE
//      MIT License (http://www.opensource.org/licenses/mit-license.php)
//
//  Copyright (c) 2023 by Roman Piksaykin
//  
//  Permission is hereby granted, free of charge,to any person obtaining a copy
//  of this software and associated documentation files (the Software), to deal
//  in the Software without restriction,including without limitation the rights
//  to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
//  copies of the Software, and to permit persons to whom the Software is
//  furnished to do so, subject to the following conditions:
//
//  The above copyright notice and this permission notice shall be included in
//  all copies or substantial portions of the Software.
//
//  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
//  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
//  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
//  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
//  LIABILITY,WHETHER IN AN ACTION OF CONTRACT,TORT OR OTHERWISE, ARISING FROM,
//  OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
//  THE SOFTWARE.
///////////////////////////////////////////////////////////////////////////////
#include "piodco.h"

#include <string.h>
#include "../lib/assert.h"

#include "build/dco.pio.h"

int32_t si32precise_cycles; /* External in order to support ISR. */

/// @brief Initializes DCO context and prepares PIO hardware.
/// @param pdco Ptr to DCO context.
/// @param gpio The GPIO of DCO output.
/// @param cpuclkhz The system CPU clock freq., Hz.
/// @return 0 if OK.
int PioDCOInit(PioDco *pdco, int gpio, int cpuclkhz)
{
    assert_(pdco);
    assert_(cpuclkhz);

    memset(pdco, 0, sizeof(PioDco));

    pdco->_clkfreq_hz = cpuclkhz;
    pdco->_pio = pio0;
    pdco->_gpio = gpio;
    pdco->_offset = pio_add_program(pdco->_pio, &dco_program);
    pdco->_ism = pio_claim_unused_sm(pdco->_pio, true);

    dco_program_init(pdco->_pio, pdco->_ism, pdco->_offset, pdco->_gpio);
    pdco->_pio_sm = dco_program_get_default_config(pdco->_offset);

    sm_config_set_set_pins(&pdco->_pio_sm, pdco->_gpio, 1);
    pio_gpio_init(pdco->_pio, pdco->_gpio);
    pio_sm_init(pdco->_pio, pdco->_ism, pdco->_offset, &pdco->_pio_sm);

    return 0;
}

/// @brief Sets DCO working frequency in Hz: Fout = ui32_frq_hz + ui32_frq_millihz * 1e-3.
/// @param pdco Ptr to DCO context.
/// @param i32_frq_hz The `coarse` part of frequency [Hz]. Might be negative.
/// @param ui32_frq_millihz The `fine` part of frequency [Hz].
/// @return 0 if OK. -1 invalid freq.
/// @attention The func can be called while DCO running.
int PioDCOSetFreq(PioDco *pdco, uint32_t ui32_frq_hz, int32_t ui32_frq_millihz)
{
    assert_(pdco);
    assert(pdco->_clkfreq_hz);

    /* RPix: Calculate an accurate value of phase increment of the freq 
       per 1 tick of CPU clock, here 2^24 is scaling coefficient. */
    const int64_t i64denominator = 2000LL * (int64_t)ui32_frq_hz + (int64_t)ui32_frq_millihz;
    pdco->_frq_cycles_per_pi = (int32_t)(((int64_t)pdco->_clkfreq_hz * (int64_t)(1<<24) * 1000LL
                                         +(i64denominator>>1)) / i64denominator);

    si32precise_cycles = pdco->_frq_cycles_per_pi;

    return 0;
}

/// @brief Obtains the frequency shift [milliHz] which is calculated for a given frequency.
/// @param pdco Ptr to Context.
/// @param u64_desired_frq_millihz The frequency for which we want to calculate correction.
/// @return The value of correction we need to subtract from desired freq. to compensate
/// @return Pico's reference clock shift. 2854974.
int32_t PioDCOGetFreqShiftMilliHertz(const PioDco *pdco, uint64_t u64_desired_frq_millihz)
{
    assert_(pdco);
    if(!pdco->_pGPStime)
    {
        return 0U;
    }

    static int64_t i64_last_correction = 0;
    const int64_t dt = pdco->_pGPStime->_time_data._i32_freq_shift_ppb; /* Parts per billion. */
    if(dt)
    {
        i64_last_correction = dt;
    }

    int32_t i32ret_millis;
    if(i64_last_correction)
    {
        int64_t i64corr_coeff = (u64_desired_frq_millihz + 500000LL) / 1000000LL;
        i32ret_millis = (i64_last_correction * i64corr_coeff + 50000LL) / 1000000LL;

        return i32ret_millis;
    }

    return 0U;
}

/// @brief Starts the DCO.
/// @param pdco Ptr to DCO context.
void PioDCOStart(PioDco *pdco)
{
    assert_(pdco);
    pio_sm_set_enabled(pdco->_pio, pdco->_ism, true);
}

/// @brief Stops the DCO.
/// @param pdco Ptr to DCO context.
void PioDCOStop(PioDco *pdco)
{
    assert_(pdco);
    pio_sm_set_enabled(pdco->_pio, pdco->_ism, false);
}

/// @brief Main worker task of DCO. It is time critical, so it ought to be run on
/// @brief the dedicated pi pico core.
/// @param pDCO Ptr to DCO context.
/// @return No return. It spins forever.
void RAM (PioDCOWorker)(PioDco *pDCO)
{
    assert_(pDCO);

    register PIO pio = pDCO->_pio;
    register uint sm = pDCO->_ism;
    register int32_t i32acc_error = 0;
    register uint32_t *preg32 = pDCO->_ui32_pioreg;
    register uint8_t *pu8reg = (uint8_t *)preg32;

    for(;;)
    {
        const register int32_t i32reg = si32precise_cycles;
        /* RPix: Load the next precise value of CPU CLK cycles per DCO cycle,
           scaled by 2^24. It yields about 24 millihertz resolution at @10MHz
           DCO frequency. */
        for(int i = 0; i < 32; ++i)
        {
            /* RPix: Calculate the integer number of CPU CLK cycles per next
               DCO cycle, corrected by accumulated error (feedback of the PLL). */
            const int32_t i32wc = iSAR32(i32reg - i32acc_error + (1<<23), 24);

            /* RPix: Calculate the difference betwixt calculated value scaled to
               fine resolution back and precise value of DCO cycles per CPU CLK cycle. 
               This forms a phase locked loop which provides precise freq */
            i32acc_error += (i32wc<<24) - i32reg;

            /* RPix: Set PIO array contents corrected by pio program delay
               of N CPU CLK cycles owing to pio asm instructions. */
            pu8reg[i] = i32wc - PIOASM_DELAY_CYCLES;
        }

        dco_program_puts(pio, sm, preg32);
    }
}

/// @brief Sets DCO running mode.
/// @param pdco Ptr to DCO context.
/// @param emode Desired mode.
/// @attention Not actual so far. User-independent freq. correction not impl'd yet. !FIXME!
void PioDCOSetMode(PioDco *pdco, enum PioDcoMode emode)
{
    assert_(pdco);
    pdco->_mode = emode;

    switch(emode)
    {
        case eDCOMODE_IDLE:
            PioDCOStop(pdco);
            break;

        case eDCOMODE_GPS_COMPENSATED:
            PioDCOStart(pdco);
            break;

        default:
            break;
    }
}
