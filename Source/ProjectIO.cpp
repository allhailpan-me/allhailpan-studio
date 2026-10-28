#include "ProjectIO.h"

namespace
{
    const juce::String rootTag  { "ALLHAILPAN_PROJECT" };
    const juce::String hostedTag { "HOSTED" };
    // Version 2 added automation clips on the playlist. Version 3 added take
    // folders, the comp over them, and the playlist loop range. Version 4
    // added the pan mode on each insert and the project's pan law. Version 5
    // added a fade in and a fade out on every clip, with a shape for each, and
    // the project's automatic de-click length. Older files load unchanged:
    // they simply have no TAKES or LOOP in them, an audio clip without a
    // folder is still an ordinary audio clip, an insert without a pan mode is
    // a balance control, which is what every version up to 3 did to every
    // insert, and a clip without a fade takes the automatic one.
    //
    // 6 adds solo and solo isolate on each insert. Older files load unchanged:
    // an insert with neither attribute is not soloed and is not isolated,
    // which is what every version up to 5 did to every insert, because there
    // was no solo at all.
    //
    // Only those two are saved and not what they add up to. Ardour saves its
    // propagation counters as well, and that is the one thing in its model
    // deliberately not copied: restoring a derived quantity means a file can
    // be inconsistent with its own routing, and the project would then load
    // audibly wrong with nothing to say why. These two are the user's own
    // instructions and the rest is recomputed on load, which is the same
    // discipline WarpMap.h encodes by normalising its markers on load.
    //
    // The bump is unconditional, as 2, 3, 4 and 5 were, so the previous
    // release refuses a project saved by this build rather than opening it.
    // An older loader would ignore both attributes without complaint, and a
    // solo isolate somebody set on their reverb returns would be silently
    // gone, so solo would then behave differently from the way they set it up
    // with nothing to say that the file had said otherwise.
    // 7 changes what a stored fader position means. Up to 6 it was a number
    // in 0..1.25 whose gain was (v / 0.8)^2, so unity sat at 0.640 of the
    // travel and the ceiling was +7.75 dB. It is now a position in 0..1 under
    // the law in FaderLaw.h, where unity is exactly 0.75 and the ceiling is
    // +10 dB.
    //
    // This is the one bump so far where older files do **not** load unchanged,
    // and that is the whole difficulty of it. Two things carry the old
    // meaning and both have to be translated: each insert's `volume`, and the
    // value of every automation curve aimed at one, because a curve is stored
    // 0..1 and was multiplied by the old travel of 1.25 to reach a position.
    //
    // Both go through gain, never through the fraction of the travel. Going
    // through the travel fraction looks right and is a silent disaster: old
    // unity is 0.640 of the travel and new unity is 0.75, so a fader saved at
    // 0 dB would come back at position 0.640, which is -4.40 dB. Every track of
    // every existing project would come back quieter, by an amount depending
    // on where its fader happened to be, and it would present as "my old
    // mixes all sound wrong and the balance has moved" with nothing anywhere
    // in the audio path to blame for it. FaderLaw::legacyToPosition and
    // FaderLaw::legacyCurveValueToPosition are that translation, and they are
    // tested without JUCE because this loader is not.
    //
    // A curve's points are translated and the line between two of them is not,
    // because the two laws interpolate differently. A migrated fade therefore
    // keeps its ends and changes shape in between. See FaderLaw.h for the
    // figures: nothing over a fade of 20 dB or less, and real over a long one.
    //
    // The bump is unconditional, as 2 to 6 were, and here it has to be: an
    // older loader reading a file saved by this build would read every fader
    // position through the old law, and the damage that does is a balance
    // change rather than a level change, which is the worse of the two. A
    // strip left at unity comes back 1.12 dB quieter, one at the top of the
    // travel 6.12 dB quieter, and one down at -60 dB 15.71 dB louder, so
    // every fader moves by a different amount and no move of the master puts
    // the mix back. Refusing to open it is the only honest answer.
    //
    // Those three figures are asserted in FaderLawTest so this paragraph
    // cannot drift, which it already had: it used to say "+3.5 dB on every
    // strip at once", a figure nothing in the repository produced and which
    // was wrong in size, in sign and in being uniform at all.
    constexpr int formatVersion = 7;

    juce::String num (double v) { return juce::String (v, 9); }

    // ---- plugins ----

    void writePlugin (juce::XmlElement& parent, juce::AudioPluginInstance* plugin,
                      const std::shared_ptr<juce::XmlElement>& missing)
    {
        if (plugin != nullptr)
        {
            auto* hosted = parent.createNewChildElement (hostedTag);
            hosted->addChildElement (plugin->getPluginDescription().createXml().release());

            juce::MemoryBlock state;
            plugin->getStateInformation (state);
            hosted->setAttribute ("state", state.toBase64Encoding());
        }
        else if (missing != nullptr)
        {
            parent.addChildElement (new juce::XmlElement (*missing));   // keep what we couldn't load
        }
    }

    juce::String pluginName (const juce::XmlElement& hosted)
    {
        if (auto* d = hosted.getFirstChildElement())
            return d->getStringAttribute ("name", "Unknown plugin");
        return "Unknown plugin";
    }

    std::unique_ptr<juce::AudioPluginInstance> readPlugin (const juce::XmlElement& hosted,
                                                           const ProjectIO::PluginFactory& factory,
                                                           juce::String& error)
    {
        juce::PluginDescription description;
        auto* descXml = hosted.getFirstChildElement();
        if (descXml == nullptr || ! description.loadFromXml (*descXml))
        {
            error = "the saved plugin details are damaged";
            return {};
        }

        auto plugin = factory (description, error);
        if (plugin == nullptr)
            return {};

        juce::MemoryBlock state;
        if (state.fromBase64Encoding (hosted.getStringAttribute ("state")) && state.getSize() > 0)
            plugin->setStateInformation (state.getData(), (int) state.getSize());
        return plugin;
    }

    // ---- samples ----

    juce::File findByName (const juce::File& folder, const juce::String& fileName)
    {
        if (! folder.isDirectory() || fileName.isEmpty())
            return {};
        for (const auto& entry : juce::RangedDirectoryIterator (folder, true, fileName, juce::File::findFiles))
            return entry.getFile();
        return {};
    }

