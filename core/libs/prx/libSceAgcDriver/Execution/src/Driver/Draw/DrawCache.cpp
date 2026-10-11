#include "prx/libSceAgcDriver/Execution/include/Driver/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Draw/DrawCache.hpp"
#include <cstdlib>

namespace AgcDriver::DriverDetail {

bool DrawRecipeRecord::Matches(const std::vector<std::shared_ptr<DispatchVariant>>& variants) const {
    if (stages.size() != variants.size()) return false;
    for (std::size_t i = 0; i < stages.size(); ++i) {
        if (stages[i].owner_before(variants[i]) || variants[i].owner_before(stages[i])) return false;
    }
    return true;
}

bool DrawRecipeRecord::Expired() const {
    return std::any_of(stages.begin(), stages.end(), [](const std::weak_ptr<const DispatchVariant>& stage) { return stage.expired(); });
}

bool Driver::drawEntries() {
    static const bool entries = std::getenv("APS5_NO_DRAW_SRT_ENTRIES") == nullptr && !stampValidate();
    return entries;
}

bool Driver::verifyDrawEntries() {
    static const bool verify = std::getenv("APS5_VERIFY_DRAW_ENTRIES") != nullptr;
    return verify;
}

bool Driver::drawEntryRunsCheck() {
    static const bool check = std::getenv("APS5_DRAW_ENTRY_RUNS_CHECK") != nullptr;
    return check;
}

bool Driver::registerKeyEnabled() {
    static const bool registerKey = std::getenv("APS5_NO_DRAW_KEY") == nullptr;
    return registerKey;
}

bool Driver::verifyDrawRecipe() {
    static const bool verify = std::getenv("APS5_VERIFY_DRAW_RECIPE") != nullptr;
    return verify;
}

std::size_t Driver::drawCacheEntries() {
    static const std::size_t entries = [] {
        const char* text = std::getenv("APS5_DRAW_CACHE_ENTRIES");
        const auto parsed = text != nullptr ? std::strtoull(text, nullptr, 10) : 0ull;
        return parsed != 0 ? static_cast<std::size_t>(parsed) : std::size_t{4096};
    }();
    return entries;
}

std::size_t Driver::drawEvictedKeyBound() {
    static const std::size_t bound = [] {
        const char* text = std::getenv("APS5_DRAW_EVICTED_KEYS");
        if (text != nullptr) return static_cast<std::size_t>(std::strtoull(text, nullptr, 10));
        return std::getenv("APS5_PROFILE_DRAW") != nullptr ? std::size_t{262144} : std::size_t{0};
    }();
    return bound;
}

// An evicted key is remembered with the eviction count (a key evicted again refreshes it); the
// evictions older than the window expire from the order's front, and a key expires with its
// last eviction.
void Driver::noteDrawEvictionLocked(std::uint64_t key) {
    const auto bound = drawEvictedKeyBound();
    if (bound == 0) return;
    const auto index = drawCacheEvictions;
    drawEvictedKeys.insert_or_assign(key, index);
    drawEvictedOrder.emplace_back(index, key);
    while (!drawEvictedOrder.empty() && index - drawEvictedOrder.front().first >= bound) {
        const auto [expired, expiredKey] = drawEvictedOrder.front();
        drawEvictedOrder.pop_front();
        const auto found = drawEvictedKeys.find(expiredKey);
        if (found != drawEvictedKeys.end() && found->second == expired) drawEvictedKeys.erase(found);
    }
}

// An absent key that was evicted would have hit with a cache of the entries plus the evictions
// since (every eviction at capacity is one new entry): bucket it by that size as a multiple of
// the cache's entries.
bool Driver::newDrawKeyTop() {
    static const bool top = std::getenv("APS5_NO_NEW_DRAW_KEY_TOP") == nullptr;
    return top;
}

void Driver::noteAbsentDrawKeyLocked(std::uint64_t key, std::uint64_t base, const DrawKey* drawKey) {
    if (drawEvictedKeyBound() == 0) return;
    auto& counters = drawEntryCounters;
    const auto found = drawEvictedKeys.find(key);
    if (found == drawEvictedKeys.end()) {
        ++counters.absentNew;
        const bool known = base != 0 && drawBaseIndex.contains(base);
        if (known) ++counters.absentNewBaseKnown;
        if (drawKey != nullptr && newDrawKeyTop()) drawNewKeys.Note(base, known, drawKey->words, drawKey->present, drawKey->programs);
        return;
    }
    const auto entries = drawCacheEntries();
    const auto needed = entries + (drawCacheEvictions - found->second);
    std::size_t bucket = 0;
    for (std::size_t size = entries * 2; bucket + 1 < counters.absentEvicted.size() && needed > size; size *= 2) ++bucket;
    ++counters.absentEvicted[bucket];
}

void Driver::accountDrawVariant(const DispatchVariant& variant, bool added) {
    if (added) {
        ++drawCacheVariants;
        drawCacheVariantBytes += variantBytes(variant);
    } else {
        --drawCacheVariants;
        drawCacheVariantBytes -= variantBytes(variant);
    }
}

void Driver::insertDrawEntry(const DrawKey& key, std::vector<std::shared_ptr<DispatchVariant>>& fresh, std::shared_ptr<const DrawDecode> decode, const DrawRelocation* relocation, const std::vector<std::shared_ptr<DispatchVariant>>* matched) {
    std::lock_guard cacheLock(drawCacheMutex);
    auto& counters = drawEntryCounters;
    ++counters.inserts;
    auto found = drawCache.find(key.key);
    // A miss under a relocation candidate moves the candidate's entry to the new key: the variants
    // of a shifted stage sit at dead addresses and go, the stages compared in place go on.
    bool rekey = false;
    if (found == drawCache.end() && relocation != nullptr && relocation->entry != nullptr) {
        found = drawCache.find(relocation->key);
        rekey = found != drawCache.end() && found->second == relocation->entry;
        if (!rekey) found = drawCache.end();
    }
    auto replacement = std::make_shared<DrawEntry>();
    replacement->stages.resize(fresh.size());
    replacement->decode = std::move(decode);
    if (found != drawCache.end()) {
        if (found->second->stages.size() == fresh.size()) replacement->stages = found->second->stages;
        if (found->second->decode != nullptr && (!rekey || replacement->decode == nullptr)) replacement->decode = found->second->decode;
        replacement->recipes.store(found->second->recipes.load());
        if (rekey) {
            // Of a stage whose pointer moved, only a variant whose rule shifts nothing stays.
            for (std::size_t i = 0; i < replacement->stages.size(); ++i) {
                if (i >= relocation->deltas.size() || relocation->deltas[i] == 0) continue;
                auto& variants = replacement->stages[i];
                std::vector<std::shared_ptr<DispatchVariant>> kept;
                for (const auto& variant : variants) {
                    if (staysOnMove(*variant)) {
                        kept.push_back(variant);
                    } else {
                        accountDrawVariant(*variant, false);
                        ++counters.variantsEvicted;
                    }
                }
                variants = std::move(kept);
            }
        }
    }
    for (std::size_t i = 0; i < fresh.size(); ++i) {
        auto& variants = replacement->stages[i];
        // A relocated miss keeps the stages it matched through a shifted variant.
        if (fresh[i] == nullptr && rekey && matched != nullptr && i < matched->size() && (*matched)[i] != nullptr && i < relocation->relocated.size() && relocation->relocated[i]) {
            accountDrawVariant(*(*matched)[i], true);
            variants.insert(variants.begin(), (*matched)[i]);
            ++counters.variantsInserted;
            continue;
        }
        if (fresh[i] == nullptr) continue;
        const auto present = std::find_if(variants.begin(), variants.end(), [&](const std::shared_ptr<DispatchVariant>& kept) { return kept->pushOffset == fresh[i]->pushOffset && kept->runs == fresh[i]->runs && kept->words == fresh[i]->words; });
        if (present != variants.end()) {
            ++counters.present;
            fresh[i] = *present;
            continue;
        }
        accountDrawVariant(*fresh[i], true);
        variants.insert(variants.begin(), fresh[i]);
        ++counters.variantsInserted;
        while (variants.size() > dispatchVariants()) {
            accountDrawVariant(*variants.back(), false);
            variants.pop_back();
            ++counters.variantsEvicted;
        }
    }
    replacement->touched = drawCacheHits;
    replacement->baseKey = key.base;
    replacement->pointerWords = key.words;
    replacement->pointerPresent = key.present;
    if (found == drawCache.end()) {
        drawOrder.push_front(key.key);
        replacement->order = drawOrder.begin();
        drawCache.emplace(key.key, std::move(replacement));
        indexDrawKeyLocked(key);
    } else if (rekey) {
        replacement->order = found->second->order;
        *replacement->order = key.key;
        drawOrder.splice(drawOrder.begin(), drawOrder, replacement->order);
        unindexDrawKeyLocked(found->second->baseKey, relocation->key);
        drawCache.erase(found);
        drawCache.emplace(key.key, std::move(replacement));
        indexDrawKeyLocked(key);
        ++counters.rekeys;
    } else {
        replacement->order = found->second->order;
        drawOrder.splice(drawOrder.begin(), drawOrder, replacement->order);
        found->second = std::move(replacement);
    }
    while (drawCache.size() > drawCacheEntries()) {
        const auto last = drawCache.find(drawOrder.back());
        for (const auto& variants : last->second->stages) {
            for (const auto& variant : variants) accountDrawVariant(*variant, false);
        }
        unindexDrawKeyLocked(last->second->baseKey, last->first);
        drawOrder.erase(last->second->order);
        ++drawCacheEvictions;
        noteDrawEvictionLocked(last->first);
        drawCache.erase(last);
    }
}

std::shared_ptr<const DrawRecipe> Driver::findDrawRecipe(std::uint64_t key, const std::vector<std::shared_ptr<DispatchVariant>>& stages) {
    std::shared_ptr<const std::vector<DrawRecipeRecord>> records;
    {
        std::lock_guard cacheLock(drawCacheMutex);
        const auto found = drawCache.find(key);
        if (found == drawCache.end()) return nullptr;
        records = found->second->recipes.load();
    }
    if (records == nullptr) return nullptr;
    for (const auto& record : *records) {
        if (record.Matches(stages)) return record.recipe;
    }
    return nullptr;
}

void Driver::attachDrawRecipe(std::uint64_t key, const std::vector<std::shared_ptr<DispatchVariant>>& stages, std::shared_ptr<const DrawRecipe> recipe) {
    std::shared_ptr<DrawEntry> entry;
    {
        std::lock_guard cacheLock(drawCacheMutex);
        const auto found = drawCache.find(key);
        if (found == drawCache.end()) return;
        entry = found->second;
    }
    const auto old = entry->recipes.load();
    auto records = std::make_shared<std::vector<DrawRecipeRecord>>();
    records->push_back({std::vector<std::weak_ptr<const DispatchVariant>>(stages.begin(), stages.end()), std::move(recipe)});
    if (old != nullptr) {
        for (const auto& record : *old) {
            if (record.Matches(stages) || record.Expired() || records->size() >= dispatchVariants()) continue;
            records->push_back(record);
        }
    }
    entry->recipes.store(std::move(records));
    VulkanDevice::NoteRecipe(VulkanDevice::RecipeEvent::Attach, VulkanDevice::RecipeKind::Draw);
}

}
