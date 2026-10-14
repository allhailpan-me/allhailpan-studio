#pragma once
#include <cmath>
#include <cstddef>
#include <deque>
#include <functional>
#include <string>
#include <vector>

//==============================================================================
/** What every effect in this studio has in common, as data rather than as a
    base class with virtual functions.

    An effect here is a plain struct with a patch, a few methods and two
    tables: one of its controls and one of its presets. It carries no JUCE and
    knows nothing about plugins, which is what lets the whole of it be tested
    at the sample level in a second, with sanitizers on, and compared against
    whatever specification it is supposed to meet.

    One templated wrapper in `Fx/Plugin.h` turns any of them into an ordinary
    hosted plugin. So adding an effect is a header and a test, plus one line
    where the plugins are listed, rather than another four hundred lines of
    parameter plumbing. The instrument learned that lesson the expensive way:
    `PanOneParams.h` is the same idea written out once for one thing, and the
    reason it is not simply reused here is that it is tied to one patch type
    and to one instrument's grouping.

    **The tables are templates over the patch type, and everything else is
    not.** `Spec` and `Preset` are the only things that need to know what a
    patch is. The wrapper, the ids, the choice lists and the round trip are
    all the same for every effect, so they are written once.

    Every rule `PanOneParams.h` arrived at applies here and for the same
    reasons, so they are stated once and not re-argued per effect:

    - A control is a line in a table, because the same list is walked when
      parameters are built, when a patch is read once a block, when a preset
      is chosen and when a project is saved. Four hand written lists is four
      chances for one to be missing an entry, and a missing entry is silent.
    - Every choice is saved by a stable string id and never by its index,
      because inserting one entry in the middle of a list would otherwise
      repoint every patch anybody had ever saved at the next one along, still
      loading and still playing.
    - The first entry of every choice list is the one that is safe to fall
      back to, because an id nobody recognises lands there.
    - Ranges carry the point where the middle of the control should land, so
      a frequency knob travels logarithmically and says so, rather than
      hiding a skew somewhere else.

    No JUCE.
*/
namespace Fx
{

enum class Kind
{
    continuous,
    choice
};

struct Choice
{
    const char* id;
    const char* name;
};

/** The index for an id, or -1 if the list has never heard of it. */
inline int choiceFromId (const std::vector<Choice>& list, const char* id)
{
    if (id == nullptr)
        return -1;

    for (std::size_t i = 0; i < list.size(); ++i)
        if (std::string (list[i].id) == id)
            return (int) i;

    return -1;
}

/** The id at an index, or the first entry's for anything out of range. */
inline const char* idOfChoice (const std::vector<Choice>& list, int index)
{
    if (list.empty())
        return "";

    if (index < 0 || index >= (int) list.size())
        return list.front().id;

    return list[(std::size_t) index].id;
}

inline int toIndex (float value, int count)
{
    const int index = (int) (value + 0.5f);
    return index < 0 ? 0 : (index >= count ? count - 1 : index);
}

/** Somewhere stable to keep a name that is generated rather than written.

    A deque and not a vector: a vector moves its contents when it grows,
    which would leave every pointer already handed out dangling. An effect
    with four bands generates its control names in a loop, so this is not a
    hypothetical.
*/
inline const char* keep (std::string text)
{
    static std::deque<std::string> held;
    held.push_back (std::move (text));
    return held.back().c_str();
}

//==============================================================================
/** One control of an effect. */
template <typename Patch>
struct Spec
{
    const char* id;
    const char* name;
    const char* unit;

    Kind  kind;
    float minimum;
    float maximum;
    float centre;

    const std::vector<Choice>& (*choices)();    /**< null for a continuous control */

    std::function<float (const Patch&)> get;
    std::function<void (Patch&, float)> set;
};

template <typename Patch>
struct Preset
{
    const char* name;
    const char* category;
    Patch patch;
};

/** Where each group of controls starts, for laying a panel out. */
struct Group
{
    const char* heading;
    std::size_t first, count;
};

//==============================================================================
/** The two things every effect's table needs doing to it, written once.

    `fromPatch` and `toPatch` are what make a patch and a list of parameter
    values two views of the same thing, and the test for any effect checks
    that going out and coming back changes nothing, which is the one property
    that catches a field somebody forgot to list.
*/
template <typename Patch>
std::vector<float> fromPatch (const std::vector<Spec<Patch>>& specs, const Patch& patch)
{
    std::vector<float> values;
    values.reserve (specs.size());

    for (const auto& spec : specs)
        values.push_back (spec.get (patch));

    return values;
}

template <typename Patch>
void toPatch (const std::vector<Spec<Patch>>& specs,
              const std::vector<float>& values, Patch& patch)
{
    for (std::size_t i = 0; i < specs.size() && i < values.size(); ++i)
        specs[i].set (patch, values[i]);
}

//==============================================================================
/** A builder, so that a table reads as a list of controls rather than as a
    list of braces.

    Worth having because the alternative is every effect repeating the same
    eleven field aggregate initialiser per control, where a field in the
    wrong place compiles and is wrong.
*/
template <typename Patch>
class Table
{
public:
    void continuous (const char* id, const char* name, const char* unit,
                     float low, float high, float centre,
                     std::function<float (const Patch&)> get,
                     std::function<void (Patch&, float)> set)
    {
        specs.push_back ({ id, name, unit, Kind::continuous, low, high, centre,
                           nullptr, std::move (get), std::move (set) });
    }

    void pick (const char* id, const char* name,
               const std::vector<Choice>& (*list)(),
               std::function<float (const Patch&)> get,
               std::function<void (Patch&, float)> set)
    {
        specs.push_back ({ id, name, "", Kind::choice,
                           0.0f, (float) (list().size() - 1), 0.0f,
                           list, std::move (get), std::move (set) });
    }

    /** A switch, which is a choice with the two entries every switch has. */
    void toggle (const char* id, const char* name,
                 std::function<bool (const Patch&)> get,
                 std::function<void (Patch&, bool)> set)
    {
        pick (id, name, offOrOn,
              [get] (const Patch& p) { return get (p) ? 1.0f : 0.0f; },
              [set] (Patch& p, float v) { set (p, v > 0.5f); });
    }

    void group (const char* heading, std::size_t count)
    {
        groups.push_back ({ heading, nextGroupStart, count });
        nextGroupStart += count;
    }

    static const std::vector<Choice>& offOrOn()
    {
        static const std::vector<Choice> list { { "off", "Off" }, { "on", "On" } };
        return list;
    }

    std::vector<Spec<Patch>> specs;
    std::vector<Group> groups;

private:
    std::size_t nextGroupStart = 0;
};

} // namespace Fx
