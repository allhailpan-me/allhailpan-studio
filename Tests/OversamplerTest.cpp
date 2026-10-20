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
//
// Two things here are the result of this test having been weaker than it
// looked, and both are the reason it now sweeps a matrix rather than one
// configuration.
//
// It used to measure at 48 kHz only. The filter specification depends on the
// base rate, so one rate proves one rate. At any rate of 40 kHz or below the
// specification inverted, `design` returned no taps at all, and a filter with
// no taps passes its input through: a device opened at 32 kHz ran with no
// antialiasing filter whatever and content folded back into the audible band
// at 0.00 dB. The old test passed, because the round trip through two filters
// that are both wires is still a wire.
//
// And it swept frequencies blindly. The worst fold always lands within a few
// hundred hertz of a stage's own stop edge, which is where Kaiser's tap
// estimate is least accurate, and a blind sweep steps over it. Probing each
// stage's stop edge found the inner stages reaching 82 dB where the design
// claims a hundred, at 88.2 kHz by four times and at 44.1 kHz by eight.

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

/** Every rate the studio can be opened at, which is not the same as the rates
    anybody chooses. The device decides, and a laptop's own converter or a
    shared mode WASAPI endpoint set up for a headset will hand over 32 kHz or
    22.05 kHz without being asked. Those two are in here because they were
    broken, silently, and one rate's worth of testing could not see it. */
const double rates[] = { 22050.0, 32000.0, 44100.0, 48000.0,
                         88200.0, 96000.0, 176400.0, 192000.0 };

/** Ninety decibels is below what any nonlinearity in front of this will
    produce, so the filter is not the floor. The design aims at a hundred and
    measures 91.9 at worst, at 48 kHz, for the reason `stopbandDb` explains. */
constexpr double stopbandRequiredDb = -90.0;

/** Runs a sine at `hz` through up then straight back down, and reports the
    output level against the input, in decibels.

    With nothing in between, this is the round trip response of the two
    filters: flat through the audible band, and steeply down above it. */
double roundTripDb (Fx::Oversampler& os, double hz, double rate, int block)
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
    // The first few blocks are discarded because the filters are still
    // filling: the longest of them is 255 taps at the oversampled rate.
    for (int pass = 0; pass < 8; ++pass)
    {
        for (int i = 0; i < block; ++i)
        {
            buffer[(std::size_t) i] = std::sin (2.0 * pi * phase);
            phase += hz / rate;

            if (phase >= 1.0)
                phase -= 1.0;
        }

        if (os.upsample (buffer.data(), block) == nullptr)
        {
            fail ("upsample refused a block it was prepared for");
            return 0.0;
        }

        os.downsample (buffer.data(), block);

        if (pass >= 4)
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
double leakDb (Fx::Oversampler& os, double hzAtHighRate, int block)
{
    os.reset();

    std::vector<double> buffer ((std::size_t) block, 0.0);

    const double highRate = os.getOversampledRate();
    double phase = 0.0;
    double sum = 0.0;
    int counted = 0;

    for (int pass = 0; pass < 8; ++pass)
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

        if (pass >= 4)
            for (int i = 0; i < block; ++i)
            {
                sum += buffer[(std::size_t) i] * buffer[(std::size_t) i];
                ++counted;
            }
    }

    const double rms = std::sqrt (sum / (double) counted);

    return 20.0 * std::log10 (rms * std::sqrt (2.0) + 1e-300);
}

/** Where a tone at `hz` of the oversampled stream lands once the rate has
    come all the way back down to `base`. */
double foldsTo (double hz, double base)
{
    double folded = std::fmod (hz, base);

    if (folded > base * 0.5)
        folded = base - folded;

    return folded;
}

/** The worst that any tone the nonlinearity could have made folds back into
    the band, and where it was.

    A filter that attenuated everything above Nyquist equally would need far
    more taps than one that only has to attenuate what lands somewhere
    audible, and the difference is most of the cost. Content just above
    Nyquist folds to just below it, which is above the top of hearing and
    harmless. So each injected tone is asked where it would fold to, and only
    the ones landing inside the band are held to the stopband.

    The frequencies are not a blind sweep. Each stage's stop edge is probed
    directly and then a few hundred hertz either side of it, because that is
    where the real filter has had the least room to get down to its stopband
    and where a blind sweep walks straight past the worst case. A coarse sweep
    runs underneath that as insurance against the worst case being somewhere
    this reasoning did not expect. */
