You can lift the feel off a part you played and put it on a part you programmed: play a bar of hats the way it should sit, press Feel from a part in the channel rack, and what you step in from then on sits that way too, with the accents as well if you want them. The studio has a time signature at last: type 3/4 or 6/8 into the Sig field next to the tempo and every bar line, click, snap and readout follows it, without moving a note you have already played. You can punch in now too: loop the line you want to redo, press Punch, and the new performance replaces that line and nothing else, with your original kept underneath to go back to. There is a tuner too, on Ctrl+T, so tuning a guitar no longer means reaching for something that is not this studio. You can pick up more than one note in the piano roll at last: drag a rectangle round a chord or a riff and move, transpose, stretch or duplicate the lot in one gesture, and a group always keeps its shape. You can tidy a part you played in too: there is a Quantise button on the piano roll, with a strength so it does not have to be an all or nothing snap. Your faders have a real scale, with 0 dB three quarters of the way up and silence at the bottom, and every mix you have saved comes back at the level you left it. Exports can be 16 bit now, dithered properly, so a file you send somewhere is not a conversion done in another program. PAN One has forty five new presets and can grow new sounds out of the one you are playing. Panning a mono track no longer makes it quieter, takes now land where you played them, which they did not quite before, and the app icon is visible again. Below that, every mixer strip taking its own input and all of them recording at once, a mono input heard in the middle, and ASIO, which is what makes an audio interface playable on Windows at all. Then the instrument itself, two fixes found by playing v0.3.0, and everything that went into v0.3.0.

## Since v0.7.0

**You can take the feel off something you played and put it on something you
programmed.** Play a bar of hi-hats or a bassline in by hand, the way it
actually sits. Then open the channel rack, press **Feel from a part**, and pick
that clip out of the list. Whatever you step in from then on sits the way you
played, rather than the way the grid says.

The swing control has been there a while and it does a different job. Swing is
one number: every offbeat moves by the same amount, which is what a shuffle is
and is not what a person does. A person is late on some notes and early on
others, leans on the backbeat, and barely touches the hat before a fill. There
is no percentage that describes that. The only thing that describes it is the
performance, which is why every studio that takes this seriously lets you lift
it off a part and apply it somewhere else.

**What it takes is the timing and the accents, and the accents are on the Feel
control.** With Feel on Tight you get the timing alone. On Played or Loose you
get how hard you hit each step as well, measured against your own average, so a
part you played at one steady level does not change anything and the one where
you leaned on the two and the four does. Turning it off is **No feel**, which
puts you back on the swing you had.

**It adds your feel to what is already there rather than straightening the
part onto it.** Ableton works this way and Pro Tools, Cubase, Logic and
Cakewalk work the other way, where the part gets dragged onto the donor's
positions. Theirs is a quantiser with a crooked grid: it throws away whatever
timing the part already had. If you do want that, quantise first and then
apply the feel, which comes to the same thing and is your decision rather than
one made for you.

I will be honest about how much of that you can hear today, because it reads
as though a lot turns on it. For a note sitting exactly on the grid the two
come to the same answer, so the difference only shows on a note that is off
the grid, and the next paragraph but one explains that those are left alone
anyway. So what actually keeps a part you played from being straightened is
that rule, not this one. This is the arithmetic being right so that it stays
right when that rule becomes a control.

**It is read at the step resolution the rack is showing**, so a feel taken at
sixteenths moves sixteenths. It is saved in the project, it is one undo, and it
is in a bounce exactly as you heard it, because the groove is worked out at the
same point for playback and for export.

**Two things I would rather tell you than have you find.** It only moves notes
that are sitting on the grid, within about two per cent of a step, which is
what keeps a feel from being applied to the very part you read it from. On
something you played in by hand that means most notes are left alone, so this
is for parts you stepped in rather than a way to re-feel a live take. And the
feel is one per song rather than one per clip, which is where the swing already
lived. Both of those are the next piece of work on this rather than oversights.

**The studio has a time signature.** There is a Sig field next to the tempo in
the transport bar: type 3/4, or 6/8, or 7/8, or 12/8, and the whole studio
follows it. Bar lines move to where the bars actually are, in the playlist and
in the piano roll. The click counts the bar properly and accents the first beat
of it. Snapping to a bar snaps to a bar. The position readout counts beats the
way the signature does, so 6/8 reads 1:1 through 1:6. The song loops at the end
of a real bar. A count-in of two bars counts two bars. A MIDI file you export
says what the piece is in, and one you drop in brings its own signature with it.

Before this, a song in anything but 4/4 would play, because playing never asks
what a bar is, and could not be written: every bar line was four beats apart,
the click accented every fourth beat, and the ruler numbered bars that were not
there. If you write in 3/4 or 6/8, the studio was no use to you.

**The tempo still means quarter notes per minute, and that is the whole thing
this turns on.** In 6/8 at 120, a bar is a second and a half, not three
seconds. That is what Logic says in as many words, it is what Steinberg's own
plugin interface assumes, and it is what Ardour's tempo defaults to. The useful
consequence for you: **changing the signature does not move a single note.** It
moves the lines you see and the clicks you hear, and nothing else. So you can
try 6/8 on something you wrote in 4/4 and change your mind, and nothing has
happened to the music.

**One signature for the whole project for now.** Every other studio lets it
change part way through a song, and this one will, but that wants a night of its
own. What is here is the part that was missing entirely.

**Two things that are not finished, and I would rather say so.** The channel
rack still works in bars of four beats, so in 6/8 its grid will not line up with
the playlist's. I built it to follow the signature, found three separate ways
that lost a pattern you had already drawn, and took it back out rather than
ship any of them. And if you drop two MIDI files in together that are in
different signatures, the clips from the first one get sized to the wrong bar.

**One thing changed in 4/4 as well, so it is worth knowing about.** The piano
roll used to draw its step grid however far out you zoomed, which meant that
past a certain point the lines were two pixels apart and read as a grey wash
rather than as a grid. It now stops drawing them when they get that close and
leaves the beat lines, so zoomed well out you see beats instead of a smear.
Snapping is untouched: if you have snap set to a sixteenth you still get
sixteenth lines wherever you got them before.

**It also fixed something that had quietly been wrong in every MIDI file this
studio has ever written.** Two bytes of the time signature message, the ones
that say how often the metronome clicks and how a quarter note is notated, were
coming out as nonsense because that is what JUCE's own helper puts there. Nothing
reads them in practice, which is why nobody noticed, and that is not a reason to
keep writing them.

**You can punch in now.** Drag a loop range over the line you want to redo,
press Punch next to the Click button, arm the track the part is on, and record.
The new performance replaces what is inside that range and nothing else. The
rest of the take is exactly as you left it.

This is the thing the studio most obviously could not do. Before tonight,
recording over a part you had already played put the new take on top of the old
one and you heard both at once, at full level, which is no use to anybody and is
not what any other studio does. So if one line of a verse was wrong, the only
way out was to sing or play the whole verse again and hope the rest of it came
out as well as it did the first time.

**It rolls in a bar before the punch**, so you get to play into it rather than
stab at the first note cold. You hear what is already on the track up to the
point the studio takes over, and because the recorder is running for that whole
bar, the join at the punch has real audio either side of it rather than a hard
cut.

