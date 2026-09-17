#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include "AudioEngine.h"
#include "SampleData.h"
#include "AhpLookAndFeel.h"

// FL-style browser: pick a sample folder, click files to hear them,
// drag them into the playlist.
class BrowserPanel : public juce::Component,
                     private juce::FileBrowserListener
{
public:
    BrowserPanel (SampleCache& c, AudioEngine& e, juce::PropertiesFile& p)
        : cache (c), engine (e), settings (p)
    {
        scanThread.startThread();

        tree.setDragAndDropDescription ("ahp-sample");
        tree.addListener (this);
        tree.setColour (juce::TreeView::backgroundColourId, Ahp::panel);
        tree.setColour (juce::DirectoryContentsDisplayComponent::highlightColourId, Ahp::panel3);
        tree.setColour (juce::DirectoryContentsDisplayComponent::textColourId, Ahp::bone);
        tree.setColour (juce::DirectoryContentsDisplayComponent::highlightedTextColourId, Ahp::bone);
        tree.setItemHeight (22);
        tree.setMouseClickGrabsKeyboardFocus (false);
        tree.setWantsKeyboardFocus (false);
        addAndMakeVisible (tree);

        title.setText ("Browser", juce::dontSendNotification);
        title.setColour (juce::Label::textColourId, Ahp::muted);
        addAndMakeVisible (title);

        folderBox.onChange = [this] { showFolder (folderBox.getSelectedItemIndex()); };
        folderBox.setWantsKeyboardFocus (false);
        addAndMakeVisible (folderBox);

        addButton.onClick    = [this] { chooseFolder(); };
        removeButton.onClick = [this] { removeCurrentFolder(); };
        previewButton.setClickingTogglesState (true);
        previewButton.setToggleState (settings.getBoolValue ("browserPreview", true), juce::dontSendNotification);
        previewButton.onClick = [this]
        {
            settings.setValue ("browserPreview", previewButton.getToggleState());
            if (! previewButton.getToggleState())
                engine.stopPreview();
        };
        for (auto* b : { &addButton, &removeButton, &previewButton })
        {
            b->setWantsKeyboardFocus (false);
            addAndMakeVisible (b);
        }

        hint.setText ("Click to hear. Drag onto a playlist track.", juce::dontSendNotification);
        hint.setColour (juce::Label::textColourId, Ahp::muted);
        hint.setFont (juce::FontOptions (12.0f));
        addAndMakeVisible (hint);

        loadFolders();
    }

    ~BrowserPanel() override
    {
        tree.removeListener (this);
        scanThread.stopThread (2000);
    }

    static juce::File recordingsFolder()
    {
        return juce::File::getSpecialLocation (juce::File::userDocumentsDirectory)
                   .getChildFile ("ALLHAILPAN Studio").getChildFile ("Recordings");
    }

    void refresh() { contents.refresh(); }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (Ahp::panel);
        g.setColour (Ahp::line);
        g.fillRect (getLocalBounds().removeFromRight (1));
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (8, 6).withTrimmedRight (1);
        auto top = r.removeFromTop (24);
        previewButton.setBounds (top.removeFromRight (70));
        title.setBounds (top);
        r.removeFromTop (4);
        folderBox.setBounds (r.removeFromTop (26));
        r.removeFromTop (4);
        auto row = r.removeFromTop (26);
        addButton.setBounds (row.removeFromLeft (row.getWidth() / 2 - 2));
        row.removeFromLeft (4);
        removeButton.setBounds (row);
        r.removeFromTop (6);
        hint.setBounds (r.removeFromBottom (20));
        tree.setBounds (r);
    }

private:
    // ---- folders ----
    void loadFolders()
    {
        folders.clear();
        folders.addTokens (settings.getValue ("browserFolders"), "|", "");
        folders.removeEmptyStrings();

        if (folders.isEmpty())
        {
            auto recordings = recordingsFolder();
            recordings.createDirectory();
            folders.add (recordings.getFullPathName());

            auto music = juce::File::getSpecialLocation (juce::File::userMusicDirectory);
            if (music.isDirectory())
                folders.add (music.getFullPathName());

            // FL Studio's bundled packs, if installed
            juce::File imageLine ("C:\\Program Files\\Image-Line");
            if (imageLine.isDirectory())
                for (auto& dir : imageLine.findChildFiles (juce::File::findDirectories, false, "FL Studio*"))
                {
                    auto packs = dir.getChildFile ("Data").getChildFile ("Patches").getChildFile ("Packs");
                    if (packs.isDirectory())
                        folders.add (packs.getFullPathName());
                }

            saveFolders();
        }

        rebuildFolderBox (0);
    }

    void saveFolders()
    {
        settings.setValue ("browserFolders", folders.joinIntoString ("|"));
        settings.saveIfNeeded();
    }

    void rebuildFolderBox (int indexToShow)
    {
        folderBox.clear (juce::dontSendNotification);
        for (int i = 0; i < folders.size(); ++i)
        {
            juce::File f (folders[i]);
            folderBox.addItem (f.getFileName() + "   " + f.getParentDirectory().getFullPathName(), i + 1);
        }
        if (folders.size() > 0)
        {
            folderBox.setSelectedItemIndex (juce::jlimit (0, folders.size() - 1, indexToShow), juce::dontSendNotification);
            showFolder (folderBox.getSelectedItemIndex());
        }
        removeButton.setEnabled (folders.size() > 0);
    }

    void showFolder (int index)
    {
        if (juce::isPositiveAndBelow (index, folders.size()))
            contents.setDirectory (juce::File (folders[index]), true, true);
    }

    void chooseFolder()
    {
        chooser = std::make_unique<juce::FileChooser> ("Add a sample folder",
                                                       juce::File::getSpecialLocation (juce::File::userMusicDirectory));
        chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
                              [this] (const juce::FileChooser& fc)
                              {
                                  const auto dir = fc.getResult();
                                  if (! dir.isDirectory())
                                      return;
                                  folders.addIfNotAlreadyThere (dir.getFullPathName());
                                  saveFolders();
                                  rebuildFolderBox (folders.indexOf (dir.getFullPathName()));
                              });
    }

    void removeCurrentFolder()
    {
        const int index = folderBox.getSelectedItemIndex();
        if (! juce::isPositiveAndBelow (index, folders.size()))
            return;
        folders.remove (index);
        saveFolders();
        rebuildFolderBox (index - 1);
    }

    // ---- FileBrowserListener ----
    void selectionChanged() override {}
    void browserRootChanged (const juce::File&) override {}
    void fileDoubleClicked (const juce::File&) override {}

    void fileClicked (const juce::File& file, const juce::MouseEvent&) override
    {
        if (! previewButton.getToggleState() || ! cache.isAudioFile (file))
            return;

        juce::String error;
        if (auto sample = cache.load (file, error))
            engine.preview (sample);
    }

    SampleCache&          cache;
    AudioEngine&          engine;
    juce::PropertiesFile& settings;

    juce::TimeSliceThread       scanThread { "ALLHAILPAN browser" };
    juce::WildcardFileFilter    filter { "*.wav;*.aif;*.aiff;*.flac;*.ogg;*.mp3", "*", "Audio files" };
    juce::DirectoryContentsList contents { &filter, scanThread };
    juce::FileTreeComponent     tree { contents };

    juce::Label      title, hint;
    juce::ComboBox   folderBox;
    juce::TextButton addButton { "Add folder" }, removeButton { "Remove" }, previewButton { "Preview" };
    juce::StringArray folders;
    std::unique_ptr<juce::FileChooser> chooser;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (BrowserPanel)
};
