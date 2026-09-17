#include "ProjectIO.h"

namespace
{
    const juce::String rootTag  { "ALLHAILPAN_PROJECT" };
    const juce::String hostedTag { "HOSTED" };
    constexpr int formatVersion = 1;

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
        ctl.volume.store (0.8f);
        ctl.pan.store (0.0f);
        ctl.mute.store (false);
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
    for (int i = 0; i < kNumInserts; ++i)
    {
        auto& ctl = engine.insert (i);
        auto* e = mixerXml->createNewChildElement ("INSERT");
        e->setAttribute ("index", i);
        e->setAttribute ("name", ctl.name);
        e->setAttribute ("volume", (double) ctl.volume.load());
        e->setAttribute ("pan", (double) ctl.pan.load());
        e->setAttribute ("mute", ctl.mute.load());

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
        e->setAttribute ("sumOutputs", engine.getChannelSumsOutputs (c));
        writePlugin (*e, engine.getChannelPlugin (c), info.missingPlugin);
    }

    // ---- samples ----
    std::map<const SampleData*, int> sampleIds;
    const auto samplesDir = projectDir.getChildFile (file.getFileNameWithoutExtension() + " Samples");
    auto* samplesXml = root.createNewChildElement ("SAMPLES");

    for (auto& clip : project.clips)
    {
        if (! clip.isAudio() || clip.sample == nullptr || sampleIds.count (clip.sample.get()))
            continue;

        auto& sample = *clip.sample;
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
        sampleIds[clip.sample.get()] = id;

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
        e->setAttribute ("type", clip.isAudio() ? "audio" : "midi");
        e->setAttribute ("track", clip.track);
        e->setAttribute ("start", clip.start);
        e->setAttribute ("offset", clip.offset);
        e->setAttribute ("length", clip.length);
        e->setAttribute ("gain", (double) clip.gainDb);
        e->setAttribute ("muted", clip.muted);

        if (clip.isAudio())
        {
            e->setAttribute ("sample", clip.sample != nullptr ? sampleIds[clip.sample.get()] : 0);
            e->setAttribute ("stretch", clip.stretch);
            e->setAttribute ("pitch", clip.pitch);
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

    const auto projectDir = file.getParentDirectory();

    resetEngine (engine);
    project.clearAll();

    report.bpm       = root->getDoubleAttribute ("bpm", 128.0);
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
        for (auto* e : mixerXml->getChildWithTagNameIterator ("INSERT"))
        {
            const int i = e->getIntAttribute ("index", -1);
            if (! juce::isPositiveAndBelow (i, kNumInserts)) continue;

            auto& ctl = engine.insert (i);
            ctl.name = e->getStringAttribute ("name", ctl.name);
            ctl.volume.store ((float) juce::jlimit (0.0, 1.25, e->getDoubleAttribute ("volume", 0.8)));
            ctl.pan.store ((float) juce::jlimit (-1.0, 1.0, e->getDoubleAttribute ("pan", 0.0)));
            ctl.mute.store (e->getBoolAttribute ("mute"));

            for (auto* fx : e->getChildWithTagNameIterator ("FX"))
            {
                const int k = fx->getIntAttribute ("slot", -1);
                auto* hosted = fx->getChildByName (hostedTag);
                if (! juce::isPositiveAndBelow (k, kNumFxSlots) || hosted == nullptr) continue;

                juce::String error;
                if (auto plugin = readPlugin (*hosted, factory, error))
                {
                    engine.setFx (i, k, std::move (plugin));
                    ctl.bypass[(size_t) k].store (fx->getBoolAttribute ("bypass"));
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
            engine.setChannelInsert (c, info.insert);

            if (auto* hosted = e->getChildByName (hostedTag))
            {
                juce::String error;
                if (auto plugin = readPlugin (*hosted, factory, error))
                {
                    info.name = e->getStringAttribute ("name", pluginName (*hosted));
                    engine.setChannelPlugin (c, std::move (plugin));
                    if (e->hasAttribute ("sumOutputs"))
                        engine.setChannelSumsOutputs (c, e->getBoolAttribute ("sumOutputs"));
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
            clip.type   = e->getStringAttribute ("type") == "midi" ? ClipType::midi : ClipType::audio;
            clip.track  = juce::jlimit (0, (int) project.tracks.size() - 1, e->getIntAttribute ("track"));
            clip.start  = std::max (0.0, e->getDoubleAttribute ("start"));
            clip.offset = std::max (0.0, e->getDoubleAttribute ("offset"));
            clip.length = std::max (0.0, e->getDoubleAttribute ("length"));
            clip.gainDb = (float) e->getDoubleAttribute ("gain");
            clip.muted  = e->getBoolAttribute ("muted");

            if (clip.isAudio())
            {
                auto it = samples.find (e->getIntAttribute ("sample"));
                if (it == samples.end()) continue;
                clip.sample  = it->second;
                clip.stretch = juce::jlimit (0.1, 10.0, e->getDoubleAttribute ("stretch", 1.0));
                clip.pitch   = juce::jlimit (-24.0, 24.0, e->getDoubleAttribute ("pitch", 0.0));
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

juce::StringArray ProjectIO::missingSampleNames (const Project& project)
{
    juce::StringArray names;
    for (auto& c : project.clips)
        if (c.isAudio() && c.sample != nullptr && c.sample->isMissing())
            names.addIfNotAlreadyThere (c.sample->file.getFileName());
    return names;
}

int ProjectIO::relinkMissing (Project& project, SampleCache& cache, const juce::File& folder)
{
    std::map<SampleData*, std::shared_ptr<SampleData>> replacements;
    for (auto& c : project.clips)
    {
        if (! c.isAudio() || c.sample == nullptr || ! c.sample->isMissing() || replacements.count (c.sample.get()))
            continue;

        std::shared_ptr<SampleData> found;
        const auto f = findByName (folder, c.sample->file.getFileName());
        if (f.existsAsFile())
        {
            juce::String error;
            found = cache.load (f, error);
        }
        replacements[c.sample.get()] = found;
    }

    int count = 0;
    for (auto& [missing, found] : replacements)
        if (found != nullptr)
            ++count;

    for (auto& c : project.clips)
        if (c.isAudio() && c.sample != nullptr)
            if (auto it = replacements.find (c.sample.get()); it != replacements.end() && it->second != nullptr)
                c.sample = it->second;

    if (count > 0)
        project.changed();
    return count;
}