**Nothing is destroyed.** The clip turns into a take folder with your original
performance as take 1 and the punch as take 2, and the join is a short
crossfade. So you can drag across a lane afterwards to move the join a bit
earlier or later, or drag back across the whole clip to put the original
performance back as if you had never punched. The audio files on disk are not
touched either way. And because this is the same comping the studio already had,
a bounce gets the punch exactly as you heard it.

**Where the crossfades go took reading the manuals rather than guessing**, and
it is the detail that decides whether this is any good. Pro Tools puts the fade
in the old material, finishing right at the punch point rather than straddling
it, so the take you just punched in is at full level from its first sample. The
obvious way to write it, with the fade centred on the join, takes a few
milliseconds off the front of the new part, and the front of the new part is
usually the exact thing you punched in to fix. Avid's own advice is to crossfade
without removing essential audio, and that is why.

A few things worth knowing, because I would rather say them than have you find
them:

- **A punch does not loop.** With Punch on, recording is one pass. Going round
  a loop would mean every pass after the first started exactly on the punch
  point with nothing to lead in from, and the join would suffer for it. Pro
  Tools takes the same view. Doing it properly needs a lead in for every pass
  and is on the list.
- **It will not punch into a clip that is warped, stretched or already comped**,
  and it says so rather than doing something odd. In both cases punching would
  change how the part you did not punch sounds, which is the opposite of the
  promise. Your take still lands on the track, above the old one, so nothing you
  played is lost.
- **Stop early and you keep what you did not punch.** If you drop out halfway
  through the range, the rest of the range stays as it was rather than going
  silent.
- **Punch is saved with the song**, along with the loop range it is taken from,
  so a song you were part way through fixing opens ready to carry on. That also
  means this version writes a project file the previous version will not open.
- Tracking a band with several inputs at once, only the first track punches; the
  others land as ordinary takes.

**And one thing I got wrong and caught before it shipped**, because it is the
kind of thing worth being honest about. The first working version placed the
punched take by measuring from where the transport started rolling instead of
from the start of the clip it was punching into. The two are the same number for
an ordinary recording, so everything looked right. For a punch they are a bar or
more apart, and the result was that the bars you punched went silent, in the
mix and in the bounce, while the studio told you the rest of the take was
untouched. Nothing errored. An audit found it, the arithmetic has moved
somewhere it can be tested, and there is now a check that a note played at a
beat ends up at that beat.

**There is a tuner.** Ctrl+T, or File > Tuner. Play one note and it tells you
which note it is and how many cents sharp or flat, with a needle that fills its
centre when you are in tune.

It listens to whatever your lowest mixer strip has an input wired to, so if your
guitar is already on a strip there is nothing to set up. If you want it on a
different socket, pick one from the menu at the top. If you have not got as far
as the mixer yet it listens to input 1, so it works on a brand new project,
which is when you are most likely to want it. There is an A4 reference too, from
410 to 480 Hz in tenths of a hertz, for anybody working away from 440; it stays
where you put it and belongs to the studio rather than to one song, because a
tuning reference is about the instruments in the room.

**It waits about one and three quarter seconds before it shows you a number,
and that is the whole point of it.** A plucked steel string is sharp when you hit it. Not
slightly: it falls 12.5 cents while it rings, with most of that gone in the
first half second, and it was measured on an acoustic guitar's open low E at a
conference paper rather than guessed at here. Twelve and a half cents is twelve
times the accuracy every tuner on the market claims. So a tuner that answers you
straight away answers you with the attack, reads sharp on every string, and
disagrees with the pedal on your floor.

What this does instead is watch the pitch stop moving. It shows you nothing
until the note has settled, and then it is inside a cent of where the string is
actually going to sit. That wait is not the software being slow: it is how long
your string takes. It is also, for what it is worth, about how long a tuning
pedal appears to take before it makes up its mind.

A couple of consequences of that worth knowing. Pluck gently, with a thumb
rather than a pick, and it settles sooner. And it will say nothing at all rather
than show you a wrong number: if it is reading dashes it has not heard a note it
can trust yet, and the line underneath says whether that is silence, no input,
or a note still settling.

**Two things it is worth me being straight about, both found by auditing it
after it had already gone in.**

It can be fooled by a deliberate vibrato. If you wobble a note at roughly the
rate it takes readings, several readings in a row can land on the same point of
the wobble, agree with each other, and give you a confident number up to about
five cents out. That is now measured and written down rather than waiting to be
discovered, and it is a good deal better than it was: before this it could be
nineteen cents out, which is a fifth of a semitone, and it would have looked
exactly as settled. Hold the note steady and it is accurate to a fraction of a
cent. Wobble it and do not trust the last digit.

And it is noticeably slower at 96 and 192 kHz, because the analysis costs four
times as much work every time the sample rate doubles. At 44.1 and 48 kHz it
takes about eight readings a second and settles in under two. At 192 kHz it
takes about one a second, wants five or six before it will commit, and the
vibrato problem above is worse there. If you are tuning, 44.1 or 48 is the
setting to be in, which is where most of us record anyway. Making that
difference go away is the next piece of work on it.

**What it covers.** Everything from a five string bass tuned down to drop A, up
past the top of a guitar's range. The low end is the same figure Korg design
their own tuners around rather than a round number picked for headroom, and the
range deliberately goes below the lowest note it names, because a tuner that
cannot tell you your low B is flat is no use for the one thing you are looking
at it for. Above the range it says nothing, which took finding: it used to read
a pinch harmonic or a whistle as an exact octave down, confidently.

It reads the note you are playing rather than the loudest thing in the sound,
which matters more than it sounds. On the bottom four strings of a guitar and
everywhere on a bass, the strongest partial is often not the fundamental, and
the fundamental can be missing altogether. Pick the loudest peak out of the
sound and you get an octave up. This finds the repeat in the waveform instead,
which is where the pitch is whether or not there is any energy at that
frequency.

**You can pick up more than one note at a time.** Until now the piano roll
edited exactly one note per gesture. There was no way to grab a chord, move a
riff a sixteenth later, or transpose eight notes together: you did it one note
at a time, eight times, and hoped you had not nudged one of them sideways on
the way.

There is a Select tool next to Draw and Delete. Drag a rectangle round the
notes you want, or hold Ctrl (Command on a Mac) and drag to do the same thing
without leaving the Draw tool. The rectangle catches anything it touches
rather than only what it surrounds, which is what every studio that says
anything about it does, so you do not have to lasso a long note from end to
end. Shift and a click adds a note to the selection or takes it out again,
Shift over a rectangle flips whatever it catches, and clicking a note that is
already selected picks up the whole group rather than throwing the rest away.

Drag the group and it moves as a group. That is the part that took the most
care, and it is worth knowing what it protects you from. If a chord is near
the top of the keyboard and you drag it up too far, every note stops together
and the chord stays a chord. The obvious way to write it lets the top note
stop while the others carry on, which turns a chord into a unison, makes no
sound you would notice at the time, and leaves you finding it later by ear
with no idea which gesture did it. The same rule runs through transposing, the
velocity and the lengths: nothing in a group ever moves without the rest of
it.