double worstFoldDb (Fx::Oversampler& os, double base, int block, double* whereOut)
{
    const double high = os.getOversampledRate();
    const double edge = Fx::Oversampler::passbandEdgeHz (base);

    std::vector<double> probes;

    for (double stageOut = base; stageOut < high; stageOut *= 2.0)
    {
        const double stop = stageOut - Fx::Oversampler::passbandEdgeHz (stageOut);

        for (double past : { 0.0, 50.0, 200.0, 1000.0, 4000.0 })
            if (stop + past < high * 0.5)
                probes.push_back (stop + past);
    }

    for (double hz = base * 0.5; hz < high * 0.5; hz += base / 16.0)
        probes.push_back (hz);

    double worst = -1000.0;
    double where = 0.0;

    for (double hz : probes)
    {
        if (foldsTo (hz, base) > edge)
            continue;           // lands above the band, so nothing is required

        const double dB = leakDb (os, hz, block);

        if (dB > worst)
        {
            worst = dB;
            where = hz;
        }
    }

    *whereOut = worst > -1000.0 ? where : 0.0;
    return worst;
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
        os.prepare (48000.0, wanted[i], 64);

        if (os.ratio() != expected[i])
            fail ("asking for " + std::to_string (wanted[i]) + " gave "
                  + std::to_string (os.ratio()) + " rather than "
                  + std::to_string (expected[i]));

        if (std::fabs (os.getOversampledRate() - 48000.0 * os.ratio()) > 1e-9)
            fail ("the oversampled rate does not match the ratio");
    }

    // A factor of one has to be a wire, so a caller can turn oversampling off
    // without a second code path and without a delay appearing from nowhere.
    Fx::Oversampler off;
    off.prepare (48000.0, 1, 64);

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

/** The specification has to stay a specification at every rate.

    The stopband starts at the rate minus the passband edge, so an edge too
    close to half the rate leaves no transition to turn over in, and an edge
    above half the rate inverts the thing entirely. That is what used to
    happen below 40 kHz, and what came out of it was a filter with no taps,
    which is a wire. Nothing errored.

    So this asserts the structure directly rather than through a measurement:
    a real transition at every rate, and the edge unchanged at 20 kHz
    wherever the rate has room for it, because a fix that quietly narrowed the
    passband at 44.1 kHz would be its own fault. */
void theDesignNeverCollapses()
{
    std::printf ("design across rates\n");

    for (double rate = 8000.0; rate <= 384000.0; rate += 250.0)
    {
        const double edge = Fx::Oversampler::passbandEdgeHz (rate);
        const double stop = rate - edge;

        if (! (edge > 0.0))
        {
            fail ("the passband edge is not positive at " + show (rate) + " Hz");
            break;
        }

        // A real transition, not a hair's width of one. Half a per cent of
        // the rate is the least that `mostTaps` can turn over in at the depth
        // this design claims, so anything narrower means the tap ceiling has
        // silently become the specification.
        if (! (stop - edge > rate * 0.005))
        {
            fail ("the transition at " + show (rate) + " Hz is only "
                  + show (stop - edge) + " Hz wide");
            break;
        }

        if (rate >= 44100.0 && edge != Fx::Oversampler::passbandHz)
        {
            fail ("the passband edge at " + show (rate) + " Hz is "
                  + show (edge) + " rather than 20 kHz");
            break;
        }

        if (rate < 40000.0 && ! (edge < rate * 0.5))
        {
            fail ("the passband edge at " + show (rate) + " Hz is at or above Nyquist");
            break;
        }
    }

    // And the delay it reports is a delay. A stage with no taps reported
    // minus half a sample per stage, because the length of an absent filter
    // is zero and the arithmetic takes one off it. At 16 kHz the whole chain
    // reported minus three quarters of a sample, which the studio's
    // compensation would have taken at its word.
    for (double rate : rates)
        for (int factor : { 2, 4, 8 })
        {
            Fx::Oversampler os;
            os.prepare (rate, factor, 256);

            if (! (os.latencySamples() > 0.0))
                fail ("at " + show (rate) + " Hz by " + std::to_string (factor)
                      + " the reported delay is " + show (os.latencySamples()));
        }
}

