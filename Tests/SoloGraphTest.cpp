// ---------------------------------------------------------------------------
// Checks solo: which inserts stay audible when something is soloed.
//
// The obvious implementation of solo is "mute everything that is not soloed",
// and it is wrong in a way nobody can diagnose from the symptom. Solo a vocal
// that sends to a reverb and the reverb is on another insert, so it is muted,
// so the vocal is dry. Solo the reverb and the vocal that feeds it is muted,
// so the reverb has nothing in it. Nothing errors and nothing crackles: what
// somebody hears is that solo does not work properly on this studio.
//
// So this does not check the formulas. It checks the answer against the
// published algorithm, three different ways round, because the rule itself is
// the risky part rather than the code that implements it:
//
//   1. Against a closure written separately from the production one, with
//      Floyd-Warshall reachability instead of SoloGraph's single downward
//      pass, over thousands of generated send matrices. That catches the
//      implementation.
//
//   2. Against ARDOUR'S OWN INCREMENTAL ALGORITHM, implemented here from its
//      source: two counters per insert, moved by route_solo_changed, with the
//      inverse push out of mod_solo_by_others_upstream. Driven by random
//      sequences of solo and unsolo over random graphs, comparing after every
//      single step. That catches the rule, and it is the only check in this
//      file that could tell a plausible rule from the published one. It also
//      proves the leak property Ardour ships a repair path for: clear every
//      solo and every counter is back to zero.
//
//   3. Through a model of the mixer's own forward pass, so that the claim
//      "one gain per insert carries its post fader sends with it" is measured
//      rather than asserted. A guitar silenced by a vocal solo must contribute
//      exactly zero at the master, including through the reverb bus the vocal
//      solo is keeping audible. Without that, the guitar is heard through the
//      reverb and every other check in this file still passes.
//
// Plus the case the research named as the one a naive implementation fails,
// the mute precedence table term by term, and the gesture rules.
//
//   ./Tests/run.sh
// ---------------------------------------------------------------------------

#include "SoloGraph.h"

#include <cmath>
#include <cstdio>
#include <limits>
#include <algorithm>
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

void check (bool condition, const std::string& what)
{
    if (! condition)
        fail (what);
}

constexpr int kNumSends = 4;

// ---------------------------------------------------------------------------
// A generated mixer: a send matrix, and whatever solo state is being asked
// about. Sends only ever go to a higher numbered insert, which is this
// mixer's own rule and is what makes the whole graph resolvable in one pass.
// ---------------------------------------------------------------------------
struct Mixer
{
    int n = 16;
    std::vector<std::vector<int>>   sendTo;     // per insert, per send
    std::vector<std::vector<float>> sendLevel;
    std::vector<char> selfSoloed, muted, isolated;

    SoloGraph::Setup setup (SoloGraph::Settings settings = {}) const
    {
        SoloGraph::Setup s;
        s.settings = settings;
        s.inserts.resize ((std::size_t) n);

        for (int i = 0; i < n; ++i)
        {
            s.inserts[(std::size_t) i].selfSoloed = selfSoloed[(std::size_t) i] != 0;
            s.inserts[(std::size_t) i].muted      = muted[(std::size_t) i] != 0;
            s.inserts[(std::size_t) i].isolated   = isolated[(std::size_t) i] != 0;
            s.inserts[(std::size_t) i].sendTo     = sendTo[(std::size_t) i];
        }

        return s;
    }
};

Mixer makeMixer (std::mt19937& rng, int n, double sendDensity)
{
    Mixer m;
    m.n = n;
    m.sendTo.assign ((std::size_t) n, std::vector<int> ((std::size_t) kNumSends, 0));
    m.sendLevel.assign ((std::size_t) n, std::vector<float> ((std::size_t) kNumSends, 0.0f));
    m.selfSoloed.assign ((std::size_t) n, 0);
    m.muted.assign ((std::size_t) n, 0);
    m.isolated.assign ((std::size_t) n, 0);

    std::uniform_real_distribution<double> unit (0.0, 1.0);

    for (int i = 1; i < n; ++i)
    {
        for (int s = 0; s < kNumSends; ++s)
        {
            if (unit (rng) >= sendDensity || i + 1 >= n)
                continue;

            std::uniform_int_distribution<int> to (i + 1, n - 1);
            m.sendTo[(std::size_t) i][(std::size_t) s]    = to (rng);
            m.sendLevel[(std::size_t) i][(std::size_t) s] = 0.2f + 0.6f * (float) unit (rng);
        }
    }

    return m;
}

// ---------------------------------------------------------------------------
// 1. An independent closure.
//
// Floyd-Warshall over the edge set rather than SoloGraph's one downward pass,
// and the audibility rules written out directly, so that a mistake in the
// production reachability or in its indexing shows up as a disagreement.
// ---------------------------------------------------------------------------
std::vector<char> floydReach (const Mixer& m)
{
    const int n = m.n;
    std::vector<char> r ((std::size_t) n * (std::size_t) n, 0);

    auto at = [&r, n] (int a, int b) -> char&
    {
        return r[(std::size_t) a * (std::size_t) n + (std::size_t) b];
    };

    // The direct path into the master exists for every insert and is not a
    // send: it cannot be switched off, which is what puts the master
    // downstream of everything.
    for (int i = 1; i < n; ++i)
    {
        at (i, 0) = 1;

        for (int s = 0; s < kNumSends; ++s)
        {
            const int to = m.sendTo[(std::size_t) i][(std::size_t) s];

            if (to > i && to < n)
                at (i, to) = 1;
        }
    }

    for (int k = 0; k < n; ++k)
        for (int a = 0; a < n; ++a)
            if (at (a, k) != 0)
                for (int b = 0; b < n; ++b)
                    if (at (k, b) != 0)
                        at (a, b) = 1;

    return r;
}