A Selection button holds everything that is not a gesture: select all, none or
invert, transpose by a semitone or an octave, nudge the velocity up or down,
duplicate, and delete. Duplicate puts the copy immediately after what you
selected and then selects the copy, so pressing it three times lays three
copies end to end instead of piling them in one place.

Drag a velocity marker belonging to a selected note and the whole selection
moves together, keeping the differences between them, so a ramp you played
stays a ramp instead of flattening the moment the loudest note hits the top.
Painting a row of velocities flat still works exactly as it did, because that
is still the quickest way to do that. Drag the right edge of a selected note
and they all resize together keeping their relative lengths; hold Shift while
you drag and they all end at the same point instead; hold Ctrl or Command and
they all get the same length.

Dragging a group snaps the note you grabbed to the grid, which is what
dragging one note has always done here, so nothing you already do changes.
There is a second mode on the Selection menu, under "Moving a group snaps to",
that moves the group by whole steps and leaves every note exactly as far off
the grid as you played it. Neither mode quantises anything: the group always
keeps its own internal timing, and the two differ only in where the note you
grabbed lands. Alt still ignores the grid entirely.

One thing it now does that it did not before: if you move a note on top of
another note at the same pitch, the one you moved wins, and the one underneath
is shortened to meet it or removed if it was covered completely. That is
Ableton's rule and it is there because two notes at one pitch overlapping is a
thing that will or will not sound depending on the synth receiving it, which
is the worst kind of problem to have in a file. It only happens when you
actually move something: clicking a note to hear it leaves everything alone.

What is missing, and is the next thing to do here: none of this is on the
keyboard yet. No Ctrl+A, no arrow keys to nudge or transpose, no Delete key.
That needs the main window to hand key presses to whichever view you are in,
and that part of the code is being changed for something else at the moment.
Everything is reachable with the mouse in the meantime.

**You can tidy a part you played in.** There is a Quantise button on the piano
roll's toolbar now, and until this there was no way at all to move a played
note onto the grid. The groove setting swings notes that are already on the
grid, at playback, and deliberately leaves anything played off it where you put
it, which is the opposite job. So a part played in live had to be redrawn by
hand or left as it was.

It is not an all or nothing snap, which is the part worth knowing. The strength
is how far each note travels toward the grid, so 100% puts everything exactly
on it and 80% takes most of the slop out and leaves the feel. That is in the
menu as the setting that keeps the feel, because it is the one you will want on
a guitar or a vocal line. Press it again at 80% and it moves everything closer
again, which is deliberate and is how every other studio behaves.

The swing is on the same scale as the groove setting, so 50% is straight and
66.67% is a triplet shuffle, and the two cannot disagree about what a number
means. You can also quantise against a swung grid, which is how you tidy a part
that was played with a shuffle without flattening the shuffle.

Two things you can do that are worth finding. You can tell it to move only the
notes that are already close, which tidies the nearly-right ones and leaves a
note you pushed late on purpose alone. Or the exact opposite: leave the close
ones and only pull in the ones that are badly out. And you can choose whether
it moves note starts, note ends, or both. Starts, with the length carried
along, is the default and is what you want nearly always.

Three things it will not do to your part. It will not push a note past the end
of the clip, where it would still be drawn in the roll and make no sound. If
quantising lands two notes of the same pitch on top of each other, which
happens when two notes a sixteenth apart both go to the same beat, it keeps the
one that was actually audible and tells you it dropped the other, rather than
keeping whichever came first: a stray blip a fraction of a tick before a long
note is common on a noisy input, and keeping the blip would have thrown away
the note. And an overlap you drew yourself is left exactly as you drew it,
because a held note restruck before you let go is ordinary playing and not
something to clean up.

It tells you what it did after each press: how many notes moved, how many were
shortened, how many were dropped, and how many it left alone because moving
them would have pushed them out of the clip.

Nothing here is invented. The strength, the swing scale, the close-notes gate
and the choice of which end moves are all taken from what Avid, Apple,
Steinberg and Ableton publish about their own, and where none of them says
anything, which is what happens when two notes collide, this follows the rule
the MIDI export here already used, so a quantised pattern bounces as itself.

**Your faders have a proper scale now, and 0 dB is three quarters of the way
up.** The number under each fader means something it can be checked against,
and the fader itself sits where a fader sits on a desk.

What was there before was not chosen so much as arrived at. Unity, the point
where a track is neither turned up nor down, sat about two thirds of the way up
the travel, so a third of every fader was spent on boost. The top of the travel
was +7.75 dB, which is not a number anybody picked: it falls out of the
arithmetic that was in there. And the bottom had no defined value at all, so
the figure printed under the fader could not be held against anything.

The scale is the one on a Behringer desk: +10 at the top, 0 at three quarters,
-5 and -10 as you come down through the middle, then -20, -30, -40, -50, -60,
and silence at the very bottom. Pulling a fader all the way down now gives you
nothing, exactly, rather than something very quiet. Double click a fader and it
goes back to precisely 0 dB. There is no detent or snap at unity, deliberately,
because a snap there would make -0.3 dB impossible to set.

Behringer was the reference rather than Ardour or Cubase for two reasons: it is
the desk convention most likely to already be in your hands given what you
record through, and its scale lands on numbers that can actually be drawn on a
fader. Ardour puts unity at 0.782 of the travel, which is a figure you cannot
mark, and it moves if you change the ceiling. Worth saying that nothing in the
audio world standardises this. There is no specification for a fader law, every
studio picks one, and the ceilings genuinely disagree: Cubase uses +12, Ardour
+6, this desk +10.

**Every mix you have already saved comes back at exactly the level you left
it.** That was the part worth being careful about. The faders will be in
different places on screen, because the scale under them changed, but what each
track is actually doing is identical to the hundredth of a decibel. The
translation goes through the gain rather than through where the handle sat,
which sounds like a technicality and is the whole thing: done the other way,
every track in every old project would have come back about 4.4 dB quieter, by a
different amount each, and it would have sounded like the mix had drifted with
nothing to point at.

Two things to know about old projects. Volume automation is translated the same
way, so every point you drew plays back at the level you drew it at, but the
line between two points is a slightly different shape than it was, because a
fade is now drawn in decibels, which is what a fade should be. On a short fade
that is nothing you would hear. On a long fade out it is real: the fade drops
away a bit quicker than it used to. If you have a long fade at the end of
something and it sounds faster than you remember, that is this, and the new
shape is the more conventional one.

And this version changes the project file, so **a project saved by this build
cannot be opened by an earlier one.** It will say so clearly rather than
opening it wrong, but if you keep an older copy of the studio around, do not
save over your only copy of something you still need to open there.

**You can choose what an export is written as, and 16 bit is dithered.**
Preferences has an Export tab with two settings: the bit depth, and whether
the reduction to it is dithered.

Every export used to be 24 bit whether that was what you wanted or not. 24
bit is right for a working master, and it is still the default, but a CD is 16
bit and so is most of what anybody asks you to send them, so getting one meant
opening the file in something else and converting it there. Now you pick 16
bit and that is what comes out. There is also 32 bit float, which is the mix
exactly as the studio calculated it, for handing to somebody who is going to
work on it further.

