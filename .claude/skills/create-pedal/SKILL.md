---
name: create-pedal
description: Design, implement, register and bench-test a new Woodshed effects pedal (or amp/cab) — DSP class, knob specs, enclosure name and colours, bench pass, optional factory preset. Use when someone wants to add a pedal, build an effect, clone the sound of a classic stompbox, or add a new amp or cab model to the rig.
---

# Create a Woodshed pedal

A pedal is one C++ class in a family file under `Source/pedals/`, one `r.add<…>()` entry
that registers it, and a clean run of `woodshed-bench`. There is no per-pedal artwork: the
enclosure is rendered once and tinted with the pedal's `body` colour, and the name and knobs
are drawn from its `PedalInfo`. So a new pedal shows up in the library, presets and the studio
rig pane as soon as it's registered.

Read `Source/engine/Pedal.h` (base class, `KnobSpec`, `dsp::Svf`, `dsp::DelayBuffer`) and one
existing pedal in the same family before writing anything. `CleanBoost` in
`Source/pedals/Dynamics.cpp` is the smallest complete example.

## 1. Design it before coding

Settle these, and tell the user what you chose:

| Field | Rule |
|---|---|
| `type` | `family.name`, lowercase, e.g. `drive.treble`. **Permanent**: presets and saved rigs store it. Check `woodshed-bench list` that it's free. |
| `name` | One ALL-CAPS invented word printed on the enclosure (GRIT, HORNET, GLINT). Never a real product, brand or trademark ("Tube Screamer", "Big Muff", "Rangemaster" are out). Must not repeat an existing name. |
| `subtitle` | What it is in two or three plain words: "treble booster", "tape echo". |
| `category` | One of `Registry::categories()`: Dynamics, Drive, Amp, Cab, Filter, Modulation, Pitch, Delay, Reverb, Weird. |
| `description` | One line for the library browser that says what it sounds like, not how it works. |
| `body`, `ink` | Enclosure colour and the text/knob-label colour. Ink must read clearly on the body (dark ink on light bodies). Pick a body that isn't already used by a pedal in the same category. |
| knobs | At most 8 (`Pedal::maxKnobs`). Order them the way a real pedal reads, left to right, with the main control first and `level` last. |

Which family file: `Dynamics.cpp`, `Drive.cpp`, `Filter.cpp`, `Modulation.cpp`, `Pitch.cpp`,
`Delay.cpp`, `Reverb.cpp`, `AmpCab.cpp` (amps and cabs), `Glitch.cpp` / `Weird.cpp` (both
register into the Weird category). Put the pedal in the file that matches its category.

### Knob specs

```cpp
knob ("time", "Time", 40.0f, 1200.0f, 380.0f, "ms", 0.6f)   // id, label, min, max, default, unit, skew
choice ("voice", "Voice", { "Flat", "Fat", "Tight" }, 0)       // stepped selector, value = index
```

- Units: `"dB"`, `"ms"`, `"Hz"`, `"%"` (0..1 shown as a percentage), `"st"`, `"x"`, or empty.
- `skew < 1` gives more travel to low values: use about 0.3–0.6 for times and frequencies.
- Defaults must already sound good. The bench requires a pedal at its defaults to stay within
  ±3 dB of the dry guitar's level, so pick a `level` default that achieves it.
- Ids are lowercase and short (`drive`, `tone`, `mix`, `level`); presets refer to them by id.

**Never change a shipped pedal's knobs.** Saved rigs store knob *positions* (slot index) as
*normalised* 0..1 values, so reordering, inserting or re-ranging a knob silently changes every
saved sound that uses that pedal. Appending a new knob at the end is the only safe change, and
its default must reproduce the old sound.

## 2. Implement the class

Put it in the family file's anonymous namespace, next to its siblings, with a one-line
comment in the house style: `// NAME: what it is, and the one idea behind its sound.`

```cpp
// GLINT: single-transistor treble booster. (Comments say what it models and the idea behind the sound.)
class TrebleBooster final : public Pedal
{
public:
    void prepared() override      { /* sample-rate-dependent setup, buffer sizing, SmoothedValue::reset */ }
    void reset() override         { /* clear filter and delay state for both channels */ }
    void update() override        { /* read knob (i) in real units, set smoothed targets */ }
    void processChannel (int ch, float* x, int n) override { /* per-sample DSP, state in channels[ch] */ }

private:
    struct Channel { /* filter states, smoothed gains */ };
    std::array<Channel, 2> channels;
};
```