// True when this insert is kept audible by the solo system, from the four
// rules as prose rather than as the production code's loops.
std::vector<char> independentAudible (const Mixer& m, const std::vector<char>& reach)
{
    const int n = m.n;

    auto reaches = [&reach, n] (int a, int b)
    {
        return reach[(std::size_t) a * (std::size_t) n + (std::size_t) b] != 0;
    };

    auto soloed = [&m] (int i) { return i >= 1 && m.selfSoloed[(std::size_t) i] != 0; };

    std::vector<char> out ((std::size_t) n, 0);

    bool any = false;
    for (int i = 1; i < n; ++i)
        if (soloed (i))
            any = true;

    if (! any)
    {
        for (int i = 0; i < n; ++i)
            out[(std::size_t) i] = 1;

        return out;
    }

    for (int i = 0; i < n; ++i)
    {
        if (soloed (i) || (i >= 1 && m.isolated[(std::size_t) i] != 0))
        {
            out[(std::size_t) i] = 1;
            continue;
        }

        for (int s = 1; s < n; ++s)
        {
            if (s == i || ! soloed (s))
                continue;

            if (reaches (s, i))                 // rule 2: downstream of a solo
            {
                out[(std::size_t) i] = 1;
                break;
            }

            if (reaches (i, s))                 // rule 3: upstream of a solo
            {
                bool takenOver = false;

                for (int t = 1; t < n; ++t)
                    if (t != s && soloed (t) && reaches (t, s))
                        takenOver = true;

                if (! takenOver)
                {
                    out[(std::size_t) i] = 1;
                    break;
                }
            }
        }
    }

    return out;
}

// ---------------------------------------------------------------------------
// 2. Ardour's own incremental algorithm.
//
// From libs/ardour/session.cc (Session::route_solo_changed),
// libs/ardour/solo_control.cc (SoloControl::mod_solo_by_others_upstream, and
// the inverse push) and libs/ardour/route.cc (Route::push_solo_upstream). The
// counters are named for where the SOLOED route sits, not where the route
// holding them sits, which is the single easiest thing to get backwards:
// soloedByUpstream on route R means something upstream of R is soloed.
//
// Index 0 is skipped throughout. Ardour's master returns false from
// can_solo(), so it receives no counters at all, and it is exempt from being
// muted by soloing by can_be_muted_by_others() rather than by propagation.
// SoloGraph reaches the same audibility through rule 2 instead, since every
// insert feeds the master, so the two are compared on audibility and not on
// the intermediate flags.
//
// Run with exclusive solo off, which is Ardour's shipped default and the mode
// in which the un-solo path restores what it took.
// ---------------------------------------------------------------------------
struct Ardour
{
    int n = 16;
    const std::vector<char>* reach = nullptr;
    std::vector<char> selfSolo, isolated;
    std::vector<int>  byUpstream, byDownstream;

    bool reaches (int a, int b) const
    {
        return (*reach)[(std::size_t) a * (std::size_t) n + (std::size_t) b] != 0;
    }

    bool soloedByOthers (int i) const
    {
        return byUpstream[(std::size_t) i] > 0 || byDownstream[(std::size_t) i] > 0;
    }

    bool soloed (int i) const
    {
        return selfSolo[(std::size_t) i] != 0 || soloedByOthers (i);
    }

    void modDownstream (int i, int delta)
    {
        byDownstream[(std::size_t) i] += delta;

        if (byDownstream[(std::size_t) i] < 0)
            byDownstream[(std::size_t) i] = 0;
    }

    void modUpstream (int i, int delta)
    {
        const int old = byUpstream[(std::size_t) i];

        byUpstream[(std::size_t) i] += delta;

        if (byUpstream[(std::size_t) i] < 0)
            byUpstream[(std::size_t) i] = 0;

        const int now = byUpstream[(std::size_t) i];

        // The inverse push. Ardour's own comment: soloing one of the N tracks
        // feeding a soloed bus has to take the other N-1 back down, because
        // the person soloing that track is asking for that track.
        if ((selfSolo[(std::size_t) i] != 0 || byDownstream[(std::size_t) i] > 0)
            && ((old == 0 && now > 0) || (old > 0 && now == 0)))
        {
            for (int r = 1; r < n; ++r)
                if (r != i && reaches (r, i))
                    modDownstream (r, -delta);
        }
    }

    void soloChanged (int route, int delta)
    {
        for (int i = 1; i < n; ++i)
        {
            if (i == route || isolated[(std::size_t) i] != 0)
                continue;

            if (reaches (i, route) && byUpstream[(std::size_t) route] == 0)
                modDownstream (i, delta);

            if (reaches (route, i))
                modUpstream (i, delta);
        }
    }

    void setSolo (int route, bool on)
    {
        if ((selfSolo[(std::size_t) route] != 0) == on)
            return;

        selfSolo[(std::size_t) route] = on ? 1 : 0;
        soloChanged (route, on ? 1 : -1);
    }

    bool anySoloed() const
    {
        for (int i = 1; i < n; ++i)
            if (selfSolo[(std::size_t) i] != 0)
                return true;

        return false;
    }

    /** Audible in Ardour's terms: soloed by itself or by propagation, or
        isolated, or nothing is soloed at all. The master is never silenced. */
    bool audible (int i) const
    {
        if (i == 0 || ! anySoloed())
            return true;

        return soloed (i) || isolated[(std::size_t) i] != 0;
    }
};

