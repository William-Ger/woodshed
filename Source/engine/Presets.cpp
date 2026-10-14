#include "Presets.h"

namespace woodshed
{

namespace
{
constexpr int boardSpots = 10;

bool readPedal (const juce::var& v, bool onBoard, PresetPedal& out, const juce::String& where, juce::StringArray& errors)
{
    auto fail = [&] (const juce::String& why) { errors.add (where + ": " + why); return false; };

    if (! v.isObject())
        return fail ("expected an object");

    out.type = v["type"].toString();
    const auto* t = Registry::get().find (out.type);
    if (t == nullptr)
        return fail ("unknown pedal type \"" + out.type + "\"");

    const auto category = t->info.category;
    const bool rig = category == "Amp" || category == "Cab";
    if (rig == onBoard)
        return fail (out.type + (rig ? " is an amp/cab and can't go on the board" : " is not an amp/cab"));

    if (onBoard)
    {
        if (! v.hasProperty ("spot") || ! juce::isPositiveAndBelow ((int) v["spot"], boardSpots))
            return fail (out.type + " needs a spot from 0 to 9");
        out.spot = (int) v["spot"];
    }

    out.on = v.hasProperty ("on") ? (bool) v["on"] : true;

    const auto& knobs = t->info.knobs;
    for (size_t k = 0; k < out.knobs.size(); ++k)
        out.knobs[k] = k < knobs.size() ? knobs[k].defaultNorm() : 0.5f;

    bool ok = true;
    if (auto* obj = v["knobs"].getDynamicObject())
    {
        for (auto& prop : obj->getProperties())
        {
            const auto id = prop.name.toString();
            const auto it = std::find_if (knobs.begin(), knobs.end(), [&] (const KnobSpec& k) { return k.id == id; });
            if (it == knobs.end())
            {
                juce::StringArray ids;
                for (auto& k : knobs) ids.add (k.id);
                ok = fail (out.type + " has no knob \"" + id + "\" (knobs: " + ids.joinIntoString (", ") + ")");
                continue;
            }

            const auto& spec = *it;
            float real = 0.0f;

            if (spec.isChoice())
            {
                const auto& value = prop.value;
                int index = -1;
                if (value.isString())
                {
                    for (int c = 0; c < spec.choices.size(); ++c)
                        if (spec.choices[c].equalsIgnoreCase (value.toString()))
                            index = c;
                }
                else if (value.isInt() || value.isDouble() || value.isInt64())
                {
                    index = (int) value;
                }

                if (! juce::isPositiveAndBelow (index, spec.choices.size()))
                {
                    ok = fail (out.type + " " + id + " must be one of: " + spec.choices.joinIntoString (", "));
                    continue;
                }
                real = (float) index;
            }
            else
            {
                if (! (prop.value.isInt() || prop.value.isDouble() || prop.value.isInt64()))
                {
                    ok = fail (out.type + " " + id + " must be a number");
                    continue;
                }

                real = (float) (double) prop.value;
                const float slack = 1.0e-4f * (spec.max - spec.min);
                if (real < spec.min - slack || real > spec.max + slack)
                {
                    ok = fail (out.type + " " + id + " = " + juce::String (real) + " is outside "
                               + juce::String (spec.min) + ".." + juce::String (spec.max)
                               + (spec.unit == "%" ? " (\"%\" knobs run 0..1)" : ""));
                    continue;
                }
            }

            out.knobs[(size_t) (it - knobs.begin())] = spec.toNorm (real);
        }
    }
    else if (v.hasProperty ("knobs") && ! v["knobs"].isVoid())
    {
        ok = fail (out.type + " knobs must be an object");
    }

    return ok;
}
}

std::vector<Preset> parsePresets (const juce::String& json, juce::StringArray& errors)
{
    std::vector<Preset> presets;
    juce::var root;
    const auto parsed = juce::JSON::parse (json, root);

    if (parsed.failed())
    {
        errors.add ("JSON: " + parsed.getErrorMessage());
        return presets;
    }

    const auto genre = root["genre"].toString();
    if (genre.isEmpty())
        errors.add ("file has no \"genre\"");

    const auto* list = root["presets"].getArray();
    if (list == nullptr)
    {
        errors.add ("file has no \"presets\" array");
        return presets;
    }

    for (auto& entry : *list)
    {
        Preset p;
        p.genre = genre;
        p.name = entry["name"].toString().trim();
        p.description = entry["description"].toString().trim();

        const auto where = p.name.isEmpty() ? juce::String ("(unnamed preset)") : p.name;
        auto fail = [&] (const juce::String& why) { errors.add (where + ": " + why); };
        bool ok = true;

        if (p.name.isEmpty())                    { fail ("needs a name"); ok = false; }
        if (p.name.length() > 24)                { fail ("name is longer than 24 characters"); ok = false; }
        if (p.description.isEmpty())             { fail ("needs a description"); ok = false; }

        std::array<bool, boardSpots> used {};
        if (const auto* board = entry["board"].getArray())
        {
            if (board->size() > boardSpots)      { fail ("more than 10 board pedals"); ok = false; }

            for (auto& b : *board)
            {
                PresetPedal pedal;
                if (! readPedal (b, true, pedal, where, errors)) { ok = false; continue; }

                if (used[(size_t) pedal.spot])   { fail ("two pedals in spot " + juce::String (pedal.spot)); ok = false; continue; }
                used[(size_t) pedal.spot] = true;
                p.pedals.push_back (pedal);
            }
        }
        else if (entry.hasProperty ("board"))
        {
            fail ("board must be an array");
            ok = false;
        }

        std::sort (p.pedals.begin(), p.pedals.end(), [] (auto& a, auto& b) { return a.spot < b.spot; });

        for (auto* key : { "amp", "cab" })
        {
            PresetPedal rig;
            if (! entry.hasProperty (key))                              { fail (juce::String ("needs an \"") + key + "\""); ok = false; continue; }
            if (! readPedal (entry[key], false, rig, where, errors))    { ok = false; continue; }

            const auto category = Registry::get().find (rig.type)->info.category;
            if (category != (juce::String (key) == "amp" ? "Amp" : "Cab"))
            {
                fail (juce::String ("\"") + key + "\" holds a " + category);
                ok = false;
                continue;
            }

            p.pedals.push_back (rig);
        }

        if (ok)
            presets.push_back (std::move (p));
    }

    return presets;
}

} // namespace woodshed
