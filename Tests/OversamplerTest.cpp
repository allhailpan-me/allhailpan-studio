// Checks Fx/Oversampler.h.
//
// The thing worth asserting here is the stopband, because it is the number
// that decides whether oversampling anything is worth doing at all, and
// because getting it wrong looks exactly like getting it right. A filter that
// only reaches 50 dB down still passes audio, still sounds fine on its own,
// and quietly puts a floor under every nonlinearity behind it: the aliasing
// it was supposed to remove comes back at 50 dB and no amount of further
// oversampling moves it.
//
// That is not hypothetical. While measuring the wavefolder, an early probe
// used a Blackman window, whose sidelobes are 58 dB down, and every reading
// sat at -55 dB however much oversampling was thrown at it. The filter was
// the floor and the measurement looked like a property of the folder.

#include "Fx/Oversampler.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

namespace
{

int failures = 0;

void fail (const std::string& what)
{
    std::printf ("  FAIL  %s\n", what.c_str());
    ++failures;
}

std::string show (double v)
{
    char buffer[32];
    std::snprintf (buffer, sizeof (buffer), "%.2f", v);
    return buffer;
}

constexpr double pi = 3.14159265358979323846;
constexpr double rate = 48000.0;

/** Runs a sine at `hz` through up then straight back down, and reports the
    output level against the input, in decibels.

    With nothing in between, this is the round trip response of the two
    filters: flat through the audible band, and steeply down above it. */
double roundTripDb (Fx::Oversampler& os, double hz, int block)
{
    os.reset();

    std::vector<double> buffer ((std::size_t) block);

    double phase = 0.0;
    double sum = 0.0;
    int counted = 0;

    // Measured as the root mean square rather than as the peak.
    //
    // A peak is the obvious thing to measure and is wrong here. These filters
    // delay the signal by half a sample at some factors, and at 18 kHz there
    // are under three samples to a cycle, so a half sample shift moves every
    // sampled point off the top of the wave: the peak then reads 0.69 dB low
    // with nothing at all wrong with the filter. The mean square does not
    // care where the samples land.
    //
    // Several blocks, because the first one is still filling the filters.
    for (int pass = 0; pass < 24; ++pass)
    {
        for (int i = 0; i < block; ++i)
        {
            buffer[(std::size_t) i] = std::sin (2.0 * pi * phase);
            phase += hz / rate;

            if (phase >= 1.0)
                phase -= 1.0;
        }

        double* high = os.upsample (buffer.data(), block);

        if (high == nullptr)
        {
            fail ("upsample refused a block it was prepared for");
            return 0.0;
        }

        os.downsample (buffer.data(), block);

        if (pass >= 12)
            for (int i = 0; i < block; ++i)
            {
                sum += buffer[(std::size_t) i] * buffer[(std::size_t) i];
                ++counted;
            }
    }

    // Against the root mean square of the input sine, which is one over root
    // two, so that an unchanged signal reads as zero decibels.
    const double rms = std::sqrt (sum / (double) counted);

    return 20.0 * std::log10 (rms * std::sqrt (2.0) + 1e-300);
}

/** Puts a tone into the oversampled stream that was never in the input, the
    way a nonlinearity does, and reports how much of it survives coming back
    down. This is the decimation filter's stopband, measured rather than
    designed. */
double stopbandDb (Fx::Oversampler& os, double hzAtHighRate, int block)
{
    os.reset();

    std::vector<double> buffer ((std::size_t) block, 0.0);

    const double highRate = os.getOversampledRate();
    double phase = 0.0;
    double sum = 0.0;
    int counted = 0;

    for (int pass = 0; pass < 24; ++pass)
    {
        double* high = os.upsample (buffer.data(), block);

        if (high == nullptr)
            return 0.0;

        const int total = block * os.ratio();

        for (int i = 0; i < total; ++i)
        {
            high[i] = std::sin (2.0 * pi * phase);
            phase += hzAtHighRate / highRate;

            if (phase >= 1.0)
                phase -= 1.0;
        }

        os.downsample (buffer.data(), block);

        if (pass >= 12)
            for (int i = 0; i < block; ++i)
            {
                sum += buffer[(std::size_t) i] * buffer[(std::size_t) i];
                ++counted;
            }
    }

    const double rms = std::sqrt (sum / (double) counted);

    return 20.0 * std::log10 (rms * std::sqrt (2.0) + 1e-300);
}

//==============================================================================
void theRatioIsAPowerOfTwo()
{
    std::printf ("ratio\n");

    const int wanted[] = { 1, 2, 3, 4, 7, 8, 16 };
    const int expected[] = { 1, 2, 2, 4, 4, 8, 16 };

    for (int i = 0; i < 7; ++i)
    {
        Fx::Oversampler os;
        os.prepare (rate, wanted[i], 64);

        if (os.ratio() != expected[i])
            fail ("asking for " + std::to_string (wanted[i]) + " gave "
                  + std::to_string (os.ratio()) + " rather than "
                  + std::to_string (expected[i]));

        if (std::fabs (os.getOversampledRate() - rate * os.ratio()) > 1e-9)
            fail ("the oversampled rate does not match the ratio");
    }

    // A factor of one has to be a wire, so a caller can turn oversampling off
    // without a second code path and without a delay appearing from nowhere.
    Fx::Oversampler off;
    off.prepare (rate, 1, 64);

    if (off.latencySamples() != 0.0)
        fail ("a factor of one still delays the signal");

    std::vector<double> buffer (64);
    std::mt19937 rng (7);
    std::uniform_real_distribution<double> audio (-1.0, 1.0);

    for (auto& v : buffer)
        v = audio (rng);

    const auto original = buffer;

    off.upsample (buffer.data(), 64);
    off.downsample (buffer.data(), 64);

    for (std::size_t i = 0; i < buffer.size(); ++i)
        if (std::fabs (buffer[i] - original[i]) > 1e-12)
        {
            fail ("a factor of one changed the signal");
            break;
        }
}

/** The audible band comes back as it went in, and everything above where the
    filters turn over does not. */
void theRoundTripIsFlatThenSteep()
{
    std::printf ("round trip response\n");

    for (int factor : { 2, 4 })
    {
        Fx::Oversampler os;
        os.prepare (rate, factor, 256);

        std::printf ("    %dx passband:", factor);

        double worstPassband = 0.0;

        // From 100 Hz, because the measurement window holds only a couple of
        // cycles below that and the mean square of two and a bit cycles is not
        // the mean square of the tone. The filters are normalised to unity at
        // DC by construction, so the bottom end is not in doubt.
        for (double hz : { 100.0, 1000.0, 5000.0, 10000.0, 15000.0, 18000.0, 19000.0 })
        {
            const double dB = roundTripDb (os, hz, 256);
            std::printf (" %.0fk:%s", hz / 1000.0, show (dB).c_str());
            worstPassband = std::max (worstPassband, std::fabs (dB));
        }

        std::printf ("\n    %dx worst %s dB", factor, show (worstPassband).c_str());

        // Half a decibel across the whole audible band. Anything worse is a
        // tone control nobody asked for, sitting in front of every effect
        // that uses this.
        if (! (worstPassband < 0.5))
            fail ("the round trip is " + show (worstPassband) + " dB off through the passband");

        // And the stopband, which is the number that matters, measured only
        // where it matters.
        //
        // A filter that attenuated everything above Nyquist equally would
        // need far more taps than one that only has to attenuate what lands
        // somewhere audible, and the difference is most of the cost. Content
        // just above Nyquist folds to just below it, which is above the top
        // of hearing and harmless. So each injected tone is asked where it
        // would fold to, and only the ones landing inside the audible band
        // are held to the stopband.
        double worstStop = -1000.0;
        double worstAt = 0.0;

        for (double hz = 25000.0; hz < os.getOversampledRate() * 0.5; hz += 500.0)
        {
            // Where a tone at hz lands after the rate comes back down.
            double folded = std::fmod (hz, rate);

            if (folded > rate * 0.5)
                folded = rate - folded;

            if (folded > Fx::Oversampler::passbandHz)
                continue;       // lands above hearing, so nothing is required

            const double dB = stopbandDb (os, hz, 256);

            if (dB > worstStop)
            {
                worstStop = dB;
                worstAt = hz;
            }
        }

        std::printf (", stopband %s dB (worst at %.0f Hz)\n",
                     show (worstStop).c_str(), worstAt);

        // Ninety decibels is below what any nonlinearity in front of this
        // will produce, so the filter is not the floor.
        if (! (worstStop < -90.0))
            fail ("content folding into the audible band only reaches "
                  + show (worstStop) + " dB, at " + show (worstAt) + " Hz");
    }
}

/** What it says its delay is, is its delay. The studio lines effects up by
    what they report, so a wrong figure here puts a track out of time by an
    amount that depends on which effects happen to be loaded. */
void theReportedDelayIsTheRealOne()
{
    std::printf ("reported delay\n");

    for (int factor : { 2, 4, 8 })
    {
        Fx::Oversampler os;
        os.prepare (rate, factor, 512);
        os.reset();

        // An impulse, far enough into the block that the whole response fits.
        std::vector<double> buffer (512, 0.0);
        buffer[8] = 1.0;

        os.upsample (buffer.data(), 512);
        os.downsample (buffer.data(), 512);

        // The centre of energy of what comes back, which is where a linear
        // phase filter puts the signal.
        double weighted = 0.0, total = 0.0;

        for (int i = 0; i < 512; ++i)
        {
            const double energy = buffer[(std::size_t) i] * buffer[(std::size_t) i];

            weighted += energy * (double) i;
            total += energy;
        }

        if (! (total > 0.0))
        {
            fail ("an impulse produced nothing at " + std::to_string (factor) + "x");
            continue;
        }

        const double measured = weighted / total - 8.0;
        const double reported = os.latencySamples();

        std::printf ("    %dx: reported %s, measured %s samples\n",
                     factor, show (reported).c_str(), show (measured).c_str());

        if (! (std::fabs (measured - reported) < 1.0))
            fail ("at " + std::to_string (factor) + "x the delay is " + show (measured)
                  + " samples but it reports " + show (reported));
    }
}

/** Nothing unreasonable breaks it, including a block larger than it was
    prepared for, which has to be refused rather than written past the end of
    the buffer. */
void nothingUnreasonableBreaksIt()
{
    std::printf ("out of range use\n");

    Fx::Oversampler os;
    os.prepare (rate, 4, 64);

    std::vector<double> big (256, 0.5);

    if (os.upsample (big.data(), 256) != nullptr)
        fail ("a block larger than it was prepared for was accepted");

    // Zero and negative rates fall back rather than dividing by them.
    Fx::Oversampler odd;
    odd.prepare (0.0, 4, 32);

    if (! (odd.getOversampledRate() > 0.0))
        fail ("a sample rate of zero left the oversampled rate at zero");

    std::mt19937 rng (99);
    std::uniform_real_distribution<double> audio (-4.0, 4.0);

    std::vector<double> buffer (32);

    for (int pass = 0; pass < 200; ++pass)
    {
        for (auto& v : buffer)
            v = audio (rng);

        odd.upsample (buffer.data(), 32);
        odd.downsample (buffer.data(), 32);

        for (double v : buffer)
            if (! std::isfinite (v))
            {
                fail ("the output stopped being finite");
                return;
            }
    }
}

} // namespace

int main()
{
    std::printf ("Fx::Oversampler\n\n");

    theRatioIsAPowerOfTwo();
    theRoundTripIsFlatThenSteep();
    theReportedDelayIsTheRealOne();
    nothingUnreasonableBreaksIt();

    std::printf ("\n");

    if (failures > 0)
    {
        std::printf ("%d check(s) failed.\n", failures);
        return 1;
    }

    std::printf ("All checks passed.\n");
    return 0;
}
