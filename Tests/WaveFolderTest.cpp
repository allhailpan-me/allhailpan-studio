// Checks Fx/WaveFolder.h.
//
// Three different kinds of claim, because the folder makes three.
//
// That it folds. A folder is defined by what it does past its limit: it sends
// the signal back rather than flattening it, so the output stays bounded
// however hard it is driven, and driving harder adds harmonics instead of
// level.
//
// That the antiderivatives are the antiderivatives. The antialiasing stands
// entirely on two closed forms being exactly the integrals of the fold, and of
// the input times the fold. They are checked against the derivative computed
// numerically, which is the definition rather than a second copy of the
// algebra. That check earned its place: an earlier version of this folder had
// a second antiderivative that was wrong by a whole term, the folder still ran
// and still folded, and its aliasing was twenty decibels worse for it.
//
// And that it does not alias inside the range it claims, with the claim
// measured too. A check that only tested where the folder works would be
// hiding the part worth knowing.

#include "Fx/WaveFolder.h"

#include <algorithm>
#include <cmath>
#include <complex>
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

//==============================================================================
/** How much of the output is not a harmonic of the note, in decibels.

    The note is deliberately off the bin grid, because a signal that repeats
    exactly still repeats exactly after its harmonics have folded, and on the
    grid the folded partials would land on harmonics and be invisible.
*/
double aliasFloorDb (const std::vector<double>& rendered, double frequency)
{
    const int n = (int) rendered.size();

    std::vector<double> windowed ((std::size_t) n);

    const double a0 = 0.35875, a1 = 0.48829, a2 = 0.14128, a3 = 0.01168;

    for (int i = 0; i < n; ++i)
    {
        const double t = 2.0 * pi * (double) i / (double) (n - 1);
        windowed[(std::size_t) i] = rendered[(std::size_t) i]
            * (a0 - a1 * std::cos (t) + a2 * std::cos (2.0 * t) - a3 * std::cos (3.0 * t));
    }

    const double binHz = rate / (double) n;

    double loudest = 0.0, stray = 0.0;
    int strayBins = 0;

    for (int k = 10; k <= n / 2; ++k)
    {
        std::complex<double> sum (0.0, 0.0);

        for (int i = 0; i < n; ++i)
            sum += windowed[(std::size_t) i]
                 * std::exp (std::complex<double> (0.0,
                       -2.0 * pi * (double) k * (double) i / (double) n));

        const double hz = (double) k * binHz;
        const double magnitude = std::abs (sum);
        const double nearest = std::floor (hz / frequency + 0.5);

        if (nearest >= 1.0 && std::fabs (hz - nearest * frequency) / binHz <= 8.0)
        {
            loudest = std::max (loudest, magnitude);
            continue;
        }

        ++strayBins;
        stray = std::max (stray, magnitude);
    }

    // At a low enough note the harmonics are packed so closely that every bin
    // is beside one and there is nothing left to read. Saying so is better
    // than returning a number that looks like silence.
    if (strayBins < 30 || ! (loudest > 0.0))
        return -1000.0;

    return 20.0 * std::log10 (stray / loudest + 1e-300);
}

std::vector<double> foldASine (double drive, double hz, int count, double symmetry = 0.0)
{
    Fx::WaveFolder folder;
    folder.prepare (rate, 256);
    folder.setDrive (drive);
    folder.setSymmetry (symmetry);

    std::vector<double> out;
    out.reserve ((std::size_t) count);

    std::vector<double> block (256);
    double phase = 0.0;

    for (int pass = 0; pass < 8 + count / 256; ++pass)
    {
        for (int i = 0; i < 256; ++i)
        {
            block[(std::size_t) i] = std::sin (2.0 * pi * phase);
            phase += hz / rate;

            if (phase >= 1.0)
                phase -= 1.0;
        }

        folder.process (block.data(), 256);

        if (pass >= 8)
            for (int i = 0; i < 256 && (int) out.size() < count; ++i)
                out.push_back (block[(std::size_t) i]);
    }

    return out;
}