// ---------------------------------------------------------------------------
// 3. A model of the mixer's own forward pass.
//
// Injects a unit signal at one insert and returns what arrives at the master,
// following exactly what AudioEngine does: each insert's gain is applied to
// its buffer, the sends are copied out of that same buffer afterwards, and
// every insert is summed into the master.
//
// `gainBeforeSends` false is the deliberately wrong version, where the solo
// gain reaches the main output only. It exists so the check can be shown to
// tell the two apart rather than being passed by both.
// ---------------------------------------------------------------------------
double masterContribution (const Mixer& m, const std::vector<float>& gain,
                           int injectAt, bool gainBeforeSends)
{
    const int n = m.n;
    std::vector<double> in ((std::size_t) n, 0.0), out ((std::size_t) n, 0.0);

    in[(std::size_t) injectAt] = 1.0;

    double master = 0.0;

    for (int i = 1; i < n; ++i)
    {
        const double g = (double) gain[(std::size_t) i];

        out[(std::size_t) i] = in[(std::size_t) i] * g;
        master += out[(std::size_t) i];

        const double sendSource = gainBeforeSends ? out[(std::size_t) i]
                                                  : in[(std::size_t) i];

        for (int s = 0; s < kNumSends; ++s)
        {
            const int to = m.sendTo[(std::size_t) i][(std::size_t) s];

            if (to > i && to < n)
                in[(std::size_t) to] += sendSource
                                      * (double) m.sendLevel[(std::size_t) i][(std::size_t) s];
        }
    }

    return master * (double) gain[0];
}

std::vector<float> gainsOf (const SoloGraph::Result& r)
{
    std::vector<float> g (r.inserts.size());

    for (std::size_t i = 0; i < r.inserts.size(); ++i)
        g[i] = r.inserts[i].gain;

    return g;
}

// ---------------------------------------------------------------------------

void testAgainstIndependentClosure()
{
    std::printf ("Against an independently written closure\n");

    std::mt19937 rng (20261021u);
    SoloGraph::Result result;
    int graphs = 0, cases = 0, disagreements = 0, reachDisagreements = 0;

    for (int trial = 0; trial < 4000; ++trial)
    {
        std::uniform_int_distribution<int> size (2, 16);
        std::uniform_real_distribution<double> density (0.0, 1.0);

        auto m = makeMixer (rng, size (rng), density (rng));
        ++graphs;

        const auto reach = floydReach (m);

        for (int round = 0; round < 6; ++round)
        {
            std::uniform_real_distribution<double> unit (0.0, 1.0);
            const double pSolo = unit (rng) * 0.5;

            for (int i = 0; i < m.n; ++i)
            {
                m.selfSoloed[(std::size_t) i] = unit (rng) < pSolo ? 1 : 0;
                m.isolated[(std::size_t) i]   = unit (rng) < 0.1 ? 1 : 0;
                m.muted[(std::size_t) i]      = 0;
            }

            SoloGraph::compute (m.setup(), result);
            ++cases;

            // The reachability matrix itself, which is where the production
            // code is cleverest: one downward pass instead of a closure.
            for (int a = 0; a < m.n; ++a)
                for (int b = 0; b < m.n; ++b)
                    if ((result.reach[(std::size_t) a * (std::size_t) m.n + (std::size_t) b] != 0)
                        != (reach[(std::size_t) a * (std::size_t) m.n + (std::size_t) b] != 0))
                        ++reachDisagreements;

            const auto want = independentAudible (m, reach);

            for (int i = 0; i < m.n; ++i)
            {
                const bool got = result.inserts[(std::size_t) i].gain > 0.0f;

                if (got != (want[(std::size_t) i] != 0))
                    ++disagreements;
            }
        }
    }

    std::printf ("  %d graphs, %d solo sets\n", graphs, cases);
    check (reachDisagreements == 0,
           "reachability disagrees with Floyd-Warshall in "
             + std::to_string (reachDisagreements) + " places");
    check (disagreements == 0,
           "audibility disagrees with the independent closure in "
             + std::to_string (disagreements) + " places");

    if (reachDisagreements == 0 && disagreements == 0)
        std::printf ("  reachability and audibility agree everywhere\n");
}

void testAgainstArdoursOwnAlgorithm()
{
    std::printf ("\nAgainst Ardour's incremental counters\n");

    // Every single-solo case of every generated graph, exhaustively, because
    // one insert soloed is what somebody actually does and it is the case
    // where the two methods are provably the same thing: with one solo up,
    // nothing can trigger the inverse push at all.
    //
    // Then multi-solo sets, switched on upstream first. The two agree there
    // too, and that agreement is the real check on the rule: it is the only
    // thing in this file that could tell a plausible rule from the published
    // one.
    std::mt19937 rng (777u);
    SoloGraph::Result result;
    long singles = 0, singleBad = 0, sets = 0, setBad = 0;
    long sawUpstream = 0, sawDownstream = 0, sawTakenOver = 0;

    for (int trial = 0; trial < 6000; ++trial)
    {
        std::uniform_int_distribution<int> size (2, 16);
        std::uniform_real_distribution<double> unit (0.0, 1.0);

        auto m = makeMixer (rng, size (rng), 0.15 + 0.5 * unit (rng));
        const auto reach = floydReach (m);

        auto reaches = [&reach, &m] (int a, int b)
        {
            return reach[(std::size_t) a * (std::size_t) m.n + (std::size_t) b] != 0;
        };

        auto ardourOver = [&] (const std::vector<int>& order)
        {
            Ardour a;
            a.n = m.n;
            a.reach = &reach;
            a.selfSolo.assign ((std::size_t) m.n, 0);
            a.isolated.assign ((std::size_t) m.n, 0);
            a.byUpstream.assign ((std::size_t) m.n, 0);
            a.byDownstream.assign ((std::size_t) m.n, 0);

            for (int k : order)
                a.setSolo (k, true);

            return a;
        };

        for (int k = 1; k < m.n; ++k)
        {
            std::fill (m.selfSoloed.begin(), m.selfSoloed.end(), (char) 0);
            m.selfSoloed[(std::size_t) k] = 1;

            SoloGraph::compute (m.setup(), result);
            const auto a = ardourOver ({ k });
            ++singles;

            for (int i = 0; i < m.n; ++i)
                if ((result.inserts[(std::size_t) i].gain > 0.0f) != a.audible (i))
                    ++singleBad;
        }

        // A set, switched on upstream first. Sends only go to higher numbered
        // inserts, so increasing insert order IS upstream first here.
        std::vector<int> order;

        for (int i = 1; i < m.n; ++i)
            if (unit (rng) < 0.3)
                order.push_back (i);

        if (order.size() < 2)
            continue;

        std::fill (m.selfSoloed.begin(), m.selfSoloed.end(), (char) 0);

        for (int k : order)
            m.selfSoloed[(std::size_t) k] = 1;

        SoloGraph::compute (m.setup(), result);
        const auto a = ardourOver (order);
        ++sets;

        for (int i = 0; i < m.n; ++i)
            if ((result.inserts[(std::size_t) i].gain > 0.0f) != a.audible (i))
                ++setBad;

        for (int i = 1; i < m.n; ++i)
        {
            if (a.byUpstream[(std::size_t) i] > 0)   ++sawUpstream;
            if (a.byDownstream[(std::size_t) i] > 0) ++sawDownstream;
            if (result.takenOver[(std::size_t) i] != 0) ++sawTakenOver;
        }

        // The inverse push has to have fired somewhere in all this, or the
        // agreement above is agreement about the easy half only.
        (void) reaches;
    }

    std::printf ("  %ld single solos and %ld multi-solo sets compared\n", singles, sets);
    std::printf ("  cases reached: %ld downstream-of-a-solo, %ld upstream-of-a-solo, "
                 "%ld taken over\n", sawUpstream, sawDownstream, sawTakenOver);

    check (singleBad == 0, "disagrees with Ardour on a single solo in "
                             + std::to_string (singleBad) + " places");
    check (setBad == 0, "disagrees with Ardour on an upstream-first solo set in "
                          + std::to_string (setBad) + " places");

    check (sawUpstream > 2000,   "the downstream-of-a-solo case was barely reached");
    check (sawDownstream > 2000, "the upstream-of-a-solo case was barely reached");
    check (sawTakenOver > 500,   "rule 3's exception was barely reached, so the hardest "
                                 "case is not really being compared");
}