The dither is the part that is not obvious, and it is why this took a night
rather than an afternoon. Squeezing a mix down to 16 bit has to round every
sample, and the rounding error is not hiss: it is a direct consequence of the
waveform, so it comes out as distortion rather than noise, and it follows the
music. On a sine a step and a half above silence the rounding alone throws
harmonics only 15 dB below the note itself, and how bad it is swings wildly
with level for no reason you could predict: 25 dB down at three and a half
steps, 39 at four, 29 at four and a half. Worse, the noise rises and falls
with the signal, so it is loudest in the quiet bits, which is where you would
hear it: fade outs and reverb tails go grainy as they disappear. Dither is a very
quiet, very specific noise added before the rounding, and what it buys is a
noise floor that stays still. Measured here, the floor moves by 0.03 dB as a
note fades where it used to move by 3.5 dB, and a tone a quarter of a step
high, which is below what 16 bit can hold at all, comes through intact instead
of vanishing.

It is only applied at 16 bit. At 24 bit the rounding is already around -149
dBFS, far below the noise of any microphone or preamp, so there is nothing
there to fix and dither would only add noise; a 32 bit float file is not
rounded at all. That is the same line Ardour draws in its own code, and what
Cubase and Logic both recommend. Turn it off if the mix is going to be
mastered somewhere else, since dithering twice is worse than once. The status
bar says which depth and whether it dithered, both while it writes and after,
so a setting you forgot you changed is not something you find out from
whoever you sent the file to.

One thing fixed on the way. Every export this studio has ever written had half
a least significant bit of constant offset in it, because of how the samples
were handed to the file writer: it rounded them down rather than to the
nearest. At 24 bit that sits at -144 dBFS and nobody could ever have heard it,
which is why nothing said so, but it is gone, and at 16 bit it would have
mattered. Your existing exports were fine. New ones are slightly more correct.

Nothing changes unless you go looking. The default is still 24 bit, and at 24
bit the dither is off, so an export you do not reconfigure writes the same
file it always did.

**PAN One has forty five new presets, sixty seven in all.** The holes in the
bank are filled: kicks, snares, rims, hats, a shaker and a zap; an acid line,
a reese, a tempo wobble and a sub boom; an electric piano, a drawbar organ, a
clavinet style funk key, brass and a string machine; bells, a music box and a
gong; tempo locked gates that turn a held chord into a part; and pads, plucks,
leads, drones and effects that use the LFOs, which the first bank never
touched. The preset menu on the panel is now grouped under headings by
category.

**The four macros now mean the same thing on every new preset**: 1 is Tone,
2 is Movement, 3 is Grit and 4 is Shape. Each rests in the middle, so the
preset sounds as designed until you move one, and either direction is a
musical move. The mod wheel is vibrato on all of them. Your old presets are
all still there, in the same places, so every project opens on the sound it
was saved with.

**PAN One can grow a new sound out of the one you are playing.** Press
**Grow** on its panel and it breeds a variation of the current patch. The
**Amount** slider is how far it may wander, from a nudge to something you
would not recognise, and **Atonal** is how far from musical pitch: at nothing,
the second oscillator only ever lands on a fourth, a fifth, an octave or the
twelfth, keeping whatever fine detune it already had. **Tween** then stands
anywhere between the sound you started from and the one you grew, and
**Back** returns to where you were. Grow again from wherever you are standing
and you are breeding over generations, keeping what you like.

It only breeds the sound: oscillators, fold, drive, filter, envelopes, slopes
and LFOs. Your pan, glide, macros, gate mode and the modulation matrix's
wiring are left exactly as they were, so a branch is a new sound on the same
instrument setup rather than a different instrument. A wavetable is swapped
for one of its own family on small amounts, and a low pass filter only ever
becomes a band pass or a high pass, never a notch or an all pass.

**Every branch is played before you hear it.** Each candidate is rendered on
a voice of its own and thrown away if it is silent, would clip a chord, or is
more than twelve decibels louder or quieter than what you started from, which
are the same rules the factory presets are tested against. So Grow cannot land
on a dud, and a click costs about a tenth of a second while it listens.

**Panning a guitar or a vocal no longer makes it quieter.** Move a mono track
off centre and it lost 3 dB. Not because anything was broken: the pan knob was
a balance control, which is the right thing for a stereo recording, where it
sets how much of the left you hear against the right. Put a mono source through
it and the maths is different. A guitar in one socket of your interface is the
same signal in both halves of the strip, so panning it hard over multiplies one
of those halves by nothing and you are left with half the sound. Then you ride
the fader back up and wonder why you had to.

So the knob is now one control or the other, and you pick which per strip:
right click a mixer strip and there is **Pan control** with **Balance (stereo
source)** and **Pan (mono source)** in it. A strip in pan mode has a brighter
ring round its knob so you can see at a glance which is which, and the tooltip
says so too.

**Balance stays the default, so nothing you have already mixed has moved.** If
you open an old project it sounds exactly as you left it. It turns out the old
behaviour is precisely what Logic calls a 0 dB pan law, so keeping it as the
default is the same thing as keeping that law.

Set your guitar and vocal strips to pan mode and they will hold their level
right across the sweep. There is also a **Pan law** menu on the same right
click, which applies to the whole project and offers the same seven choices
Logic does, from 0 dB through -3, -4.5 and -6 dB with and without
compensation. The default is -3 dB compensated, which is the one that both
holds the level all the way across and leaves a centred track exactly where it
was, so switching a strip over while it sits in the middle changes nothing you
can hear. If you prefer things a little more forward in the middle, -4.5 dB is
the usual compromise and is worth a try.

The master strip does not get this, deliberately. It is where everything is
added together, so there is no single thing sitting on it to place, and a pan
law there would just move the level of your whole mix.

Two things worth knowing. A compensated law is louder than unity everywhere
except dead centre, by up to 6 dB at the ends of the -6 dB law, so a strip
panned hard over can clip where it did not before. Watch the strip meter if you
go to the extremes. And a bounce gets whatever law you were listening to: the
studio locks the law for the length of the export, so you cannot end up with
half a mixdown done one way and half the other.


**A take now lands where you played it.** What you play arrives back at the
studio later than your hands did. It has to go out through your interface, be
heard, played along with, and come back in through the other converter, and
if anything in the mix is holding the sound back for a lookahead plugin it is
behind that too. The studio was putting some of that back and not all of it,
so an overdub sat a few milliseconds behind the part it was played against.

That is a horrible thing to be wrong, because nothing tells you. No error, no
crackle, no meter moving. The guitar is just late, and the only person
standing there to take the blame for that is you.

Three things were wrong with the old correction. It used only what your
interface reports, so the moment a lookahead plugin went on the master every
take went back to landing late by its whole lookahead. It was limited to half
the length of a take, so a short one was half corrected. And recording round a
loop, it cut each pass in the right place but read past the head of the next
one, which threw away the last few milliseconds of every pass: your last note
before the loop came round, every time.

What that correction never had any answer for at all is a driver that does
not report its latency. Plenty do not, and the studio then had nothing to go
on and no way for you to tell it. **So Preferences > Audio now prints what is
actually being taken off a take, in milliseconds and in samples, and has a
correction you can set if it is wrong.** If you want to check it rather than
trust it: patch a cable from one of your outputs straight back into an input,
record the metronome through it, and look at where the recorded clicks sit
against the grid. They should be on it.

