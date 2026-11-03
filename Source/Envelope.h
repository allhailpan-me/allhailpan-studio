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

    The times mean something exact here, which they often do not elsewhere.
    The release time is how long a note released from full scale takes to fall
    to a thousandth of it, which is sixty decibels down, and a note released
    from lower down reaches that floor sooner, the way a discharging capacitor
    does.

    The decay is the same rule with one consequence worth stating, because the
    two halves of it only agree at a sustain of nothing. The per sample
    multiplier covers a thousandth of the distance still to cover in the time
    dialled, and the stage ends when that distance is a thousandth of full
    scale. A decay into a high sustain has little distance to cover, so it
    reaches that absolute floor before it has used its time: measured, the
    stage lasts the whole dial at a sustain of nothing, 0.900 of it at 0.5,
    0.667 at 0.9 and 0.333 at 0.99. The shortfall is exactly where the
    movement left to make is inaudible: at a sustain of 0.9 the entire decay
    is one decibel. An absolute floor is the right one to stop on for that
    reason, and `EnvelopeTest` pins the figures against the closed form rather
    than leaving this paragraph to be taken on trust.

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

    /** The coefficients are derived from the settings, so they have to be
        derived once before anybody asks for a sample. Without this a fresh
        envelope ran every stage on its first sample whatever `Settings`
        declared: an object whose stated defaults and whose actual state
        disagree works everywhere that configures it and wrongly in the one
        place that forgets, which is how an unconfigured section got into the
        state variable filter. Nothing in the studio reaches it today, because
        `Voice::setSampleRate` runs in `prepareToPlay` before any note can
        arrive, and that is exactly the kind of reason that stops being true
        later. */
    Envelope() noexcept
    {
        recalculate();
    }

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
