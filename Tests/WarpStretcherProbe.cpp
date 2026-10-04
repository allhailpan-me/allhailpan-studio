// ---------------------------------------------------------------------------
// How accurately does Rubber Band place a point that a warp marker has pinned?
//
// This is the measurement that decided how warping is implemented, and it is
// kept so the decision can be rechecked rather than taken on trust.
//
// Rubber Band offers setKeyFrameMap, which takes a mapping from source sample
// positions to output sample positions and is documented as the way to do
// "pre-planned variable time" stretching in offline mode. That is exactly what
// a list of warp markers is, so it looks like the obvious way to warp a clip
// while keeping its pitch. The numbers below are why the studio does not do it
// that way. See the pull request that added warping for the reasoning.
//
// The ruler is a linear chirp. A pitch preserving stretcher leaves frequency
// alone, so the instantaneous frequency heard at an output position says
// exactly which part of the source is playing there. A click train is a far
// worse ruler, because a stretcher smears a transient by most of its window
// and the smearing is not symmetric when the rate is changing.
//
// Needs a Rubber Band checkout, which the build normally fetches itself:
//
//   git clone --depth 1 https://github.com/breakfastquay/rubberband.git rb
//   g++ -std=c++17 -O2 -I rb -w -o probe WarpStretcherProbe.cpp \
//       rb/single/RubberBandSingle.cpp && ./probe
//
// Measured with Rubber Band at e4296ac, R3 "finer" engine, 44100Hz mono:
//
//   control, ratio 1, no map           worst 2.79 ms   (the ruler's own noise)
//   uniform x1.5, no map               worst 0.00 ms
//   markers 1s apart                   worst 9.57 ms
//   markers 500ms apart                worst 68.82 ms
//   markers 1s apart, plus half speed  worst 9.14 ms
//
// So a key frame map is good to about ten milliseconds at best, and gets worse
// as markers get closer together. Ten milliseconds is two percent of a beat at
// 120bpm, and seventy is a snare that is audibly late. A warp marker promises
// that this point lands on this beat, and that promise has to be exact.
// ---------------------------------------------------------------------------

#include <rubberband/RubberBandStretcher.h>
#include <cmath>
#include <cstdio>
#include <map>
#include <vector>
using RB = RubberBand::RubberBandStretcher;
static const double rate = 44100.0;
static const double f0 = 300.0, f1 = 3000.0;

static std::vector<float> chirp (double duration)
{
    std::vector<float> x ((size_t) (duration * rate));
    const double k = (f1 - f0) / duration;
    for (size_t i = 0; i < x.size(); ++i)
    {
        const double t = (double) i / rate;
        x[i] = (float) (0.5 * std::sin (2.0 * M_PI * (f0 * t + 0.5 * k * t * t)));
    }
    return x;
}
static double sourceTimeOfFreq (double f, double duration)
{
    return (f - f0) * duration / (f1 - f0);
}
// Instantaneous frequency around an output time, from zero crossings.
static double freqAt (const std::vector<float>& x, double t)
{
    const long c = (long) (t * rate);
    const long half = (long) (0.015 * rate);
    long a = std::max (0L, c - half), b = std::min ((long) x.size() - 1, c + half);
    int crossings = 0;
    long firstX = -1, lastX = -1;
    for (long i = a + 1; i <= b; ++i)
        if ((x[i-1] < 0.0f) != (x[i] < 0.0f))
        {
            if (firstX < 0) firstX = i;
            lastX = i;
            ++crossings;
        }
    if (crossings < 3 || lastX <= firstX) return 0.0;
    return (crossings - 1) * 0.5 * rate / (double) (lastX - firstX);
}
static std::vector<float> render (const std::vector<float>& in, double timeRatio,
                                  const std::map<size_t,size_t>& keys)
{
    RB st ((size_t) rate, 1, RB::OptionProcessOffline | RB::OptionEngineFiner | RB::OptionChannelsTogether,
           timeRatio, 1.0);
    if (! keys.empty()) st.setKeyFrameMap (keys);
    st.setExpectedInputDuration (in.size());
    const size_t block = 4096;
    st.setMaxProcessSize (block);
    for (size_t pos = 0; pos < in.size(); pos += block)
    { const size_t n = std::min (block, in.size()-pos); const float* p = in.data()+pos;
      st.study (&p, n, pos+n >= in.size()); }
    std::vector<float> out, scratch (block);
    auto drain = [&] { int a; while ((a = st.available()) > 0) {
        float* o = scratch.data(); const size_t g = st.retrieve (&o, std::min ((size_t) a, block));
        if (g == 0) break; out.insert (out.end(), scratch.begin(), scratch.begin()+g); } };
    for (size_t pos = 0; pos < in.size(); pos += block)
    { const size_t n = std::min (block, in.size()-pos); const float* p = in.data()+pos;
      st.process (&p, n, pos+n >= in.size()); drain(); }
    for (int g = 0; g < 2000 && st.available() >= 0; ++g) drain();
    return out;
}

