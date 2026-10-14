#include "../engine/Registry.h"

namespace woodshed
{
namespace
{

//==============================================================================
// ROOM: stereo room reverb (takes mono in, gives stereo out).
class RoomReverb final : public Pedal
{
public:
    bool producesStereo() const override        { return true; }
    void prepared() override                    { reverb.setSampleRate (sampleRate); }
    void reset() override                       { reverb.reset(); }

    void update() override
    {
        juce::Reverb::Parameters p;
        p.roomSize = knob (0);
        p.damping  = knob (1);
        p.wetLevel = knob (2) * 0.3f;
        p.dryLevel = 0.5f * (1.0f - 0.3f * knob (2));   // juce::Reverb doubles the dry signal internally
        p.width    = 1.0f;
        reverb.setParameters (p);
    }

    void processChannel (int, float*, int) override {}
    void processStereo (float* left, float* right, int n) override   { reverb.processStereo (left, right, n); }

private:
    juce::Reverb reverb;
};

} // namespace

void registerReverbPedals (Registry& r)
{
    r.add<RoomReverb> ({ "reverb.room", "ROOM", "reverb", "Reverb",
                         "A natural-sounding room around your guitar",
                         juce::Colour (0xffd8cdb4), juce::Colour (0xff2a2520),
                         { knob ("size", "Size", 0.0f, 1.0f, 0.55f, "%"),
                           knob ("damp", "Damp", 0.0f, 1.0f, 0.45f, "%"),
                           knob ("mix", "Mix", 0.0f, 1.0f, 0.22f, "%") } });
}

} // namespace woodshed
