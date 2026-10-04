#pragma once
#include <cmath>

//==============================================================================
/** An attack, decay, sustain, release envelope.

    Linear attack, exponential decay and release. That combination is a
    choice rather than a standard: there is no specification for the shape of
    an envelope, and studios differ. It is the common one because a linear
    attack reaches full level predictably, which is what a percussive
    transient wants, while a decay and release that fall exponentially match
    how notes actually die away on a struck or plucked instrument and how the
    discharge curves in analogue envelope generators behave.

    The times mean something exact here, which they often do not elsewhere:
    the decay time is how long it takes to arrive within a thousandth of the
    sustain level, and the release time is how long it takes to fall to a
    thousandth of full scale, which is sixty decibels down.

    The release ends at exactly zero and the envelope then reports itself
    finished. That matters more than it looks. An exponential approaches zero
    without ever arriving, so an envelope that simply multiplies down leaves
    every voice very quietly running for ever: the voice pool fills with
    notes that ended a minute ago, new notes start stealing from voices that
    are still sounding, and the arithmetic runs on into denormals, which on
    some processors are slow enough to matter in a callback. So the last
    stretch is snapped.

    No JUCE, so the tests can reach it.
*/
class Envelope
{
public:
    struct Settings
    {
        double attackSeconds  = 0.005;
        double decaySeconds   = 0.120;
        double sustainLevel   = 0.700;
        double releaseSeconds = 0.200;
    };

    void setSampleRate (double newRate) noexcept
    {
        sampleRate = newRate > 0.0 ? newRate : 44100.0;
        recalculate();
    }

    void setSettings (const Settings& s) noexcept
    {
        settings = s;
        settings.sustainLevel = clamp01 (settings.sustainLevel);
        settings.attackSeconds  = std::fmax (0.0, settings.attackSeconds);
        settings.decaySeconds   = std::fmax (0.0, settings.decaySeconds);
        settings.releaseSeconds = std::fmax (0.0, settings.releaseSeconds);
        recalculate();
    }

    const Settings& getSettings() const noexcept { return settings; }

    /** Starts a note. Deliberately does not reset the level: retriggering
        while a previous note is still fading would otherwise drop to zero
        first and click. */
    void noteOn() noexcept
    {
        stage = Stage::attack;
    }

    void noteOff() noexcept
    {
        if (stage != Stage::idle)
            stage = Stage::release;
    }

    /** Stops immediately and silently. For taking a voice back when there is
        nothing left to steal. */
    void reset() noexcept
    {
        stage = Stage::idle;
        level = 0.0;
    }

    bool isActive() const noexcept   { return stage != Stage::idle; }
    bool isReleasing() const noexcept { return stage == Stage::release; }
    float getLevel() const noexcept  { return (float) level; }

    float nextSample() noexcept
    {
        switch (stage)
        {
            case Stage::idle:
                return 0.0f;

            case Stage::attack:
                // Linear, and it arrives exactly rather than approaching.
                level += attackStep;

                if (level >= 1.0 || attackStep <= 0.0)
                {
                    level = 1.0;
                    stage = settings.sustainLevel >= 1.0 ? Stage::sustain : Stage::decay;
                }
                break;

            case Stage::decay:
            {
                const double sustain = settings.sustainLevel;
                level = sustain + (level - sustain) * decayCoefficient;

                // Snapped once it is close enough to hear no difference, so
                // the stage actually ends.
                if (decayCoefficient <= 0.0 || level - sustain <= closeEnough)
                {
                    level = sustain;
                    stage = Stage::sustain;
                }
                break;
            }

            case Stage::sustain:
                level = settings.sustainLevel;
                break;

            case Stage::release:
                level *= releaseCoefficient;

                if (releaseCoefficient <= 0.0 || level <= closeEnough)
                {
                    level = 0.0;
                    stage = Stage::idle;
                }
                break;
        }

        return (float) level;
    }

private:
    enum class Stage { idle = 0, attack, decay, sustain, release };

    static double clamp01 (double v) noexcept
    {
        return v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v);
    }

    void recalculate() noexcept
    {
        attackStep = settings.attackSeconds > 0.0
                       ? 1.0 / (settings.attackSeconds * sampleRate)
                       : 1.0e9;                       // a zero attack lands on the next sample

        decayCoefficient   = coefficientFor (settings.decaySeconds);
        releaseCoefficient = coefficientFor (settings.releaseSeconds);
    }

    /** Per sample multiplier that covers a thousandth of the distance to the
        target in the time given, which is what makes the dial read in real
        seconds rather than in an arbitrary time constant. */
    double coefficientFor (double seconds) const noexcept
    {
        const double samples = seconds * sampleRate;

        if (samples < 1.0)
            return 0.0;                               // snap on the next sample

        return std::pow (closeEnough, 1.0 / samples);
    }

    static constexpr double closeEnough = 0.001;      // sixty decibels down

    Settings settings;
    double sampleRate = 44100.0;
    double level = 0.0;
    double attackStep = 1.0;
    double decayCoefficient = 0.0;
    double releaseCoefficient = 0.0;
    Stage  stage = Stage::idle;
};
