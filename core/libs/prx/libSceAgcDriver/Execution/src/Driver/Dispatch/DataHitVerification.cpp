#include "prx/libSceAgcDriver/Execution/include/Driver/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Shaders/ShaderRegistry.hpp"
#include <cstdlib>
#include <cstring>
#include <string>

namespace AgcDriver::DriverDetail {

void Driver::verifyDataHit(const ShaderSnapshot& snapshot, std::size_t codeOffset, std::uint64_t deviceSerial, ShaderRecompiler::RecompileRequest request, std::span<const ShaderRecompiler::MemoryRegion> memory, std::uint64_t address, const DispatchVariant& variant, std::span<const std::uint32_t> liveWords, const ShaderRecompiler::RecompileResult& patched) {
    const auto fail = [&](const char* what, std::size_t position, std::size_t slot) {
        std::fprintf(stderr, "[dispatch-cache] APS5_VERIFY_DATA_HITS: %s disagrees (program 0x%llx, position %zu, slot %zu)\n", what, static_cast<unsigned long long>(address), position, slot);
        std::fflush(stderr);
        std::abort();
    };
    auto verifyMemory = std::make_shared<ShaderMemory>(memory, &queryPendingWrite, &observePendingWrite, hookWaitCounter());
    const auto handle = SourceHandleFor(snapshot, codeOffset, deviceSerial, request, false);
    const auto capture = verifyMemory->Capture(request, handle.get());
    const auto regions = verifyMemory->Regions();
    const auto readSet = [&](std::size_t position, std::size_t run) {
        std::string text = "stored";
        char item[48];
        for (const auto& [begin, end] : variant.runs) {
            std::snprintf(item, sizeof(item), " %llx+%llx", static_cast<unsigned long long>(begin), static_cast<unsigned long long>(end - begin));
            text += item;
        }
        text += ", captured";
        for (const auto& region : regions) {
            std::snprintf(item, sizeof(item), " %llx+%zx", static_cast<unsigned long long>(region.guestAddress), region.bytes.size());
            text += item;
        }
        std::fprintf(stderr, "[dispatch-cache] APS5_VERIFY_DATA_HITS read sets: %s\n", text.c_str());
        fail("the read set", position, run);
    };
    request.context.memory = regions;
    const auto result = ShaderRecompiler::Recompile(request, *capture);
    std::size_t position = 0, run = 0;
    for (const auto& region : regions) {
        const bool registered = std::any_of(memory.begin(), memory.end(), [&](const auto& known) { return region.guestAddress >= known.guestAddress && region.guestAddress < known.guestAddress + known.bytes.size(); });
        if (registered) continue;
        if (run >= variant.runs.size() || variant.runs[run].first != region.guestAddress || variant.runs[run].second != region.guestAddress + region.bytes.size()) readSet(position, run);
        const auto count = region.bytes.size() / sizeof(std::uint32_t);
        if (position + count > liveWords.size() || std::memcmp(region.bytes.data(), liveWords.data() + position, region.bytes.size()) != 0) fail("the captured bytes", position, run);
        position += count;
        ++run;
    }
    if (run != variant.runs.size()) readSet(position, run);
    if (result->variantId != patched.variantId) fail("the variant", 0, 0);
    if (result->bindings.size() != patched.bindings.size() || result->pushConstants != patched.pushConstants) fail("the binding count or push constants", 0, 0);
    for (std::size_t b = 0; b < result->bindings.size(); ++b) {
        const auto& fresh = result->bindings[b];
        const auto& reused = patched.bindings[b];
        if (fresh.kind != reused.kind || fresh.role != reused.role || fresh.binding != reused.binding || fresh.count != reused.count || fresh.guestDescriptor != reused.guestDescriptor) {
            std::string text;
            char item[64];
            for (std::size_t w = 0; w < fresh.guestDescriptor.size() && w < reused.guestDescriptor.size(); ++w) {
                if (fresh.guestDescriptor[w] == reused.guestDescriptor[w]) continue;
                std::snprintf(item, sizeof(item), " [%zu] %08x (reused %08x)", w, fresh.guestDescriptor[w], reused.guestDescriptor[w]);
                text += item;
            }
            std::fprintf(stderr, "[dispatch-cache] APS5_VERIFY_DATA_HITS binding %zu (role %d kind %d, %zu / %zu words, flat %u, %zu shift slots):%s\n", b, static_cast<int>(fresh.role), static_cast<int>(fresh.kind), fresh.guestDescriptor.size(), reused.guestDescriptor.size(), variant.flatBinding, variant.shiftSlots.size(), text.c_str());
            fail("the bindings", b, 0);
        }
    }
    std::lock_guard cacheLock(dispatchCacheMutex);
    ++entryCounters.dataVerified;
}

}