    juce::File resolveSample (const juce::XmlElement& e, const juce::File& projectDir)
    {
        const juce::File absolute (e.getStringAttribute ("path"));
        if (absolute.existsAsFile())
            return absolute;

        const auto relative = e.getStringAttribute ("relative");
        if (relative.isNotEmpty())
        {
            const auto f = projectDir.getChildFile (relative);
            if (f.existsAsFile())
                return f;
        }

        // Moved computers or drives: look beside the project by file name
        return findByName (projectDir, absolute.getFileName());
    }

    juce::String notesToText (const std::vector<MidiNote>& notes)
    {
        juce::StringArray parts;
        for (auto& n : notes)
            parts.add (num (n.start) + "," + num (n.length) + "," + juce::String (n.note) + "," + juce::String (n.velocity, 4));
        return parts.joinIntoString (";");
    }

    std::vector<MidiNote> notesFromText (const juce::String& text)
    {
        std::vector<MidiNote> notes;
        for (auto& part : juce::StringArray::fromTokens (text, ";", {}))
        {
            auto f = juce::StringArray::fromTokens (part, ",", {});
            if (f.size() < 4) continue;
            MidiNote n;
            n.start    = f[0].getDoubleValue();
            n.length   = std::max (1.0 / 256.0, f[1].getDoubleValue());
            n.note     = juce::jlimit (0, 127, f[2].getIntValue());
            n.velocity = juce::jlimit (0.0f, 1.0f, f[3].getFloatValue());
            notes.push_back (n);
        }
        return notes;
    }

    // Warp markers, as source seconds and beats, in the same compact form the
    // notes and automation points use, and to the same precision, which is far
    // finer than a sample: a marker reloads exactly where it was put.
    juce::String warpToText (const std::vector<WarpMarker>& markers)
    {
        juce::StringArray parts;
        for (auto& m : markers)
            parts.add (num (m.source) + "," + num (m.beat));
        return parts.joinIntoString (";");
    }

    std::vector<WarpMarker> warpFromText (const juce::String& text)
    {
        std::vector<WarpMarker> markers;
        for (auto& part : juce::StringArray::fromTokens (text, ";", {}))
        {
            auto f = juce::StringArray::fromTokens (part, ",", {});
            if (f.size() < 2) continue;
            markers.push_back ({ f[0].getDoubleValue(), f[1].getDoubleValue() });
        }
        return markers;
    }

    // ---- comps ----
    //
    // A comp is a short list of "from here, this take", so it is written the
    // way the warp markers above are rather than as an element each: a vocal
    // comped bar by bar has a few dozen of them and an element apiece would
    // be most of the file.

    juce::String compToText (const std::vector<CompSegment>& segments)
    {
        juce::StringArray parts;
        for (auto& c : segments)
            parts.add (num (c.at) + "," + juce::String (c.take));
        return parts.joinIntoString (";");
    }

    std::vector<CompSegment> compFromText (const juce::String& text)
    {
        std::vector<CompSegment> segments;
        for (auto& part : juce::StringArray::fromTokens (text, ";", {}))
        {
            auto f = juce::StringArray::fromTokens (part, ",", {});
            if (f.size() < 2) continue;
            segments.push_back ({ f[0].getDoubleValue(), f[1].getIntValue() });
        }
        return segments;
    }

    // ---- automation targets and curves ----
    //
    // The kind is written as a word rather than as the enumeration's number so
    // that a project file stays readable and so that reordering the enumeration
    // cannot silently re-aim every curve in every saved project.

    const char* targetKindName (AutoTargetKind k)
    {
        switch (k)
        {
            case AutoTargetKind::channelParam:  return "channelParam";
            case AutoTargetKind::insertVolume:  return "insertVolume";
            case AutoTargetKind::insertPan:     return "insertPan";
            case AutoTargetKind::insertFxParam: return "insertFxParam";
        }
        return "channelParam";
    }

    void writeTarget (juce::XmlElement& e, const AutoTarget& t)
    {
        e.setAttribute ("kind", targetKindName (t.kind));
        e.setAttribute ("channel", t.channel);
        e.setAttribute ("insert", t.insert);
        e.setAttribute ("fxSlot", t.fxSlot);
        e.setAttribute ("param", t.paramIndex);
        e.setAttribute ("paramName", t.paramName);
    }

    AutoTarget readTarget (const juce::XmlElement& e)
    {
        AutoTarget t;

        // Missing means a file written before anything but a channel parameter
        // could be automated, which is exactly what that default is.
        const auto kind = e.getStringAttribute ("kind", "channelParam");
        if      (kind == "insertVolume")  t.kind = AutoTargetKind::insertVolume;
        else if (kind == "insertPan")     t.kind = AutoTargetKind::insertPan;
        else if (kind == "insertFxParam") t.kind = AutoTargetKind::insertFxParam;
        else                              t.kind = AutoTargetKind::channelParam;

        t.channel    = juce::jlimit (0, kNumChannels - 1, e.getIntAttribute ("channel"));
        t.insert     = juce::jlimit (0, kNumInserts - 1, e.getIntAttribute ("insert"));
        t.fxSlot     = juce::jlimit (0, kNumFxSlots - 1, e.getIntAttribute ("fxSlot"));
        t.paramIndex = std::max (0, e.getIntAttribute ("param"));
        t.paramName  = e.getStringAttribute ("paramName");
        return t;
    }

    juce::String curveToText (const std::vector<AutoCurvePoint>& points)
    {
        juce::StringArray parts;
        for (auto& p : points)
            parts.add (num (p.beat) + "," + juce::String (p.value, 6) + "," + juce::String (p.bend, 6));
        return parts.joinIntoString (";");
    }

    std::vector<AutoCurvePoint> curveFromText (const juce::String& text)
    {
        std::vector<AutoCurvePoint> points;
        for (auto& part : juce::StringArray::fromTokens (text, ";", {}))
        {
            auto f = juce::StringArray::fromTokens (part, ",", {});
            if (f.size() < 2) continue;

            // A missing bend is a straight line, which is what a point written
            // by anything that does not know about bends would have meant.
            points.push_back ({ f[0].getDoubleValue(),
                                f[1].getDoubleValue(),
                                f.size() > 2 ? f[2].getDoubleValue() : 0.0 });
        }
        return points;
    }

    juce::String pointsToText (const std::vector<AutoPoint>& points)
    {
        juce::StringArray parts;
        for (auto& p : points)
            parts.add (num (p.beat) + "," + juce::String (p.value, 6));
        return parts.joinIntoString (";");
    }