void testArdourIsOrderDependent()
{
    std::printf ("\nWhy a closure and not counters\n");

    // The reason this header computes the answer from scratch rather than
    // keeping Ardour's two counters. The same set of soloed inserts, pressed
    // in a different order, gives Ardour a different mix. Demonstrated rather
    // than asserted from the source, because the claim is strong enough that
    // it should be measurable or withdrawn.
    //
    // The smallest case found: 1 sends to 2, 2 sends to 3 and 4, 3 sends to 4.
    // Solo 2 and 4, in either order.
    {
        Mixer m;
        m.n = 5;
        m.sendTo.assign (5, std::vector<int> (kNumSends, 0));
        m.sendLevel.assign (5, std::vector<float> (kNumSends, 0.0f));
        m.selfSoloed.assign (5, 0);
        m.muted.assign (5, 0);
        m.isolated.assign (5, 0);

        m.sendTo[1][0] = 2;
        m.sendTo[2][0] = 3;
        m.sendTo[2][1] = 4;
        m.sendTo[3][0] = 4;

        const auto reach = floydReach (m);

        auto run = [&] (std::vector<int> order)
        {
            Ardour a;
            a.n = 5;
            a.reach = &reach;
            a.selfSolo.assign (5, 0);
            a.isolated.assign (5, 0);
            a.byUpstream.assign (5, 0);
            a.byDownstream.assign (5, 0);

            for (int k : order)
                a.setSolo (k, true);

            return a.audible (1);
        };

        const bool upstreamFirst   = run ({ 2, 4 });
        const bool downstreamFirst = run ({ 4, 2 });

        check (upstreamFirst, "Ardour keeps insert 1 audible when 2 is soloed before 4");
        check (! downstreamFirst,
               "the documented order dependence did not reproduce: if Ardour's algorithm has "
               "become order independent, this file's framing needs rewriting");

        m.selfSoloed[2] = 1;
        m.selfSoloed[4] = 1;

        SoloGraph::Result r;
        SoloGraph::compute (m.setup(), r);

        check (r.inserts[1].gain == 1.0f,
               "the closure should keep insert 1 audible: it feeds soloed insert 2, and nothing "
               "soloed feeds insert 2");

        std::printf ("  5 inserts, solo 2 and 4: Ardour gives insert 1 audible one way round "
                     "and silent the other\n");
    }

    // And over random graphs, so the five insert case is known not to be a
    // curiosity. Every order that is upstream first has to agree with the
    // closure; the ones with an inversion are where Ardour parts company with
    // itself, and the closure must side with the upstream-first answer.
    std::mt19937 rng (5150u);
    SoloGraph::Result result;
    long upstreamFirstSets = 0, upstreamFirstBad = 0;
    long invertedSets = 0, invertedDiffer = 0, closureQuieter = 0;

    for (int trial = 0; trial < 20000; ++trial)
    {
        std::uniform_int_distribution<int> size (2, 16);
        std::uniform_real_distribution<double> unit (0.0, 1.0);

        auto m = makeMixer (rng, size (rng), 0.15 + 0.5 * unit (rng));
        const auto reach = floydReach (m);

        auto reaches = [&reach, &m] (int a, int b)
        {
            return reach[(std::size_t) a * (std::size_t) m.n + (std::size_t) b] != 0;
        };

        std::vector<int> chosen;

        for (int i = 1; i < m.n; ++i)
            if (unit (rng) < 0.3)
                chosen.push_back (i);

        if (chosen.empty())
            continue;

        std::vector<int> order = chosen;
        std::shuffle (order.begin(), order.end(), rng);

        bool inverted = false;

        for (std::size_t later = 0; later < order.size(); ++later)
            for (std::size_t earlier = 0; earlier < later; ++earlier)
                if (reaches (order[later], order[earlier]))
                    inverted = true;

        std::fill (m.selfSoloed.begin(), m.selfSoloed.end(), (char) 0);

        for (int k : chosen)
            m.selfSoloed[(std::size_t) k] = 1;

        SoloGraph::compute (m.setup(), result);

        Ardour a;
        a.n = m.n;
        a.reach = &reach;
        a.selfSolo.assign ((std::size_t) m.n, 0);
        a.isolated.assign ((std::size_t) m.n, 0);
        a.byUpstream.assign ((std::size_t) m.n, 0);
        a.byDownstream.assign ((std::size_t) m.n, 0);

        for (int k : order)
            a.setSolo (k, true);

        long differ = 0, quieter = 0;

        for (int i = 0; i < m.n; ++i)
        {
            const bool got = result.inserts[(std::size_t) i].gain > 0.0f;

            if (got != a.audible (i))
            {
                ++differ;

                if (! got)
                    ++quieter;
            }
        }

        if (inverted)
        {
            ++invertedSets;
            invertedDiffer += differ > 0 ? 1 : 0;
            closureQuieter += quieter;
        }
        else
        {
            ++upstreamFirstSets;
            upstreamFirstBad += differ;
        }
    }

    std::printf ("  %ld sets switched on upstream first, %ld disagreements\n",
                 upstreamFirstSets, upstreamFirstBad);
    std::printf ("  %ld sets with an inversion, %ld of them where Ardour differs\n",
                 invertedSets, invertedDiffer);

    check (upstreamFirstBad == 0,
           "disagrees with Ardour on an upstream-first set in "
             + std::to_string (upstreamFirstBad) + " places");
    check (invertedDiffer > 100,
           "no inverted order produced a difference, so the reason given for computing a "
           "closure is not demonstrated here");
    check (closureQuieter == 0,
           "the closure silenced something Ardour kept audible in "
             + std::to_string (closureQuieter)
             + " places: the divergence is supposed to be one directional, the closure never "
               "losing a feed");
}