//==============================================================================
void foldingIsBoundedAndSymmetric()
{
    std::printf ("the fold characteristic\n");

    std::mt19937 rng (4242);
    std::uniform_real_distribution<double> wild (-1e4, 1e4);

    double biggest = 0.0;

    for (int i = 0; i < 200000; ++i)
    {
        const double x = i < 1000 ? (double) (i - 500) * 0.01 : wild (rng);
        const double y = Fx::WaveFolder::foldOnce (x);

        if (! std::isfinite (y))
        {
            fail ("the fold produced something not finite");
            break;
        }

        biggest = std::max (biggest, std::fabs (y));

        // Odd symmetry, which is what makes a folder give only odd harmonics
        // from a symmetric input, and is what the symmetry control breaks on
        // purpose.
        if (std::fabs (y + Fx::WaveFolder::foldOnce (-x)) > 1e-9)
        {
            fail ("the fold is not symmetric about zero at " + show (x));
            break;
        }
    }

    std::printf ("    never exceeds %s\n", show (biggest).c_str());

    if (! (biggest <= 1.0 + 1e-12))
        fail ("the fold reached " + show (biggest) + ", so it has stopped folding");

    // Near zero it passes the signal rather than doing anything to it, which
    // is what makes drive a usable control: turning it down gives the signal
    // back.
    for (double x : { -0.2, -0.05, 0.0, 0.05, 0.2 })
    {
        const double expected = pi * 0.5 * x;
        const double got = Fx::WaveFolder::foldOnce (x);

        if (! (std::fabs (got - expected) < 0.06 * (std::fabs (expected) + 0.1)))
            fail ("the fold is not roughly linear near zero at " + show (x));
    }
}

/** Each antiderivative is the antiderivative, checked against the derivative
    computed numerically rather than against the algebra again.

    This is the check that found the real bug in this file's history. The
    second antiderivative, of the input times the fold, was built for a
    triangle characteristic by counting whole folding periods and adding the
    part period left over. That construction is right for the first
    antiderivative and wrong for the second, because the second one's
    integrand still carries the unwrapped input, so a term proportional to the
    number of periods goes missing. Differentiating it numerically found it in
    one line, and pointed straight at the size of the missing term.
*/
void theAntiderivativesAreTheAntiderivatives()
{
    std::printf ("antiderivatives\n");

    double worstFirst = 0.0, worstSecond = 0.0;
    double atFirst = 0.0, atSecond = 0.0;

    for (double x = -40.0; x <= 40.0; x += 0.0011)
    {
        const double h = 1e-6;

        const double slopeFirst = (Fx::WaveFolder::foldIntegral (x + h)
                                 - Fx::WaveFolder::foldIntegral (x - h)) / (2.0 * h);

        const double slopeSecond = (Fx::WaveFolder::foldSecondIntegral (x + h)
                                  - Fx::WaveFolder::foldSecondIntegral (x - h)) / (2.0 * h);

        const double offFirst = std::fabs (slopeFirst - Fx::WaveFolder::foldOnce (x));
        const double offSecond = std::fabs (slopeSecond - x * Fx::WaveFolder::foldOnce (x));

        if (offFirst > worstFirst)   { worstFirst = offFirst;   atFirst = x; }
        if (offSecond > worstSecond) { worstSecond = offSecond; atSecond = x; }
    }

    std::printf ("    the fold's integral differentiates back to within %.2e, at %s\n",
                 worstFirst, show (atFirst).c_str());
    std::printf ("    and x times the fold's to within %.2e, at %s\n",
                 worstSecond, show (atSecond).c_str());

    if (! (worstFirst < 1e-6))
        fail ("the first antiderivative differentiates to something else, by "
              + show (worstFirst));

    // Looser, only because a numerical derivative of a function whose values
    // reach into the tens loses digits to the subtraction.
    if (! (worstSecond < 1e-4))
        fail ("the second antiderivative differentiates to something else, by "
              + show (worstSecond));

    if (std::fabs (Fx::WaveFolder::foldIntegral (0.0)) > 1e-12)
        fail ("the first antiderivative is not zero at zero");

    if (std::fabs (Fx::WaveFolder::foldSecondIntegral (0.0)) > 1e-12)
        fail ("the second antiderivative is not zero at zero");
}