Nothing is lost at the front of a take, either. The audio you played before
the record point is still in the file, it is just not in the clip, so you can
always drag a take's left edge back over it.

Two things this does not cover yet, said plainly. A **MIDI** recording is
still not corrected, so recording a socket and a keyboard into one take
leaves the two halves slightly apart. And the very **end** of a take now
stops a few milliseconds earlier than it used to, because the studio stops
listening the moment the transport does and the last of what you played is
still on its way back.

**The app icon shows up now.** The mark is white line art, and it was sitting
on a transparent background with nothing behind it, so on the Windows light
theme, which is the default, there was nothing to see: a blank space in the
taskbar, in Explorer, in Alt-Tab and in the title bar. It now has its own
black background, so it reads on a light theme and a dark one alike.

It was also mush at small sizes. Windows asks for the icon at 16, 32 and 48
pixels as well as 256, and the mark's lines are a tenth of a pixel wide once
it is that small. There is now a second drawing of it made for those sizes,
using the mark's silhouette rather than its outline, so the shape survives
down to about 32. At 16 pixels it is a bright mark rather than a readable
one, which is as far as line art of this kind goes without drawing a simpler
version of the logo.

**A short sound in the playlist plays once a bar, not over and over.** Drop
a one shot into an empty playlist and press play, and it used to repeat back
to back, because playback looped at the exact end of the last clip. It now
plays to the end of the bar the last clip finishes in, with silence for the
rest of that bar, so what you hear is a bar of music rather than a stutter.
A loop you have cut to exactly one bar, or four, stays exactly that long. A
loop range you set yourself works as it always did, and an export still ends
where your last sound does.

## Since v0.6.1

**Every mixer strip can take its own input, and they all record at once.**
This is what the studio needed in order to record more than one person at a
time. Open the mixer and each strip now has an input menu and an arm button:
pick a socket on your interface and that strip is armed. Press record and
every armed strip is captured, each to its own playlist track, each named
after the strip it came from.

Right click a strip for **Map inputs from here**, which gives every input on
your interface a strip of its own in one action rather than eight. There is a
version for stereo pairs, and one to clear the lot.

The menu lists stereo pairs above mono inputs and uses your interface's own
names for its sockets, so what is on screen matches what is written on the box
in front of you. Choosing an input also arms the strip, because wiring a
socket to a strip and then not recording it is almost never what anybody
meant.

The input selector that was on the toolbar is gone. One input for the whole
studio was the thing standing in the way of recording a band, and it was in
the wrong place: the choice belongs next to the strip it feeds.

A few things that follow from it. The **Monitor** box now means the armed
strips, or every strip that has an input, rather than one chosen insert. Your
routing is saved with the project, since which microphone is on which strip is
part of a session rather than a preference. And a bounce is now fully delay
compensated whatever monitoring is doing: skipping compensation is the right
trade while somebody is playing and the wrong one in a finished file.

## Since v0.6.0

**One instrument in one socket is now heard in the middle, not out of one
speaker.** The studio took input 1 as the left of a stereo pair and input 2 as
the right, always. That is right for a keyboard using two sockets and wrong for
everything else: a guitar in input 1, with nothing plugged into input 2, came
out of the left speaker with silence in the right. Takes were recorded the same
way, as a stereo file with the performance down one side.

It looks like an output fault, which is the worst part of it. One speaker sends
you to the output settings, and nothing there can help, because the problem was
on the way in.

There is now an input selector on the toolbar, next to the record mode. Mono
inputs are listed first and are centred; pick a pair only for something
genuinely stereo. **If you were monitoring a real stereo pair on inputs 1 and
2, you will need to select "In 1+2" after upgrading,** because the default is
now a single instrument on the first input, which is what almost everybody
plugs in first.

The input meter follows the selection too, so a meter that moves is a meter for
the channel you are actually recording. Changing input while monitoring fades
across rather than cutting, because a full scale step into an amp simulator is
loud.

## Since v0.5.1

**The Windows builds now include ASIO.** If you have an audio interface, this
is the change that matters, and it is the reason monitoring was still not
playable after two previous attempts at it. Without ASIO the studio cannot see
an interface's own driver at all, and the makers of interfaces mostly did not
implement Windows' own low latency mode, because they ship an ASIO driver and
expect it to be used. So on that hardware there was nothing fast for the studio
to find, and it correctly reported finding nothing, which looked exactly like a
machine that simply could not go faster.

It could not be included before. The ASIO SDK's licence was proprietary only
and imposed terms the GPL family forbids adding, which is why these downloads
shipped without it. Steinberg dual licensed the SDK under GPL-3.0 on 15 October
2025, and this project's AGPL-3.0 may combine with GPL-3.0 under section 13 of
each, so the obstacle is gone. The build fetches the SDK, checks it against a
known SHA-256, and fails rather than quietly producing a Windows binary without
it.

**Exclusive mode is tried last rather than refused.** The previous release made
a point of never choosing it for you, on the grounds that it holds your sound
card and nothing else on the machine plays through it. That is right on a
laptop with one sound card and wrong on a machine with an interface, where your
system sound is not going through the interface anyway: refusing protected
nobody and left people unable to play. Drivers that share the device are still
tried first, and the studio says plainly when it has taken the card.

**The studio now shows its working.** Preferences > Audio lists every driver it
tried and what each one gave, or why it was skipped, and says outright whether
the build you are running includes ASIO. That last line matters more than it
looks: a build compiled without ASIO behaves exactly like a machine with no
ASIO driver installed, and from the outside there was no way to tell those
apart. That is why this took three attempts.

## Since v0.5.0

**Monitoring, properly this time.** v0.5.0 moved the studio onto a faster
driver, and that was necessary but not sufficient: it was still not playable,
and the report that it was not came with the detail that settled it, which was
that the same machine is fine in another studio. That rules out the hardware.
Four things were wrong, and two of them were ours.

*The buffer was four times larger than it needed to be.* Microsoft document
the standard Windows audio driver as supporting buffer sizes from 128 samples
up to 480, with 480 being what every application gets by default. The studio
was aiming at five milliseconds, which asks for 240 samples and therefore
takes 256, stepping straight over 128 and 192. Most of the gain from the
faster driver was being thrown away immediately. It now lands on 128.

*The buffer size control really does nothing on plain Windows Audio.* This was
reported and assumed to be a misunderstanding. It is not: in that mode JUCE
publishes a single buffer size and ignores any request to change it, because
the wakeup period belongs to the driver. If you were changing that setting and
hearing no difference, you were right.

*You could hear yourself twice.* A send is a copy of a channel taken after its
fader, so on the channel you are monitoring through it carries your own
playing. The direct path was exempted from delay compensation and the send
copy was not, so your input arrived once immediately and again at send level,
separated by the longest plugin lookahead in the project and moving every time
a plugin was loaded anywhere. Anybody who has put a reverb on a vocal while
tracking it was hearing that.

*Anything routed straight to the master played early.* The opposite problem. A
synth or a playlist clip sent directly to the master waited for far less than
the rest of the mix, so it sat ahead of everything by whatever lookahead
happened to be loaded on any insert. Nothing in the original report pointed at
this; writing the test found it.

