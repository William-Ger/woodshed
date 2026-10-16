#include "ScoreRender.h"

namespace woodshed::score
{

namespace
{
using P = juce::Point<float>;

/** A calligraphic stroke: a centre line whose width changes along it, built as one filled
    outline. The clefs, rests, flat and brace are drawn with these. */
class Ribbon
{
public:
    Ribbon (P start, float w)                           { points.push_back ({ start, w }); }

    Ribbon& cubic (P c1, P c2, P end, float w, float bulge = 0.0f, int steps = 28)
    {
        const auto p0 = points.back().p;
        const float w0 = points.back().w;
        for (int i = 1; i <= steps; ++i)
        {
            const float t = (float) i / (float) steps, u = 1.0f - t;
            const auto p = p0 * (u * u * u) + c1 * (3.0f * u * u * t) + c2 * (3.0f * u * t * t) + end * (t * t * t);
            const float s = t * t * (3.0f - 2.0f * t);
            points.push_back ({ p, w0 + (w - w0) * s + bulge * std::sin (juce::MathConstants<float>::pi * t) });
        }
        return *this;
    }

    Ribbon& line (P end, float w, int steps = 8)
    {
        const auto p0 = points.back().p;
        return cubic (p0 + (end - p0) / 3.0f, p0 + (end - p0) * (2.0f / 3.0f), end, w, 0.0f, steps);
    }