/** The audible band comes back as it went in, and anything the nonlinearity
    makes that would fold into it does not, at every rate and every factor.

    One rate proving one rate is the whole reason this is a matrix. */
void theFiltersHoldAtEveryRate()
{
    std::printf ("round trip and stopband, every rate\n");

    for (double rate : rates)
    {
        const double edge = Fx::Oversampler::passbandEdgeHz (rate);

        for (int factor : { 2, 4, 8 })
        {
            Fx::Oversampler os;
            os.prepare (rate, factor, 256);

            double worstPassband = 0.0;

            // As fractions of the edge rather than fixed frequencies, because
            // the edge moves with the rate once the rate is low enough that
            // 20 kHz will not fit under it. The bottom of the range is a
            // fortieth of the edge rather than DC: the counted window holds
            // only a few cycles below that and the mean square of three and a
            // bit cycles is not the mean square of the tone. The filters are
            // normalised to unity at DC by construction, so the bottom end is
            // not in doubt.
            for (double share : { 0.025, 0.125, 0.25, 0.5, 0.75, 0.9, 0.95 })
                worstPassband = std::max (worstPassband,
                                          std::fabs (roundTripDb (os, edge * share, rate, 256)));

            double where = 0.0;
            const double worstStop = worstFoldDb (os, rate, 256, &where);

            std::printf ("    %8.1f Hz %dx  edge %5.0f  passband %s dB  fold %s dB at %.0f Hz\n",
                         rate, factor, edge, show (worstPassband).c_str(),
                         show (worstStop).c_str(), where);

            // Half a decibel across the whole band it claims to pass.
            // Anything worse is a tone control nobody asked for, sitting in
            // front of every effect that uses this.
            if (! (worstPassband < 0.5))
                fail ("at " + show (rate) + " Hz by " + std::to_string (factor)
                      + " the round trip is " + show (worstPassband) + " dB off");

            if (! (worstStop < stopbandRequiredDb))
                fail ("at " + show (rate) + " Hz by " + std::to_string (factor)
                      + " content folding into the band reaches " + show (worstStop)
                      + " dB, at " + show (where) + " Hz");
        }
    }
}

/** A steady input comes back steady, which is the one case where the answer
    is known exactly.

    This is the per-phase normalisation, and nothing else here can see it. An
    interpolator sees a zero between every input sample, so only every other
    tap has anything under it at any moment and the two sets take turns: each
    set has to add to one on its own. Scaling the whole filter so all of its
    taps add to one leaves the two sets slightly unequal, and a constant then
    comes out with a ripple on it at half the oversampled rate.

    The round trip cannot see that ripple at all, because it sits exactly
    where the decimation filter is designed to stop. It is visible only in the
    oversampled stream, which is also the only place it matters: that stream
    is what the nonlinearity reads, so a folder handed a steady voltage does
    not give back the fold of that voltage. It measured as five parts in a
    million, which is nothing to hear and is still the one answer that was
    known for certain. */