Both of those last two survived because they were somewhere no test could
reach, and this is the arithmetic in the studio least able to announce that it
is wrong. Nothing errors, nothing crackles: tracks simply sit a few
milliseconds apart and the record sounds slightly smeared. Both had a comment
above them claiming the opposite of what the code did.

So the whole compensation graph is now plain arithmetic in a file of its own,
and the test does not check the formulas. It walks every route through a
generated mixer, totals everything on the path, and asserts that all of them
reach the master on the same sample: twenty thousand mixers, with sends chained
forward, output buses split across destinations and lookahead plugins anywhere.
Monitoring is asserted as the deliberate exception, early by exactly the amount
skipped and with its send copy not put back. Then the file was broken on
purpose twenty seven ways, including both original bugs reintroduced verbatim,
and all twenty seven were caught.

**A "Find the fastest device" button,** in Preferences > Audio. The automatic
search runs only on a first run, because a settings file that already names a
device is a choice the studio has to assume you made. That left everybody
upgrading without the fix, which is most of the people who need it. The button
asks for the same search on purpose and prints what it found underneath.

**Exclusive mode is tried last rather than refused.** It is the fastest thing
Windows has and it holds your sound card, so nothing else on the computer plays
through it while the studio is open. That is a cost, not a veto, and the first
version of this got the balance wrong: refusing outright sounds careful, but on
a machine with an audio interface it protects you from nothing, since the
system's sound is not going through the interface anyway. It just leaves you
unable to play. So drivers that share the device are tried first, exclusive
mode is reached for only when none of them was fast enough, and the studio says
plainly when it has taken the card.

## Since v0.4.0

**Monitoring is low enough to play through without you setting anything.** This is the second half of a fix. The first half, in v0.4.0, was a real defect: the monitored signal was being held back by plugin delay compensation. What was left was not a defect in the studio's code at all, which is why it survived that fix: the studio was accepting whatever driver Windows offered, and what Windows offers by default is tuned for playing a video back without glitching rather than for hearing yourself in time. That is tens of milliseconds, and no buffer size setting recovers it.

Windows has been offering a faster route to the same device the whole time. Alongside the shared mode everything gets by default, it exposes the same device through IAudioClient3, which JUCE calls Windows Audio (Low Latency Mode): single digit milliseconds, and still shared, so a video in a browser keeps playing. The studio was not asking for it.

So on first run, and only on first run, the engine measures what monitoring costs and, if it is too slow to play through, works down a list until something is fast enough. The choice is then saved as if you had made it, so this happens once, and a machine that was already fast enough is not touched at all.

Exclusive Mode is deliberately not on that list, although it would win. It holds the sound card, so nothing else on the machine plays while the studio is open, and somebody who asked to hear their guitar and found their browser had gone silent would have no way to connect the two: they would report a second fault rather than the price of the first one being fixed. The studio names it where the latency is shown, and leaves it to you.

**If you are upgrading, press the button.** The search only runs when there is no saved audio device, because a settings file that already names one is a choice the studio has to assume you made. **Preferences > Audio** has a *Find the fastest device* button that asks for the same search on purpose, with what it found printed underneath.

Buffer size is chosen the same way, against a five millisecond target rather than the smallest the driver will admit to. A buffer at the floor is where dropouts live, and a studio that crackles until you raise a setting is a worse first impression than one four milliseconds slower than it could be. If you want the floor, it is still yours to take.

Turning monitoring on now also prints the round trip in milliseconds, and says plainly when it is too slow to play through rather than leaving you to work that out with an instrument in your hands.

The ordering and the arithmetic are in a header with no JUCE in it, so they are tested rather than asserted: the buffer choice is checked against an independently written second implementation over twenty thousand generated driver lists, with sizes drawn at the boundaries because that is where it can be wrong. Nineteen deliberate breakages of that header, including putting the buffered shared mode back at the top of the preference list, are all caught by the tests.

**A preferences window,** on Ctrl+, or from the File menu. The audio device was behind one button and the plugin list behind another, with no relationship between the two windows, and three more settings were constants nobody could reach. All of it is in one window now, in three tabs:

- **General:** how often the autosave runs, including off; how loud the metronome is; and the tempo a new project starts at. Each applies as you change it, so you can set the click level while the metronome is running rather than guessing and reopening the window. There is a button to open the folder your settings, plugin list and autosave live in, and one to put those three back to their defaults.
- **Audio:** the device chooser, with the monitoring round trip still printed in milliseconds underneath it.
- **Plugins:** scanning, exactly as before.

The **Audio** and **Plugins** buttons in the toolbar now open that window on those tabs, because picking a device and scanning for plugins are what a fresh install needs and neither should be behind a tab. Monitoring, the record mode and the count-in stay on the toolbar: they are decisions you make between takes, not settings you go looking for.

The metronome level was a fixed number in the engine before this, and the default is that same number to within a tenth of a decibel, so your click does not change level on upgrade. It is read once per click rather than per sample, so turning it down while the metronome is running cannot step on a click that is already decaying. The autosave default is the same two minutes it has always been.

**PAN One has a front panel.** The instrument shipped in v0.3.1 with no controls at all, so its window opened black: there were no parameters, and the generic editor every host falls back on had nothing to draw. There are eighteen controls now, grouped into oscillators, filter, amplitude envelope and filter envelope, with the presets in a box at the top. A marker appears when the knobs no longer match the preset they came from, so a preset that has stopped sounding like itself explains itself.

They are ordinary plugin parameters, which means **the modulators reach into PAN One** exactly as they reach into anything else you host: wire a shape to its cutoff and it sweeps. The note in v0.3.1 saying that needed real work first was simply wrong, and reading the framework's own source rather than trusting that note is what settled it.

Projects now save the knobs rather than only the preset number, written by name so that a later version adding a control in the middle cannot shift your saved values along by one. A project saved before the knobs existed carries a preset number and nothing else, and loads as that preset.

The knobs and the presets are a table in a header with no JUCE in it, rather than four hand written lists in four places, which is what lets a test check the things that would otherwise be silent: that no two controls write the same part of the patch, that every value in every preset is inside the range of the control that will hold it, and that the panel draws every control exactly once. Twenty one deliberate breakages of that table are all caught.

**A monitoring fault that a settings file could cause.** The monitoring mode, the record mode and the count-in were restored from the settings file without checking that the stored value was one the control offers. An unknown value leaves a combo box with nothing selected, which reads back as zero, and for monitoring that zero became a mode the engine does not have: the callback took its low latency path, which deliberately skips delay compensation, while the mixer matched neither "armed" nor "on" and passed no input through. The result would have been monitoring that is silent and costs the compensation anyway, which is a hard thing to diagnose from the symptom. No released version wrote a value that could trigger it, so nobody will have seen this, but a hand edited or future written settings file would have been enough.

Related, and the reason the above was found: a setting that is not a number parses as zero, and zero is a real value for several of these. It is off for the autosave and full level for the metronome, so a single garbled line in the settings file would have read back as a deliberate choice, and for the click as the loudest one in the range. Settings are now checked before they are acted on, and anything unreadable falls back to its default.

## Since v0.3.1