    std::vector<AutoPoint> pointsFromText (const juce::String& text)
    {
        std::vector<AutoPoint> points;
        for (auto& part : juce::StringArray::fromTokens (text, ";", {}))
        {
            auto f = juce::StringArray::fromTokens (part, ",", {});
            if (f.size() < 2) continue;
            points.push_back ({ f[0].getDoubleValue(), juce::jlimit (0.0f, 1.0f, f[1].getFloatValue()) });
        }
        return points;
    }
}

juce::File ProjectIO::autosaveFile()
{
    return juce::File::getSpecialLocation (juce::File::userDocumentsDirectory)
               .getChildFile ("ALLHAILPAN Studio").getChildFile ("Autosave").getChildFile ("autosave" + extension);
}

void ProjectIO::resetEngine (AudioEngine& engine)
{
    // The pan law belongs to the project, so a new project starts on the
    // default rather than keeping whatever the last one was open with. This is
    // the one of the four that survives testing: a law left behind would make
    // the same session sound different depending on what was loaded before it.
    engine.setPanLaw (PanLaw::defaultLaw);

    for (int c = 0; c < kNumChannels; ++c)
    {
        engine.setChannelPlugin (c, nullptr);
        engine.setChannelInsert (c, c + 1);
    }
    for (int i = 0; i < kNumInserts; ++i)
    {
        for (int k = 0; k < kNumFxSlots; ++k)
            engine.setFx (i, k, nullptr);

        auto& ctl = engine.insert (i);
        ctl.name = i == 0 ? juce::String ("Master") : "Insert " + juce::String (i);
        ctl.volume.store ((float) FaderLaw::unityPosition);
        ctl.pan.store (0.0f);
        ctl.panMode.store (PanLaw::defaultMode);

        // Through the engine rather than through the atomic, because what an
        // insert is multiplied by now depends on every other insert's solo
        // and on the send matrix. See AudioEngine::setMute.
        engine.setMute (i, false);

        // Reset, which is the one of the four that survives testing and then
        // leaks between projects: a strip still wired to a microphone in a
        // project that never mentioned one, or a vocal left soloed in the
        // last session so that a new project comes up with fifteen inserts
        // silent.
        if (i > 0)
        {
            engine.setInsertInput (i, {});
            engine.setInsertArmed (i, false);
            engine.setSolo (i, false);
            engine.setSoloIsolate (i, false);
        }

        for (int k = 0; k < kNumSends; ++k)
        {
            ctl.sendTo[(size_t) k].store (0);
            ctl.sendLevel[(size_t) k].store (0.0f);
        }
        for (auto& b : ctl.bypass)
            b.store (false);
    }
}

// ---------------------------------------------------------------------------

