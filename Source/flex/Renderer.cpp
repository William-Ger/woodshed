#include "Renderer.h"

namespace woodshed::flex
{

namespace
{
constexpr size_t maxRenderBytes = (size_t) 768 << 20;      // older renders go past this

size_t bytesOf (const Renderer::Audio& a)
{
    return a == nullptr ? 0 : (size_t) a->getNumSamples() * sizeof (float);
}
}

struct Renderer::Job final : public juce::ThreadPoolJob
{
    Job (Renderer& r, juce::String k, RenderRequest q, juce::String p, std::shared_ptr<std::atomic<bool>> a,
         std::shared_ptr<std::atomic<bool>> stale)
        : juce::ThreadPoolJob ("Woodshed stretch"), owner (r), key (std::move (k)), request (std::move (q)),
          path (std::move (p)), alive (std::move (a)), superseded (std::move (stale)) {}

    JobStatus runJob() override
    {
        // A newer edit of the same region makes this render pointless: it stops early.
        Rendered result;
        if (! superseded->load())
        {
            owner.analyse (request, path);
            result = render (request, superseded.get());
        }
        const bool finished = result != nullptr && ! shouldExit();
        if (finished)
            owner.store (key, result);
        {
            std::lock_guard<std::mutex> l (owner.lock);
            owner.queued.erase (key);
        }

        // Done (or dropped for a newer edit, which another region on the same file may still
        // want): the engine rebuilds its snapshot and asks again.
        if (finished || superseded->load())
        {
            auto flag = alive;
            auto* renderer = &owner;
            juce::MessageManager::callAsync ([flag, renderer]
            {
                if (flag->load() && renderer->onRendered)
                    renderer->onRendered();
            });
        }
        return jobHasFinished;
    }

    Renderer& owner;
    juce::String key;
    RenderRequest request;
    juce::String path;
    std::shared_ptr<std::atomic<bool>> alive, superseded;
};

Renderer::Renderer() = default;

Renderer::~Renderer()
{
    alive->store (false);
    cancelAll();
    workers.removeAllJobs (true, 10000);
}

void Renderer::cancelAll()
{
    std::lock_guard<std::mutex> l (lock);
    for (auto& [id, flag] : latest)
        flag->store (true);
    latest.clear();
}

juce::String Renderer::keyFor (const juce::ValueTree& region, const juce::File& file, double sampleRate)
{
    const auto mode = modeOf (region.getParent());
    juce::String key;
    key << file.getFullPathName() << "|" << modeId (mode) << "|" << sampleRate << "|" << warpOf (region).encode();
    if (mode != Mode::speed)
        for (auto& n : notesOf (region))
            if (n.isEdited())
                key << "|" << encodeNotes ({ n });
    return key;
}

RenderRequest Renderer::requestFor (const juce::ValueTree& region, const Audio& fileAudio, double sampleRate)
{
    RenderRequest rq;
    rq.source = fileAudio;
    rq.sampleRate = sampleRate;
    rq.mode = modeOf (region.getParent());
    rq.warp = warpOf (region);
    rq.notes = notesOf (region);
    return rq;
}

void Renderer::analyse (RenderRequest& rq, const juce::String& path)
{
    bool edits = false;
    for (auto& n : rq.notes)
        edits = edits || n.isEdited();
    if (edits && rq.mode != Mode::speed)
        rq.pitch = analyses.pitch (path, rq.source, rq.sampleRate);
    if (rq.mode != Mode::speed && ! rq.warp.isIdentity())
        rq.transients = analyses.transients (path, rq.source, rq.sampleRate);
}

Renderer::Audio Renderer::audioFor (const juce::ValueTree& region, const juce::File& file, const Audio& fileAudio,
                                    double sampleRate, bool wait)
{
    if (fileAudio == nullptr || ! isProcessed (region))
        return fileAudio;

    const auto key = keyFor (region, file, sampleRate);
    {
        std::lock_guard<std::mutex> l (lock);
        if (auto it = done.find (key); it != done.end())
        {
            recent.erase (std::remove (recent.begin(), recent.end(), key), recent.end());
            recent.push_back (key);
            return it->second;
        }
    }

    if (wait)
    {
        auto request = requestFor (region, fileAudio, sampleRate);
        analyse (request, file.getFullPathName());
        Audio result = render (request);
        if (result != nullptr)
            store (key, result);
        return result != nullptr ? result : fileAudio;
    }

    // Queue the proper render, and play a quick varispeed one meanwhile.
    bool start = false;
    auto stale = std::make_shared<std::atomic<bool>> (false);
    {
        std::lock_guard<std::mutex> l (lock);
        start = queued.insert (key).second;
        if (start)
        {
            auto& flag = latest[(int) region[juce::Identifier ("id")]];
            if (flag != nullptr)
                flag->store (true);
            flag = stale;
        }
    }
    if (start)
        workers.addJob (new Job (*this, key, requestFor (region, fileAudio, sampleRate), file.getFullPathName(), alive, stale), true);

    const auto standInKey = file.getFullPathName() + "|" + warpOf (region).encode();
    std::lock_guard<std::mutex> l (lock);
    auto& standIn = standIns[standInKey];
    if (standIn == nullptr)
        standIn = renderVarispeed (*fileAudio, warpOf (region));
    return standIn;
}

void Renderer::store (const juce::String& key, const Audio& audio)
{
    std::lock_guard<std::mutex> l (lock);
    done[key] = audio;
    recent.erase (std::remove (recent.begin(), recent.end(), key), recent.end());
    recent.push_back (key);
    standIns.clear();

    size_t total = 0;
    for (auto& [k, a] : done)
        total += bytesOf (a);
    while (total > maxRenderBytes && recent.size() > 1)
    {
        const auto oldest = recent.front();
        recent.erase (recent.begin());
        if (auto it = done.find (oldest); it != done.end())
        {
            total -= bytesOf (it->second);
            done.erase (it);
        }
    }
}

int Renderer::pending() const
{
    std::lock_guard<std::mutex> l (lock);
    return (int) queued.size();
}

void Renderer::waitForAll()
{
    while (workers.getNumJobs() > 0)
        juce::Thread::sleep (2);
}

void Renderer::clear()
{
    cancelAll();
    workers.removeAllJobs (true, 10000);
    std::lock_guard<std::mutex> l (lock);
    done.clear();
    standIns.clear();
    recent.clear();
    queued.clear();
    analyses.clear();
}

} // namespace woodshed::flex