**It comes with an instrument.** Until now the studio was silent on a new machine until you supplied your own VST3s, which is where most people who try a studio stop trying it. PAN One is built in: two oscillators with selectable shapes and detune, a sub an octave down, a resonant filter with its own envelope, an amplitude envelope, sixteen voices, and five presets (Sub Bass, Soft Keys, Slow Pad, Short Pluck, Reed Lead).

It is in the instrument list the first time you open the studio. There is nothing to scan for and nothing to download, and it behaves as an ordinary plugin everywhere else: it loads the same way a VST3 does, saves into a project the same way, and records, modulates and mixes through the same code rather than through a special case.

The oscillators are band limited. A naive saw or square is a discontinuity sampled directly, and everything above Nyquist folds back as partials that are not harmonically related to the note, so they move the wrong way as you play up the keyboard. That shimmer in the top two octaves is what gives a soft synth away, and no filter afterwards removes it. Measured against the naive version at 2 kHz, the saw puts 17 dB less energy into those partials and the square 18 dB less.

The filter is a state variable design with the integrators kept as integrators, which matters because a filter envelope changes the cutoff every single sample. The usual alternative clicks on fast sweeps and can be driven unstable by the modulation alone. Its resonant peak lands on the cutoff to four decimal places from a Q of 0.5 to 10.

## Since v0.3.0

**Monitoring is no longer delayed by plugin delay compensation.** Playing a guitar through the studio was pushed back by an amount that had nothing to do with the audio interface, and that changed whenever a plugin was loaded or bypassed. Monitored input is mixed into an insert, and that insert was then held back along with everything else to keep the tracks lined up with each other. A lookahead limiter or a linear phase EQ anywhere in the project was enough on its own to make the instrument unplayable.

The insert being monitored through is no longer held back. Anything else landing on that insert plays early by the same amount while monitoring is on, which is the right trade while tracking: the player has to be able to play, and lining the take up with the arrangement afterwards is the recorder's job and already happens.

**The audio settings window now says what your latency actually is.** A buffer size in samples does not tell you what you have chosen until it is divided by the sample rate and added to the driver's own buffering either side. The round trip is now printed in milliseconds, split into input, output and buffer, taken from what the driver reports rather than inferred.

If the number is high on Windows, the buffer size is often not the lever. Shared mode Windows Audio and DirectSound buffer heavily whatever is asked of them. Changing **Audio device type** to Windows Audio (Exclusive Mode) is usually a large improvement. ASIO is better still, but note that these downloads are built without it: `JUCE_ASIO` only compiles in when the Steinberg SDK is present, which the build machine does not have. Building locally with `AHP_ASIO_SDK_DIR` set enables it.

**Plugin editors and the mixer stay in front.** Opening the mixer over the playlist and clicking an insert appeared to do nothing: the editor opened behind the main window with no sign it had opened at all. Editors now come to the front when opened, and both they and the mixer float above the main window while the studio is the application in front. They drop back when you switch to something else, so they will not sit over your browser, and while a dialog is open so nothing covers it.

## Since v0.2.0

**Loop recording, take folders and comping.** Set a loop range by dragging in the ruler and record over it, and each time round is kept as its own take rather than recorded over the pass before. The passes arrive as one take folder on the track: one clip holding several alternatives, each still written to its own WAV in `Recordings`. Recording the same loop again adds to the folder already there.

A take folder draws one lane per pass, with the part that is being heard lit and the parts you have not chosen dimmed but still present. Drag across a lane to give that stretch of the song to that pass, so the first line can come from pass three and the next from pass seven. Double-click a lane to give it the whole folder, Alt-click to throw a pass away. The strip along the bottom shows the comp itself.

Every join is crossfaded, ten milliseconds by default, with an equal-power curve. Cutting at a zero crossing is not enough between two different performances: the waveform is continuous at the join but its slope is not, and a voice clicks on that. Equal power is the right shape because two takes of one singer are not the same waveform, so their powers add rather than their amplitudes, and a linear fade dips audibly at every join. The comp is held in seconds into the folder rather than in beats, so changing the tempo moves the folder on the grid without moving the joins through the performance.

The takes are never touched, so a choice can be remade at any time, including after the project has been closed and reopened, and the export reads the comp the same way playback does rather than through a second code path. A comp swipe is one undo step.

**Automation on the playlist, and on the mixer.** Automation could only live inside a MIDI clip, which left two things impossible to move at all: anything on a mixer insert, and anything that needs to run across a section rather than within one pattern. Both work now. Right-click a playlist track header, choose Automate, and pick an insert's volume or pan, a parameter of any effect on it, or a parameter of any instrument you have loaded. What appears is a clip: drag it, copy it, trim it, mute it and delete it like audio or MIDI, drawn as the curve rather than as a block. It starts at the value the control is already sitting at, so drawing one on a mix you have balanced does not throw the balance away, and the mixer's own fader follows the curve as it plays.

Every segment has a bend, dragged from the circle between two points, because a straight line is the wrong shape for most of what automation is asked to do: a filter that should sit closed for four bars and then open over one is two points with a bend, not a dozen points that are then impossible to move as a gesture. The bend warps the position along the segment rather than the value, which is what guarantees it can never take the value outside the two points it joins, so what you hear is the line on the screen rather than a spline bulging past it. Two points on one beat is an instant jump.

**MIDI file import and export.** A part written here can now be taken somewhere else, and a part written anywhere else can be brought in. Drop a `.mid` file on the playlist and you get one clip per track in the file, placed where you dropped it and routed to free instrument channels, with the playlist tracks named after the tracks in the file; a single track carrying several MIDI channels is split by channel rather than flattened into one unplayable pile. The file's own tempo is adopted while the project is still at the tempo nobody chose, and left alone once you have set one. Export writes the arrangement, or just the selected clips, as a standard multi-track file with the tempo and the track names. The round trip is exact for notes: a file exported and imported again gives back the same starts, lengths, pitches and velocities. Automation is not exported, because a MIDI file has no way to say which plugin parameter a lane belongs to, and the export says so rather than pretending.

**Warp markers.** Tempo matching fixes audio played at one steady tempo. A warp marker fixes audio that drifts inside the take, which is the normal case for a live performance or a vocal. Pin a point in the audio to a position on the grid and everything between two markers is stretched to fit: put a marker on the snare that landed late, drag it onto the beat, and the bar either side comes with it. Select an audio clip and double-click it to add one. A warped clip always follows the project tempo, and the waveform is drawn through the warp, so a transient appears under the grid line you pinned it to. Trimming and splitting understand markers; cutting a warped clip in two is inaudible.

**True peak, measured the way the standard defines it.** The peak meter now oversamples four times through the interpolator ITU-R BS.1770-4 Annex 2 tabulates, because the peak of the waveform your converter reconstructs is not the largest sample in the file. The meter here used to report the largest sample, which reads around three decibels low on exactly the material where it matters: a limited master that measures clean and then distorts after encoding. If a mix you measured before this release looked safe at -0.3 dB, measure it again.

**Loudness range now follows EBU Tech 3342.** It was the plain spread between the 95th and the 10th percentile of the short term readings, with no gating, so a quiet intro or a fade out became the bottom of the distribution and a track that never changes level could report tens of LU of dynamic range. It is now gated at -70 LUFS and then at 20 LU below the mean of what survives that, which is what the standard specifies and what other meters report.