juce::Result ProjectIO::save (const juce::File& file, Project& project, AudioEngine& engine, const SaveOptions& options)
{
    const auto projectDir = file.getParentDirectory();
    if (! projectDir.createDirectory())
        return juce::Result::fail ("Couldn't create the folder " + projectDir.getFullPathName());

    juce::XmlElement root (rootTag);
    root.setAttribute ("version", formatVersion);
    root.setAttribute ("app", JUCE_APPLICATION_VERSION_STRING);
    root.setAttribute ("bpm", engine.getBpm());
    root.setAttribute ("songStart", engine.getSongStart());

    // ---- loop range ----
    // Where you were working. Not part of the undo history, for the same
    // reason arming is not, but worth coming back to.
    root.setAttribute ("loopStart", project.loopStart);
    root.setAttribute ("loopEnd", project.loopEnd);

    // ---- channel rack ----
    root.setAttribute ("rackStart", project.rackStart);
    root.setAttribute ("rackBars", project.rackBars);
    root.setAttribute ("rackStepsPerBar", project.rackStepsPerBar);

    // ---- groove ----
    root.setAttribute ("grooveOn", project.groove.enabled);
    root.setAttribute ("grooveBase", (int) project.groove.base);
    root.setAttribute ("grooveSwing", project.groove.swing);
    root.setAttribute ("grooveAmount", project.groove.amount);
    root.setAttribute ("grooveVelocity", project.groove.velocity);
    root.setAttribute ("grooveRandom", project.groove.random);

    // The automatic de-click fade length. What makes a project written by an
    // older build open sensibly is the default on the load side rather than
    // anything here, since an older build cannot write this attribute at all.
    root.setAttribute ("autoFade", project.autoFadeSeconds);

    // ---- modulators ----
    if (! project.modulators.empty())
    {
        auto* modsXml = root.createNewChildElement ("MODULATORS");

        for (const auto& m : project.modulators)
        {
            auto* e = modsXml->createNewChildElement ("MOD");
            e->setAttribute ("name", m.name);
            e->setAttribute ("shape", (int) m.shape);
            e->setAttribute ("rate", m.rateBeats);
            e->setAttribute ("phase", m.phase);
            e->setAttribute ("bipolar", m.bipolar);
            e->setAttribute ("enabled", m.enabled);

            for (const auto& t : m.targets)
            {
                auto* target = e->createNewChildElement ("TARGET");
                writeTarget (*target, t);
                target->setAttribute ("depth", (double) t.depth);
            }
        }
    }

    // ---- tracks ----
    auto* tracksXml = root.createNewChildElement ("TRACKS");
    for (auto& t : project.tracks)
    {
        auto* e = tracksXml->createNewChildElement ("TRACK");
        e->setAttribute ("name", t.name);
        e->setAttribute ("muted", t.muted);
        e->setAttribute ("insert", t.insert);
    }

    // ---- mixer ----
    auto* mixerXml = root.createNewChildElement ("MIXER");

    // The law belongs to the project rather than to a strip, the way Logic and
    // Reaper both keep it, so it is written once here rather than sixteen
    // times below.
    mixerXml->setAttribute ("panLaw", PanLaw::lawToStored (engine.getPanLaw()));

    for (int i = 0; i < kNumInserts; ++i)
    {
        auto& ctl = engine.insert (i);
        auto* e = mixerXml->createNewChildElement ("INSERT");
        e->setAttribute ("index", i);
        e->setAttribute ("name", ctl.name);
        e->setAttribute ("volume", (double) ctl.volume.load());
        e->setAttribute ("pan", (double) ctl.pan.load());
        e->setAttribute ("panMode", PanLaw::modeToStored (ctl.panMode.load()));
        e->setAttribute ("mute", ctl.mute.load());

        // The two instructions, and not the answer: see the note on the
        // format version. Insert 0 is never soloed or isolated, so its row
        // does not carry either, and a file that says otherwise is refused by
        // the engine rather than by the loader.
        if (i > 0)
        {
            e->setAttribute ("solo", engine.isSoloed (i));
            e->setAttribute ("soloIsolate", engine.isSoloIsolated (i));
        }

        // Which socket feeds this strip, and whether it records. Saved with
        // the project rather than the application, because which microphone
        // is on which strip is part of a session rather than a preference,
        // and reopening a session to find the routing gone would mean setting
        // it up again every time.
        if (i > 0)
        {
            const auto assignment = engine.getInsertInput (i);

            if (assignment.assigned)
            {
                e->setAttribute ("input", InputSource::assignmentToStored (assignment));
                e->setAttribute ("armed", engine.isInsertArmed (i));
            }
        }

        for (int k = 0; k < kNumSends; ++k)
            if (ctl.sendTo[(size_t) k].load() > 0)
            {
                auto* send = e->createNewChildElement ("SEND");
                send->setAttribute ("index", k);
                send->setAttribute ("to", ctl.sendTo[(size_t) k].load());
                send->setAttribute ("level", (double) ctl.sendLevel[(size_t) k].load());
            }

        for (int k = 0; k < kNumFxSlots; ++k)
        {
            auto* plugin = engine.getFx (i, k);
            std::shared_ptr<juce::XmlElement> missing;
            if (auto it = project.missingFx.find (i * kNumFxSlots + k); it != project.missingFx.end())
                missing = it->second;
            if (plugin == nullptr && missing == nullptr)
                continue;

            auto* slot = e->createNewChildElement ("FX");
            slot->setAttribute ("slot", k);
            slot->setAttribute ("bypass", ctl.bypass[(size_t) k].load());
            writePlugin (*slot, plugin, missing);
        }
    }

    // ---- channels ----
    auto* channelsXml = root.createNewChildElement ("CHANNELS");
    for (int c = 0; c < kNumChannels; ++c)
    {
        const auto& info = project.channels[(size_t) c];
        auto* e = channelsXml->createNewChildElement ("CHANNEL");
        e->setAttribute ("index", c);
        e->setAttribute ("name", info.name);
        e->setAttribute ("insert", info.insert);
        e->setAttribute ("rackNote", info.rackNote);
        e->setAttribute ("rackTrack", info.rackTrack);
        e->setAttribute ("sumOutputs", engine.getChannelSumsOutputs (c));
        e->setAttribute ("splitBuses", engine.getChannelSplitsBuses (c));

        for (int b = 0; b < kMaxOutBuses; ++b)
            if (const int to = engine.getBusInsert (c, b); to > 0)
            {
                auto* bus = e->createNewChildElement ("BUS");
                bus->setAttribute ("index", b);
                bus->setAttribute ("insert", to);
            }
        writePlugin (*e, engine.getChannelPlugin (c), info.missingPlugin);
    }

    // ---- samples ----
    std::map<const SampleData*, int> sampleIds;
    const auto samplesDir = projectDir.getChildFile (file.getFileNameWithoutExtension() + " Samples");
    auto* samplesXml = root.createNewChildElement ("SAMPLES");

    // Every take in a folder has its own file, so the list of audio a project
    // depends on is the clips' samples plus the folders' takes. Missing one
    // here is how a project collected into a folder loses nine of its ten
    // passes and keeps the comp that points at them.
    std::vector<std::shared_ptr<SampleData>> audioInUse;
    for (auto& clip : project.clips)
    {
        if (clip.folder != nullptr)
            for (auto& take : clip.folder->takes)
                audioInUse.push_back (take.sample);
        if (clip.isAudio() && clip.sample != nullptr)
            audioInUse.push_back (clip.sample);
    }

    for (auto& held : audioInUse)
    {
        if (held == nullptr || sampleIds.count (held.get()))
            continue;

        auto& sample = *held;
        if (options.collectSamples && ! options.isAutosave && ! sample.isMissing()
            && sample.file.existsAsFile() && ! sample.file.isAChildOf (projectDir))
        {
            samplesDir.createDirectory();
            auto dest = samplesDir.getNonexistentChildFile (sample.file.getFileNameWithoutExtension(),
                                                            sample.file.getFileExtension(), false);
            if (sample.file.copyFileTo (dest))
                sample.file = dest;
        }

        const int id = (int) sampleIds.size() + 1;
        sampleIds[held.get()] = id;

        auto* e = samplesXml->createNewChildElement ("SAMPLE");
        e->setAttribute ("id", id);
        e->setAttribute ("name", sample.isMissing() ? sample.file.getFileNameWithoutExtension() : sample.name);
        e->setAttribute ("path", sample.file.getFullPathName());
        e->setAttribute ("relative", sample.file.getRelativePathFrom (projectDir));
        e->setAttribute ("seconds", sample.durationSeconds());
    }

    // ---- clips ----
    auto* clipsXml = root.createNewChildElement ("CLIPS");
    for (auto& clip : project.clips)
    {
        auto* e = clipsXml->createNewChildElement ("CLIP");
        e->setAttribute ("type", clip.isAudio() ? "audio"
                                 : clip.isAutomation() ? "automation" : "midi");
        e->setAttribute ("track", clip.track);
        e->setAttribute ("start", clip.start);
        e->setAttribute ("offset", clip.offset);
        e->setAttribute ("length", clip.length);
        e->setAttribute ("gain", (double) clip.gainDb);
        e->setAttribute ("muted", clip.muted);

        // The fades somebody drew. Written for every clip type rather than
        // only for audio, so that the four fields travel together and a later
        // change that gives another clip type a fade does not have to find
        // this. Zero is the common case and means the edge takes the
        // project's automatic fade.
        //
        // The shape is saved by its id and read back through
        // `ClipFade::shapeFromId`, so an id this build does not know lands on
        // `linear` rather than off the end of the enum. Named ids would be
        // better still, the way a modulation routing is named, but a fade
        // shape is not a routing: there are three of them, nothing points at
        // them, and the fallback loses a curve rather than redirecting one.
        e->setAttribute ("fadeIn",       clip.fadeIn);
        e->setAttribute ("fadeOut",      clip.fadeOut);
        e->setAttribute ("fadeInShape",  ClipFade::idOfShape (clip.fadeInShape));
        e->setAttribute ("fadeOutShape", ClipFade::idOfShape (clip.fadeOutShape));

        if (clip.isAudio())
        {
            e->setAttribute ("sample", clip.sample != nullptr ? sampleIds[clip.sample.get()] : 0);
            e->setAttribute ("stretch", clip.stretch);
            e->setAttribute ("sourceBpm", clip.sourceBpm);
            e->setAttribute ("followTempo", clip.followTempo);
            e->setAttribute ("pitch", clip.pitch);
            if (! clip.warp.empty())
                e->createNewChildElement ("WARP")->addTextElement (warpToText (clip.warp));

            if (clip.folder != nullptr && ! clip.folder->takes.empty())
            {
                auto* takesXml = e->createNewChildElement ("TAKES");
                takesXml->setAttribute ("crossfadeMs", clip.folder->crossfadeMs);
                takesXml->setAttribute ("comp", compToText (clip.folder->comp));

                for (auto& take : clip.folder->takes)
                {
                    auto* t = takesXml->createNewChildElement ("TAKE");
                    t->setAttribute ("name", take.name);
                    t->setAttribute ("sample", take.sample != nullptr ? sampleIds[take.sample.get()] : 0);
                    t->setAttribute ("start", take.start);
                    t->setAttribute ("offset", take.offset);
                    t->setAttribute ("length", take.length);
                }
            }
        }
        else if (clip.pattern != nullptr)
        {
            e->setAttribute ("channel", clip.channel);
            e->createNewChildElement ("NOTES")->addTextElement (notesToText (clip.pattern->notes));
            for (auto& lane : clip.pattern->lanes)
            {
                auto* l = e->createNewChildElement ("LANE");
                l->setAttribute ("param", lane.paramIndex);
                l->setAttribute ("name", lane.name);
                l->addTextElement (pointsToText (lane.points));
            }
        }
        else if (clip.isAutomation() && clip.curve != nullptr)
        {
            auto* a = e->createNewChildElement ("AUTO");
            writeTarget (*a, clip.curve->target);
            a->addTextElement (curveToText (clip.curve->points));
        }
    }

    // ---- write safely (temp file, then swap) ----
    juce::TemporaryFile temp (file);
    {
        juce::FileOutputStream out (temp.getFile());
        if (! out.openedOk())
            return juce::Result::fail ("Couldn't write to " + file.getFullPathName());
        {
            juce::GZIPCompressorOutputStream zipped (out, 6);
            root.writeTo (zipped);
            zipped.flush();
        }
        out.flush();
        if (out.getStatus().failed())
            return out.getStatus();
    }
    if (! temp.overwriteTargetFileWithTemporary())
        return juce::Result::fail ("Couldn't replace " + file.getFullPathName() + ". Is it open somewhere else?");

    return juce::Result::ok();
}

