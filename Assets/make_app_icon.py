#!/usr/bin/env python3
"""Builds the two app icon masters from ahp_logo.png.

Run it after changing the logo:

    python3 Assets/make_app_icon.py

Why this exists rather than one hand-exported PNG.

The mark is white line art on transparency, with a stroke 7 pixels wide on a
858 x 900 canvas. That is three separate problems for an app icon, and the
icon had all three.

White on transparency is invisible on a light background. Windows Explorer,
the light theme taskbar, Alt-Tab, the open and save dialogs and the installer
are all light, and a Win32 .ico carries one image with no light and dark
variants, unlike the UWP `altform-lightunplated` assets Microsoft document.
So the only way a white only mark can be seen on both themes is to put it on
its own background. Hence the black plate.

A 7 pixel stroke is a tenth of a pixel once the mark is 16 pixels tall, so it
vanishes. JUCE writes 16, 32, 48 and 256 into the .ico
(extras/Build/juce_build_tools/utils/juce_Icons.cpp, `writeWinIcon`), and with
only ICON_BIG set every one of those is a plain downscale of the one file, so
the three small entries were grey mush. `getBestIconForSize` in the same file
picks the smaller of the two drawables when both are at least the size asked
for, so a 48 pixel ICON_SMALL is what gets used for 16, 32 and 48, and the
1024 ICON_BIG is used for 256. Two masters is the most JUCE will take, and
this is the split that puts them where they help.

The small master is the mark's silhouette rather than its outline. Apple's
WWDC25 session 220, on the current icon design system, says to avoid thin
lines and use bolder line weights because that "will preserve details at a
smaller scale". Filling the outline is the strongest form of that: it keeps
the silhouette, which is what identifies the mark at a glance, and drops the
interior lines, which are gone at that size anyway. Checked by rendering 16,
32, 48 and 256 and looking at them, which is in the end the only test an icon
has.

Below about 32 pixels the mark is a bright shape rather than a readable
monogram. That is a limit of intricate line art at 16 pixels, not of this
script: making it legible there would mean drawing a simpler mark, which is a
brand decision rather than a build step.

Sources:
  Microsoft, "App icon construction":
    https://learn.microsoft.com/en-us/windows/apps/design/iconography/app-icon-construction
  Apple, WWDC25 session 220, "Say hello to the new look of app icons":
    https://developer.apple.com/videos/play/wwdc2025/220/
"""

import os
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
SOURCE = os.path.join(HERE, "ahp_logo.png")

# The big master is Apple's stated 1024 canvas and the largest size JUCE will
# write. 0.78 leaves the mark clear of the corners, which macOS rounds off.
BIG_SIZE, BIG_INSET = 1024, 0.78

# 48 so that JUCE uses it for 16, 32 and 48 and nothing larger. 0.86 because
# 0.78 wastes pixels at this size and 1.0 runs the spikes into the edge.
SMALL_SIZE, SMALL_INSET = 48, 0.86

PLATE = (0, 0, 0, 255)


def trimmed_alpha(image):
    """The mark's alpha, cropped to what is actually drawn.

    The source art runs right up to its canvas edge, so the padding below has
    to be measured from the ink rather than from the file.
    """
    alpha = image.getchannel("A")
    box = alpha.getbbox()
    return alpha.crop(box)


def filled(alpha):
    """The outline's silhouette: everything the stroke encloses, filled in.

    Flood fills the transparent background inward from all four edges, then
    takes what the flood did not reach. Anything enclosed by the stroke is
    unreachable from outside, so this fills the counters without needing to
    know where they are.
    """
    from PIL import ImageDraw

    # One pixel of margin so the flood always has a route around the mark,
    # even where the stroke touches the edge of the crop.
    w, h = alpha.size
    work = Image.new("L", (w + 2, h + 2), 0)
    work.paste(alpha, (1, 1))

    # 0 is background, 255 is ink. Flood the background with 128 from a corner.
    ImageDraw.floodfill(work, (0, 0), 128, thresh=127)

    # Ink, plus anything the flood could not reach, is the silhouette.
    out = work.point(lambda v: 0 if v == 128 else 255)
    return out.crop((1, 1, w + 1, h + 1))


def plate(alpha, size, inset):
    """The white mark centred on an opaque black square, aspect preserved."""
    w, h = alpha.size
    scale = min(size * inset / w, size * inset / h)
    new = (max(1, round(w * scale)), max(1, round(h * scale)))

    mark = Image.new("RGBA", new, (255, 255, 255, 255))
    mark.putalpha(alpha.resize(new, Image.LANCZOS))

    canvas = Image.new("RGBA", (size, size), PLATE)
    canvas.alpha_composite(mark, ((size - new[0]) // 2, (size - new[1]) // 2))
    return canvas


def main():
    source = Image.open(SOURCE).convert("RGBA")
    alpha = trimmed_alpha(source)

    big = plate(alpha, BIG_SIZE, BIG_INSET)
    big.save(os.path.join(HERE, "app_icon.png"))

    small = plate(filled(alpha), SMALL_SIZE, SMALL_INSET)
    small.save(os.path.join(HERE, "app_icon_small.png"))

    print("app_icon.png      %d x %d  outline" % big.size)
    print("app_icon_small.png %d x %d  silhouette" % small.size)


if __name__ == "__main__":
    main()