**Groove.** Swing in the channel rack sets where the offbeat sits inside its pair: 50 is straight, 66 lands it on the third triplet, which is a full shuffle. That is the definition hardware samplers established, so the number means the same thing here as it does to a drummer or another studio. Feel goes further, because swing on its own still sounds programmed: a played pattern has offbeats that are slightly quieter and slightly early or late, and played and loose soften and drift them by increasing amounts. The drift comes from each note's own position rather than from a live random source, so a pattern plays the same way twice and an export matches what you heard.

Groove never changes the stored notes. It is applied as the arrangement is handed to the engine, so it can be dialled while playing and turned off without having lost the original timing, and notes played off the grid are left where you played them.

**Per-step velocity.** How full a step is drawn in the channel rack shows its velocity, and Shift+drag up or down on a lit step sets it directly, which is how hardware step sequencers do accents.

**Input monitoring.** Hear the interface input through the studio's own effects while you play. Off, armed only, or always, with a level control and a choice of insert. Deliberately not delay compensated: that compensation lines internal paths up with each other, and on a monitor path it would only be latency you feel directly. Lining the take up with the arrangement is still the recorder's job.

**A test suite, run on every push.** JUCE takes minutes to compile, but most of what would be silently wrong in a studio is arithmetic that does not need it: where a warped clip reads from, how a comp crossfades, what a swing percentage means, what a loudness figure is. That arithmetic now lives in headers with no JUCE in them, with checks over it that run in about a minute with address and undefined behaviour sanitizers on, as their own CI job that reports before the platform builds have finished fetching their dependencies. A red test does not stop the build; it does stop a release from publishing. `./Tests/run.sh` runs the same thing locally.

## Fixed in this release

All of these were found by reviewing the work above rather than by anything going visibly wrong, which is the point: every one of them is the kind of fault that leaves the program running.

- **Picking a shuffle did nothing until you made another edit.** Groove is applied where the arrangement is handed to the engine, and the control set the value without telling anything, so the swing was stored, saved to the file and shown in the box while the pattern kept playing straight.
- **And it was not an undo step.** A groove change recorded no step, so the next Ctrl+Z undid the edit before it instead.
- **The loudness meter was allocating on the audio thread.** Reading the loudness range copied and sorted the entire short term history on every audio block, and the history grew for as long as the session stayed open: after an hour of playback that was a six-figure allocation and sort inside every five millisecond buffer, which is a dropout. The history is now kept as fixed histograms and reading a figure costs nothing.
- **Modulation allocated on the audio thread too**, on the first block after any plugin was loaded, and carried the previous plugin's resting values onto the new one when the two exposed the same number of parameters.
- **Monitoring then switching to an interface with no inputs** read past the end of the input array.
- **A plugin reporting a knob move from its own process call** could leave the audio thread spinning on a lock the message thread was holding across an allocation.
- **Changing the audio device during an export** could reallocate a buffer the export was writing through.
- **Every system exclusive message from a control surface** allocated on the audio thread.
- **File > New kept the old tempo**, so a MIDI file dropped into a fresh project decided the tempo had been chosen deliberately and arrived at the wrong speed without saying so.
- **A project file carrying both a comp and warp markers** loaded as a take folder with live markers on it, which nothing downstream expects.

## Before this release

**Plugin delay compensation.** Plugins that look ahead (mastering processors, look-ahead limiters, spectral effects) hold audio back. Nothing accounted for that, so a track carrying one played late against the others, by an amount that changed every time a plugin was loaded. Every path is now levelled, including sends and playlist audio, so everything reaches the master on the same sample. The status bar reports the compensation when it is not zero.

**Modulators.** A tempo-locked shape wired to any parameter of any plugin you host. Add one, click Learn, then grab the knob you want inside the plugin's own window. Seven shapes, rates from 1/16 to 8 bars, bipolar or unipolar, any number of targets each with its own depth. Modulation rides on top of whatever the knob is set to, so turning it by hand still works and the modulator moves around the new value.

**Mix report.** Measures your master after every effect on it, to ITU-R BS.1770-4, the standard streaming services normalise to. Integrated loudness, true peak, loudness range and mono compatibility, each with what it actually means. Verified against EBU Tech 3341 compliance test cases at 44.1, 48 and 96 kHz, within 0.011 LU against a tolerance of 0.1.

**Channel rack.** One row per channel, one column per step, on F6. Steps are ordinary notes in ordinary MIDI clips, so a pattern drawn here opens in the piano roll and can be edited there.

**Mixer sends.** Two per insert, post-fader, so a reverb can be shared instead of loaded on every track.

**Per-output routing.** Multi-output instruments like Microtonic Multi can send each output bus to its own mixer insert, every drum with its own fader and effects. Route outs... in the channel bar.

**Tempo matching.** Tell an audio clip the tempo it was recorded at and it stays on the grid at any project tempo, and follows if you change the tempo later. Pitch is unaffected, so a vocal stays in key.

**Also:** plugins are scanned in a separate process, so one that crashes on load can no longer take the studio down with it; denormal protection, which is the CPU spiking seconds after sound stops; and JUCE pinned to a release tag rather than tracking master, so this builds the same way in a year.

## Downloads

| Platform | File | Notes |
|---|---|---|
| Windows | `ALLHAILPAN-Studio-Windows.zip` | 64-bit, no installer needed |
| macOS | `ALLHAILPAN-Studio-macOS.zip` | Universal, Apple Silicon and Intel |
| Linux | `ALLHAILPAN-Studio-Linux.zip` | Needs ALSA or JACK |

All three are built by GitHub Actions directly from the source in this repository. The build log is under the Actions tab.

## Before you run it

Not code-signed, so every operating system will complain. Nothing is wrong with the file.

**Windows:** SmartScreen warns about an unknown publisher. More info, then Run anyway. Defender may also report `Trojan:Script/Wacatac.B!ml`, which is a false positive: the `!ml` suffix means a machine-learning guess rather than a signature match, and Wacatac is the bucket Defender uses for any unfamiliar unsigned executable. If it gets quarantined, Windows Security, Protection history, find the entry, Actions, Allow.

**macOS:** Gatekeeper refuses on a double-click. Right-click the app, Open, then Open again. If that fails, System Settings, Privacy and Security, scroll down, Open Anyway.

## Known limitations

- **Low latency on Windows needs an interface with an ASIO driver,** or a sound card whose driver supports Windows' small shared buffers. Plenty of built-in sound chips support neither, and on those the studio will offer exclusive mode and little else.
- **4/4 only, and one tempo for the whole song.** A MIDI file that changes tempo part way through is read at its opening tempo, and the status bar says so.
- **Loading a plugin and dragging a mixer fader are not undo steps.** Everything else that edits the arrangement is.
- **An instrument with 32 or more output channels** (sixteen stereo buses, which is the most this studio will route) makes the engine allocate once per block on the audio thread. Instruments with eight buses or fewer, which is all of the ones tested, are unaffected.
- **macOS and Linux builds are lightly tested.** Please report anything broken.
- **Nothing here has been through a long session on real hardware yet.** The arithmetic is tested; the feel is not. If something sounds wrong, that is worth a bug report even if you cannot say exactly what.

Licensed under AGPL-3.0. The name and logo are excluded, see TRADEMARKS.md.