// ---------------------------------------------------------------------------

juce::Result ProjectIO::load (const juce::File& file, Project& project, AudioEngine& engine, SampleCache& cache,
                              const PluginFactory& factory, LoadReport& report)
{
    std::unique_ptr<juce::XmlElement> root;
    {
        juce::FileInputStream in (file);
        if (! in.openedOk())
            return juce::Result::fail ("Couldn't open " + file.getFullPathName());

        juce::GZIPDecompressorInputStream unzipped (&in, false);
        root = juce::parseXML (unzipped.readEntireStreamAsString());
    }
    if (root == nullptr)
        root = juce::parseXML (file);   // uncompressed, in case someone edited it by hand

    if (root == nullptr || ! root->hasTagName (rootTag))
        return juce::Result::fail (file.getFileName() + " isn't an ALLHAILPAN Studio project, or it's damaged.");
    if (root->getIntAttribute ("version", 1) > formatVersion)
        return juce::Result::fail ("This project was made with a newer version of ALLHAILPAN Studio. Please update.");

    // Read before anything is restored, because two things in this file carry
    // a meaning that changed at version 7 and both need to know whether they
    // are reading an older one. See the note on formatVersion.
    const int fileVersion = root->getIntAttribute ("version", 1);

    const auto projectDir = file.getParentDirectory();

    resetEngine (engine);
    project.clearAll();

    report.bpm       = root->getDoubleAttribute ("bpm", 128.0);

    project.setLoopRange (root->getDoubleAttribute ("loopStart", 0.0),
                          root->getDoubleAttribute ("loopEnd", 0.0));

    project.rackStart       = std::max (0.0, root->getDoubleAttribute ("rackStart", 0.0));
    project.rackBars        = juce::jlimit (1, 4, root->getIntAttribute ("rackBars", 1));
    project.rackStepsPerBar = juce::jlimit (1, 32, root->getIntAttribute ("rackStepsPerBar", 16));

    project.groove.enabled  = root->getBoolAttribute ("grooveOn", false);
    project.groove.base     = (Groove::Base) juce::jlimit (0, 2, root->getIntAttribute ("grooveBase", 1));
    project.groove.swing    = juce::jlimit (0.3, 0.8, root->getDoubleAttribute ("grooveSwing", 0.5));
    project.groove.amount   = juce::jlimit (0.0, 1.0, root->getDoubleAttribute ("grooveAmount", 1.0));
    project.groove.velocity = juce::jlimit (0.0, 1.0, root->getDoubleAttribute ("grooveVelocity", 0.0));
    project.groove.random   = juce::jlimit (0.0, 1.0, root->getDoubleAttribute ("grooveRandom", 0.0));

    // A project written before fades existed has no attribute here and gets
    // the default, so every old project opens de-clicked. The upper bound is
    // a second, which is far longer than a de-click fade has any business
    // being and is there so that a corrupt or hand-edited file cannot fade a
    // whole clip out: `ClipFade::resolve` would honour it.
    project.autoFadeSeconds = juce::jlimit (0.0, 1.0,
        root->getDoubleAttribute ("autoFade", ClipFade::defaultAutoSeconds));

    project.modulators.clear();
    if (auto* modsXml = root->getChildByName ("MODULATORS"))
        for (auto* e : modsXml->getChildWithTagNameIterator ("MOD"))
        {
            Modulator m;
            m.name      = e->getStringAttribute ("name", "LFO");
            m.shape     = (ModShape) juce::jlimit (0, 6, e->getIntAttribute ("shape", 0));
            m.rateBeats = juce::jlimit (0.0625, 128.0, e->getDoubleAttribute ("rate", 4.0));
            m.phase     = juce::jlimit (0.0, 1.0, e->getDoubleAttribute ("phase", 0.0));
            m.bipolar   = e->getBoolAttribute ("bipolar", true);
            m.enabled   = e->getBoolAttribute ("enabled", true);

            for (auto* t : e->getChildWithTagNameIterator ("TARGET"))
            {
                ModTarget target;
                static_cast<AutoTarget&> (target) = readTarget (*t);
                target.depth = (float) juce::jlimit (-1.0, 1.0, t->getDoubleAttribute ("depth", 0.5));
                m.targets.push_back (std::move (target));
            }

            project.modulators.push_back (std::move (m));
        }
    report.songStart = root->getDoubleAttribute ("songStart", 0.0);
    engine.setBpm (report.bpm);
    engine.setSongStart (report.songStart);
    project.bpm = engine.getBpm();

    // ---- tracks ----
    if (auto* tracksXml = root->getChildByName ("TRACKS"))
    {
        int i = 0;
        for (auto* e : tracksXml->getChildWithTagNameIterator ("TRACK"))
        {
            if (i >= (int) project.tracks.size()) break;
            auto& t = project.tracks[(size_t) i++];
            t.name   = e->getStringAttribute ("name", t.name);
            t.muted  = e->getBoolAttribute ("muted");
            t.insert = juce::jlimit (0, kNumInserts - 1, e->getIntAttribute ("insert", t.insert));
        }
    }

    // ---- mixer ----
    if (auto* mixerXml = root->getChildByName ("MIXER"))
    {
        // A project written before version 4 has no law in it, and every
        // insert in it was a balance control, so the law it gets back cannot
        // change how it sounds whatever it is.
        engine.setPanLaw (PanLaw::lawFromStored (
            mixerXml->getIntAttribute ("panLaw", PanLaw::lawToStored (PanLaw::defaultLaw))));

        for (auto* e : mixerXml->getChildWithTagNameIterator ("INSERT"))
        {
            const int i = e->getIntAttribute ("index", -1);
            if (! juce::isPositiveAndBelow (i, kNumInserts)) continue;

            auto& ctl = engine.insert (i);
            ctl.name = e->getStringAttribute ("name", ctl.name);
            // A file older than 7 stores a position over 0..1.25 under the
            // old (v / 0.8)^2 law, so it is translated through gain rather
            // than read. A missing attribute is unity either way, which is
            // what a file that never had a fader in it meant.
            //
            // The jlimit is a backstop for a file at version 7 or later, and
            // for a legacy file it does nothing: legacyPositionToGain clamps
            // the old position to the old travel itself, so a hand edited file
            // claiming a position of 9 goes through the old law as 1.25 and
            // comes back at 0.943820, which is the old ceiling of +7.75 dB and
            // is inside the new travel. That is the same answer the old loader
            // gave, which clamped to 0..1.25 before applying the law.
            //
            // Said this way round because the inner clamp is what is doing the
            // work. A reader who takes this comment to mean the jlimit handles
            // it would be free to delete the inner clamp, and then a stored 9
            // would go through the old law unclamped to a gain of 126.5625 and
            // come back saturated at the top of the new travel, +10 dB rather
            // than the +7.75 dB the old loader gave. The jlimit cannot catch
            // that, because 1.0 is inside its own range. An earlier version of
            // this warning quoted 0.943820 as the consequence of the deletion,
            // which is the answer the clamp already gives, so checking the
            // comment against the code argued for removing the clamp.
            const double storedVolume = e->getDoubleAttribute ("volume",
                                            fileVersion < 7 ? FaderLaw::legacyUnityPosition
                                                            : FaderLaw::unityPosition);
            const double faderPosition = fileVersion < 7
                                           ? FaderLaw::legacyToPosition (storedVolume)
                                           : storedVolume;
            ctl.volume.store ((float) juce::jlimit (FaderLaw::minPosition,
                                                    FaderLaw::maxPosition, faderPosition));
            ctl.pan.store ((float) juce::jlimit (-1.0, 1.0, e->getDoubleAttribute ("pan", 0.0)));
            // Insert 0 is always a balance control whatever the file says.
            // The master is what every other insert is summed into, so there
            // is no source on it to place, and a law there would move the
            // level of the whole mix. The menu does not offer it either; this
            // is the half that a hand edited or newer file cannot get past.
            ctl.panMode.store (i == 0 ? PanLaw::defaultMode
                                      : PanLaw::modeFromStored (
                                            e->getIntAttribute ("panMode",
                                                PanLaw::modeToStored (PanLaw::defaultMode))));
            // All three through the engine so the solo closure is recomputed
            // afterwards, the way the sends and the inputs below already are.
            // Stored straight into the atomics instead, a project would load
            // with its solos set and its mixer still mixing as though none of
            // them were, until the next time somebody happened to move a send.
            engine.setMute (i, e->getBoolAttribute ("mute"));

            if (i > 0)
            {
                engine.setSolo (i, e->getBoolAttribute ("solo"));
                engine.setSoloIsolate (i, e->getBoolAttribute ("soloIsolate"));
            }

            // Routed through the engine so the delay compensation is
            // recomputed: an insert carrying a live input is exempt from it.
            if (i > 0)
            {
                const auto assignment = InputSource::assignmentFromStored (e->getIntAttribute ("input", 0));
                engine.setInsertInput (i, assignment);
                engine.setInsertArmed (i, assignment.assigned && e->getBoolAttribute ("armed", true));
            }

            for (auto* send : e->getChildWithTagNameIterator ("SEND"))
            {
                const int k = send->getIntAttribute ("index", -1);
                if (! juce::isPositiveAndBelow (k, kNumSends))
                    continue;

                // Routed through the engine so it is validated and the delay
                // compensation is recomputed.
                engine.setSend (i, k, send->getIntAttribute ("to", 0),
                                (float) send->getDoubleAttribute ("level", 0.0));
            }

            for (auto* fx : e->getChildWithTagNameIterator ("FX"))
            {
                const int k = fx->getIntAttribute ("slot", -1);
                auto* hosted = fx->getChildByName (hostedTag);
                if (! juce::isPositiveAndBelow (k, kNumFxSlots) || hosted == nullptr) continue;

                juce::String error;
                if (auto plugin = readPlugin (*hosted, factory, error))
                {
                    engine.setFx (i, k, std::move (plugin));
                    engine.setFxBypass (i, k, fx->getBoolAttribute ("bypass"));
                }
                else
                {
                    project.missingFx[i * kNumFxSlots + k] = std::make_shared<juce::XmlElement> (*hosted);
                    report.missingPlugins.add (pluginName (*hosted) + " (" + ctl.name + ", slot " + juce::String (k + 1) + "): " + error);
                }
            }
        }
    }

    // ---- channels ----
    if (auto* channelsXml = root->getChildByName ("CHANNELS"))
    {
        for (auto* e : channelsXml->getChildWithTagNameIterator ("CHANNEL"))
        {
            const int c = e->getIntAttribute ("index", -1);
            if (! juce::isPositiveAndBelow (c, kNumChannels)) continue;

            auto& info = project.channels[(size_t) c];
            info.insert = juce::jlimit (0, kNumInserts - 1, e->getIntAttribute ("insert", c + 1));
            info.rackNote  = juce::jlimit (0, 127, e->getIntAttribute ("rackNote", 60));
            info.rackTrack = e->getIntAttribute ("rackTrack", -1);
            engine.setChannelInsert (c, info.insert);

            info.splitBuses = e->getBoolAttribute ("splitBuses", false);
            info.busInsert.fill (0);

            for (auto* bus : e->getChildWithTagNameIterator ("BUS"))
            {
                const int b = bus->getIntAttribute ("index", -1);
                if (juce::isPositiveAndBelow (b, kMaxOutBuses))
                    info.busInsert[(size_t) b] =
                        juce::jlimit (0, kNumInserts - 1, bus->getIntAttribute ("insert", 0));
            }

            if (auto* hosted = e->getChildByName (hostedTag))
            {
                juce::String error;
                if (auto plugin = readPlugin (*hosted, factory, error))
                {
                    info.name = e->getStringAttribute ("name", pluginName (*hosted));
                    engine.setChannelPlugin (c, std::move (plugin));
                    if (e->hasAttribute ("sumOutputs"))
                        engine.setChannelSumsOutputs (c, e->getBoolAttribute ("sumOutputs"));

                    // After the plugin, not before: loading one discovers the
                    // bus layout afresh and clears any routing already set.
                    for (int b = 0; b < kMaxOutBuses; ++b)
                        if (info.busInsert[(size_t) b] > 0)
                            engine.setBusInsert (c, b, info.busInsert[(size_t) b]);

                    engine.setChannelSplitsBuses (c, info.splitBuses);
                }
                else
                {
                    info.name = "(missing) " + pluginName (*hosted);
                    info.missingPlugin = std::make_shared<juce::XmlElement> (*hosted);
                    report.missingPlugins.add (pluginName (*hosted) + " (channel " + juce::String (c + 1) + "): " + error);
                }
            }
        }
    }

    // ---- samples ----
    std::map<int, std::shared_ptr<SampleData>> samples;
    if (auto* samplesXml = root->getChildByName ("SAMPLES"))
    {
        for (auto* e : samplesXml->getChildWithTagNameIterator ("SAMPLE"))
        {
            const int id = e->getIntAttribute ("id");
            const juce::File saved (e->getStringAttribute ("path"));
            const auto name = e->getStringAttribute ("name", saved.getFileNameWithoutExtension());

            std::shared_ptr<SampleData> sample;
            const auto found = resolveSample (*e, projectDir);
            if (found.existsAsFile())
            {
                juce::String error;
                sample = cache.load (found, error);
            }
            if (sample == nullptr)
            {
                sample = cache.placeholder (saved, name, e->getDoubleAttribute ("seconds", 1.0));
                report.missingSamples.add (saved.getFullPathName());
            }
            samples[id] = sample;
        }
    }

    // ---- clips ----
    if (auto* clipsXml = root->getChildByName ("CLIPS"))
    {
        for (auto* e : clipsXml->getChildWithTagNameIterator ("CLIP"))
        {
            Clip clip;
            const auto typeWord = e->getStringAttribute ("type");
            clip.type   = typeWord == "midi"       ? ClipType::midi
                        : typeWord == "automation" ? ClipType::automation
                                                   : ClipType::audio;
            clip.track  = juce::jlimit (0, (int) project.tracks.size() - 1, e->getIntAttribute ("track"));
            clip.start  = std::max (0.0, e->getDoubleAttribute ("start"));
            clip.offset = std::max (0.0, e->getDoubleAttribute ("offset"));
            clip.length = std::max (0.0, e->getDoubleAttribute ("length"));
            clip.gainDb = (float) e->getDoubleAttribute ("gain");
            clip.muted  = e->getBoolAttribute ("muted");

            // Negative is refused rather than clamped into meaning, because a
            // negative length is not a short fade: `ClipFade::resolve` reads
            // anything at or below zero as "this edge has no fade of its own"
            // and hands it the automatic one, which is the right answer for a
            // file that has written nonsense here.
            clip.fadeIn       = std::max (0.0, e->getDoubleAttribute ("fadeIn"));
            clip.fadeOut      = std::max (0.0, e->getDoubleAttribute ("fadeOut"));
            clip.fadeInShape  = ClipFade::shapeFromId (e->getIntAttribute ("fadeInShape",
                                                       ClipFade::idOfShape (ClipFade::defaultShape)));
            clip.fadeOutShape = ClipFade::shapeFromId (e->getIntAttribute ("fadeOutShape",
                                                       ClipFade::idOfShape (ClipFade::defaultShape)));

            if (clip.isAudio())
            {
                // A take folder has no sample of its own: its audio is in its
                // takes. Skipping the clip for want of one would drop the
                // whole folder and everything comped from it.
                auto* takesXml = e->getChildByName ("TAKES");
                auto  it = samples.find (e->getIntAttribute ("sample"));
                if (it == samples.end() && takesXml == nullptr)
                    continue;
                if (it != samples.end())
                    clip.sample = it->second;
                clip.stretch = juce::jlimit (0.1, 10.0, e->getDoubleAttribute ("stretch", 1.0));
                clip.sourceBpm = juce::jlimit (0.0, 400.0, e->getDoubleAttribute ("sourceBpm", 0.0));
                clip.followTempo = e->getBoolAttribute ("followTempo", false);
                clip.pitch   = juce::jlimit (-24.0, 24.0, e->getDoubleAttribute ("pitch", 0.0));

                // Markers are normalised on the way in rather than trusted, so
                // a hand edited or truncated project cannot produce a map that
                // reads backwards.
                if (auto* warp = e->getChildByName ("WARP"))
                    clip.warp = warpFromText (warp->getAllSubText());
                clip.tidyWarp();

                if (takesXml != nullptr)
                {
                    auto folder = std::make_shared<TakeFolder>();
                    folder->crossfadeMs = juce::jlimit (0.0, 250.0,
                                                        takesXml->getDoubleAttribute ("crossfadeMs", 10.0));

                    for (auto* t : takesXml->getChildWithTagNameIterator ("TAKE"))
                    {
                        Take take;
                        take.name   = t->getStringAttribute ("name");
                        take.start  = std::max (0.0, t->getDoubleAttribute ("start"));
                        take.offset = std::max (0.0, t->getDoubleAttribute ("offset"));
                        take.length = std::max (0.0, t->getDoubleAttribute ("length"));

                        // A take whose audio could not be found still has a
                        // placeholder behind it, exactly as a missing clip
                        // does, so the comp survives the file coming back.
                        if (auto found = samples.find (t->getIntAttribute ("sample")); found != samples.end())
                            take.sample = found->second;
                        if (take.sample == nullptr)
                            continue;

                        if (take.name.isEmpty())
                            take.name = "Take " + juce::String (folder->size() + 1);

                        folder->takes.push_back (std::move (take));

                        if ((size_t) folder->size() >= kMaxCompTakes)
                            break;
                    }

                    if (! folder->takes.empty())
                    {
                        folder->comp = compFromText (takesXml->getStringAttribute ("comp"));
                        clip.folder  = std::move (folder);
                        clip.sample.reset();      // a folder reads its takes

                        // Through the same function every other path to a
                        // folder goes through, rather than repeating the part
                        // of it that sizes the clip. This loader was
                        // repeating three of its lines and leaving out the
                        // ones that turn warping, stretching and tempo
                        // following off, so a file that carried both a comp
                        // and a set of warp markers loaded as a folder with
                        // live markers on it. This program writes no such
                        // file, but a hand edited or hand merged one is
                        // exactly what the normalising here exists for, and
                        // the renderer deliberately does not guard against
                        // it: see Project::tidyFolder and CLAUDE.md.
                        project.tidyFolder (clip);
                    }
                }
            }
            else if (clip.isAutomation())
            {
                clip.curve = std::make_shared<AutoCurve>();
                if (auto* a = e->getChildByName ("AUTO"))
                {
                    clip.curve->target = readTarget (*a);
                    clip.curve->points = curveFromText (a->getAllSubText());

                    // A curve aimed at a fader is stored as a position, and
                    // what a position means changed at version 7, so every
                    // point in an older one is translated. Translating the
                    // fader and leaving the curves would leave every drawn
                    // fade pointing somewhere else: a curve sitting at unity
                    // would play back at -4.40 dB, which is the same fault as an
                    // untranslated fader and harder to notice, because it is
                    // only audible where the automation is.
                    //
                    // What this preserves is each point, not the line between
                    // two of them. The two laws interpolate differently, so a
                    // migrated fade keeps its ends and changes shape in
                    // between: nothing worth hearing over a fade of 20 dB or
                    // less, and 13.6 dB at worst over one of 60 dB. FaderLaw.h
                    // has the table and the reason subdividing the segment was
                    // not done here.
                    //
                    // Done here rather than in curveFromText because only
                    // this one target kind means a fader position: a pan curve
                    // and a plugin parameter curve are unaffected, and a
                    // translation applied to all of them would quietly bend
                    // every automation lane in the project.
                    if (fileVersion < 7 && clip.curve->target.kind == AutoTargetKind::insertVolume)
                        for (auto& point : clip.curve->points)
                            point.value = FaderLaw::legacyCurveValueToPosition (point.value);
                }

                // Normalised on the way in rather than trusted, so a hand
                // edited or truncated project cannot hand the audio thread a
                // list that is out of order or out of range.
                clip.curve->tidy();

                // A curve with nothing in it reads as nothing automated, which
                // is a clip whose effect cannot be seen. One point at the
                // bottom is at least honest about what it is doing.
                if (clip.curve->points.empty())
                    clip.curve->points.push_back ({ 0.0, 0.0, 0.0 });
            }
            else
            {
                clip.channel = juce::jlimit (0, kNumChannels - 1, e->getIntAttribute ("channel"));
                clip.pattern = std::make_shared<MidiPattern>();
                if (auto* notes = e->getChildByName ("NOTES"))
                    clip.pattern->notes = notesFromText (notes->getAllSubText());
                for (auto* l : e->getChildWithTagNameIterator ("LANE"))
                {
                    AutoLane lane;
                    lane.paramIndex = l->getIntAttribute ("param");
                    lane.name       = l->getStringAttribute ("name");
                    lane.points     = pointsFromText (l->getAllSubText());
                    lane.sort();
                    clip.pattern->lanes.push_back (std::move (lane));
                }
            }
            project.addClip (clip);
        }
    }

    project.selection.clear();
    project.resetHistory();
    project.changed();
    return juce::Result::ok();
}

