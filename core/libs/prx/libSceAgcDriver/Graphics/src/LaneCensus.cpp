#include "prx/libSceAgcDriver/Graphics/include/LaneCensus.hpp"
#include <algorithm>
#include <cstdio>
#include <string>

namespace AgcDriver::Graphics {

namespace {

constexpr std::size_t laneIndex(Lane lane) {
    return static_cast<std::size_t>(lane);
}

constexpr Lane other(Lane lane) {
    return lane == Lane::G ? Lane::C : Lane::G;
}

double mean(std::uint64_t total, std::uint64_t count) {
    return count == 0 ? 0.0 : static_cast<double>(total) / static_cast<double>(count);
}

}

void LaneCensus::Submit(SubmitCause cause, Lane lane, bool passEnded, bool hostRead, bool copiesFlushed) {
    std::lock_guard lock(mutex);
    ++counts.causes[static_cast<std::size_t>(cause)];
    ++counts.submits[laneIndex(lane)];
    if (passEnded) ++counts.passEnded;
    if (hostRead) ++counts.hostRead;
    if (copiesFlushed) ++counts.copiesFlushed;
}

void LaneCensus::Item(Lane lane) {
    std::lock_guard lock(mutex);
    ++counts.items[laneIndex(lane)];
    if (!started || lane == current) {
        started = true;
        current = lane;
        ++run;
        if (lane == Lane::G && slackOpen) ++slackCount;
        return;
    }
    ++counts.switches;
    ++counts.runs[laneIndex(current)];
    counts.longest[laneIndex(current)] = std::max(counts.longest[laneIndex(current)], run);
    if (lane == Lane::G) {
        // A C run ended: G's items count as slack until G takes an edge on C.
        slackOpen = !cRunEdged;
        slackCount = slackOpen ? 1 : 0;
    } else {
        // A new C run began before G took an edge on the last one.
        if (slackOpen) {
            ++counts.slackUnmet;
            counts.slackUnmetItems += slackCount;
        }
        slackOpen = false;
        slackCount = 0;
        cRunEdged = false;
    }
    current = lane;
    run = 1;
}

void LaneCensus::Commands(Lane lane, bool passOpen, bool copiesQueued) {
    std::lock_guard lock(mutex);
    // The pass and the copies are the previous item's lane's work (approximation: a copy queued
    // earlier by this lane counts too).
    if (!started || lane == current) return;
    if (passOpen) ++counts.crossPasses[laneIndex(lane)];
    if (copiesQueued) ++counts.crossCopies[laneIndex(lane)];
}

void LaneCensus::LabelHit(Lane waiter, Lane producer, LabelState state, std::uint64_t producerSerial) {
    if (waiter == producer) return;
    std::lock_guard lock(mutex);
    ++counts.crossHits[laneIndex(waiter)][static_cast<std::size_t>(state)];
    auto& newest = edge[laneIndex(waiter)][laneIndex(producer)];
    newest = std::max(newest, producerSerial);
    if (waiter != Lane::G || cRunEdged || !started) return;
    // G's edge on C (approximation: on the newest C run, whatever batch the label came from).
    if (slackOpen) {
        ++counts.slackEdges;
        counts.slackItems += slackCount;
        slackOpen = false;
        slackCount = 0;
        cRunEdged = true;
    } else if (current == Lane::C) {
        // Taken before G recorded anything after the run: no slack.
        ++counts.slackEdges;
        cRunEdged = true;
    }
}

void LaneCensus::access(Lane lane, std::uint64_t serial, std::uint64_t begin, std::uint64_t end, bool write) {
    if (end <= begin) return;
    const auto mine = laneIndex(lane), theirs = laneIndex(other(lane));
    // Covered: retired, or at or before the newest batch of the other lane this lane waited on.
    const auto covered = std::max(retired, edge[mine][theirs]);
    const auto first = begin >> BlockShift;
    const auto count = std::min<std::uint64_t>(((end - 1) >> BlockShift) - first + 1, MaxBlocks);
    bool raw = false, waw = false, war = false;
    for (auto block = first; block < first + count; ++block) {
        auto& marks = blocks[block];
        if (write) {
            waw = waw || marks.write[theirs] > covered;
            war = war || marks.read[theirs] > covered;
            marks.write[mine] = std::max(marks.write[mine], serial);
        } else {
            raw = raw || marks.write[theirs] > covered;
            marks.read[mine] = std::max(marks.read[mine], serial);
        }
    }
    auto& hazards = counts.hazards[mine];
    if (raw) ++hazards[static_cast<std::size_t>(Hazard::Raw)];
    if (waw) ++hazards[static_cast<std::size_t>(Hazard::Waw)];
    if (war) ++hazards[static_cast<std::size_t>(Hazard::War)];
}

void LaneCensus::Access(Lane lane, std::uint64_t serial, std::uint64_t begin, std::uint64_t end, bool write) {
    std::lock_guard lock(mutex);
    access(lane, serial, begin, end, write);
}

void LaneCensus::Accesses(Lane lane, std::uint64_t serial, std::span<const std::pair<std::uint64_t, std::uint64_t>> ranges, bool write) {
    std::lock_guard lock(mutex);
    for (const auto& [begin, end] : ranges) access(lane, serial, begin, end, write);
}

void LaneCensus::Retired(std::uint64_t serial) {
    std::lock_guard lock(mutex);
    retired = std::max(retired, serial);
}

void LaneCensus::ShadowRegistered(Lane lane, std::uint64_t begin, std::uint64_t end) {
    std::lock_guard lock(mutex);
    // The registry starts over at its own bound; entries it dropped otherwise linger until then.
    if (shadows.size() >= MaxShadows) shadows.clear();
    shadows[{begin, end}] = lane;
}

void LaneCensus::ShadowTaken(Lane lane, std::uint64_t begin, std::uint64_t end) {
    std::lock_guard lock(mutex);
    ++counts.shadowTakes;
    const auto found = shadows.find({begin, end});
    if (found == shadows.end()) return;
    if (found->second != lane) ++counts.crossShadowTakes;
    shadows.erase(found);
}

LaneCensus::Counts LaneCensus::Take() {
    std::lock_guard lock(mutex);
    const auto taken = counts;
    counts = Counts{};
    for (auto it = blocks.begin(); it != blocks.end();) {
        const auto& marks = it->second;
        if (std::max({marks.write[0], marks.write[1], marks.read[0], marks.read[1]}) <= retired) it = blocks.erase(it);
        else ++it;
    }
    return taken;
}

std::size_t LaneCensus::Blocks() {
    std::lock_guard lock(mutex);
    return blocks.size();
}

std::string LaneCensus::CausesLine(const Counts& counts) {
    std::string causes;
    for (std::size_t cause = 0; cause < SubmitCauses; ++cause) {
        if (counts.causes[cause] == 0) continue;
        char item[48];
        std::snprintf(item, sizeof(item), "%s %s %llu", causes.empty() ? "" : ",", SubmitCauseNames[cause], static_cast<unsigned long long>(counts.causes[cause]));
        causes += item;
    }
    if (causes.empty()) causes = " none";
    char text[320];
    std::snprintf(text, sizeof(text), "; by lane: G %llu, C %llu; submits that ended a render pass %llu, recorded the host-read barrier %llu, flushed deferred copies %llu", static_cast<unsigned long long>(counts.submits[0]), static_cast<unsigned long long>(counts.submits[1]), static_cast<unsigned long long>(counts.passEnded), static_cast<unsigned long long>(counts.hostRead), static_cast<unsigned long long>(counts.copiesFlushed));
    return "[recorder] submit causes (10 s, APS5_LANE_CENSUS):" + causes + text;
}

std::string LaneCensus::LanesLine(const Counts& counts) {
    const auto hits = [&](Lane waiter, LabelState state) { return static_cast<unsigned long long>(counts.crossHits[laneIndex(waiter)][static_cast<std::size_t>(state)]); };
    const auto hazard = [&](Lane lane, Hazard kind) { return static_cast<unsigned long long>(counts.hazards[laneIndex(lane)][static_cast<std::size_t>(kind)]); };
    char text[2048];
    std::snprintf(text, sizeof(text),
                  "[recorder] lanes (10 s, APS5_LANE_CENSUS): items G %llu / C %llu, %llu switches, runs G %llu / C %llu (mean %.1f / %.1f items, longest %llu / %llu); "
                  "Commands() after the other lane's item: passes ended by G %llu / C %llu, copies flushed by G %llu / C %llu; "
                  "cross-lane label hits (open batch / in flight / late): G on C %llu / %llu / %llu, C on G %llu / %llu / %llu; "
                  "hazards on the other lane's unretired work past its label edge (64 KiB blocks): G RAW %llu WAW %llu WAR %llu, C RAW %llu WAW %llu WAR %llu; "
                  "staged shadow takes %llu (%llu registered by the other lane); "
                  "slack: %llu C runs met by a G edge after %.1f G items (mean), %llu followed by the next C run without one (%.1f G items)",
                  static_cast<unsigned long long>(counts.items[0]), static_cast<unsigned long long>(counts.items[1]), static_cast<unsigned long long>(counts.switches),
                  static_cast<unsigned long long>(counts.runs[0]), static_cast<unsigned long long>(counts.runs[1]), mean(counts.items[0], counts.runs[0]), mean(counts.items[1], counts.runs[1]),
                  static_cast<unsigned long long>(counts.longest[0]), static_cast<unsigned long long>(counts.longest[1]),
                  static_cast<unsigned long long>(counts.crossPasses[0]), static_cast<unsigned long long>(counts.crossPasses[1]), static_cast<unsigned long long>(counts.crossCopies[0]), static_cast<unsigned long long>(counts.crossCopies[1]),
                  hits(Lane::G, LabelState::Open), hits(Lane::G, LabelState::InFlight), hits(Lane::G, LabelState::Late), hits(Lane::C, LabelState::Open), hits(Lane::C, LabelState::InFlight), hits(Lane::C, LabelState::Late),
                  hazard(Lane::G, Hazard::Raw), hazard(Lane::G, Hazard::Waw), hazard(Lane::G, Hazard::War), hazard(Lane::C, Hazard::Raw), hazard(Lane::C, Hazard::Waw), hazard(Lane::C, Hazard::War),
                  static_cast<unsigned long long>(counts.shadowTakes), static_cast<unsigned long long>(counts.crossShadowTakes),
                  static_cast<unsigned long long>(counts.slackEdges), mean(counts.slackItems, counts.slackEdges), static_cast<unsigned long long>(counts.slackUnmet), mean(counts.slackUnmetItems, counts.slackUnmet));
    return text;
}

LaneCensus& LaneCensus::Shared() {
    // Never destroyed: a late hook may run after static destructors began.
    static auto* census = new LaneCensus;
    return *census;
}

}