void aSteadyInputComesBackSteady()
{
    std::printf ("steady input\n");

    double worstEverywhere = 0.0;

    for (double rate : rates)
        for (int factor : { 2, 4, 8 })
        {
            Fx::Oversampler os;
            os.prepare (rate, factor, 256);
            os.reset();

            std::vector<double> buffer (256, 1.0);
            double worst = 0.0;

            // Several blocks so the longest filter in the chain, 255 taps at
            // the oversampled rate, is full before anything is looked at.
            for (int pass = 0; pass < 6; ++pass)
            {
                double* high = os.upsample (buffer.data(), 256);

                if (high == nullptr)
                {
                    fail ("upsample refused a block it was prepared for");
                    return;
                }

                if (pass >= 4)
                    for (int i = 0; i < 256 * os.ratio(); ++i)
                        worst = std::max (worst, std::fabs (high[i] - 1.0));

                os.downsample (buffer.data(), 256);

                for (auto& v : buffer)
                    v = 1.0;
            }

            worstEverywhere = std::max (worstEverywhere, worst);

            // And the same constant all the way back down, which is what
            // pins the decimation filter's gain at DC. That one is scaled so
            // that all of its taps add to one, and a windowed sinc very
            // nearly does already, so leaving the scaling out moves the gain
            // by about four parts in a hundred thousand: too little for the
            // round trip's half decibel to notice, and still not unity on
            // the one input whose answer is known.
            os.reset();

            std::vector<double> flat (256, 1.0);
            double worstDown = 0.0;

            for (int pass = 0; pass < 6; ++pass)
            {
                os.upsample (flat.data(), 256);
                os.downsample (flat.data(), 256);

                if (pass >= 4)
                    for (double v : flat)
                        worstDown = std::max (worstDown, std::fabs (v - 1.0));

                for (auto& v : flat)
                    v = 1.0;
            }

            if (! (worstDown < 1e-9))
                fail ("at " + show (rate) + " Hz by " + std::to_string (factor)
                      + " a constant 1.0 came back from the round trip varying by "
                      + std::to_string (worstDown));

            // Rounding only. Every branch sums to one exactly by
            // construction, so what is left is the error of adding a couple
            // of hundred doubles. Normalising the two branches together
            // instead puts this up by ten orders of magnitude.
            if (! (worst < 1e-9))
                fail ("at " + show (rate) + " Hz by " + std::to_string (factor)
                      + " a constant 1.0 came out of the interpolator varying by "
                      + std::to_string (worst));
        }

    std::printf ("    worst departure from a constant: %.2e\n", worstEverywhere);
}

/** What it says its delay is, is its delay. The studio lines effects up by
    what they report, so a wrong figure here puts a track out of time by an
    amount that depends on which effects happen to be loaded. */
void theReportedDelayIsTheRealOne()
{
    std::printf ("reported delay\n");

    for (double rate : { 22050.0, 48000.0, 192000.0 })
        for (int factor : { 2, 4, 8 })
        {
            Fx::Oversampler os;
            os.prepare (rate, factor, 1024);
            os.reset();

            // An impulse, far enough into the block that the whole response
            // fits. The longest chain here delays by about 165 samples and a
            // linear phase response is symmetric about that, so it needs
            // twice as much room again after it.
            std::vector<double> buffer (1024, 0.0);
            buffer[16] = 1.0;

            os.upsample (buffer.data(), 1024);
            os.downsample (buffer.data(), 1024);

            // The centre of energy of what comes back, which is where a
            // linear phase filter puts the signal.
            double weighted = 0.0, total = 0.0;

            for (int i = 0; i < 1024; ++i)
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

            const double measured = weighted / total - 16.0;
            const double reported = os.latencySamples();

            std::printf ("    %8.1f Hz %dx: reported %s, measured %s samples\n",
                         rate, factor, show (reported).c_str(), show (measured).c_str());

            // A ten thousandth of a sample, not a whole one. The agreement
            // is exact to within a tenth of a picosecond of a sample, because
            // a cascade of linear phase filters puts the centre of energy
            // exactly on the group delay, so a tolerance of one sample is a
            // thousand times looser than the code. Three separate mistakes
            // walked through it, all worth half an oversampled sample: the
            // sinc centred on the wrong tap, the filter length counted whole
            // rather than less one, and the second of each pair kept on the
            // way down rather than the first.
            if (! (std::fabs (measured - reported) < 1e-4))
                fail ("at " + show (rate) + " Hz by " + std::to_string (factor)
                      + " the delay is " + show (measured) + " samples but it reports "
                      + show (reported));
        }
}

/** Nothing unreasonable breaks it, including a block larger than it was
    prepared for, which has to be refused rather than written past the end of
    the buffer. */
void nothingUnreasonableBreaksIt()
{
    std::printf ("out of range use\n");

    Fx::Oversampler os;
    os.prepare (48000.0, 4, 64);

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
    theDesignNeverCollapses();
    theFiltersHoldAtEveryRate();
    aSteadyInputComesBackSteady();
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