// ---------------------------------------------------------------------------

// Every piece of audio a project points at, clips and takes alike. Both of
// the functions below used to walk the clips only, which would have reported
// nine of a folder's ten passes as present and relinked none of them.
namespace
{
    template <typename ProjectRef, typename Fn>
    void forEachHeldSample (ProjectRef& project, Fn&& fn)
    {
        for (auto& c : project.clips)
        {
            if (c.folder != nullptr)
                for (auto& take : c.folder->takes)
                    if (take.sample != nullptr)
                        fn (take.sample);

            if (c.isAudio() && c.sample != nullptr)
                fn (c.sample);
        }
    }
}

juce::StringArray ProjectIO::missingSampleNames (const Project& project)
{
    juce::StringArray names;
    forEachHeldSample (project, [&names] (auto& s)
    {
        if (s->isMissing())
            names.addIfNotAlreadyThere (s->file.getFileName());
    });
    return names;
}

int ProjectIO::relinkMissing (Project& project, SampleCache& cache, const juce::File& folder)
{
    std::map<SampleData*, std::shared_ptr<SampleData>> replacements;
    forEachHeldSample (project, [&] (auto& s)
    {
        if (! s->isMissing() || replacements.count (s.get()))
            return;

        std::shared_ptr<SampleData> found;
        const auto f = findByName (folder, s->file.getFileName());
        if (f.existsAsFile())
        {
            juce::String error;
            found = cache.load (f, error);
        }
        replacements[s.get()] = found;
    });

    int count = 0;
    for (auto& [missing, found] : replacements)
        if (found != nullptr)
            ++count;

    forEachHeldSample (project, [&replacements] (auto& s)
    {
        if (auto it = replacements.find (s.get()); it != replacements.end() && it->second != nullptr)
            s = it->second;
    });

    if (count > 0)
        project.changed();
    return count;
}