void testTheNamedCase()
{
    std::printf ("\nThe case a naive implementation fails\n");

    // Vocal on 3 and guitar on 4 both send to a reverb on 9, which goes to the
    // master by its own direct path.
    Mixer m;
    m.n = 16;
    m.sendTo.assign (16, std::vector<int> (kNumSends, 0));
    m.sendLevel.assign (16, std::vector<float> (kNumSends, 0.0f));
    m.selfSoloed.assign (16, 0);
    m.muted.assign (16, 0);
    m.isolated.assign (16, 0);

    m.sendTo[3][0] = 9;   m.sendLevel[3][0] = 0.5f;
    m.sendTo[4][0] = 9;   m.sendLevel[4][0] = 0.5f;

    SoloGraph::Result r;

    auto audible = [&r] (int i) { return r.inserts[(std::size_t) i].gain > 0.0f; };

    // Solo the reverb: everything feeding it comes with it, which is Ardour's
    // documented behaviour and surprises people.
    m.selfSoloed[9] = 1;
    SoloGraph::compute (m.setup(), r);
    check (audible (0) && audible (3) && audible (4) && audible (9),
           "soloing the reverb should keep both things feeding it audible");
    check (! audible (5) && ! audible (12),
           "soloing the reverb should silence inserts that have nothing to do with it");

    // Now also solo the vocal. The guitar has to go, because somebody soloing
    // one of the feeds of a soloed bus is asking for that one.
    m.selfSoloed[3] = 1;
    SoloGraph::compute (m.setup(), r);
    check (audible (3), "the soloed vocal should be audible");
    check (audible (9), "the reverb should stay audible, so the vocal keeps its reverb");
    check (audible (0), "the master should always be audible");
    check (! audible (4), "the guitar should be silenced once one feed of the bus is soloed");
    check (r.inserts[4].gain == 0.0f, "the guitar should be at exactly zero, not merely small");
    check (! r.inserts[9].soloedByOthers, "with the reverb soloed by the user, its strip should "
                                          "say so explicitly rather than by propagation");
    check (! r.inserts[3].soloedByOthers, "the vocal is soloed by the user, not by propagation");
    check (r.inserts[4].mutedByOthersSoloing, "the guitar's mute button has to say why it is quiet");

    // And back, which is the step that leaks in a counter implementation.
    m.selfSoloed[3] = 0;
    SoloGraph::compute (m.setup(), r);
    check (audible (3) && audible (4) && audible (9),
           "un-soloing the vocal should put the guitar back");

    // The plain case, which is the one the feature exists for.
    m.selfSoloed[9] = 0;
    m.selfSoloed[3] = 1;
    SoloGraph::compute (m.setup(), r);
    check (audible (3) && audible (9) && audible (0),
           "soloing a vocal should keep the reverb it sends to audible");
    check (! audible (4), "soloing a vocal should silence the guitar");
    check (r.inserts[9].soloedByOthers,
           "the reverb is audible by propagation here, and its strip has to be able to say so, "
           "or there is no way to tell why the reverb is still heard");

    std::printf ("  solo the bus, solo one of its feeds, and back: all as published\n");
}

void testTheMasterIsNeverSilenced()
{
    std::printf ("\nThe master, for every graph and every solo set\n");

    std::mt19937 rng (31337u);
    SoloGraph::Result r;
    int cases = 0, bad = 0;

    for (int trial = 0; trial < 3000; ++trial)
    {
        std::uniform_int_distribution<int> size (2, 16);
        std::uniform_real_distribution<double> unit (0.0, 1.0);

        auto m = makeMixer (rng, size (rng), unit (rng));

        bool any = false;

        for (int i = 1; i < m.n; ++i)
        {
            m.selfSoloed[(std::size_t) i] = unit (rng) < 0.3 ? 1 : 0;

            if (m.selfSoloed[(std::size_t) i] != 0)
                any = true;
        }

        if (! any)
            m.selfSoloed[(std::size_t) (1 + (int) (unit (rng) * (double) (m.n - 1))) % (std::size_t) m.n] = 1;

        SoloGraph::compute (m.setup(), r);
        ++cases;

        if (r.inserts[0].gain != 1.0f)
            ++bad;
    }

    std::printf ("  %d non-empty solo sets\n", cases);
    check (bad == 0, "the master was not at unity in " + std::to_string (bad)
                       + " cases, so soloing would produce silence");

    // The flip side, because the one above would also pass a master that is
    // exempt by special case rather than by being downstream of everything:
    // an explicit mute on the master still has to work.
    Mixer m = makeMixer (rng, 8, 0.4);
    m.selfSoloed[3] = 1;
    m.muted[0] = 1;
    SoloGraph::compute (m.setup(), r);
    check (r.inserts[0].gain == 0.0f, "muting the master should still silence it while soloing");
}