Rules the chain relies on (see the comment on `class Pedal`):

- **Two channels of state.** `processChannel` is called for channel 0, and for channel 1 once
  the signal is stereo. Keep every filter, envelope and smoother in a per-channel struct.
- **Stereo makers** (chorus, ping-pong, reverbs, wide delays) return `true` from
  `producesStereo()` and implement `processStereo (left, right, n)` instead.
- **Real-time safe.** No allocation, locks, logging or file access in `update()` or
  `process*()`. Size buffers in `prepared()`, where `sampleRate` and `maxBlock` are valid.
- **Knobs arrive in real units** through `knob (i)`, once per block, followed by `update()`.
  Smooth anything that would zipper (gains, cutoffs, delay times): `juce::SmoothedValue` or a
  one-pole using `dsp::onePoleCoeff`.
- **Filters:** use `dsp::Svf` for anything that's modulated; `dsp::DelayBuffer` for delays,
  keeping the read delay at 2 samples or more.
- **Latency:** if the pedal looks ahead, return it from `getLatencySamples()`.
- **Stay finite and bounded.** Guard feedback loops (soft-clip inside them), flush denormals
  with tiny offsets or `juce::ScopedNoDenormals`, and keep extremes under +12 dBFS.
- Drives and amps: oversample or band-limit the nonlinearity if the bench render aliases;
  copy what the neighbouring drive pedals do.

## 3. Register it

Add an entry to the family's `register…Pedals (Registry& r)` at the bottom of the file, in
the order it should appear in the library. For the format, here is GLINT's real entry:

```cpp
r.add<TrebleBooster> ({ "drive.treble", "GLINT", "treble booster", "Drive",
                        "Bright germanium push that makes a dark amp bite",
                        juce::Colour (0xffbfc5c9), juce::Colour (0xff3a1c14),
                        { knob ("boost", "Boost", 0.0f, 1.0f, 0.5f, "%"),
                          choice ("range", "Range", { "Treble", "Mid", "Full" }, 0),
                          knob ("level", "Level", -24.0f, 12.0f, 0.0f, "dB") } });
```

`Registry::add` asserts on a duplicate `type` and on more than 8 knobs. No CMake change is
needed unless you create a new family file (then add it to
`WOODSHED_ENGINE_SOURCES` in `CMakeLists.txt` (shared by the app, the AU/VST3 rig and the bench) and declare its `register…` function in `Source/engine/Registry.h`).

## 4. Bench it until it passes

```bash
cmake --build build-bench --target WoodshedBench -j8          # slow first time; run in background
B=build-bench/WoodshedBench_artefacts/Release/WoodshedBench
$B knobs drive.treble                                          # check ids, ranges, defaults
$B run drive.treble --wav tools/testdata/guitar-di-48k.wav     # the pass/fail checks
$B run drive.treble --wav tools/testdata/guitar-di-48k.wav --render <scratch dir>   # WAVs to listen to
```

`run` fails a pedal that produces NaN/inf, makes noise from silence (above −80 dB), moves a
guitar DI's level by more than 3 dB at defaults, blows up at any knob extreme or under random
knob automation, or costs more than 100 µs per 64-sample stereo block. Fix the DSP until it
says PASS; don't loosen the bench. Then run the whole registry (`$B run --wav …`) to make sure
nothing else broke.

Listen-check what you can't measure: render it and send the WAV to the user (any DI files
in `tools/testdata/` work as input). Describe what you expect them to hear.

## 5. Optional: show it off in a factory preset

Factory presets live in `presets/<genre>.json` (board pedals have a `spot` of 0–9; `amp` and
`cab` are separate). Knobs are set by id in real units, or by choice name. Preset names are
evocative and generic, like the existing ones. Then:

```bash
$B presets presets/<genre>.json --wav tools/testdata/guitar-di-48k.wav
```

A preset must not clip (peak ≤ 0 dBFS), must land in the shared loudness window so switching
presets doesn't jump in volume (trim the cab `level` to fix it), and must cost under 600 µs
per block.

## 6. Build the app and finish

```bash
cmake --build build --target WoodshedStudio --config Release -j8
```

Check it compiles cleanly. Then report: the type id, name and knobs, the bench line (level,
peak, cpu), and anything you couldn't verify by ear. Work on a branch; commit only when the
user asks.