/** A steady input is the case the antialiasing is undefined on, and the
    paper's fallback has to take over cleanly: a folder fed a constant has to
    give back the fold of that constant, not a division by zero. */
void aSteadyInputIsJustFolded()
{
    std::printf ("steady input\n");

    for (double level : { -3.0, -0.4, 0.0, 0.4, 1.7, 3.0 })
    {
        Fx::WaveFolder folder;
        folder.prepare (rate, 64);
        folder.setDrive (1.0);

        std::vector<double> block (64, level);

        for (int pass = 0; pass < 40; ++pass)
        {
            std::fill (block.begin(), block.end(), level);
            folder.process (block.data(), 64);
        }

        const double expected = Fx::WaveFolder::foldOnce (level);

        for (int i = 0; i < 64; ++i)
            if (! (std::fabs (block[(std::size_t) i] - expected) < 1e-9))
            {
                char detail[160];
                std::snprintf (detail, sizeof (detail),
                               "a steady %.2f gave %.12f rather than %.12f",
                               level, block[(std::size_t) i], expected);
                fail (detail);
                break;
            }
    }
}

/** A symmetric fold gives odd harmonics, and the symmetry control brings the
    even ones in.

    This is the claim the control is sold on, and it is also what identifies a
    folder by ear: the sound this was built to reach had its even harmonics 20
    to 40 dB below the odd ones, which is a folder near the middle of its
    symmetry, and is not something a filter does to a sawtooth.

    The note has a whole number of cycles in the measurement window, so every
    harmonic lands on a bin and nothing leaks. Measured off the grid instead,
    the strong odd harmonics smear into their neighbours and the even ones
    read 28 dB down on a fold that has none at all.
*/
void symmetryDecidesTheEvenHarmonics()
{
    std::printf ("odd and even harmonics\n");

    const int points = 4096;
    const double hz = rate * 10.0 / (double) points;

    for (double offset : { 0.0, 0.5 })
    {
        const auto rendered = foldASine (4.0, hz, points, offset);

        double odd = 0.0, even = 0.0;

        for (int h = 2; h <= 12; ++h)
        {
            std::complex<double> sum (0.0, 0.0);

            for (int i = 0; i < points; ++i)
                sum += rendered[(std::size_t) i]
                     * std::exp (std::complex<double> (0.0,
                           -2.0 * pi * (double) h * hz * (double) i / rate));

            const double magnitude = std::abs (sum) / (double) points;

            if (h % 2 == 0)
                even = std::max (even, magnitude);
            else
                odd = std::max (odd, magnitude);
        }

        const double ratio = 20.0 * std::log10 (even / (odd + 1e-300) + 1e-300);

        std::printf ("    symmetry %s: evens are %s dB against the odds\n",
                     show (offset).c_str(), show (ratio).c_str());

        if (offset == 0.0)
        {
            if (! (ratio < -60.0))
                fail ("centred, the fold still has even harmonics at " + show (ratio) + " dB");
        }
        else
        {
            if (! (ratio > -12.0))
                fail ("off centre the even harmonics did not arrive: " + show (ratio) + " dB");
        }
    }
}