    juce::Path build (bool roundEnds = false) const
    {
        const size_t n = points.size();
        std::vector<P> left (n), right (n);
        for (size_t i = 0; i < n; ++i)
        {
            auto dir = points[juce::jmin (i + 1, n - 1)].p - points[i > 0 ? i - 1 : 0].p;
            const float len = dir.getDistanceFromOrigin();
            dir = len > 1.0e-6f ? dir / len : P (1.0f, 0.0f);
            const P normal (-dir.y, dir.x);
            left[i] = points[i].p + normal * (points[i].w * 0.5f);
            right[i] = points[i].p - normal * (points[i].w * 0.5f);
        }
        juce::Path path;
        path.startNewSubPath (left[0]);
        for (size_t i = 1; i < n; ++i)
            path.lineTo (left[i]);
        for (size_t i = n; i-- > 0;)
            path.lineTo (right[i]);
        path.closeSubPath();
        if (roundEnds)
        {
            const auto& a = points.front();
            const auto& b = points.back();
            path.addEllipse (a.p.x - a.w * 0.5f, a.p.y - a.w * 0.5f, a.w, a.w);
            path.addEllipse (b.p.x - b.w * 0.5f, b.p.y - b.w * 0.5f, b.w, b.w);
        }
        return path;
    }

private:
    struct Point { P p; float w; };
    std::vector<Point> points;
};

juce::Path ellipse (float cx, float cy, float w, float h, float angle)
{
    juce::Path p;
    p.addEllipse (-w * 0.5f, -h * 0.5f, w, h);
    p.applyTransform (juce::AffineTransform::rotation (angle).translated (cx, cy));
    return p;
}

juce::Path stroked (const juce::Path& centre, float thickness, juce::PathStrokeType::EndCapStyle cap = juce::PathStrokeType::butt)
{
    juce::Path out;
    juce::PathStrokeType (thickness, juce::PathStrokeType::curved, cap).createStrokedPath (out, centre, {}, 80.0f);
    return out;
}

void addQuad (juce::Path& p, P a, P b, P c, P d)
{
    p.startNewSubPath (a);
    p.lineTo (b);
    p.lineTo (c);
    p.lineTo (d);
    p.closeSubPath();
}

/** Sharps' and flats' staff steps in a key signature, for a treble staff (bass: two octaves down). */
constexpr int sharpSteps[7] { 38, 35, 39, 36, 33, 37, 34 };
constexpr int flatSteps[7]  { 34, 37, 33, 36, 32, 35, 31 };
} // namespace

//==============================================================================
namespace glyphs
{
// Noteheads: origin at the left edge, centred vertically on the note's line or space.
const juce::Path& filledHead()
{
    static const juce::Path p = ellipse (headWidth * 0.5f, 0.0f, 1.24f, 0.88f, -0.36f);
    return p;
}

const juce::Path& halfHead()
{
    static const juce::Path p = []
    {
        auto outer = ellipse (headWidth * 0.5f, 0.0f, 1.26f, 0.90f, -0.36f);
        outer.addPath (ellipse (headWidth * 0.5f, 0.0f, 1.0f, 0.38f, -0.58f));
        outer.setUsingNonZeroWinding (false);
        return outer;
    }();
    return p;
}

const juce::Path& wholeHead()
{
    static const juce::Path p = []
    {
        auto outer = ellipse (wholeHeadWidth * 0.5f, 0.0f, wholeHeadWidth, 1.0f, 0.0f);
        outer.addPath (ellipse (wholeHeadWidth * 0.5f, 0.0f, 0.86f, 0.52f, 1.05f));
        outer.setUsingNonZeroWinding (false);
        return outer;
    }();
    return p;
}

const juce::Path& crossHead()
{
    static const juce::Path p = []
    {
        juce::Path c;
        c.startNewSubPath (0.12f, -0.46f);
        c.lineTo (headWidth - 0.12f, 0.46f);
        c.startNewSubPath (0.12f, 0.46f);
        c.lineTo (headWidth - 0.12f, -0.46f);
        return stroked (c, 0.17f, juce::PathStrokeType::rounded);
    }();
    return p;
}

// Clefs: origin at the clef's left; y = 0 on the G line (treble), the F line (bass) or the middle line.
const juce::Path& trebleClef()
{
    static const juce::Path p = []
    {
        Ribbon r ({ 1.42f, 0.18f }, 0.10f);
        r.cubic ({ 1.50f, -0.42f }, { 0.75f, -0.62f }, { 0.52f, 0.02f }, 0.22f)          // over the inner curl
         .cubic ({ 0.32f, 0.78f }, { 1.30f, 1.30f }, { 2.02f, 0.82f }, 0.30f, 0.08f)     // down the left, round the bottom
         .cubic ({ 2.70f, 0.30f }, { 2.40f, -1.05f }, { 1.48f, -1.18f }, 0.20f, 0.08f)   // up the right side
         .cubic ({ 0.42f, -1.38f }, { 0.20f, -2.55f }, { 1.02f, -3.30f }, 0.30f, 0.06f)  // the big sweep to the upper left
         .cubic ({ 1.45f, -3.72f }, { 1.92f, -4.20f }, { 1.70f, -4.92f }, 0.16f)         // up to the top
         .cubic ({ 1.52f, -5.42f }, { 1.02f, -5.05f }, { 1.02f, -4.10f }, 0.12f)         // over the top
         .cubic ({ 1.02f, -2.40f }, { 1.48f, 1.20f }, { 1.52f, 2.25f }, 0.15f)           // the long stem down
         .cubic ({ 1.56f, 3.00f }, { 0.78f, 3.18f }, { 0.62f, 2.62f }, 0.13f);           // the tail
        auto path = r.build();
        path.addEllipse (0.58f, 2.12f, 0.64f, 0.62f);
        return path;
    }();
    return p;
}

const juce::Path& bassClef()
{
    static const juce::Path p = []
    {
        Ribbon r ({ 0.30f, -0.05f }, 0.16f);
        r.cubic ({ 0.36f, -0.82f }, { 0.98f, -1.10f }, { 1.38f, -1.06f }, 0.20f)
         .cubic ({ 2.14f, -1.02f }, { 2.50f, -0.36f }, { 2.40f, 0.36f }, 0.44f)
         .cubic ({ 2.26f, 1.42f }, { 1.30f, 2.36f }, { 0.12f, 2.92f }, 0.07f);
        auto path = r.build();
        path.addEllipse (0.06f, -0.38f, 0.78f, 0.78f);
        path.addEllipse (2.80f, -0.70f, 0.38f, 0.38f);
        path.addEllipse (2.80f, 0.32f, 0.38f, 0.38f);
        return path;
    }();
    return p;
}

const juce::Path& percussionClef()
{
    static const juce::Path p = []
    {
        juce::Path c;
        c.addRectangle (0.40f, -1.0f, 0.42f, 2.0f);
        c.addRectangle (1.20f, -1.0f, 0.42f, 2.0f);
        return c;
    }();
    return p;
}

// Accidentals: origin at the glyph's centre, on the note's line or space.
const juce::Path& sharp()
{
    static const juce::Path p = []
    {
        juce::Path s;
        s.addRectangle (-0.26f, -1.18f, 0.11f, 2.50f);
        s.addRectangle (0.15f, -1.32f, 0.11f, 2.50f);
        for (float yc : { -0.42f, 0.42f })
            addQuad (s, { -0.52f, yc + 0.13f - 0.17f }, { 0.52f, yc - 0.13f - 0.17f }, { 0.52f, yc - 0.13f + 0.17f }, { -0.52f, yc + 0.13f + 0.17f });
        return s;
    }();
    return p;
}

const juce::Path& flat()
{
    static const juce::Path p = []
    {
        juce::Path f;
        f.addRectangle (-0.40f, -1.78f, 0.13f, 2.40f);
        Ribbon r ({ -0.31f, -0.04f }, 0.06f);
        r.cubic ({ 0.04f, -0.64f }, { 0.70f, -0.58f }, { 0.50f, -0.06f }, 0.32f)
         .cubic ({ 0.38f, 0.26f }, { 0.02f, 0.46f }, { -0.31f, 0.62f }, 0.11f);
        f.addPath (r.build());
        return f;
    }();
    return p;
}

const juce::Path& natural()
{
    static const juce::Path p = []
    {
        juce::Path n;
        n.addRectangle (-0.34f, -1.34f, 0.12f, 1.84f);
        n.addRectangle (0.22f, -0.50f, 0.12f, 1.84f);
        for (float yc : { -0.34f, 0.34f })
            addQuad (n, { -0.34f, yc + 0.09f - 0.16f }, { 0.34f, yc - 0.09f - 0.16f }, { 0.34f, yc - 0.09f + 0.16f }, { -0.34f, yc + 0.09f + 0.16f });
        return n;
    }();
    return p;
}

// A flag for an up stem: origin at the stem's tip (the stem's centre line). Flip it for down stems.
const juce::Path& flag()
{
    static const juce::Path p = []
    {
        juce::Path f;
        f.startNewSubPath (-0.06f, 0.0f);
        f.cubicTo (0.10f, 0.95f, 1.18f, 1.18f, 0.98f, 2.45f);
        f.cubicTo (0.94f, 2.72f, 0.86f, 2.92f, 0.74f, 3.12f);
        f.cubicTo (0.92f, 2.52f, 0.78f, 1.62f, -0.06f, 1.10f);
        f.closeSubPath();
        return f;
    }();
    return p;
}

// Rests: origin at the rest's horizontal centre, on the staff's middle line.
const juce::Path& wholeRest()
{
    static const juce::Path p = []
    {
        juce::Path r;
        r.addRectangle (-0.62f, -1.0f, 1.24f, 0.52f);
        return r;
    }();
    return p;
}

const juce::Path& halfRest()
{
    static const juce::Path p = []
    {
        juce::Path r;
        r.addRectangle (-0.62f, -0.52f, 1.24f, 0.52f);
        return r;
    }();
    return p;
}

const juce::Path& quarterRest()
{
    static const juce::Path p = []
    {
        Ribbon r ({ -0.20f, -1.52f }, 0.08f);
        r.line ({ 0.36f, -0.78f }, 0.20f)
         .cubic ({ 0.12f, -0.52f }, { -0.18f, -0.30f }, { -0.18f, -0.10f }, 0.40f, 0.04f)
         .line ({ 0.32f, 0.58f }, 0.12f)
         .cubic ({ -0.10f, 0.38f }, { -0.48f, 0.66f }, { 0.04f, 1.42f }, 0.22f, 0.06f);
        return r.build();
    }();
    return p;
}

const juce::Path& shortRest (int flags)
{
    static const auto make = [] (int n)
    {
        juce::Path path;
        const P top (0.42f + 0.16f * (float) (n - 1), -0.66f - 1.0f * (float) (n - 1));
        const P bottom (n == 3 ? -0.18f : -0.04f, n == 3 ? 1.96f : 1.02f);
        Ribbon stem (top, 0.12f);
        stem.line (bottom, 0.15f);
        path.addPath (stem.build());
        for (int k = 0; k < n; ++k)
        {
            const float t = (float) k * 1.0f / (bottom.y - top.y);
            const P attach = top + (bottom - top) * t;
            const P ball = attach + P (-0.72f, 0.20f);
            Ribbon hook (ball + P (0.06f, 0.12f), 0.08f);
            hook.cubic (ball + P (0.34f, 0.30f), attach + P (-0.14f, 0.12f), attach, 0.13f);
            path.addPath (hook.build());
            path.addEllipse (ball.x - 0.24f, ball.y - 0.24f, 0.48f, 0.48f);
        }
        return path;
    };
    static const juce::Path paths[3] { make (1), make (2), make (3) };
    return paths[juce::jlimit (1, 3, flags) - 1];
}

const juce::Path& digit (int d)
{
    static const auto make = [] (int n)
    {
        juce::Path c;
        switch (n)
        {
            case 0: c.addEllipse (-0.56f, -0.86f, 1.12f, 1.72f); break;
            case 1:
                c.startNewSubPath (-0.42f, -0.48f); c.lineTo (0.10f, -0.92f); c.lineTo (0.10f, 0.92f);
                c.startNewSubPath (-0.38f, 0.90f); c.lineTo (0.58f, 0.90f);
                break;
            case 2:
                c.startNewSubPath (-0.52f, -0.42f);
                c.cubicTo (-0.48f, -1.02f, 0.56f, -1.04f, 0.54f, -0.42f);
                c.cubicTo (0.52f, 0.08f, -0.50f, 0.42f, -0.56f, 0.92f);
                c.lineTo (0.62f, 0.92f);
                break;
            case 3:
                c.startNewSubPath (-0.52f, -0.58f);
                c.cubicTo (-0.36f, -1.04f, 0.52f, -1.02f, 0.48f, -0.48f);
                c.cubicTo (0.44f, -0.10f, 0.12f, -0.04f, -0.10f, -0.04f);
                c.cubicTo (0.30f, -0.04f, 0.58f, 0.16f, 0.58f, 0.44f);
                c.cubicTo (0.58f, 1.06f, -0.44f, 1.06f, -0.58f, 0.62f);
                break;
            case 4:
                c.startNewSubPath (0.30f, 0.94f); c.lineTo (0.30f, -0.94f); c.lineTo (-0.62f, 0.40f); c.lineTo (0.68f, 0.40f);
                break;
            case 5:
                c.startNewSubPath (0.52f, -0.90f); c.lineTo (-0.40f, -0.90f); c.lineTo (-0.50f, -0.06f);
                c.cubicTo (-0.02f, -0.34f, 0.60f, -0.20f, 0.58f, 0.36f);
                c.cubicTo (0.56f, 1.06f, -0.40f, 1.06f, -0.58f, 0.64f);
                break;
            case 6:
                c.startNewSubPath (0.42f, -0.86f);
                c.cubicTo (-0.26f, -1.06f, -0.58f, -0.40f, -0.56f, 0.30f);
                c.addEllipse (-0.56f, -0.14f, 1.12f, 1.06f);
                break;
            case 7:
                c.startNewSubPath (-0.58f, -0.90f); c.lineTo (0.60f, -0.90f);
                c.cubicTo (0.12f, -0.30f, -0.14f, 0.36f, -0.14f, 0.94f);
                break;
            case 8:
                c.addEllipse (-0.44f, -0.94f, 0.88f, 0.88f);
                c.addEllipse (-0.54f, -0.06f, 1.08f, 1.0f);
                break;
            case 9:
            default:
                c.startNewSubPath (-0.42f, 0.86f);
                c.cubicTo (0.26f, 1.06f, 0.58f, 0.40f, 0.56f, -0.30f);
                c.addEllipse (-0.56f, -0.92f, 1.12f, 1.06f);
                break;
        }
        return stroked (c, 0.36f);
    };
    static const juce::Path paths[10] { make (0), make (1), make (2), make (3), make (4), make (5), make (6), make (7), make (8), make (9) };
    return paths[juce::jlimit (0, 9, d)];
}

// The brace: x from about -0.04 to 0.04 of its height... drawn in a unit-high box, origin at its top right.
const juce::Path& brace()
{
    static const juce::Path p = []
    {
        Ribbon r ({ 0.0f, 0.0f }, 0.004f);
        r.cubic ({ -0.075f, 0.06f }, { 0.0f, 0.36f }, { -0.07f, 0.5f }, 0.008f, 0.03f, 40)
         .cubic ({ 0.0f, 0.64f }, { -0.075f, 0.94f }, { 0.0f, 1.0f }, 0.004f, 0.03f, 40);
        return r.build();
    }();
    return p;
}
} // namespace glyphs

//==============================================================================
float Renderer::headWidthOf (const Element& e) const
{
    return (e.base >= wholeNote ? glyphs::wholeHeadWidth : glyphs::headWidth) * sp;
}

float Renderer::yForStep (int staff, int step) const
{
    const auto clef = layout.staves[(size_t) juce::jlimit (0, (int) layout.staves.size() - 1, staff)];
    return staffTops[(size_t) staff] + (float) (topLineStep (clef) - step) * sp * 0.5f;
}

float Renderer::middleY (int staff) const
{
    return staffTops[(size_t) staff] + 2.0f * sp;
}

int Renderer::staffAt (float y) const
{
    if (staffTops.empty())
        return 0;
    int best = 0;
    float bestDistance = std::numeric_limits<float>::max();
    for (size_t i = 0; i < staffTops.size(); ++i)
    {
        const float d = std::abs (y - middleY ((int) i));
        if (d < bestDistance)
        {
            bestDistance = d;
            best = (int) i;
        }
    }
    return best;
}

int Renderer::stepAt (int staff, float y) const
{
    const auto clef = layout.staves[(size_t) staff];
    return topLineStep (clef) - juce::roundToInt ((y - staffTops[(size_t) staff]) / (sp * 0.5f));
}

float Renderer::xForTick (double tick) const
{
    if (layout.bars.empty() || barX.empty())
        return contentLeft;
    const int b = layout.barAt ((juce::int64) std::floor (tick));
    const auto& bar = layout.bars[(size_t) b];
    return barX[(size_t) b] + padLeft + (float) ((tick - (double) bar.start) * ppt);
}

double Renderer::tickAtX (float x) const
{
    if (layout.bars.empty() || barX.empty())
        return 0.0;
    size_t b = 0;
    while (b + 1 < barX.size() && x >= barX[b + 1])
        ++b;
    const auto& bar = layout.bars[b];
    const double t = (double) bar.start + (double) (x - barX[b] - padLeft) / ppt;
    if (b + 1 < barX.size() || x < barX[b] + barWidth[b])
        return juce::jlimit ((double) bar.start, (double) (bar.start + bar.length - 1), t);
    return t;
}

int Renderer::noteAt (juce::Point<float> p) const
{
    for (auto it = hits.rbegin(); it != hits.rend(); ++it)
        if (it->area.contains (p))
            return it->id;
    return -1;
}

juce::Rectangle<float> Renderer::noteBounds (int id) const
{
    for (auto& h : hits)
        if (h.id == id)
            return h.area;
    return {};
}

//==============================================================================
void Renderer::setLayout (const Layout& l, float staffSpace, double pixelsPerTick)
{
    layout = l;
    sp = juce::jmax (3.0f, staffSpace);
    ppt = juce::jmax (0.001, pixelsPerTick);
    padLeft = 2.0f * sp;
    padRight = 0.8f * sp;

    if (layout.staves.empty())
    {
        staffTops.clear();
        barX.clear();
        barWidth.clear();
        geometry.clear();
        hits.clear();
        width = height = contentLeft = 0.0f;
        return;
    }

    // Vertically: room for ledger lines and stems above and below each staff.
    const size_t numStaves = layout.staves.size();
    std::vector<float> above (numStaves, 0.0f), below (numStaves, 0.0f);
    for (auto& e : layout.elements)
        for (auto& h : e.notes)
        {
            const auto clef = layout.staves[(size_t) e.staff];
            above[(size_t) e.staff] = juce::jmax (above[(size_t) e.staff], (float) (h.step - topLineStep (clef)) * 0.5f * sp);
            below[(size_t) e.staff] = juce::jmax (below[(size_t) e.staff], (float) (bottomLineStep (clef) - h.step) * 0.5f * sp);
        }
    staffTops.assign (numStaves, 0.0f);
    float y = 4.0f * sp + above[0];
    for (size_t i = 0; i < numStaves; ++i)
    {
        if (i > 0)
            y += juce::jmax (6.5f * sp, below[i - 1] + above[i] + 3.0f * sp);
        staffTops[i] = y;
        y += 4.0f * sp;
    }
    height = y + juce::jmax (4.0f * sp, below.back() + 3.0f * sp);

    // Horizontally: the system's start (brace, clef, key, time), then the bars.
    const bool grand = numStaves > 1;
    const float systemX = (grand ? 1.8f : 0.8f) * sp;
    clefX = systemX + 0.5f * sp;
    const auto firstClef = layout.staves.front();
    keyX = clefX + (firstClef == Clef::percussion ? 2.4f : firstClef == Clef::bass ? 3.7f : 3.1f) * sp;
    const int accidentals = std::abs (layout.keyFifths);
    timeX = keyX + (float) accidentals * 0.95f * sp + (accidentals > 0 ? 0.7f * sp : 0.0f);
    const int digits = juce::jmax (juce::String (layout.beatsPerBar).length(), juce::String (layout.beatUnit).length());
    contentLeft = timeX + (float) digits * 1.3f * sp + 0.9f * sp;

    barX.clear();
    barWidth.clear();
    float x = contentLeft;
    for (auto& bar : layout.bars)
    {
        barX.push_back (x);
        barWidth.push_back (padLeft + (float) ((double) bar.length * ppt) + padRight);
        x += barWidth.back();
    }
    width = x + 2.0f * sp;

    placeElements();
    placeBeams();
}

void Renderer::placeElements()
{
    geometry.assign (layout.elements.size(), {});
    hits.clear();
    const float stemT = juce::jmax (1.0f, 0.12f * sp);

    for (size_t i = 0; i < layout.elements.size(); ++i)
    {
        const auto& e = layout.elements[i];
        auto& g = geometry[i];
        g.x = e.barRest ? barX[(size_t) e.bar] + barWidth[(size_t) e.bar] * 0.5f : xForTick ((double) e.start);
        if (e.rest || e.notes.empty())
        {
            g.left = g.x;
            g.right = g.x + 1.2f * sp;
            continue;
        }

        const float hw = headWidthOf (e);
        const size_t n = e.notes.size();
        std::vector<bool> displaced (n, false);
        const bool upward = e.stemUp || ! e.hasStem();
        if (upward)
        {
            for (size_t k = 1; k < n; ++k)
                displaced[k] = e.notes[k].step - e.notes[k - 1].step == 1 && ! displaced[k - 1];
        }
        else
        {
            for (size_t k = n - 1; k-- > 0;)
                displaced[k] = e.notes[k + 1].step - e.notes[k].step == 1 && ! displaced[k + 1];
        }

        g.left = std::numeric_limits<float>::max();
        g.right = std::numeric_limits<float>::lowest();
        for (size_t k = 0; k < n; ++k)
        {
            const float shift = displaced[k] ? (upward ? hw - stemT : -(hw - stemT)) : 0.0f;
            const Head h { g.x + shift, yForStep (e.staff, e.notes[k].step), (int) k };
            g.heads.push_back (h);
            g.left = juce::jmin (g.left, h.x);
            g.right = juce::jmax (g.right, h.x + hw);
            hits.push_back ({ juce::Rectangle<float> (h.x - 0.15f * sp, h.y - 0.6f * sp, hw + 0.3f * sp, 1.2f * sp), e.notes[k].id });
        }

        const float lowY = g.heads.front().y, highY = g.heads.back().y;
        const float extra = (float) juce::jmax (0, e.flags() - 1) * 0.55f * sp;
        const float mid = middleY (e.staff);
        if (e.stemUp)
        {
            g.stemX = g.x + hw - stemT * 0.5f;
            g.stemNoteY = lowY;
            g.stemTipY = juce::jmin (highY - 3.5f * sp - extra, mid);
        }
        else
        {
            g.stemX = g.x + stemT * 0.5f;
            g.stemNoteY = highY;
            g.stemTipY = juce::jmax (lowY + 3.5f * sp + extra, mid);
        }
    }
}

void Renderer::placeBeams()
{
    for (auto& b : layout.beams)
    {
        if (b.elements.size() < 2)
            continue;
        int maxFlags = 1;
        for (int i : b.elements)
            maxFlags = juce::jmax (maxFlags, layout.elements[(size_t) i].flags());
        const float length = 3.25f * sp + (float) (maxFlags - 1) * 0.75f * sp;

        std::vector<float> xs, wanted;
        for (int i : b.elements)
        {
            const auto& e = layout.elements[(size_t) i];
            const auto& g = geometry[(size_t) i];
            const float mid = middleY (e.staff);
            xs.push_back (g.stemX);
            if (b.stemUp)
                wanted.push_back (juce::jmin (g.heads.back().y - length, mid));
            else
                wanted.push_back (juce::jmax (g.heads.front().y + length, mid));
        }

        const float dx = juce::jmax (1.0f, xs.back() - xs.front());
        float slope = (wanted.back() - wanted.front()) / dx;
        const float limit = juce::jmin (0.22f, sp / dx);
        slope = juce::jlimit (-limit, limit, slope);

        float intercept = b.stemUp ? std::numeric_limits<float>::max() : std::numeric_limits<float>::lowest();
        for (size_t k = 0; k < xs.size(); ++k)
        {
            const float c = wanted[k] - slope * (xs[k] - xs.front());
            intercept = b.stemUp ? juce::jmin (intercept, c) : juce::jmax (intercept, c);
        }
        for (size_t k = 0; k < xs.size(); ++k)
            geometry[(size_t) b.elements[k]].stemTipY = intercept + slope * (xs[k] - xs.front());
    }
}

//==============================================================================
void Renderer::fill (juce::Graphics& g, const juce::Path& path, float x, float y, float scale, bool flipY) const
{
    const float s = sp * scale;
    g.fillPath (path, juce::AffineTransform::scale (s, flipY ? -s : s).translated (x, y));
}

void Renderer::paint (juce::Graphics& g, const Style& style, const std::function<bool (int)>& isSelected, juce::Range<float> visible) const
{
    if (layout.staves.empty())
        return;

    drawSystem (g, style, visible);

    const auto inView = [&] (const Geometry& geo)
    {
        return geo.right + 4.0f * sp >= visible.getStart() && geo.left - 4.0f * sp <= visible.getEnd();
    };

    g.setColour (style.ink);
    for (auto& b : layout.beams)
        if (! b.elements.empty() && (inView (geometry[(size_t) b.elements.front()]) || inView (geometry[(size_t) b.elements.back()])))
            drawBeam (g, b);

    for (size_t i = 0; i < layout.elements.size(); ++i)
        if (inView (geometry[i]))
        {
            drawTies (g, style, i);
            drawElement (g, style, i, isSelected);
        }

    drawTuplets (g, style, visible);
}

void Renderer::drawSystem (juce::Graphics& g, const Style& style, juce::Range<float> visible) const
{
    const float lineT = juce::jmax (1.0f, 0.11f * sp);
    const float barT = juce::jmax (1.0f, 0.16f * sp);
    const bool grand = layout.staves.size() > 1;
    const float systemX = (grand ? 1.8f : 0.8f) * sp;
    const float top = staffTops.front(), bottom = staffTops.back() + 4.0f * sp;
    const float right = barX.empty() ? width : barX.back() + barWidth.back();
    const float from = juce::jmax (systemX, visible.getStart() - 2.0f), to = juce::jmin (right, visible.getEnd() + 2.0f);

    // Staff lines.
    g.setColour (style.lines);
    for (auto staffTop : staffTops)
        for (int line = 0; line < 5; ++line)
            g.fillRect (juce::Rectangle<float> (from, staffTop + (float) line * sp - lineT * 0.5f, to - from, lineT));

    // The system's opening line and, for a grand staff, its brace.
    g.setColour (style.ink);
    g.fillRect (juce::Rectangle<float> (systemX - barT * 0.5f, top - lineT * 0.5f, barT, bottom - top + lineT));
    if (grand)
    {
        const float h = bottom - top;
        g.fillPath (glyphs::brace(), juce::AffineTransform::scale (h, h).translated (systemX - 0.45f * sp, top));
    }

    // Clef, key signature and time signature on each staff.
    for (size_t s = 0; s < layout.staves.size(); ++s)
    {
        const auto clef = layout.staves[s];
        const float staffTop = staffTops[s];
        switch (clef)
        {
            case Clef::treble:     fill (g, glyphs::trebleClef(), clefX, staffTop + 3.0f * sp); break;
            case Clef::bass:       fill (g, glyphs::bassClef(), clefX, staffTop + 1.0f * sp); break;
            case Clef::percussion:
            default:               fill (g, glyphs::percussionClef(), clefX, staffTop + 2.0f * sp); break;
        }

        if (clef != Clef::percussion)
        {
            const int count = juce::jmin (7, std::abs (layout.keyFifths));
            const int shift = clef == Clef::bass ? -14 : 0;
            for (int k = 0; k < count; ++k)
            {
                const int step = (layout.keyFifths > 0 ? sharpSteps[k] : flatSteps[k]) + shift;
                const float x = keyX + (float) k * 0.95f * sp + 0.5f * sp;
                fill (g, layout.keyFifths > 0 ? glyphs::sharp() : glyphs::flat(), x, yForStep ((int) s, step));
            }
        }

        const auto drawNumber = [&] (int value, float centreY)
        {
            const auto text = juce::String (value);
            const float w = 1.3f * sp;
            const float digitsWidth = w * (float) text.length();
            const int maxDigits = juce::jmax (juce::String (layout.beatsPerBar).length(), juce::String (layout.beatUnit).length());
            float x = timeX + (w * (float) maxDigits - digitsWidth) * 0.5f + w * 0.5f;
            for (auto c : text)
            {
                fill (g, glyphs::digit ((int) (c - '0')), x, centreY);
                x += w;
            }
        };
        drawNumber (layout.beatsPerBar, staffTop + 1.0f * sp);
        drawNumber (layout.beatUnit, staffTop + 3.0f * sp);
    }

    // Bar lines (through both staves of a grand staff), the final double bar and bar numbers.
    for (size_t b = 0; b < barX.size(); ++b)
    {
        const float x = barX[b] + barWidth[b];
        if (x < visible.getStart() - 40.0f || barX[b] > visible.getEnd() + 40.0f)
            continue;
        const bool last = b + 1 == barX.size();
        g.setColour (style.ink);
        if (last)
        {
            g.fillRect (juce::Rectangle<float> (x - 0.5f * sp, top - lineT * 0.5f, 0.5f * sp, bottom - top + lineT));
            g.fillRect (juce::Rectangle<float> (x - 1.0f * sp - barT, top - lineT * 0.5f, barT, bottom - top + lineT));
        }
        else if (grand)
            g.fillRect (juce::Rectangle<float> (x - barT * 0.5f, top - lineT * 0.5f, barT, bottom - top + lineT));
        else
            for (auto staffTop : staffTops)
                g.fillRect (juce::Rectangle<float> (x - barT * 0.5f, staffTop - lineT * 0.5f, barT, 4.0f * sp + lineT));

        g.setColour (style.faint);
        g.setFont (style.numberFont);
        g.drawText (juce::String (layout.bars[b].number), juce::Rectangle<float> (barX[b] + (b == 0 ? 0.0f : -0.4f * sp), top - 2.9f * sp, 40.0f, 1.6f * sp),
                    juce::Justification::centredLeft, false);
    }
}

void Renderer::drawElement (juce::Graphics& g, const Style& style, size_t index, const std::function<bool (int)>& isSelected) const
{
    const auto& e = layout.elements[index];
    const auto& geo = geometry[index];
    const float mid = middleY (e.staff);
    g.setColour (style.ink);

    if (e.rest)
    {
        const float cx = e.barRest ? geo.x : geo.x + 0.6f * sp;
        if (e.barRest || e.base >= wholeNote)  fill (g, glyphs::wholeRest(), cx, mid);
        else if (e.base >= 1920)               fill (g, glyphs::halfRest(), cx, mid);
        else if (e.base >= quarter)            fill (g, glyphs::quarterRest(), cx, mid);
        else                                   fill (g, glyphs::shortRest (e.base >= 480 ? 1 : e.base >= 240 ? 2 : 3), cx, mid);
        for (int d = 0; d < e.dots; ++d)
            g.fillEllipse (cx + (0.95f + 0.5f * (float) d) * sp, mid - 0.5f * sp - 0.18f * sp, 0.36f * sp, 0.36f * sp);
        return;
    }

    const auto clef = layout.staves[(size_t) e.staff];
    const int topLine = topLineStep (clef), bottomLine = bottomLineStep (clef);
    const float hw = headWidthOf (e);
    const float ledgerT = juce::jmax (1.0f, 0.15f * sp);

    // Ledger lines.
    int highest = e.notes.back().step, lowest = e.notes.front().step;
    for (int s = topLine + 2; s <= highest; s += 2)
        g.fillRect (juce::Rectangle<float> (geo.left - 0.38f * sp, yForStep (e.staff, s) - ledgerT * 0.5f, geo.right - geo.left + 0.76f * sp, ledgerT));
    for (int s = bottomLine - 2; s >= lowest; s -= 2)
        g.fillRect (juce::Rectangle<float> (geo.left - 0.38f * sp, yForStep (e.staff, s) - ledgerT * 0.5f, geo.right - geo.left + 0.76f * sp, ledgerT));

    // Stem and flags.
    if (e.hasStem())
    {
        const float stemT = juce::jmax (1.0f, 0.12f * sp);
        const float y1 = juce::jmin (geo.stemNoteY, geo.stemTipY), y2 = juce::jmax (geo.stemNoteY, geo.stemTipY);
        g.fillRect (juce::Rectangle<float> (geo.stemX - stemT * 0.5f, y1, stemT, y2 - y1));
        if (e.beam < 0)
            for (int f = 0; f < e.flags(); ++f)
                fill (g, glyphs::flag(), geo.stemX - stemT * 0.5f, geo.stemTipY + (e.stemUp ? 1.0f : -1.0f) * (float) f * 0.82f * sp, 1.0f, ! e.stemUp);
    }

    // Noteheads, accidentals, dots.
    struct Placed { int column, step; };
    std::vector<Placed> placed;
    for (size_t k = e.notes.size(); k-- > 0;)
    {
        const auto& note = e.notes[k];
        const auto& head = geo.heads[k];
        const bool selected = note.id >= 0 && isSelected != nullptr && isSelected (note.id);
        g.setColour (selected ? style.selected : style.ink);

        const auto& glyph = note.cross ? glyphs::crossHead() : e.base >= wholeNote ? glyphs::wholeHead()
                          : e.base >= 1920 ? glyphs::halfHead() : glyphs::filledHead();
        fill (g, glyph, head.x, head.y);

        if (note.accidental != Accidental::none)
        {
            int column = 0;
            while (std::any_of (placed.begin(), placed.end(), [&] (const Placed& p) { return p.column == column && std::abs (p.step - note.step) < 6; }))
                ++column;
            placed.push_back ({ column, note.step });
            const float x = geo.left - 0.3f * sp - (float) column * 1.15f * sp - 0.55f * sp;
            fill (g, note.accidental == Accidental::sharp ? glyphs::sharp() : note.accidental == Accidental::flat ? glyphs::flat() : glyphs::natural(),
                  x, head.y);
        }

        if (e.dots > 0)
        {
            const bool onLine = (topLine - note.step) % 2 == 0;
            const float y = head.y - (onLine ? 0.5f * sp : 0.0f);
            for (int d = 0; d < e.dots; ++d)
                g.fillEllipse (geo.right + (0.32f + 0.5f * (float) d) * sp, y - 0.18f * sp, 0.36f * sp, 0.36f * sp);
        }
    }
    juce::ignoreUnused (hw);
}

void Renderer::drawBeam (juce::Graphics& g, const Beam& b) const
{
    const float thick = 0.5f * sp, gap = 0.75f * sp;
    const auto& first = geometry[(size_t) b.elements.front()];
    const auto& last = geometry[(size_t) b.elements.back()];
    const float stemT = juce::jmax (1.0f, 0.12f * sp);
    const float x0 = first.stemX - stemT * 0.5f, x1 = last.stemX + stemT * 0.5f;
    const float slope = (last.stemTipY - first.stemTipY) / juce::jmax (1.0f, last.stemX - first.stemX);
    const auto yAt = [&] (float x) { return first.stemTipY + slope * (x - first.stemX); };
    const float dir = b.stemUp ? 1.0f : -1.0f;      // beams stack towards the noteheads

    const auto bar = [&] (float xa, float xb, int level)
    {
        const float off = dir * (float) level * gap;
        const float ya = yAt (xa) + off, yb = yAt (xb) + off;
        juce::Path p;
        addQuad (p, { xa, ya }, { xb, yb }, { xb, yb + dir * thick }, { xa, ya + dir * thick });
        g.fillPath (p);
    };

    bar (x0, x1, 0);

    int maxFlags = 1;
    for (int i : b.elements)
        maxFlags = juce::jmax (maxFlags, layout.elements[(size_t) i].flags());
    for (int level = 2; level <= maxFlags; ++level)
    {
        const size_t n = b.elements.size();
        for (size_t k = 0; k < n;)
        {
            if (layout.elements[(size_t) b.elements[k]].flags() < level)
            {
                ++k;
                continue;
            }
            size_t j = k;
            while (j + 1 < n && layout.elements[(size_t) b.elements[j + 1]].flags() >= level)
                ++j;
            const auto& a = geometry[(size_t) b.elements[k]];
            if (j > k)
                bar (a.stemX - stemT * 0.5f, geometry[(size_t) b.elements[j]].stemX + stemT * 0.5f, level - 1);
            else
            {
                const float stub = juce::jmin (1.1f * sp, 0.5f * std::abs (last.stemX - first.stemX));
                if (k + 1 < n) bar (a.stemX - stemT * 0.5f, a.stemX + stub, level - 1);
                else           bar (a.stemX - stub, a.stemX + stemT * 0.5f, level - 1);
            }
            k = j + 1;
        }
    }
}

void Renderer::drawTies (juce::Graphics& g, const Style& style, size_t index) const
{
    const auto& e = layout.elements[index];
    if (e.rest)
        return;
    const auto& geo = geometry[index];
    const Element* next = index + 1 < layout.elements.size() && layout.elements[index + 1].staff == e.staff ? &layout.elements[index + 1] : nullptr;
    const Geometry* nextGeo = next != nullptr ? &geometry[index + 1] : nullptr;
    const float hw = headWidthOf (e);

    g.setColour (style.ink);
    const size_t n = e.notes.size();
    for (size_t k = 0; k < n; ++k)
    {
        const auto& note = e.notes[k];
        if (! note.tieToNext)
            continue;
        const auto& head = geo.heads[k];
        float x1 = head.x + hw + 0.12f * sp, x2 = x1 + 2.0f * sp;
        if (next != nullptr && ! next->rest)
            for (size_t m = 0; m < next->notes.size(); ++m)
                if (next->notes[m].step == note.step)
                    x2 = nextGeo->heads[m].x - 0.12f * sp;
        if (x2 - x1 < 0.8f * sp)
            x2 = x1 + 0.8f * sp;

        float dir = e.stemUp ? 1.0f : -1.0f;              // away from the stem
        if (n > 1)
            dir = k * 2 + 1 == n ? dir : (k * 2 + 1 > n ? -1.0f : 1.0f);
        const float y = head.y + dir * 0.55f * sp;
        const float h = juce::jmin (1.0f * sp, 0.35f * sp + 0.06f * (x2 - x1));
        const float in = juce::jmin (0.16f * sp, h * 0.4f);
        const float span = x2 - x1;

        juce::Path tie;
        tie.startNewSubPath (x1, y);
        tie.cubicTo (x1 + span * 0.2f, y + dir * h, x2 - span * 0.2f, y + dir * h, x2, y);
        tie.cubicTo (x2 - span * 0.2f, y + dir * (h - in), x1 + span * 0.2f, y + dir * (h - in), x1, y);
        tie.closeSubPath();
        g.fillPath (tie);
    }
}

void Renderer::drawTuplets (juce::Graphics& g, const Style& style, juce::Range<float> visible) const
{
    g.setColour (style.ink);
    const auto beat = layout.beatUnit == 8 && layout.beatsPerBar % 3 == 0 ? (juce::int64) quarter * 3 / 2
                                                                         : (juce::int64) wholeNote / juce::jmax (1, layout.beatUnit);
    for (size_t i = 0; i < layout.elements.size();)
    {
        const auto& e = layout.elements[i];
        if (! e.tuplet)
        {
            ++i;
            continue;
        }
        const auto beatIndex = (e.start - layout.bars[(size_t) e.bar].start) / beat;
        size_t j = i;
        while (j + 1 < layout.elements.size() && layout.elements[j + 1].tuplet && layout.elements[j + 1].staff == e.staff
               && layout.elements[j + 1].bar == e.bar
               && (layout.elements[j + 1].start - layout.bars[(size_t) e.bar].start) / beat == beatIndex)
            ++j;

        const float x = (geometry[i].left + geometry[j].right) * 0.5f;
        if (x > visible.getStart() - 20.0f && x < visible.getEnd() + 20.0f)
        {
            bool up = true;
            float y = staffTops[(size_t) e.staff] - 1.4f * sp;
            float minTip = std::numeric_limits<float>::max(), maxTip = std::numeric_limits<float>::lowest();
            for (size_t k = i; k <= j; ++k)
            {
                const auto& el = layout.elements[k];
                if (el.rest)
                    continue;
                up = el.stemUp;
                minTip = juce::jmin (minTip, geometry[k].stemTipY, geometry[k].heads.back().y);
                maxTip = juce::jmax (maxTip, geometry[k].stemTipY, geometry[k].heads.front().y);
            }
            if (minTip < std::numeric_limits<float>::max())
                y = up ? juce::jmin (minTip - 1.3f * sp, y) : maxTip + 1.3f * sp;
            fill (g, glyphs::digit (3), x, y, 0.62f);
        }
        i = j + 1;
    }
}

} // namespace woodshed::score