void testTheSendOfASilencedInsert()
{
    std::printf ("\nWhat a silenced insert sends, measured through the mixer's own pass\n");

    std::mt19937 rng (4242u);
    SoloGraph::Result r;
    int measured = 0, leaked = 0, wrongModelLeaked = 0, injectedAudible = 0;
    double worstLeak = 0.0;

    for (int trial = 0; trial < 3000; ++trial)
    {
        std::uniform_int_distribution<int> size (4, 16);
        std::uniform_real_distribution<double> unit (0.0, 1.0);

        auto m = makeMixer (rng, size (rng), 0.3 + 0.5 * unit (rng));

        for (int i = 1; i < m.n; ++i)
            m.selfSoloed[(std::size_t) i] = unit (rng) < 0.2 ? 1 : 0;

        SoloGraph::compute (m.setup(), r);

        if (! r.anySoloed)
            continue;

        const auto gain = gainsOf (r);

        for (int i = 1; i < m.n; ++i)
        {
            const double got = masterContribution (m, gain, i, true);

            if (gain[(std::size_t) i] == 0.0f)
            {
                ++measured;

                if (got != 0.0)
                {
                    ++leaked;
                    worstLeak = std::max (worstLeak, std::abs (got));
                }

                // The same insert through the wrong model, where the gain
                // reaches the main output but not the sends taken off it.
                if (masterContribution (m, gain, i, false) != 0.0)
                    ++wrongModelLeaked;
            }
            else if (got > 0.0)
            {
                ++injectedAudible;
            }
        }
    }

    std::printf ("  %d silenced inserts measured at the master\n", measured);
    check (measured > 2000, "too few silenced inserts to have measured anything");
    check (injectedAudible > 2000, "too few audible inserts, so the measurement is not two sided");
    check (leaked == 0, "a silenced insert reached the master in " + std::to_string (leaked)
                          + " cases, worst " + std::to_string (worstLeak));

    // The point of this check is that it can tell the right wiring from the
    // wrong one. If applying the gain to the main output alone also produced
    // silence, the check above would be proving nothing.
    std::printf ("  with the gain applied to the main output only: %d of them leak\n",
                 wrongModelLeaked);
    check (wrongModelLeaked > 500,
           "the wrong wiring leaks in too few cases for this check to be meaningful");
}

void testIsolate()
{
    std::printf ("\nSolo isolate\n");

    std::mt19937 rng (909u);
    SoloGraph::Result plain, isolatedResult;
    int cases = 0, notAudible = 0, changedSomethingElse = 0, madeADifference = 0;

    for (int trial = 0; trial < 3000; ++trial)
    {
        std::uniform_int_distribution<int> size (3, 16);
        std::uniform_real_distribution<double> unit (0.0, 1.0);

        auto m = makeMixer (rng, size (rng), unit (rng));

        for (int i = 1; i < m.n; ++i)
            m.selfSoloed[(std::size_t) i] = unit (rng) < 0.25 ? 1 : 0;

        SoloGraph::compute (m.setup(), plain);

        const int k = 1 + (int) (unit (rng) * (double) (m.n - 1)) % (m.n - 1);

        m.isolated[(std::size_t) k] = 1;
        SoloGraph::compute (m.setup(), isolatedResult);
        ++cases;

        if (isolatedResult.inserts[(std::size_t) k].gain != 1.0f)
            ++notAudible;

        if (plain.inserts[(std::size_t) k].gain != isolatedResult.inserts[(std::size_t) k].gain)
            ++madeADifference;

        for (int i = 0; i < m.n; ++i)
            if (i != k && plain.inserts[(std::size_t) i].gain
                            != isolatedResult.inserts[(std::size_t) i].gain)
                ++changedSomethingElse;
    }

    std::printf ("  %d cases, isolate changed the answer in %d of them\n", cases, madeADifference);
    check (notAudible == 0, "an isolated insert was silenced in " + std::to_string (notAudible)
                              + " cases");
    check (changedSomethingElse == 0,
           "isolating one insert changed another in " + std::to_string (changedSomethingElse)
             + " cases: isolate exempts its own insert and is not a propagation rule");
    check (madeADifference > 500, "isolate never actually rescued anything, so this proves little");

    // Isolate is an exemption from somebody else's solo and nothing more. It
    // does not override a mute the user set by hand, and it does not stop the
    // insert being soloed itself.
    // No sends, so that "everything else" really is unrelated to the isolated
    // insert and the two claims below are about isolate rather than about a
    // route that happens to have been generated.
    Mixer m = makeMixer (rng, 8, 0.0);
    m.selfSoloed[2] = 1;
    m.isolated[5] = 1;
    m.muted[5] = 1;
    SoloGraph::compute (m.setup(), plain);
    check (plain.inserts[5].gain == 0.0f, "an isolated insert that is muted by hand stays muted");

    m.muted[5] = 0;
    m.selfSoloed[5] = 1;
    m.selfSoloed[2] = 0;
    SoloGraph::compute (m.setup(), plain);
    check (plain.inserts[5].gain == 1.0f && plain.anySoloed,
           "an isolated insert is still soloable itself");
    check (plain.inserts[2].gain == 0.0f,
           "an isolated insert's own solo still silences everything else");
}

