#include <juce_gui_basics/juce_gui_basics.h>
#include "MainComponent.h"
#include "AhpLookAndFeel.h"
#include "DarkTitleBar.h"
#include "PluginScanner.h"

class AhpApplication : public juce::JUCEApplication
{
public:
    const juce::String getApplicationName() override    { return JUCE_APPLICATION_NAME_STRING; }
    const juce::String getApplicationVersion() override { return JUCE_APPLICATION_VERSION_STRING; }

    /** Only one studio at a time, but plugin scanning launches this same
        executable as a short lived child. Without this exception JUCE would
        hand the child's command line to the running studio and quit, and no
        plugin would ever be scanned. */
    bool moreThanOneInstanceAllowed() override
    {
        return getCommandLineParameters().contains (OutOfProcessScanner::scanFlag);
    }

    void initialise (const juce::String& commandLine) override
    {
        // Launched to examine a single plugin? Do that and exit, with no
        // window and no audio device. If the plugin crashes, only this
        // throwaway process dies.
        if (runScanIfRequested (commandLine))
        {
            quit();
            return;
        }

        juce::LookAndFeel::setDefaultLookAndFeel (&lookAndFeel);
        mainWindow = std::make_unique<MainWindow> (getApplicationName());
    }

    void shutdown() override
    {
        mainWindow = nullptr;
        juce::LookAndFeel::setDefaultLookAndFeel (nullptr);
    }

    void systemRequestedQuit() override
    {
        if (mainWindow != nullptr)
            if (auto* main = dynamic_cast<MainComponent*> (mainWindow->getContentComponent()))
            {
                main->requestQuit ([] { juce::JUCEApplication::getInstance()->quit(); });
                return;
            }
        quit();
    }

private:
    class MainWindow : public juce::DocumentWindow
    {
    public:
        explicit MainWindow (const juce::String& name)
            : DocumentWindow (name, Ahp::black, DocumentWindow::allButtons)
        {
            setUsingNativeTitleBar (true);
            setContentOwned (new MainComponent(), true);
            setResizable (true, true);
            setResizeLimits (900, 560, 10000, 10000);
            centreWithSize (getWidth(), getHeight());
            setVisible (true);
            Ahp::applyDarkTitleBar (*this);
        }

        void closeButtonPressed() override
        {
            juce::JUCEApplication::getInstance()->systemRequestedQuit();
        }

    private:
        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainWindow)
    };

    AhpLookAndFeel lookAndFeel;
    std::unique_ptr<MainWindow> mainWindow;
};

START_JUCE_APPLICATION (AhpApplication)