/** The headline, measured inside the range the folder claims.

    Each drive is tested at the note it says it is clean up to, which is the
    only honest way to test a claim that depends on the setting: a fixed note
    would be trivially inside the range at low drive and outside it at high.
*/
void itIsCleanInsideItsOwnClaim()
{
    std::printf ("stray partials at the edge of the claimed range\n");

    double worst = -1000.0;

    for (double drive : { 2.0, 4.0, 8.0, 16.0 })
    {
        Fx::WaveFolder folder;
        folder.prepare (rate, 256);
        folder.setDrive (drive);

        const double claimed = folder.foldsCleanlyBelow();
        const double hz = claimed * 0.9;        // just inside

        const auto rendered = foldASine (drive, hz + 0.37, 4096);
        const double dB = aliasFloorDb (rendered, hz + 0.37);

        std::printf ("    drive %-5s clean below %6.0f Hz, measures %s dB at %.0f Hz\n",
                     show (drive).c_str(), claimed, show (dB).c_str(), hz);

        if (dB > -999.0)
            worst = std::max (worst, dB);
    }

    if (! (worst < -70.0))
        fail ("inside its own claimed range the folder strays to " + show (worst) + " dB");
}

/** And the claim itself behaves: more drive means a lower ceiling, and it
    scales with the sample rate, because the whole reason there is a ceiling
    is that there are only so many samples to put harmonics in. */
void theClaimedRangeBehaves()
{
    std::printf ("the claimed range\n");

    Fx::WaveFolder folder;
    folder.prepare (rate, 64);

    double previous = 1e18;

    for (double drive = 0.5; drive <= 32.0; drive *= 1.4)
    {
        folder.setDrive (drive);

        const double edge = folder.foldsCleanlyBelow();

        if (! (edge < previous))
            fail ("the clean range did not fall as the drive rose");

        previous = edge;
    }

    folder.setDrive (4.0);
    const double at48 = folder.foldsCleanlyBelow();

    Fx::WaveFolder faster;
    faster.prepare (96000.0, 64);
    faster.setDrive (4.0);

    const double at96 = faster.foldsCleanlyBelow();

    std::printf ("    at four times drive: %.0f Hz at 48 kHz, %.0f Hz at 96 kHz\n", at48, at96);

    if (! (std::fabs (at96 - at48 * 2.0) < 1.0))
        fail ("the clean range does not scale with the sample rate");
}

void nothingUnreasonableBreaksIt()
{
    std::printf ("out of range use\n");

    Fx::WaveFolder folder;
    folder.prepare (rate, 128);

    std::mt19937 rng (31337);
    std::uniform_real_distribution<double> audio (-50.0, 50.0);
    std::uniform_real_distribution<double> knob (-100.0, 100.0);

    std::vector<double> block (128);

    for (int pass = 0; pass < 400; ++pass)
    {
        folder.setDrive (knob (rng));
        folder.setSymmetry (knob (rng));

        for (auto& v : block)
            v = audio (rng);

        folder.process (block.data(), 128);

        for (double v : block)
        {
            if (! std::isfinite (v))
            {
                fail ("the output stopped being finite");
                return;
            }

            // However it is driven, the folder cannot hand back more than its
            // own limit, give or take the overshoot of the filters around it.
            if (std::fabs (v) > 2.0)
            {
                fail ("the folder let through " + show (v) + ", which is past its own fold");
                return;
            }
        }
    }

    // A block bigger than it was prepared for is refused rather than written
    // past the end of anything.
    std::vector<double> big (512, 0.5);
    folder.process (big.data(), 512);

    for (double v : big)
        if (! std::isfinite (v))
        {
            fail ("an oversized block produced something not finite");
            break;
        }
}

} // namespace

int main()
{
    std::printf ("Fx::WaveFolder\n\n");

    foldingIsBoundedAndSymmetric();
    theAntiderivativesAreTheAntiderivatives();
    aSteadyInputIsJustFolded();
    symmetryDecidesTheEvenHarmonics();
    itIsCleanInsideItsOwnClaim();
    theClaimedRangeBehaves();
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