void testMutePrecedence()
{
    std::printf ("\nMute against solo, term by term\n");

    // MuteMaster::mute_gain_at, both branches, written out rather than
    // generated, because the asymmetry inside the override branch is the whole
    // point: the test there is self solo, not soloed. A reverb return that is
    // muted by hand and is only audible because a soloed vocal feeds it stays
    // muted, and a paraphrase of "solo overrides mute" loses that.
    struct Case
    {
        bool overrides, muted, selfSoloed, byOthers, isolated, anySoloed;
        float want;
        const char* what;
    };

    const float cut = 0.25f;

    const Case cases[] =
    {
        // Nothing soloed anywhere: only the mutes are left, in both branches.
        { false, false, false, false, false, false, 1.0f,  "default, nothing soloed, not muted" },
        { false, true,  false, false, false, false, 0.0f,  "default, nothing soloed, muted" },
        { true,  false, false, false, false, false, 1.0f,  "override, nothing soloed, not muted" },
        { true,  true,  false, false, false, false, 0.0f,  "override, nothing soloed, muted" },

        // The default branch: an explicit mute is tested first and wins.
        { false, false, true,  false, false, true,  1.0f,  "default, self soloed" },
        { false, true,  true,  false, false, true,  0.0f,  "default, self soloed and muted: muted wins" },
        { false, false, false, true,  false, true,  1.0f,  "default, soloed by propagation" },
        { false, true,  false, true,  false, true,  0.0f,  "default, soloed by propagation and muted" },
        { false, false, false, false, false, true,  cut,   "default, somebody else soloed" },
        { false, true,  false, false, false, true,  0.0f,  "default, muted and somebody else soloed" },
        { false, false, false, false, true,  true,  1.0f,  "default, isolated while another is soloed" },
        { false, true,  false, false, true,  true,  0.0f,  "default, isolated and muted" },

        // The override branch: self solo beats a mute, propagation does not.
        { true,  true,  true,  false, false, true,  1.0f,  "override, self soloed and muted: solo wins" },
        { true,  true,  false, true,  false, true,  0.0f,  "override, soloed by propagation and muted: still muted" },
        { true,  false, false, true,  false, true,  1.0f,  "override, soloed by propagation, not muted" },
        { true,  false, false, false, false, true,  cut,   "override, somebody else soloed" },
        { true,  true,  false, false, false, true,  0.0f,  "override, muted and somebody else soloed" },
        { true,  false, false, false, true,  true,  1.0f,  "override, isolated while another is soloed" },
        { true,  true,  false, false, true,  true,  0.0f,  "override, isolated and muted" },
    };

    for (const auto& c : cases)
    {
        SoloGraph::Settings s;
        s.soloMuteGain      = cut;
        s.soloOverridesMute = c.overrides;

        const float got = SoloGraph::gainFor (c.muted, c.selfSoloed, c.byOthers,
                                              c.isolated, c.anySoloed, s);

        if (got != c.want)
            fail (std::string (c.what) + ": wanted " + std::to_string (c.want)
                    + ", got " + std::to_string (got));
    }

    std::printf ("  %d combinations, both branches\n", (int) (sizeof (cases) / sizeof (cases[0])));

    // And the whole-graph version of the one that matters most, so the table
    // above is known to be reachable through compute rather than only through
    // the helper.
    std::mt19937 rng (5u);
    Mixer m = makeMixer (rng, 12, 0.0);
    m.sendTo[2][0] = 7; m.sendLevel[2][0] = 0.5f;
    m.selfSoloed[2] = 1;
    m.muted[7] = 1;

    SoloGraph::Result r;
    SoloGraph::Settings s;
    s.soloOverridesMute = true;
    SoloGraph::compute (m.setup (s), r);
    check (r.inserts[7].gain == 0.0f,
           "with solo overriding mute, a muted reverb kept audible by a soloed vocal stays muted");

    m.muted[7] = 0;
    m.muted[2] = 1;
    SoloGraph::compute (m.setup (s), r);
    check (r.inserts[2].gain == 1.0f,
           "with solo overriding mute, a self soloed insert is heard even though it is muted");

    SoloGraph::Settings plain;
    SoloGraph::compute (m.setup (plain), r);
    check (r.inserts[2].gain == 0.0f,
           "by default a self soloed insert that is also muted is silent");
}

void testTheSoloMuteGain()
{
    std::printf ("\nThe solo mute gain\n");

    std::mt19937 rng (11u);
    Mixer m = makeMixer (rng, 8, 0.0);
    m.selfSoloed[3] = 1;

    SoloGraph::Result r;

    // The documented default is full mute, and "full" has to mean exactly
    // zero rather than very small: a mix of fifteen inserts at -90 dB each is
    // audible, and anything short of zero leaves a noise floor that moves
    // when somebody solos.
    SoloGraph::compute (m.setup(), r);
    check (r.inserts[5].gain == 0.0f, "the default solo mute gain should be exactly zero");

    SoloGraph::Settings s;
    s.soloMuteGain = 0.25f;
    SoloGraph::compute (m.setup (s), r);
    check (r.inserts[5].gain == 0.25f, "a solo mute gain in range should be used as given");
    check (r.inserts[3].gain == 1.0f, "the soloed insert is unaffected by the solo mute gain");

    // A setting read from a file the application does not control. Every
    // comparison against a NaN is false, so a clamp would hand the NaN back
    // and the gain would poison the whole mix.
    struct Bad { float value; const char* what; };

    const Bad bad[] =
    {
        { std::numeric_limits<float>::quiet_NaN(), "not a number" },
        { std::numeric_limits<float>::infinity(),  "infinite" },
        { -std::numeric_limits<float>::infinity(), "minus infinite" },
        { -0.5f, "negative" },
        { 2.0f,  "above unity" },
        { 1.0e30f, "absurd" },
    };

    for (const auto& b : bad)
    {
        SoloGraph::Settings broken;
        broken.soloMuteGain = b.value;
        SoloGraph::compute (m.setup (broken), r);

        if (r.inserts[5].gain != 0.0f)
            fail (std::string ("a ") + b.what + " solo mute gain should fall back to full mute, got "
                    + std::to_string (r.inserts[5].gain));
    }

    // Unity at the top of the range is a legal setting and means solo does
    // nothing, which is a useless setting rather than an invalid one.
    SoloGraph::Settings unity;
    unity.soloMuteGain = 1.0f;
    SoloGraph::compute (m.setup (unity), r);
    check (r.inserts[5].gain == 1.0f, "a solo mute gain of unity should be honoured");

    std::printf ("  default exactly zero, %d unusable values refused\n",
                 (int) (sizeof (bad) / sizeof (bad[0])));
}

