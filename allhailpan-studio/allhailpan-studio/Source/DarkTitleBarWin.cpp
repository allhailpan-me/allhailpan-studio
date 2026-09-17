// Kept separate from JUCE headers so windows.h never collides with JUCE names.

#ifdef _WIN32
 #ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
 #endif
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #include <windows.h>
 #include <dwmapi.h>

void ahpSetDarkTitleBarNative (void* nativeWindowHandle)
{
    if (nativeWindowHandle == nullptr)
        return;

    const auto hwnd = static_cast<HWND> (nativeWindowHandle);

    // 20 = DWMWA_USE_IMMERSIVE_DARK_MODE (Windows 10 2004 and later, Windows 11)
    BOOL dark = TRUE;
    DwmSetWindowAttribute (hwnd, static_cast<DWMWINDOWATTRIBUTE> (20), &dark, sizeof (dark));

    // 35 = DWMWA_CAPTION_COLOR (Windows 11). Ignored on Windows 10.
    COLORREF black = RGB (0, 0, 0);
    DwmSetWindowAttribute (hwnd, static_cast<DWMWINDOWATTRIBUTE> (35), &black, sizeof (black));
}

#else

void ahpSetDarkTitleBarNative (void*) {}

#endif
