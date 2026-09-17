#pragma once
#include <juce_gui_basics/juce_gui_basics.h>

// Implemented in DarkTitleBarWin.cpp. Does nothing outside Windows.
void ahpSetDarkTitleBarNative (void* nativeWindowHandle);

namespace Ahp
{
    // Makes a native Windows title bar dark to match the app.
    // Call after the window is visible.
    inline void applyDarkTitleBar (juce::Component& window)
    {
        if (auto* peer = window.getPeer())
            ahpSetDarkTitleBarNative (peer->getNativeHandle());
    }
}