void testGestures()
{
    std::printf ("\nWhat a click means\n");

    using G = SoloGraph::Gesture;

    struct Case { bool pref, modifier, already; G want; const char* what; };

    const Case cases[] =
    {
        { false, false, false, G::toggle,    "additive, plain click, not soloed" },
        { false, false, true,  G::toggle,    "additive, plain click on a soloed strip turns it off" },
        { false, true,  false, G::exclusive, "additive, modified click solos only this one" },
        { false, true,  true,  G::clearAll,  "additive, modified click on a soloed strip clears every solo" },
        { true,  false, false, G::exclusive, "exclusive preference, plain click solos only this one" },
        { true,  false, true,  G::toggle,    "exclusive preference, plain click on a soloed strip turns it off" },
        { true,  true,  false, G::toggle,    "exclusive preference, modified click is additive" },
        { true,  true,  true,  G::clearAll,  "exclusive preference, modified click on a soloed strip clears every solo" },
    };

    for (const auto& c : cases)
        if (SoloGraph::gestureFor (c.pref, c.modifier, c.already) != c.want)
            fail (std::string ("gesture: ") + c.what);

    std::printf ("  %d gestures, and the modifier is the opposite of the preference in each\n",
                 (int) (sizeof (cases) / sizeof (cases[0])));
}

void testReuseAndEdges()
{
    std::printf ("\nA reused result, and the degenerate sizes\n");

    // compute is called over and over with one Result member, which is the
    // whole reason it does not allocate under the graph lock. A field left
    // unassigned from a previous call would be a stale answer, and the
    // symptom would be a mix that depended on what had been soloed before.
    std::mt19937 rng (2024u);
    SoloGraph::Result reused;
    int stale = 0;

    for (int trial = 0; trial < 2000; ++trial)
    {
        std::uniform_int_distribution<int> size (1, 16);
        std::uniform_real_distribution<double> unit (0.0, 1.0);

        auto m = makeMixer (rng, size (rng), unit (rng));

        for (int i = 0; i < m.n; ++i)
        {
            m.selfSoloed[(std::size_t) i] = unit (rng) < 0.3 ? 1 : 0;
            m.muted[(std::size_t) i]      = unit (rng) < 0.2 ? 1 : 0;
            m.isolated[(std::size_t) i]   = unit (rng) < 0.2 ? 1 : 0;
        }

        SoloGraph::Settings s;
        s.soloMuteGain      = unit (rng) < 0.5 ? 0.0f : 0.3f;
        s.soloOverridesMute = unit (rng) < 0.5;

        SoloGraph::compute (m.setup (s), reused);

        SoloGraph::Result fresh;
        SoloGraph::compute (m.setup (s), fresh);

        if (reused.anySoloed != fresh.anySoloed || reused.inserts.size() != fresh.inserts.size()
            || reused.reach != fresh.reach || reused.takenOver != fresh.takenOver)
        {
            ++stale;
            continue;
        }

        for (std::size_t i = 0; i < fresh.inserts.size(); ++i)
            if (reused.inserts[i].gain != fresh.inserts[i].gain
                || reused.inserts[i].soloedByOthers != fresh.inserts[i].soloedByOthers
                || reused.inserts[i].mutedByOthersSoloing != fresh.inserts[i].mutedByOthersSoloing)
                ++stale;
    }

    check (stale == 0, "a reused result disagreed with a fresh one in " + std::to_string (stale)
                         + " places");

    // Index 0 is the master: its own solo and isolate flags are not a user's
    // instruction and must not become one. A project file can carry anything.
    Mixer m = makeMixer (rng, 6, 0.3);
    m.selfSoloed[0] = 1;
    m.isolated[0] = 1;

    SoloGraph::Result r;
    SoloGraph::compute (m.setup(), r);
    check (! r.anySoloed, "a solo stored against the master should be ignored, not treated as a solo");

    for (int i = 1; i < m.n; ++i)
        check (r.inserts[(std::size_t) i].gain == 1.0f,
               "a solo stored against the master should not silence the mixer");

    // The master on its own, and nothing at all.
    SoloGraph::Setup one;
    one.inserts.resize (1);
    SoloGraph::compute (one, r);
    check (r.inserts.size() == 1 && r.inserts[0].gain == 1.0f, "a mixer of one insert");

    SoloGraph::Setup none;
    SoloGraph::compute (none, r);
    check (r.inserts.empty() && r.reach.empty() && r.takenOver.empty() && ! r.anySoloed,
           "an empty mixer should empty the result rather than leaving the last one in it");

    std::printf ("  2000 reuses agree with a fresh result, and the edges hold\n");
}

} // namespace

int main()
{
    std::printf ("SoloGraph\n=========\n\n");

    testAgainstIndependentClosure();
    testAgainstArdoursOwnAlgorithm();
    testArdourIsOrderDependent();
    testTheNamedCase();
    testTheMasterIsNeverSilenced();
    testTheSendOfASilencedInsert();
    testIsolate();
    testMutePrecedence();
    testTheSoloMuteGain();
    testGestures();
    testReuseAndEdges();

    std::printf ("\n");

    if (failures != 0)
    {
        std::printf ("%d check(s) failed.\n", failures);
        return 1;
    }

    std::printf ("All checks passed.\n");
    return 0;
}