// src[i] -> dst[i], both in seconds, strictly increasing, plus (0,0) and the ends.
static void trial (const char* label, std::vector<double> src, std::vector<double> dst,
                   double duration, double outDuration)
{
    auto in = chirp (duration);
    std::map<size_t,size_t> keys;
    for (size_t i = 0; i < src.size(); ++i)
    {
        const size_t sf = (size_t) llround (src[i] * rate), of = (size_t) llround (dst[i] * rate);
        if (sf == 0 || of == 0 || sf >= in.size()) continue;
        keys[sf] = of;
    }
    auto out = render (in, outDuration / duration, keys);

    std::printf ("%s   keys %zu   out %.3fs (wanted %.3fs)\n", label, keys.size(),
                 (double) out.size()/rate, outDuration);

    double worst = 0.0, sum = 0.0; int n = 0;
    for (size_t i = 0; i < src.size(); ++i)
    {
        if (dst[i] < 0.05 || dst[i] > outDuration - 0.05) continue;
        const double f = freqAt (out, dst[i]);
        if (f <= 0.0) { std::printf ("     marker %zu: no reading\n", i); continue; }
        const double heardSource = sourceTimeOfFreq (f, duration);
        const double err = (heardSource - src[i]) * 1000.0;
        worst = std::max (worst, std::abs (err)); sum += std::abs (err); ++n;
        std::printf ("     marker %zu at out %.3fs: hearing source %.4fs, marked %.4fs, off by %+7.2f ms\n",
                     i, dst[i], heardSource, src[i], err);
    }
    std::printf ("   worst %.2f ms, mean %.2f ms\n\n", worst, n ? sum/n : 0.0);
}

int main()
{
    // Control: the measurement itself, with no stretching.
    trial ("control, ratio 1, no map", { 1.0, 2.0, 3.0, 4.0, 5.0 }, { 1.0, 2.0, 3.0, 4.0, 5.0 }, 6.0, 6.0);

    // Uniform stretch, no map: checks the ruler under stretching.
    trial ("uniform x1.5, no map", {}, {}, 6.0, 9.0);

    // A drifting performance pulled onto a 1 second grid.
    trial ("drift -> 1s grid", { 0.95, 2.10, 3.05, 4.30, 5.00 },
           { 1.00, 2.00, 3.00, 4.00, 5.00 }, 6.0, 6.0);

    // Dense markers, every 500ms.
    {
        std::vector<double> s, d;
        for (int i = 1; i <= 20; ++i) { d.push_back (i * 0.5); s.push_back (i * 0.5 + 0.08 * std::sin (i * 1.7)); }
        trial ("dense, every 500ms", s, d, 11.0, 10.5);
    }

    // Drift plus a big overall tempo change.
    trial ("drift and half speed", { 0.95, 2.10, 3.05, 4.30, 5.00 },
           { 2.00, 4.00, 6.00, 8.00, 10.00 }, 6.0, 12.0);
    return 0;
}
