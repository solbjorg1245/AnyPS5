#include "prx/libSceAgcDriver/Execution/include/Driver/Driver.hpp"

namespace AgcDriver::DriverDetail {

void Driver::reportDrawCache(DrawEntryCounters& counters) {
    const auto count = [](std::uint64_t value) { return static_cast<unsigned long long>(value); };
    const auto validated = counters.lookups - counters.absent;
    const auto miss = [&](DrawMiss reason) { return count(counters.misses[static_cast<std::size_t>(reason)]); };
    std::string ranks;
    for (std::size_t rank = 0; rank < dispatchVariants(); ++rank) {
        char text[32];
        std::snprintf(text, sizeof(text), "%s%llu", rank == 0 ? "" : " / ", count(counters.variantHitsByRank[rank]));
        ranks += text;
    }
    std::fprintf(stderr, "[draw-cache] %llu lookups (10 s): %llu no entry, %llu validated in %.1f us each (key, lookup and every stage): %llu hits (every stage equal; stage hits by variant rank 1..k %s), misses by reason: front stage differing %llu, fragment differing %llu, other stage differing %llu, layout (push offset) %llu, gate %llu, stage count %llu; %llu stage validations (%llu equal, %.2f variants compared each); %llu inserts (%llu variants inserted, %llu evicted beyond k, %llu already present, %llu unstable), %zu entries (%llu variants, ~%.1f MiB), %llu evictions in total, %llu LRU moves; verify: %llu hits captured again, %llu stages differed\n", count(counters.lookups), count(counters.absent), count(validated), validated != 0 ? counters.validateUs / static_cast<double>(validated) : 0.0, count(counters.hits), ranks.c_str(), miss(DrawMiss::FrontDiffering), miss(DrawMiss::FragmentDiffering), miss(DrawMiss::OtherDiffering), miss(DrawMiss::Layout), miss(DrawMiss::Gate), miss(DrawMiss::Stages), count(counters.stageValidations), count(counters.stageEqual), counters.stageValidations != 0 ? static_cast<double>(counters.variantsCompared) / static_cast<double>(counters.stageValidations) : 0.0, count(counters.inserts), count(counters.variantsInserted), count(counters.variantsEvicted), count(counters.present), count(counters.unstable), drawCache.size(), count(drawCacheVariants), static_cast<double>(drawCacheVariantBytes) / (1024.0 * 1024.0), count(drawCacheEvictions), count(counters.touches), count(counters.verifyHits), count(counters.verifyMismatches));
    std::fprintf(stderr, "[draw-cache] register key (10 s): %llu lookups, %llu hits, key build %.1f us each; decode skipped %llu, partial (state/pixel/programs from the entry) %llu; facade log != table %llu; verify: %llu decodes compared, %llu differed\n", count(counters.registerKeyLookups), count(counters.registerKeyHits), counters.registerKeyLookups != 0 ? counters.keyUs / static_cast<double>(counters.registerKeyLookups) : 0.0, count(counters.decodeSkipped), count(counters.decodePartial), count(counters.facadeMismatches), count(counters.verifyDecodes), count(counters.verifyDecodeMismatches));
    std::fprintf(stderr, "[draw-cache] data hits (10 s): %llu (%llu stages, %llu words refreshed), verified %llu; inserts with data positions %llu (%.1f positions each); partial hits %llu (%llu stages kept)\n", count(counters.dataHits), count(counters.dataStages), count(counters.dataWordsRefreshed), count(counters.dataVerified), count(counters.dataInserts), counters.dataInserts != 0 ? static_cast<double>(counters.dataPositionsInserted) / static_cast<double>(counters.dataInserts) : 0.0, count(counters.partialHits), count(counters.partialStagesKept));
    const auto perValidated = [&](double us) { return validated != 0 ? us / static_cast<double>(validated) : 0.0; };
    std::fprintf(stderr, "[draw-cache] validate split (10 s), us per validated lookup: key %.2f, stage compares %.2f (%llu validateVariant calls), patched results %.2f (%llu made, %llu reused), rest %.2f\n", perValidated(counters.keyUs), perValidated(counters.compareUs), count(counters.compareCalls), perValidated(counters.patchUs), count(counters.patchedMade), count(counters.patchedReused), perValidated(counters.validateUs - counters.keyUs - counters.compareUs - counters.patchUs));
    counters = DrawEntryCounters{};
}

}
