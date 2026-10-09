#include "prx/libSceAgcDriver/Graphics/include/Recorder.hpp"
#include "prx/libSceAgcDriver/Graphics/include/BufferPool.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Draw.hpp"
#include "prx/libSceAgcDriver/Graphics/include/DccMetadata.hpp"
#include "prx/libSceAgcDriver/Graphics/include/FastDispatch.hpp"
#include "prx/libSceAgcDriver/Graphics/include/FastRing.hpp"
#include "prx/libSceAgcDriver/Graphics/include/GuestBufferMemory.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Resources.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Sampler.hpp"
#include "prx/libSceAgcDriver/Graphics/include/ShaderResources.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Shaders.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Texture.hpp"
#include "prx/libSceAgcDriver/Graphics/include/TextureDetiler.hpp"
#include "prx/libSceAgcDriver/Graphics/include/UnitShadow.hpp"
#include "prx/libSceAgcDriver/Execution/include/BdaFeatures.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Draw/FastRead.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/ShaderMemory.hpp"
#include "prx/libc/include/GuestAllocations.hpp"
#include "prx/libc/include/GuestArena.hpp"
#include "prx/libc/include/GuestWriteWatch.hpp"
#include "Optimization/ResourceProgram.hpp"
#include "ResidentPresent.hpp"
#include "SampleLod_spv.h"
#include "DataSlot_spv.h"
#include <SDL_loadso.h>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/mman.h>
#endif
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace AgcDriver::Graphics;
using AgcDriver::GuestMemory::GpuMutex;

void* AllocateWatched(std::size_t bytes, std::size_t alignment) {
    if (!AgcDriver::GuestMemory::WriteWatched()) return nullptr;
#ifdef _WIN32
    void* block = GuestArena::GuestArenaAllocate_nid_postfix(bytes, alignment);
    GuestArena::GuestArenaCommit_nid_postfix(block, bytes, PAGE_READWRITE, bytes);
#else
    void* raw = mmap(nullptr, bytes + alignment, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (raw == MAP_FAILED) throw std::runtime_error("cannot map the watched block");
    const auto begin = reinterpret_cast<std::uintptr_t>(raw);
    const auto aligned = (begin + alignment - 1) & ~(static_cast<std::uintptr_t>(alignment) - 1);
    if (aligned != begin) munmap(raw, aligned - begin);
    if (aligned + bytes != begin + bytes + alignment) munmap(reinterpret_cast<void*>(aligned + bytes), begin + alignment - aligned);
    void* block = reinterpret_cast<void*>(aligned);
    GuestWriteWatch::GuestWriteWatchRegister_nid_postfix(block, bytes);
#endif
    if (!AgcDriver::GuestMemory::Watched(reinterpret_cast<std::uint64_t>(block), bytes)) throw std::runtime_error("the watched block is not watched");
    return block;
}

void ReleaseWatched(void* block, std::size_t bytes) {
#ifdef _WIN32
    GuestArena::GuestArenaReset_nid_postfix(block, bytes);
    GuestArena::GuestArenaRelease_nid_postfix(block, bytes);
#else
    munmap(block, bytes);
    GuestWriteWatch::GuestWriteWatchUnregister_nid_postfix(block, bytes);
#endif
}

// A compute-capable device with host imports (VK_EXT_external_memory_host) when the host offers
// them, as the driver creates its own; the recorder's batches need a real queue and real fences.
class Device {
public:
    Device() {
#ifdef _WIN32
        library = SDL_LoadObject("vulkan-1.dll");
#else
        library = SDL_LoadObject("libvulkan.so.1");
#endif
        Require(library != nullptr, "cannot load Vulkan");
        try {
            instanceProc = reinterpret_cast<PFN_vkGetInstanceProcAddr>(SDL_LoadFunction(library, "vkGetInstanceProcAddr"));
            Require(instanceProc != nullptr, "missing Vulkan instance resolver");
            VkApplicationInfo application{VK_STRUCTURE_TYPE_APPLICATION_INFO};
            application.apiVersion = VK_API_VERSION_1_1;
            VkInstanceCreateInfo info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
            info.pApplicationInfo = &application;
            Check(function<PFN_vkCreateInstance>("vkCreateInstance")(&info, nullptr, &instance), "vkCreateInstance");
            std::uint32_t count = 0;
            const auto enumerate = function<PFN_vkEnumeratePhysicalDevices>("vkEnumeratePhysicalDevices");
            Check(enumerate(instance, &count, nullptr), "vkEnumeratePhysicalDevices");
            Require(count != 0, "no Vulkan device");
            std::vector<VkPhysicalDevice> devices(count);
            Check(enumerate(instance, &count, devices.data()), "vkEnumeratePhysicalDevices");
            context.physical = devices.front();
            const auto extensions = function<PFN_vkEnumerateDeviceExtensionProperties>("vkEnumerateDeviceExtensionProperties");
            Check(extensions(context.physical, nullptr, &count, nullptr), "vkEnumerateDeviceExtensionProperties");
            std::vector<VkExtensionProperties> available(count);
            Check(extensions(context.physical, nullptr, &count, available.data()), "vkEnumerateDeviceExtensionProperties");
            const auto hasExtension = [&](const char* name) {
                for (const auto& extension : available) {
                    if (std::strcmp(extension.extensionName, name) == 0) return true;
                }
                return false;
            };
            auto bytes = AgcDriver::QueryBdaByteFeatures(context.physical, function<PFN_vkGetPhysicalDeviceFeatures2>("vkGetPhysicalDeviceFeatures2"), available);
            auto address = AgcDriver::QueryBdaFeatures(context.physical, function<PFN_vkGetPhysicalDeviceFeatures2>("vkGetPhysicalDeviceFeatures2"), available);
            const auto queues = function<PFN_vkGetPhysicalDeviceQueueFamilyProperties>("vkGetPhysicalDeviceQueueFamilyProperties");
            queues(context.physical, &count, nullptr);
            std::vector<VkQueueFamilyProperties> families(count);
            queues(context.physical, &count, families.data());
            std::uint32_t family = 0;
            while (family < count && (families[family].queueFlags & VK_QUEUE_COMPUTE_BIT) == 0) ++family;
            Require(family < count, "no Vulkan compute queue");
            const float priority = 1;
            VkDeviceQueueCreateInfo queue{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
            queue.queueFamilyIndex = family;
            queue.queueCount = 1;
            queue.pQueuePriorities = &priority;
            VkPhysicalDeviceFeatures enabled{};
            enabled.shaderInt64 = VK_TRUE;
            address.pNext = &bytes;
            std::vector<const char*> extensionsEnabled{VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME, VK_KHR_8BIT_STORAGE_EXTENSION_NAME};
            if (hasExtension(VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME)) {
                extensionsEnabled.push_back(VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME);
                VkPhysicalDeviceExternalMemoryHostPropertiesEXT hostProperties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_MEMORY_HOST_PROPERTIES_EXT};
                VkPhysicalDeviceProperties2 properties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &hostProperties};
                function<PFN_vkGetPhysicalDeviceProperties2>("vkGetPhysicalDeviceProperties2")(context.physical, &properties);
                context.hostImportAlignment = hostProperties.minImportedHostPointerAlignment;
            }
            VkPhysicalDeviceImageViewMinLodFeaturesEXT minLod{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_VIEW_MIN_LOD_FEATURES_EXT};
            if (hasExtension(VK_EXT_IMAGE_VIEW_MIN_LOD_EXTENSION_NAME)) {
                VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &minLod};
                function<PFN_vkGetPhysicalDeviceFeatures2>("vkGetPhysicalDeviceFeatures2")(context.physical, &features);
                context.imageViewMinLod = minLod.minLod == VK_TRUE;
            }
            minLod = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_VIEW_MIN_LOD_FEATURES_EXT};
            minLod.minLod = VK_TRUE;
            if (context.imageViewMinLod) {
                extensionsEnabled.push_back(VK_EXT_IMAGE_VIEW_MIN_LOD_EXTENSION_NAME);
                minLod.pNext = address.pNext;
                address.pNext = &minLod;
            }
            VkDeviceCreateInfo device{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &address};
            device.queueCreateInfoCount = 1;
            device.pQueueCreateInfos = &queue;
            device.enabledExtensionCount = static_cast<std::uint32_t>(extensionsEnabled.size());
            device.ppEnabledExtensionNames = extensionsEnabled.data();
            device.pEnabledFeatures = &enabled;
            Check(function<PFN_vkCreateDevice>("vkCreateDevice")(context.physical, &device, nullptr, &context.device), "vkCreateDevice");
            context.deviceProc = function<PFN_vkGetDeviceProcAddr>("vkGetDeviceProcAddr");
            function<PFN_vkGetPhysicalDeviceMemoryProperties>("vkGetPhysicalDeviceMemoryProperties")(context.physical, &context.memory);
            VkPhysicalDeviceProperties properties{};
            function<PFN_vkGetPhysicalDeviceProperties>("vkGetPhysicalDeviceProperties")(context.physical, &properties);
            context.limits = properties.limits;
            context.bufferDeviceAddress = true;
            context.formatProperties = function<PFN_vkGetPhysicalDeviceFormatProperties>("vkGetPhysicalDeviceFormatProperties");
            context.Function<PFN_vkGetDeviceQueue>("vkGetDeviceQueue")(context.device, family, 0, &context.queue);
            VkCommandPoolCreateInfo pool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
            pool.queueFamilyIndex = family;
            Check(context.Function<PFN_vkCreateCommandPool>("vkCreateCommandPool")(context.device, &pool, nullptr, &context.pool), "vkCreateCommandPool");
        } catch (...) {
            release();
            throw;
        }
    }

    ~Device() { release(); }
    const Context& GetContext() const { return context; }
    void WaitQueue() const { Check(context.Function<PFN_vkQueueWaitIdle>("vkQueueWaitIdle")(context.queue), "vkQueueWaitIdle"); }

private:
    template<typename TFunction>
    TFunction function(const char* name) const {
        const auto result = reinterpret_cast<TFunction>(instanceProc(instance, name));
        Require(result != nullptr, name);
        return result;
    }

    void release() noexcept {
        if (context.pool != VK_NULL_HANDLE) context.Function<PFN_vkDestroyCommandPool>("vkDestroyCommandPool")(context.device, context.pool, nullptr);
        context.bufferPool.reset();
        if (context.device != VK_NULL_HANDLE) function<PFN_vkDestroyDevice>("vkDestroyDevice")(context.device, nullptr);
        if (instance != VK_NULL_HANDLE) function<PFN_vkDestroyInstance>("vkDestroyInstance")(instance, nullptr);
        if (library != nullptr) SDL_UnloadObject(library);
    }

    void* library = nullptr;
    PFN_vkGetInstanceProcAddr instanceProc = nullptr;
    VkInstance instance = VK_NULL_HANDLE;
    Context context{};
};

using Kind = Recorder::ReadKind;

void readTrackingTests(const Device& device, Recorder& recorder) {
    Require(Recorder::ReadTracking(), "read tracking is off (APS5_COPY_READ_TRACKING=0 set?)");
    Require(recorder.Idle() && !recorder.PendingReadOverlaps(0x10000, 16), "a fresh recorder reports a read");
    recorder.NotePendingRead(0x10000, 0x100, Kind::DispatchElement);
    Require(recorder.Recording(), "a read note did not open a batch");
    Require(recorder.PendingReadOverlaps(0x10080, 4) && recorder.PendingReadOverlaps(0xff00, 0x101) && recorder.PendingReadOverlaps(0x100ff, 1), "an open batch's read is not seen");
    Require(!recorder.PendingReadOverlaps(0x10100, 4) && !recorder.PendingReadOverlaps(0xff00, 0x100) && !recorder.PendingReadOverlaps(0x10000, 0), "a disjoint range is reported as read");
    const auto open = recorder.DescribePendingRead(0x10000, 4);
    Require(open.has_value() && open->open && !open->signaled && open->kind == Kind::DispatchElement && open->serial == recorder.Submissions() + 1, "the open batch's read is described wrongly");
    recorder.Submit();
    Require(!recorder.Recording() && !recorder.Idle(), "the batch is not in flight after Submit");
    Require(recorder.PendingReadOverlaps(0x10000, 4, false), "the raw scan does not see the in-flight batch's read");
    // An empty batch completes at once: once its fence signaled, its read is no reader any more,
    // although the batch stays in flight until it is reaped.
    device.WaitQueue();
    Require(!recorder.PendingReadOverlaps(0x10000, 4), "a signaled batch's read still refuses");
    Require(!recorder.Idle(), "an overlap query reaped the batch");
    const auto signaled = recorder.DescribePendingRead(0x10000, 4);
    Require(signaled.has_value() && !signaled->open && signaled->signaled && signaled->serial == recorder.Submissions(), "the signaled batch's read is described wrongly");
    const auto counts = Recorder::ReadCounts();
    Require(counts.noted == 1 && counts.staleIgnored >= 1 && counts.hits[static_cast<std::size_t>(Kind::DispatchElement)] >= 3 && counts.queries >= 6, "read counters are off");
    recorder.Sync();
    Require(recorder.Idle() && !recorder.PendingReadOverlaps(0x10000, 4, false) && !recorder.DescribePendingRead(0x10000, 4).has_value(), "a read outlived its batch");
    const std::vector<std::pair<std::uint64_t, std::uint64_t>> ranges{{0x20000, 0x21000}, {0x30000, 0x30010}, {0x40000, 0x40000}};
    recorder.NotePendingReads(ranges, Kind::AddressBased);
    Require(recorder.PendingReadOverlaps(0x20fff, 1) && recorder.PendingReadOverlaps(0x30000, 16) && !recorder.PendingReadOverlaps(0x21000, 16) && !recorder.PendingReadOverlaps(0x40000, 16), "noted ranges are not seen as reads");
    const auto batch = recorder.DescribePendingRead(0x30008, 4);
    Require(batch.has_value() && batch->kind == Kind::AddressBased && batch->open, "a noted range has the wrong reader kind");
    Require(Recorder::ReadCounts().hits[static_cast<std::size_t>(Kind::AddressBased)] >= 2, "address-based hits are not counted");
    recorder.Sync();
    Require(recorder.Idle() && !recorder.PendingReadOverlaps(0x20000, 0x1000, false), "noted ranges outlived their batch");
}

void writeSettledTests(const Device& device, Recorder& recorder) {
    Require(recorder.PendingWriteSettled(0x50000, 0x100), "an unwritten range is not settled");
    recorder.NotePendingWrite(0x50000, 0x100);
    Require(recorder.PendingWriteOverlaps(0x50000, 0x100) && !recorder.PendingWriteSettled(0x50000, 0x100) && recorder.PendingWriteSettled(0x50100, 0x100), "an open batch's write counts as settled");
    recorder.Submit();
    device.WaitQueue();
    Require(recorder.PendingWriteOverlaps(0x50000, 0x100) && recorder.PendingWriteSettled(0x50000, 0x100), "a signaled batch's write is not settled");
    recorder.NotePendingWrite(0x50080, 0x10);
    Require(!recorder.PendingWriteSettled(0x50000, 0x100) && recorder.PendingWriteSettled(0x50000, 0x80), "a later open write over the range still counts as settled");
    recorder.Sync();
    Require(recorder.Idle() && !recorder.PendingWriteOverlaps(0x50000, 0x100), "writes outlived their batches");
}

// Pending blocks (the fast walk's reader, docs/design/draw-fastpath.md F2): a noted write marks
// its 64 KiB blocks with its batch's serial; they read pending while the batch is open, in flight
// or signaled but not reaped, and clear once CompletedSerial reaches the batch.
void pendingBlockTests(const Device& device, Recorder& recorder) {
    Recorder::TrackPendingBlocks(true);
    recorder.Sync();
    constexpr std::uint64_t base = 0x7a0000;
    Require(!Recorder::BlockPending(base) && !Recorder::BlockPending(base + 0x10000), "a block reads pending before any note");
    const auto before = Recorder::CompletedSerial();
    recorder.NotePendingWrite(base + 0xff00, 0x200);
    Require(Recorder::BlockPending(base) && Recorder::BlockPending(base + 0xfffc) && Recorder::BlockPending(base + 0x10000) && Recorder::BlockPending(base + 0x1fffc), "a noted write did not mark its blocks pending");
    Require(!Recorder::BlockPending(base + 0x20000) && !Recorder::BlockPending(base - 4), "a noted write marked a block it does not touch");
    recorder.Submit();
    device.WaitQueue();
    Require(Recorder::BlockPending(base), "a signaled batch's block cleared before the batch was reaped");
    recorder.NotePendingWrite(base + 0x20000, 4);
    recorder.Sync();
    Require(Recorder::CompletedSerial() > before && !Recorder::BlockPending(base) && !Recorder::BlockPending(base + 0x10000) && !Recorder::BlockPending(base + 0x20000), "blocks stayed pending after their batches finished");
    // The older of two batches finishing clears its own block, not the newer one's.
    recorder.NotePendingWrite(base, 4);
    recorder.Submit();
    recorder.NotePendingWrite(base + 0x40000, 4);
    const auto newest = recorder.SubmitAndEpoch();
    recorder.FinishUpTo(newest - 1);
    Require(!Recorder::BlockPending(base) && Recorder::BlockPending(base + 0x40000), "finishing the older batch did not clear exactly its own block");
    recorder.Sync();
    Require(!Recorder::BlockPending(base + 0x40000), "the newer batch's block stayed pending after it finished");
    // A block 16 GiB away shares the slot: a collision reads pending, never clear.
    recorder.NotePendingWrite(base + (std::uint64_t{1} << 34u), 4);
    Require(Recorder::BlockPending(base), "a slot collision read clear");
    recorder.Sync();
    Require(!Recorder::BlockPending(base), "a collided slot stayed pending after its batch finished");
    Recorder::TrackPendingBlocks(false);
    recorder.NotePendingWrite(base, 4);
    Require(!Recorder::BlockPending(base), "an untracked note marked a block pending");
    recorder.Sync();
}

// The fast walks' reader over pending writes (FastSrtRead, docs/design/draw-fastpath.md F2, F3b,
// F5): a word in a pending 64 KiB block declines only when an exact pending range overlaps it (the
// old capture's raw-read rule), so a word adjacent to a range reads and a word one byte into one
// declines, over block boundaries too; a range noted after the reader loaded its snapshot is seen
// (the publish generation moved); a completion label noted on an in-flight batch (noteWriteOn on a
// submitted batch) declines its word only; a slot collision with no range near the word reads; the
// block alone declines with the exact test off (APS5_FAST_PENDING_BLOCKS=1); a finished batch's
// words read without the exact test; a queued label of this thread declines its word without
// counting as a capture page query ([labels] line).
void fastReaderPendingTests(Recorder& recorder) {
    using AgcDriver::DriverDetail::DeferredLabel;
    using AgcDriver::DriverDetail::FastReader;
    using AgcDriver::DriverDetail::FastSrtRead;
    using AgcDriver::DriverDetail::WalkDecline;
    Recorder::TrackPendingBlocks(true);
    recorder.Sync();
    // Live words over four 64 KiB blocks, the first block-aligned; each word holds its offset.
    constexpr std::uint64_t Block = 0x10000;
    std::vector<std::uint32_t> storage(static_cast<std::size_t>(5 * Block / 4));
    const auto base = (reinterpret_cast<std::uint64_t>(storage.data()) + Block - 1) & ~(Block - 1);
    const auto expected = [&](std::uint64_t address) { return static_cast<std::uint32_t>(address - base) ^ 0x5a5a0000u; };
    for (std::uint64_t at = base; at < base + 4 * Block; at += 4) *reinterpret_cast<std::uint32_t*>(static_cast<std::uintptr_t>(at)) = expected(at);
    const std::vector<DeferredLabel> noLabels;
    // Whether the word was served (and then with its live value).
    const auto read = [&](FastReader& reader, std::uint64_t address) {
        reader.labels = &noLabels;
        reader.declined.reset();
        std::uint32_t value = 0;
        const bool served = FastSrtRead(&reader, address, &value);
        if (served && value != expected(address)) throw std::runtime_error("the fast reader served a wrong word");
        return served;
    };
    const auto declinesPending = [&](FastReader& reader, std::uint64_t address) { return !read(reader, address) && reader.declined == WalkDecline::Pending; };
    FastReader reader{};
    reader.exactPending = true;
    // Adjacent: [0x100, 0x200) leaves the words ending at 0x100 and starting at 0x200 readable.
    recorder.NotePendingWrite(base + 0x100, 0x100);
    Require(Recorder::BlockPending(base + 0xfc) && Recorder::BlockPending(base + 0x200), "the noted range did not mark its block pending");
    Require(read(reader, base + 0xfc) && read(reader, base + 0x200), "a word adjacent to a pending range declined");
    Require(declinesPending(reader, base + 0x100) && declinesPending(reader, base + 0x1fc), "a word inside a pending range was read");
    Require(reader.pendingPassed == 2 && reader.pendingInBlocks == 4, "the reads in and past a pending block are not counted");
    // One byte of overlap: a range of one byte at a word's last byte, at a word's first byte, and a
    // two-byte range over the boundary of two words.
    recorder.NotePendingWrite(base + 0x303, 1);
    recorder.NotePendingWrite(base + 0x404, 1);
    recorder.NotePendingWrite(base + 0x5ff, 2);
    Require(declinesPending(reader, base + 0x300) && read(reader, base + 0x2fc) && read(reader, base + 0x304), "a word overlapping a one-byte range at its last byte was misjudged");
    Require(declinesPending(reader, base + 0x404) && read(reader, base + 0x400) && read(reader, base + 0x408), "a word overlapping a one-byte range at its first byte was misjudged");
    Require(declinesPending(reader, base + 0x5fc) && declinesPending(reader, base + 0x600) && read(reader, base + 0x5f8) && read(reader, base + 0x604), "the words a two-byte range straddles were misjudged");
    // Spanning blocks: [Block - 2, Block + 2) overlaps the last word of block 0 and the first of
    // block 1; [Block + 0x8000, 3 * Block + 0x10) covers block 2 whole and ends inside block 3.
    recorder.NotePendingWrite(base + Block - 2, 4);
    recorder.NotePendingWrite(base + Block + 0x8000, 2 * Block - 0x8000 + 0x10);
    Require(declinesPending(reader, base + Block - 4) && declinesPending(reader, base + Block), "the words a range over a block boundary straddles were read");
    Require(read(reader, base + Block - 8) && read(reader, base + Block + 4), "a word beside a range over a block boundary declined");
    Require(declinesPending(reader, base + 2 * Block) && declinesPending(reader, base + 2 * Block + 0x8000) && declinesPending(reader, base + 3 * Block + 0xc), "a word inside a range over three blocks was read");
    Require(read(reader, base + Block + 0x7ffc) && read(reader, base + 3 * Block + 0x10) && read(reader, base + 3 * Block + 0x8000), "a word beside a range over three blocks declined");
    // A range noted after the reader loaded its snapshot.
    Require(read(reader, base + 0x800), "a word with no range over it declined");
    recorder.NotePendingWrite(base + 0x800, 4);
    Require(declinesPending(reader, base + 0x800) && read(reader, base + 0x804), "a range noted after the reader's snapshot was missed");
    // A completion label on an in-flight batch: with no open batch, AfterCompletions appends to the
    // submitted one, and noteWriteOn marks the block and publishes the range there. The completion
    // stores the word's own value at the finish.
    Require(read(reader, base + 0x900), "a word with no range over it declined");
    recorder.Submit();
    std::array<std::byte, 4> label{};
    const auto labelValue = expected(base + 0x900);
    std::memcpy(label.data(), &labelValue, label.size());
    recorder.AfterCompletions(base + 0x900, label, 1, 0, false);
    Require(declinesPending(reader, base + 0x900) && read(reader, base + 0x8fc) && read(reader, base + 0x904), "a completion label noted on an in-flight batch was misjudged");
    // The block alone (APS5_FAST_PENDING_BLOCKS=1).
    FastReader blocksOnly{};
    blocksOnly.exactPending = false;
    Require(declinesPending(blocksOnly, base + 0xfc) && declinesPending(blocksOnly, base + 3 * Block + 0x8000) && blocksOnly.pendingPassed == 0, "the block-only rule read a word in a pending block");
    recorder.Sync();
    Require(!Recorder::BlockPending(base) && read(reader, base + 0x100) && read(blocksOnly, base + 0x100), "a finished batch's word declined");
    // A block 16 GiB away shares the slot: the block reads pending, the word has no range over it.
    recorder.NotePendingWrite(base + (std::uint64_t{1} << 34u), 4);
    const auto passed = reader.pendingPassed;
    Require(Recorder::BlockPending(base) && read(reader, base + 0x100) && reader.pendingPassed == passed + 1, "a slot collision declined a word no range overlaps");
    Require(declinesPending(blocksOnly, base + 0x100), "the block-only rule read a word in a collided block");
    recorder.Sync();
    // A queued label of this thread (NoteQueuedLabel): its word declines, its neighbour reads, and
    // the reader's query is not counted as the old capture's page query.
    std::array<std::byte, 4> queued{};
    const auto queuedValue = expected(base + 0xa00);
    std::memcpy(queued.data(), &queuedValue, queued.size());
    Recorder::NoteQueuedLabel(base + 0xa00, queued, 1, AgcDriver::GuestMemory::GpuLockThreadTag());
    const auto pageQueries = Recorder::StoreCounts().queuedPageQueries;
    Require(!read(reader, base + 0xa00) && reader.declined == WalkDecline::QueuedLabel && read(reader, base + 0xa04), "a queued label's word was misjudged by the fast reader");
    Require(Recorder::QueuedLabelOverlapsThisThreadUncounted(base + 0xa00, 4) && !Recorder::QueuedLabelOverlapsThisThreadUncounted(base + 0xa04, 4), "the uncounted queued-label query answers differently");
    Require(Recorder::StoreCounts().queuedPageQueries == pageQueries, "the fast reader's queued-label query counted as a capture page query");
    Recorder::ForgetQueuedLabels();
    Recorder::TrackPendingBlocks(false);
}

// The classification both readers of fastReaderKnownValueTests ask (the driver's
// classifyPendingWrite stands behind both in the game): per word, the answer and its value; a
// page query is word-wise when an answered word lies in it. The fast reader's form can name a
// KnownValue range (FastPendingAnswer) taken at the current writer push count and write generation.
struct FakeAnswer {
    AgcDriver::ShaderMemory::PendingWrite policy;
    std::uint32_t word;
};
std::map<std::uint64_t, FakeAnswer> fakeAnswers;
struct FakeRange {
    std::uint64_t begin = 0;
    std::uint64_t end = 0;
    std::shared_ptr<const std::vector<std::byte>> bytes;
};
FakeRange fakeRange;
std::uint64_t fakeFastQueries = 0;

AgcDriver::ShaderMemory::PendingWrite fakeCaptureQuery(std::uint64_t address, std::size_t bytes, std::span<std::byte> known) {
    using Policy = AgcDriver::ShaderMemory::PendingWrite;
    if (bytes != sizeof(std::uint32_t)) {
        const auto found = fakeAnswers.lower_bound(address);
        return found != fakeAnswers.end() && found->first < address + bytes ? Policy::Sync : Policy::None;
    }
    const auto found = fakeAnswers.find(address);
    if (found == fakeAnswers.end()) return Policy::None;
    if (known.size() == sizeof(std::uint32_t)) std::memcpy(known.data(), &found->second.word, sizeof(std::uint32_t));
    return found->second.policy;
}

void fakeFastQuery(std::uint64_t address, const AgcDriver::DriverDetail::PendingView&, AgcDriver::DriverDetail::FastPendingAnswer& answer) {
    using Policy = AgcDriver::ShaderMemory::PendingWrite;
    ++fakeFastQueries;
    answer = {};
    const auto found = fakeAnswers.find(address);
    if (found == fakeAnswers.end()) {
        answer.policy = Policy::None;
        return;
    }
    answer.policy = found->second.policy;
    answer.word = found->second.word;
    if (answer.policy != Policy::KnownValue || fakeRange.bytes == nullptr || address < fakeRange.begin || address + sizeof(std::uint32_t) > fakeRange.end) return;
    answer.rangeBegin = fakeRange.begin;
    answer.rangeEnd = fakeRange.end;
    answer.rangeBytes = fakeRange.bytes;
    answer.writers = AgcDriver::DriverDetail::WrittenBufferPushes().load();
    answer.writes = Recorder::WriteGeneration();
}

// Known-value serving in the fast reader (FastSrtRead with APS5_FAST_KNOWN_VALUES, s53-known-values):
// a pending word is served as the old capture serves it. The old capture is ShaderMemory over the
// same answers, capturing a shader that loads a table pointer from its user data and a payload
// through it (the payload a pure flat slot); the fast walk is WalkResources over FastSrtRead, the
// payload's word in an exact pending range of the recorder. A KnownValue (a pending copy's
// destination whose bytes the driver keeps) gives both the known word; RawExpected the live word
// while it holds the expected value; a queued label of this thread its bytes, which the old path
// reads once it recorded the label (stored here by hand, as the record lands it). Where the
// capture reads through the flush hook (Sync: deferred or waited; RawExpected with another value:
// waited) the fast walk declines "pending block". A word changed after it was served: within a
// walk the served range answers while nothing the classification sees moved; a new writer (the
// push count), a new pending range (the publish generation), a label noted inside a range the
// snapshot already covers (the write generation; nothing is published) or another live word
// (RawExpected) re-serves or declines. Evidence words decline with APS5_FAST_KNOWN_EVIDENCE=0.
// Recorder::LabelValueIn answers as LookupLabelValue per dword, without counting wait hits.
void fastReaderKnownValueTests(Recorder& recorder) {
    using namespace ShaderRecompiler;
    using AgcDriver::DriverDetail::DeferredLabel;
    using AgcDriver::DriverDetail::FastReader;
    using AgcDriver::DriverDetail::FastSrtRead;
    using AgcDriver::DriverDetail::WalkDecline;
    using AgcDriver::DriverDetail::WrittenBufferPushes;
    using Policy = AgcDriver::ShaderMemory::PendingWrite;
    Recorder::TrackPendingBlocks(true);
    recorder.Sync();
    constexpr std::uint64_t Block = 0x10000;
    std::vector<std::uint32_t> storage(static_cast<std::size_t>(2 * Block / 4));
    const auto base = (reinterpret_cast<std::uint64_t>(storage.data()) + Block - 1) & ~(Block - 1);
    const auto word = [](std::uint64_t address) -> std::uint32_t& { return *reinterpret_cast<std::uint32_t*>(static_cast<std::uintptr_t>(address)); };
    const auto table = base + 0x100;
    const auto payload = base + 0x200;
    *reinterpret_cast<std::uint64_t*>(static_cast<std::uintptr_t>(table)) = payload;
    constexpr std::uint32_t Live = 0x3f800000u;
    word(payload) = Live;
    // tests/ShaderMemory.cpp's shader: s_load_dwordx2 of the table pointer, s_load_dword of the
    // payload through it, exported.
    const std::array<std::uint32_t, 8> code{0xf4040004u, 0xfa000000u, 0xf4000080u, 0xfa000000u, 0x7e000202u, 0xf80008cfu, 0u, 0xbf810000u};
    const std::array<std::uint32_t, 2> userData{static_cast<std::uint32_t>(table), static_cast<std::uint32_t>(table >> 32u)};
    RecompileRequest request{};
    request.shader = {ShaderStage::Vertex, 0x10000u, code, 0, {}};
    request.context.waveSize = 64;
    request.context.userDataBaseRegister = 8;
    request.context.userData = userData;
    request.context.vertex = ShaderVertexStageInfo{};
    request.target.vulkanVersion = 0x00401000u;
    request.target.spirvVersion = 0x00010300u;
    request.target.subgroupSize = 64;
    request.target.fragmentShaderBarycentricEnabled = false;
    request.layout.pushConstantSizeBytes = 128;
    const auto handle = ResolveSource(request);
    Require(handle != nullptr, "known values: the test shader has no source handle");
    const std::vector<DeferredLabel> noLabels;
    // The old capture's flat SRT and deferred words, over the fake answers.
    struct Old {
        std::vector<std::uint32_t> flat;
        bool deferred = false;
    };
    const auto capture = [&] {
        AgcDriver::ShaderMemory memory({}, &fakeCaptureQuery);
        const auto captured = memory.Capture(request);
        return Old{captured->snapshot.flattenedSrt, !captured->snapshot.deferredFlat.empty()};
    };
    // The fast walk; the reader is kept for its counters.
    FastReader last{};
    const auto walk = [&](const std::vector<DeferredLabel>& labels, std::vector<std::uint32_t>& flat) {
        last = FastReader{};
        last.exactPending = true;
        last.knownValues = true;
        last.knownLabels = true;
        last.knownEvidence = true;
        last.pendingQuery = &fakeFastQuery;
        last.labels = &labels;
        SrtRuntime runtime;
        runtime.userContext = &last;
        runtime.readMemory = &FastSrtRead;
        runtime.readSpecializationMemory = &FastSrtRead;
        runtime.expressRead = &FastSrtRead;
        ResourceSnapshot snapshot;
        ResourceSpecialization specialization;
        const auto status = WalkResources(*handle, userData, request.shader.codeAddress, runtime, snapshot, specialization);
        flat = snapshot.flattenedSrt;
        return status == WalkStatus::Walked;
    };
    const auto contains = [](const std::vector<std::uint32_t>& flat, std::uint32_t value) { return std::find(flat.begin(), flat.end(), value) != flat.end(); };
    std::vector<std::uint32_t> flat;

    // A pending copy's destination with known bytes: both serve the known word, not the live one.
    constexpr std::uint32_t Known = 0x40490fdbu;
    fakeAnswers = {{payload, {Policy::KnownValue, Known}}};
    recorder.NotePendingWrite(payload, 4);
    auto old = capture();
    Require(walk(noLabels, flat), "known values: a known pending word declined the fast walk");
    Require(flat == old.flat && contains(flat, Known) && !contains(flat, Live) && !old.deferred, "known values: the fast walk's known word differs from the old capture's");
    Require(last.servedKnown == 1 && last.pendingInBlocks >= 1, "known values: the known word was not counted");
    // RawExpected holding the expected value: both read the live word.
    fakeAnswers = {{payload, {Policy::RawExpected, Live}}};
    old = capture();
    Require(walk(noLabels, flat) && flat == old.flat && contains(flat, Live) && last.servedEvidence == 1, "known values: an evidence word the old capture reads raw differs or declined");
    // Raw: the live word.
    fakeAnswers = {{payload, {Policy::Raw, 0}}};
    old = capture();
    Require(walk(noLabels, flat) && flat == old.flat && contains(flat, Live) && last.servedEvidence == 1, "known values: a raw-read word differs or declined");
    // Sync: the capture defers the pure slot to the GPU (or would wait); the fast walk declines.
    fakeAnswers = {{payload, {Policy::Sync, 0}}};
    Require(!walk(noLabels, flat) && last.declined == WalkDecline::Pending, "known values: a word the old capture waits for was served");
    for (const auto policy : {Policy::VerifyKnownValue, Policy::VerifyRaw}) {
        fakeAnswers = {{payload, {policy, Live}}};
        Require(!walk(noLabels, flat) && last.declined == WalkDecline::Pending, "known values: a word the old capture verifies through the hook was served");
    }
    fakeAnswers = {{payload, {Policy::Sync, 0}}};
    old = capture();
    Require(old.deferred, "known values: the old capture did not leave the waited pure word to the GPU");
    // RawExpected with another value in memory: the capture waits for the writer (the hook syncs
    // the pending range: the batch is submitted and finishes), the fast walk declines.
    fakeAnswers = {{payload, {Policy::RawExpected, Live ^ 1u}}};
    Require(!walk(noLabels, flat) && last.declined == WalkDecline::Pending, "known values: an evidence word that changed was served");
    old = capture();
    Require(contains(old.flat, Live) && !old.deferred && !Recorder::BlockPending(payload), "known values: the old capture did not wait for the changed evidence word");

    // A queued label of this thread over the payload: the fast walk serves its bytes; the old path
    // records the label and waits, then reads them (the record's store done here by hand).
    constexpr std::uint32_t Labeled = 0x41200000u;
    std::vector<DeferredLabel> labels{{payload, 4, {}}};
    std::memcpy(labels[0].bytes.data(), &Labeled, sizeof(Labeled));
    fakeAnswers.clear();
    recorder.NotePendingWrite(payload, 4);
    Require(walk(labels, flat) && contains(flat, Labeled) && last.servedLabels == 1, "known values: a queued label's word was not served with its bytes");
    recorder.Sync();
    word(payload) = Labeled;
    old = capture();
    Require(flat == old.flat, "known values: the queued label's word differs from the old path's once the label landed");
    word(payload) = Live;

    // A word changed after it was served. Direct reads of one reader stand for one walk.
    const std::vector<DeferredLabel> none;
    FastReader reader{};
    reader.exactPending = true;
    reader.knownValues = true;
    reader.knownLabels = true;
    reader.knownEvidence = true;
    reader.pendingQuery = &fakeFastQuery;
    reader.labels = &none;
    const auto read = [&](std::uint32_t& value) {
        reader.declined.reset();
        value = 0xdeadbeefu;
        return FastSrtRead(&reader, payload, &value);
    };
    std::uint32_t value = 0;
    constexpr std::uint32_t Newer = 0x40000000u;
    const auto rangeOf = [&](std::uint32_t known) {
        auto bytes = std::make_shared<std::vector<std::byte>>(4);
        std::memcpy(bytes->data(), &known, 4);
        return FakeRange{payload, payload + 4, std::move(bytes)};
    };
    recorder.NotePendingWrite(payload, 4);
    fakeAnswers = {{payload, {Policy::KnownValue, Known}}};
    fakeRange = rangeOf(Known);
    Require(read(value) && value == Known, "known values: a known word was not served");
    // The classification changes where nothing it reads moved: the walk keeps its answer (one
    // classification per range and walk, as the old validation's per region).
    fakeAnswers = {{payload, {Policy::KnownValue, Newer}}};
    fakeRange = rangeOf(Newer);
    auto queries = fakeFastQueries;
    Require(read(value) && value == Known && fakeFastQueries == queries, "known values: a served range was classified again with nothing moved");
    // A writer noted since (the push count moved): classified again, the new known word served.
    WrittenBufferPushes().fetch_add(1);
    Require(read(value) && value == Newer && fakeFastQueries == queries + 1, "known values: a word was not served again after a new writer");
    // A pending range noted since (the generation moved) and the word now waited for: declines.
    fakeAnswers = {{payload, {Policy::Sync, 0}}};
    recorder.NotePendingWrite(payload + 0x40, 4);
    Require(!read(value) && reader.declined == WalkDecline::Pending, "known values: a word the old capture now waits for was served from the walk's range");
    // A label noted inside the served range (another queue's EOP or WRITE_DATA into a pending
    // copy's destination): the snapshot already covers it, so nothing is published and no writer
    // is pushed, but the old capture's next classification finds the label (Sync). The write
    // generation moves: classified again, declined.
    fakeAnswers = {{payload, {Policy::KnownValue, Known}}};
    fakeRange = rangeOf(Known);
    Require(read(value) && value == Known, "known values: a known word was not served again");
    fakeAnswers = {{payload, {Policy::Sync, 0}}};
    queries = fakeFastQueries;
    const auto published = Recorder::PublishGeneration();
    const auto writes = Recorder::WriteGeneration();
    const auto pushes = WrittenBufferPushes().load();
    std::array<std::byte, 4> labelBytes{};
    std::memcpy(labelBytes.data(), &Live, sizeof(Live));
    recorder.NoteLabel(payload, labelBytes, 6, 0);
    Require(Recorder::PublishGeneration() == published && WrittenBufferPushes().load() == pushes && Recorder::WriteGeneration() != writes, "known values: the covered label note was published or noted no write");
    Require(!read(value) && reader.declined == WalkDecline::Pending && fakeFastQueries == queries + 1, "known values: a word under a label noted inside the served range was served from the range");
    // An evidence word: served while memory holds the expected value, declined once it changed.
    fakeAnswers = {{payload, {Policy::RawExpected, Live}}};
    Require(read(value) && value == Live, "known values: an evidence word holding its value was not served");
    word(payload) = Live ^ 0x10u;
    Require(!read(value) && reader.declined == WalkDecline::Pending, "known values: an evidence word that changed after it was served was served again");
    word(payload) = Live;
    // Evidence words switched off (APS5_FAST_KNOWN_EVIDENCE=0): RawExpected and Raw decline,
    // known values are still served.
    reader.knownEvidence = false;
    for (const auto policy : {Policy::RawExpected, Policy::Raw}) {
        fakeAnswers = {{payload, {policy, Live}}};
        Require(!read(value) && reader.declined == WalkDecline::Pending, "known values: an evidence word was served with evidence serving off");
    }
    fakeAnswers = {{payload, {Policy::KnownValue, Known}}};
    fakeRange = {};
    Require(read(value) && value == Known, "known values: a known word declined with evidence serving off");
    reader.knownEvidence = true;
    // Switched off (APS5_FAST_KNOWN_VALUES=0) or without a query: declines as before.
    fakeAnswers = {{payload, {Policy::KnownValue, Known}}};
    reader.knownValues = false;
    Require(!read(value) && reader.declined == WalkDecline::Pending, "known values: a pending word was served with known values off");
    reader.knownValues = true;
    reader.pendingQuery = nullptr;
    Require(!read(value) && reader.declined == WalkDecline::Pending, "known values: a pending word was served without a query");
    recorder.Sync();
    fakeAnswers.clear();
    fakeRange = {};

    // LabelValueIn: as LookupLabelValue over each dword of the range, for recorded labels, a queued
    // one of this thread and words without any.
    std::array<std::byte, 8> bytes{};
    recorder.NoteLabel(base + 0x1000, std::span(bytes).first(4), 7, 0);
    recorder.NoteLabel(base + 0x1010, bytes, 8, 0);
    Recorder::NoteQueuedLabel(base + 0x1100, std::span(bytes).first(4), 9, AgcDriver::GuestMemory::GpuLockThreadTag());
    for (const auto& [begin, size] : std::initializer_list<std::pair<std::uint64_t, std::size_t>>{{0xff0, 0x10}, {0xffc, 4}, {0x1000, 4}, {0x1002, 4}, {0x1004, 0xc}, {0x1004, 0xd}, {0x1014, 4}, {0x1018, 0x100}, {0x10fc, 4}, {0x1100, 4}, {0x1104, 0x40}, {0x800, 0x1000}}) {
        bool any = false;
        for (auto dword = (base + begin) & ~std::uint64_t{3}; dword < base + begin + size; dword += 4) any = any || Recorder::LookupLabelValue(dword, 4, 0).has_value();
        Require(Recorder::LabelValueIn(base + begin, size) == any, "known values: LabelValueIn differs from LookupLabelValue over the range's dwords");
    }
    // Its scan is no wait: this thread's queued label is not a same-queue wait hit ([labels] line).
    const auto hits = Recorder::StoreCounts().queuedHits;
    Require(Recorder::LabelValueIn(base + 0x1000, 4) && Recorder::LabelValueIn(base + 0x1100, 4) && !Recorder::LabelValueIn(base + 0x1004, 0xc), "known values: LabelValueIn missed a label or found one where there is none");
    Require(Recorder::StoreCounts().queuedHits == hits, "known values: LabelValueIn counted a same-queue wait hit");
    Require(Recorder::LookupLabelValue(base + 0x1100, 4, 0).has_value() && Recorder::StoreCounts().queuedHits == hits + 1, "known values: LookupLabelValue no longer counts a same-queue wait hit");
    Recorder::ForgetQueuedLabels();
    recorder.Sync();
    Recorder::TrackPendingBlocks(false);
}

// Completion counting: a write-back completion (OnComplete) is pending until its batch finished;
// a completion label the GPU also stored (AfterCompletions storedOnGpu) is pending only once a CPU
// write-back overlapped it (NoteWrittenBack, once per label), one the GPU has no view of from its
// registration; both counts return to 0 when the batch finishes.
void completionCountTests(const Device& device, Recorder& recorder) {
    alignas(64) static std::uint32_t memory[64];
    const auto base = reinterpret_cast<std::uint64_t>(memory);
    const std::array<std::byte, 4> value{std::byte{1}, std::byte{0}, std::byte{0}, std::byte{0}};
    Require(recorder.Idle() && Recorder::PendingCompletionLabels() == 0 && Recorder::PendingWriteBackCompletions() == 0, "a fresh recorder reports pending completions");
    int ran = 0;
    recorder.OnComplete([&] { ++ran; });
    Require(Recorder::PendingWriteBackCompletions() == 1 && recorder.HasCompletions(), "a write-back completion is not pending");
    // APS5_COUNT_ALL_COMPLETION_LABELS=1 (or APS5_LABEL_STORE_ALWAYS=1) counts the GPU-stored
    // labels at registration, as before; the write-back then changes nothing.
    const bool countAll = std::getenv("APS5_COUNT_ALL_COMPLETION_LABELS") != nullptr || std::getenv("APS5_LABEL_STORE_ALWAYS") != nullptr;
    const std::uint64_t registered = countAll ? 2 : 0;
    recorder.AfterCompletions(base, value, 3, 0, true);
    recorder.AfterCompletions(base + 8, value, 4, 0, true);
    Require(Recorder::PendingCompletionLabels() == registered, "a GPU-stored completion label counts before any write-back");
    recorder.AfterCompletions(base + 0x40, value, 5, 0, false);
    Require(Recorder::PendingCompletionLabels() == registered + 1, "a label the GPU has no view of is not pending");
    // A late lookup (stamp <= afterStamp) of a label whose value reaches memory only through the
    // batch's completion action is refused as BehindCompletion (a poller reaps for it); a GPU-stored
    // one keeps the ordinary reason (unclosed group, or trust off) until a write-back overlaps it.
    using Refusal = Recorder::LabelRefusal;
    const Refusal ordinary = Recorder::LateTrust() ? Refusal::Unclosed : Refusal::TrustOff;
    Refusal refusal{};
    Require(!Recorder::LookupLabel(base + 0x40, 4, 5, &refusal).has_value() && refusal == Refusal::BehindCompletion, "a not-imported completion label is not refused as behind a completion");
    Require(!Recorder::LookupLabel(base, 4, 3, &refusal).has_value() && refusal == (countAll ? Refusal::BehindCompletion : ordinary), "a GPU-stored label is refused as behind a completion before any write-back");
    Require(Recorder::LookupLabel(base, 4, 2, &refusal).has_value() && refusal == Refusal::None, "an early lookup of a completion label is refused");
    Recorder::NoteWrittenBack(base + 0x80, 0x40);
    Require(Recorder::PendingCompletionLabels() == registered + 1, "a disjoint write-back counted a label");
    Recorder::NoteWrittenBack(base, 4);
    Recorder::NoteWrittenBack(base + 2, 4);
    const std::uint64_t counted = countAll ? 3 : 2;
    Require(Recorder::PendingCompletionLabels() == counted, "a write-back over a GPU-stored label did not count it exactly once");
    Require(!Recorder::LookupLabel(base, 4, 3, &refusal).has_value() && refusal == Refusal::BehindCompletion, "a written-back GPU-stored label is not refused as behind a completion");
    Require(!Recorder::LookupLabel(base + 8, 4, 4, &refusal).has_value() && refusal == (countAll ? Refusal::BehindCompletion : ordinary), "an untouched GPU-stored label became behind a completion");
    recorder.Submit();
    device.WaitQueue();
    Require(Recorder::PendingCompletionLabels() == counted && Recorder::PendingWriteBackCompletions() == 1, "counts dropped before the batch finished");
    memory[0] = 7;
    memory[2] = 7;
    memory[16] = 7;
    recorder.Sync();
    Require(ran == 1 && Recorder::PendingCompletionLabels() == 0 && Recorder::PendingWriteBackCompletions() == 0 && recorder.Idle(), "counts did not return to 0 at finish");
    const bool storeAlways = std::getenv("APS5_LABEL_STORE_ALWAYS") != nullptr;
    Require(memory[0] == 1 && memory[2] == (storeAlways ? 1u : 7u) && memory[16] == 1, "completion stores ran for the wrong labels (overlapped and not-imported ones store, the untouched GPU-stored one skips)");
}

void afterRecordedWorkTests(const Device& device, Recorder& recorder) {
    alignas(64) static std::uint32_t memory[16];
    const auto base = reinterpret_cast<std::uint64_t>(memory);
    const std::array<std::byte, 4> value{std::byte{1}, std::byte{0}, std::byte{0}, std::byte{0}};
    recorder.Sync();
    std::vector<int> ran;
    std::uint32_t seen = 0;
    Require(recorder.Idle() && !recorder.AfterRecordedWork([&] { ran.push_back(0); }) && ran.empty() && Recorder::PendingCompletionLabels() == 0, "an idle recorder kept an action for recorded work");
    recorder.NotePendingWrite(0x52000, 0x100);
    Require(!Recorder::PendingLabelSince().has_value(), "a write started the label flush deadline");
    Require(recorder.AfterRecordedWork([&] { ran.push_back(1); }) && Recorder::PendingCompletionLabels() == 1 && Recorder::PendingLabelSince().has_value(), "an action behind the open batch is not pending under the label flush deadline");
    recorder.AfterCompletions(base, value, 6, 0, false);
    Require(recorder.AfterRecordedWork([&] { seen = memory[0]; ran.push_back(2); }) && Recorder::PendingCompletionLabels() == 3, "an action behind a completion label is not pending");
    recorder.Submit();
    Require(recorder.AfterRecordedWork([&] { ran.push_back(3); }) && Recorder::PendingCompletionLabels() == 4, "an action behind an in-flight batch is not pending");
    device.WaitQueue();
    Require(ran.empty(), "an action ran before its batch was reaped");
    recorder.Sync();
    Require(ran == std::vector<int>{1, 2, 3} && Recorder::PendingCompletionLabels() == 0 && recorder.Idle(), "actions behind recorded work did not run once each, in order");
    Require(seen == 1, "an action ran before the completion label recorded ahead of it");
}

void batchStampTests(Recorder& recorder) {
    Require(recorder.Idle(), "batch stamps: the recorder is busy");
    double previousEnd = 0;
    for (int round = 0; round < 3; ++round) {
        recorder.NotePendingWrite(0x60000, 0x100);
        const auto serial = recorder.Submissions() + 1;
        recorder.Submit();
        recorder.Sync();
        std::size_t missing = 0;
        const auto batches = recorder.CompletedBatches(serial - 1, serial, missing);
        Require(missing == 0 && batches.size() == 1 && batches.front().serial == serial, "a finished batch has no completion record");
        const auto& batch = batches.front();
        if (Recorder::BatchStampsEnabled()) {
            Require(batch.gpuStartNs > 0 && batch.gpuEndNs >= batch.gpuStartNs && batch.gpuStartNs >= previousEnd, "batch stamps are missing or out of order");
            previousEnd = batch.gpuEndNs;
        } else {
            Require(batch.gpuStartNs == 0 && batch.gpuEndNs == 0, "batch stamps were written with profiling off");
        }
    }
    std::cout << "batch stamps " << (Recorder::BatchStampsEnabled() ? "checked" : "off") << '\n';
}

void labelTests(Recorder& recorder) {
    const std::array<std::byte, 4> value{std::byte{1}, std::byte{0}, std::byte{0}, std::byte{0}};
    Require(!recorder.PendingLabelIn(0x60000, 0x100), "an empty table reports a label");
    // A label this worker queued but has not recorded yet is inside the range for a CPU store decision.
    Recorder::NoteQueuedLabel(0x60010, value, 1, AgcDriver::GuestMemory::GpuLockThreadTag());
    Require(recorder.PendingLabelIn(0x60010, 4) && recorder.PendingLabelIn(0x60000, 0x100) && !recorder.PendingLabelIn(0x60014, 4) && !recorder.PendingLabelIn(0x60000, 0x10), "a queued label is not seen by PendingLabelIn");
    // The capture's page query sees it (the page is then read word by word); a word beside it does not.
    const auto pageQueries = Recorder::StoreCounts().queuedPageQueries;
    Require(Recorder::QueuedLabelOverlapsThisThread(0x60000, 0x1000) && !Recorder::QueuedLabelOverlapsThisThread(0x60014, 4) && !Recorder::QueuedLabelOverlapsThisThread(0x61000, 0x1000), "a queued label's page overlap is wrong");
    Require(Recorder::StoreCounts().queuedPageQueries == pageQueries + 1, "a queued label's page query was not counted once");
    Recorder::ForgetQueuedLabels();
    Require(!Recorder::QueuedLabelOverlapsThisThread(0x60000, 0x1000), "a forgotten queued label still overlaps its page");
    Require(!recorder.PendingLabelIn(0x60000, 0x100), "a forgotten queued label is still pending");
    recorder.NoteLabel(0x60020, value, 2, 0);
    Require(recorder.PendingLabelIn(0x60020, 4) && recorder.PendingLabelIn(0x60000, 0x100) && !recorder.PendingLabelIn(0x60000, 0x20), "a recorded label is not seen by PendingLabelIn");
    recorder.Sync();
    Require(!recorder.PendingLabelIn(0x60000, 0x100), "a recorded label outlived its batch");
}

// The late rule (Recorder::NoteLabel): an entry with stamp <= afterStamp is served once its group
// closed, unless a non-label write covered it, it is queued, or APS5_LABEL_TRUST_LATE=0 (run the
// binary with that set for the switch-off case).
void lateLabelTests(Recorder& recorder) {
    using Refusal = Recorder::LabelRefusal;
    const std::array<std::byte, 4> one{std::byte{1}, std::byte{0}, std::byte{0}, std::byte{0}};
    const std::array<std::byte, 4> two{std::byte{2}, std::byte{0}, std::byte{0}, std::byte{0}};
    const std::array<std::byte, 8> pair{std::byte{1}, std::byte{0}, std::byte{0}, std::byte{0}, std::byte{2}, std::byte{0}, std::byte{0}, std::byte{0}};
    const auto tag = AgcDriver::GuestMemory::GpuLockThreadTag();
    constexpr std::uint64_t a = 0x70000, b = 0x70100, c = 0x70200, d = 0x70300;
    Refusal refusal{};
    const auto before = Recorder::LateCounts();
    recorder.NoteLabel(a, one, 10, 0);
    const auto early = Recorder::LookupLabel(a, 4, 5, &refusal);
    Require(early.has_value() && !early->late && early->value == 1 && early->queue == 0 && early->stamp == 10 && early->generation == 0 && refusal == Refusal::None, "an early lookup is wrong");
    auto late = Recorder::LookupLabel(a, 4, 10, &refusal);
    if (!Recorder::LateTrust()) {
        Require(!late.has_value() && refusal == Refusal::TrustOff, "(1) a late lookup is not refused with the switch off");
        Require(Recorder::LateCounts().candidates == before.candidates + 1, "(1) the late candidate is not counted with the switch off");
        Recorder::CloseLabelGroup(AgcDriver::GuestMemory::TrackerGeneration());
        Require(!Recorder::LookupLabel(a, 4, 10, &refusal).has_value() && refusal == Refusal::TrustOff, "(1) a closed group is served with the switch off");
        Require(Recorder::LookupLabel(a, 4, 5).has_value(), "(1) the early lookup broke with the switch off");
        recorder.Sync();
        Require(!Recorder::LookupLabel(a, 4, 5).has_value(), "(7) the entry outlived its batch");
        std::cout << "late label tests ran with APS5_LABEL_TRUST_LATE=0\n";
        return;
    }
    Require(!late.has_value() && refusal == Refusal::Unclosed, "(2) an unclosed group is served");
    Require(Recorder::LateCounts().candidates == before.candidates + 1 && Recorder::LateCounts().unclosed == before.unclosed + 1, "(2) the unclosed refusal is not counted");
    const auto generation = AgcDriver::GuestMemory::TrackerGeneration();
    Recorder::CloseLabelGroup(generation);
    late = Recorder::LookupLabel(a, 4, 10, &refusal);
    Require(late.has_value() && late->late && late->value == 1 && late->generation == generation && late->stamp == 10 && refusal == Refusal::None, "(2) a closed group is not served");
    Require(Recorder::LookupLabel(a, 4, 9)->late == false, "(2) an early lookup became late");
    // (3) A disjoint write leaves the entry alone; an overlapping non-label write refuses the late
    // lookup only (the early one still composes what memory will hold in order).
    recorder.NotePendingWrite(a + 4, 16);
    Require(Recorder::LookupLabel(a, 4, 10).has_value(), "(3) a disjoint write flagged the entry");
    recorder.NotePendingWrite(a - 8, 12);
    late = Recorder::LookupLabel(a, 4, 10, &refusal);
    Require(!late.has_value() && refusal == Refusal::Overwritten, "(3) an overwritten entry is served late");
    Require(Recorder::LookupLabel(a, 4, 5).has_value(), "(3) an overwrite refused the early lookup");
    // The label's own note (inside NoteLabel) is no overwrite: an 8-byte label closed as a group.
    recorder.NoteLabel(b, pair, 20, 0);
    Recorder::CloseLabelGroup(AgcDriver::GuestMemory::TrackerGeneration());
    const auto wide = Recorder::LookupLabel(b, 8, 20, &refusal);
    Require(wide.has_value() && wide->late && wide->value == 0x200000001ull && refusal == Refusal::None, "(3) the label's own note refused it");
    // (4) A later label on the dword replaces the entry: unclosed until its own group closes.
    recorder.NoteLabel(a, two, 30, 0);
    Require(!Recorder::LookupLabel(a, 4, 40, &refusal).has_value() && refusal == Refusal::Unclosed, "(4) a replaced entry kept the old close or flag");
    const auto replaced = Recorder::LookupLabel(a, 4, 25);
    Require(replaced.has_value() && !replaced->late && replaced->value == 2 && replaced->stamp == 30, "(4) the replaced entry's early lookup is wrong");
    const auto generation2 = AgcDriver::GuestMemory::TrackerGeneration();
    Recorder::CloseLabelGroup(generation2);
    late = Recorder::LookupLabel(a, 4, 40, &refusal);
    Require(late.has_value() && late->late && late->value == 2 && late->generation == generation2, "(4) the replaced entry is not late-trustable after its close");
    // (5) Queued entries: another queue's is never seen, this queue's is early-only.
    const std::uint32_t other = tag == 7 ? 8 : 7;
    Recorder::NoteQueuedLabel(d, one, 50, other);
    Require(!Recorder::LookupLabel(d, 4, 60, &refusal).has_value() && refusal == Refusal::None && !Recorder::LookupLabel(d, 4, 40).has_value(), "(5) another queue's queued label is served");
    Recorder::NoteQueuedLabel(d, one, 51, tag);
    Recorder::ForgetQueuedLabels();
    Recorder::NoteQueuedLabel(c, one, 50, tag);
    Require(Recorder::LookupLabel(c, 4, 40).has_value() && !Recorder::LookupLabel(c, 4, 40)->late, "(5) this queue's queued label is not served early");
    Require(!Recorder::LookupLabel(c, 4, 60, &refusal).has_value() && refusal == Refusal::Queued, "(5) a queued entry is late-trusted");
    Recorder::CloseLabelGroup(AgcDriver::GuestMemory::TrackerGeneration());
    Require(!Recorder::LookupLabel(c, 4, 60, &refusal).has_value() && refusal == Refusal::Queued, "(5) a close closed a queued entry");
    Recorder::ForgetQueuedLabels();
    Require(!Recorder::LookupLabel(c, 4, 40).has_value(), "(5) a forgotten queued entry is still served");
    const auto after = Recorder::LateCounts();
    Require(after.queued == before.queued + 2 && after.overwritten == before.overwritten + 1 && after.unclosed == before.unclosed + 2 && after.candidates >= before.candidates + 6, "late counters are off");
    // (7) Entries leave the table with their batch.
    recorder.Sync();
    Require(!Recorder::LookupLabel(a, 4, 5).has_value() && !Recorder::LookupLabel(b, 8, 5).has_value() && !recorder.PendingLabelIn(0x70000, 0x400), "(7) entries outlived their batch");
}

// (6) GuestMemory::UnchangedSinceCollected: false outside the watched arena, and around a
// MarkWritten or a CPU write of the block inside it.
void unchangedSinceTests() {
    using namespace AgcDriver::GuestMemory;
    static std::uint32_t outside[16];
    Require(!UnchangedSinceCollected(reinterpret_cast<std::uint64_t>(outside), 4, TrackerGeneration()), "(6) a range outside the watched memory is unchanged");
    Require(!Watched(reinterpret_cast<std::uint64_t>(outside), 4) && CollectWrites(reinterpret_cast<std::uint64_t>(outside), 4) == 0, "(6) a range outside the watched memory is watched");
    constexpr std::size_t bytes = 65536;
    void* block = AllocateWatched(bytes, bytes);
    if (block == nullptr) {
        std::cout << "no write watching: UnchangedSinceCollected is always false\n";
        return;
    }
    auto* words = static_cast<volatile std::uint32_t*>(block);
    const auto address = reinterpret_cast<std::uint64_t>(block);
    words[0] = 0;
    CollectWritesUncached(address, 4);
    const auto generation = TrackerGeneration();
    Require(UnchangedSinceCollected(address, 4, generation), "(6) an untouched range is not unchanged");
    Require(UnchangedSinceCollected(address, 4, generation), "(6) the check itself dirtied the range");
    Require(!UnchangedSinceCollected(address, 4, 0), "(6) generation 0 is unchanged");
    MarkWritten(address, 4);
    Require(UnchangedSinceCollected(address, 4, generation), "(6) a MarkWritten of the block (a GPU label record) refuses");
    Require(!UnchangedSince(address, 4, generation), "(6) a MarkWritten of the block is not seen by UnchangedSince");
    const auto generation2 = TrackerGeneration();
    Require(UnchangedSinceCollected(address, 4, generation2), "(6) unchanged after the stamp fails");
    words[1] = 1;
    Require(!UnchangedSinceCollected(address, 4, generation2), "(6) a CPU write to the page is not seen");
    Require(UnchangedSinceCollected(address, 4, TrackerGeneration()), "(6) the collect did not reset the page");
    const auto generation3 = TrackerGeneration();
    words[1] = 2;
    CollectWritesUncached(address, 4);
    Require(!UnchangedSinceCollected(address, 4, generation3), "(6) a CPU write collected by another caller is not seen");
    ReleaseWatched(block, bytes);
}

// (9) ProvedClearKeys: a surface's DCC keys are scanned once and answered from the proof after,
// until the key range is stamped (a GPU key store's MarkWritten) or the CPU writes it; a scan made
// while recorded work still writes the keys is not kept. With APS5_NO_KEY_FAST_PATH=1 every call
// scans and no proof is stored.
void keyProofTests(const Device& device, Recorder& recorder) {
    using namespace AgcDriver::GuestMemory;
    constexpr std::size_t bytes = 65536;
    void* block = AllocateWatched(bytes, bytes);
    if (block == nullptr) {
        std::cout << "no write watching: key proofs not tested\n";
        return;
    }
    struct Release {
        void* block;
        ~Release() { ReleaseWatched(block, bytes); }
    } release{block};
    constexpr std::size_t keyCount = 1024;
    constexpr std::uint64_t surfaceBytes = keyCount * 256;
    auto* keys = static_cast<std::uint8_t*>(block);
    std::memset(keys, 0x00, keyCount);
    const auto address = reinterpret_cast<std::uint64_t>(block);
    GuestTextureResource resource{};
    resource.baseAddress = address + 8192;
    resource.width = 256;
    resource.height = 256;
    resource.mipCount = 1;
    resource.tileMode = TextureTileMode::kLinear;
    resource.dimension = TextureDimension::k2D;
    resource.format = 56;
    resource.dccAddress = address;
    DccKeyProof proof;
    auto before = KeyProofCounts();
    Require(ProvedClearKeys(resource, surfaceBytes, proof) == DccKeys::Clear0000, "(9) 0x00 keys are not a 0000 clear");
    auto after = KeyProofCounts();
    Require(after.scanned == before.scanned + 1 && after.proved == before.proved, "(9) the first call did not scan");
    if (!KeyFastPath()) {
        Require(proof.generation == 0, "(9) APS5_NO_KEY_FAST_PATH stored a proof");
        Require(ProvedClearKeys(resource, surfaceBytes, proof) == DccKeys::Clear0000 && proof.generation == 0, "(9) APS5_NO_KEY_FAST_PATH proved keys");
        std::cout << "key fast path off: every call scans\n";
        return;
    }
    Require(proof.generation != 0 && proof.keys == DccKeys::Clear0000 && after.unstable == before.unstable, "(9) a stable scan left no proof");
    Require(ProvedClearKeys(resource, surfaceBytes, proof) == DccKeys::Clear0000, "(9) the proof answers other keys");
    before = KeyProofCounts();
    Require(before.proved == after.proved + 1 && before.scanned == after.scanned, "(9) the second call scanned");
    const auto generation = proof.generation;
    MarkWritten(address, keyCount);
    Require(ProvedClearKeys(resource, surfaceBytes, proof) == DccKeys::Clear0000, "(9) unchanged bytes read differently");
    after = KeyProofCounts();
    Require(after.scanned == before.scanned + 1 && after.proved == before.proved && proof.generation > generation, "(9) a stamped key range was proved");
    std::memset(keys, 0x40, keyCount);
    Require(ProvedClearKeys(resource, surfaceBytes, proof) == DccKeys::Clear0001, "(9) a CPU write of the keys was not seen");
    before = KeyProofCounts();
    Require(before.scanned == after.scanned + 1 && proof.keys == DccKeys::Clear0001, "(9) the CPU write did not make a scan");
    Require(ProvedClearKeys(resource, surfaceBytes, proof) == DccKeys::Clear0001 && KeyProofCounts().proved == before.proved + 1, "(9) the new keys were not proved");
    // A recorded write over the keys (noted and stamped as the driver's key stores are): the scans
    // are not kept until its batch signaled.
    recorder.NotePendingWrite(address, keyCount);
    MarkWritten(address, keyCount);
    before = KeyProofCounts();
    Require(ProvedClearKeys(resource, surfaceBytes, proof) == DccKeys::Clear0001 && proof.generation == 0, "(9) a scan under a pending write was kept");
    Require(ProvedClearKeys(resource, surfaceBytes, proof) == DccKeys::Clear0001 && proof.generation == 0, "(9) a second scan under a pending write was kept");
    after = KeyProofCounts();
    Require(after.scanned == before.scanned + 2 && after.unstable == before.unstable + 2 && after.proved == before.proved, "(9) unstable scans were miscounted");
    recorder.Submit();
    device.WaitQueue();
    Require(ProvedClearKeys(resource, surfaceBytes, proof) == DccKeys::Clear0001 && proof.generation != 0, "(9) a signaled write kept the scan unstable");
    recorder.Sync();
    before = KeyProofCounts();
    Require(ProvedClearKeys(resource, surfaceBytes, proof) == DccKeys::Clear0001 && KeyProofCounts().proved == before.proved + 1, "(9) the proof did not hold after the batch finished");
}

// (8) The group close against a collect in progress: a CPU store made after the close must refuse
// the entry although another thread's resetting walk of the label's page (over a large range) may
// absorb the store and stamp the block with the generation it took before the close. And an entry
// whose batch was submitted before the close stays unclosed.
void closeRaceTests(const Device& device, Recorder& recorder) {
    using namespace AgcDriver::GuestMemory;
    using Refusal = Recorder::LabelRefusal;
    const std::array<std::byte, 4> one{std::byte{1}, std::byte{0}, std::byte{0}, std::byte{0}};
    Refusal refusal{};
    if (!Recorder::LateTrust()) return;
    recorder.NoteLabel(0x80000, one, 70, 0);
    recorder.Submit();
    Recorder::CloseLabelGroup(TrackerGeneration());
    Require(!Recorder::LookupLabel(0x80000, 4, 70, &refusal).has_value() && refusal == Refusal::Unclosed, "(8) a submitted batch's entry was closed");
    device.WaitQueue();
    recorder.Sync();
    constexpr std::size_t bytes = 1u << 20;
    void* block = AllocateWatched(bytes, 65536);
    if (block == nullptr) return;
    const auto base = reinterpret_cast<std::uint64_t>(block);
    // Late in the range, so the walker is usually still ahead of the page when the store lands.
    const auto label = base + 4096 * 250;
    auto* word = reinterpret_cast<volatile std::uint32_t*>(label);
    *word = 0;
    CollectWritesUncached(base, bytes);
    // Joined on every exit, so a failed Require reports instead of terminating on the thread.
    struct Walker {
        std::atomic<bool> stop{false};
        std::thread thread;
        ~Walker() {
            stop.store(true, std::memory_order_relaxed);
            if (thread.joinable()) thread.join();
        }
    } walker;
    walker.thread = std::thread([&] {
        while (!walker.stop.load(std::memory_order_relaxed)) CollectWritesUncached(base, bytes);
    });
    for (std::uint32_t i = 0; i < 4000; ++i) {
        const std::uint64_t stamp = 100 + i;
        recorder.NoteLabel(label, one, stamp, 0);
        Recorder::CloseLabelGroup(TrackerGeneration());
        *word = i + 2;
        const auto hit = Recorder::LookupLabel(label, 4, stamp, &refusal);
        Require(hit.has_value() && hit->late && hit->generation != 0, "(8) the closed entry is not served late");
        Require(!UnchangedSinceCollected(label, 4, hit->generation), "(8) a store after the close is hidden by a racing collect");
    }
    walker.stop.store(true, std::memory_order_relaxed);
    walker.thread.join();
    recorder.Sync();
    Require(!Recorder::LookupLabel(label, 4, 5).has_value(), "(8) the entry outlived its batch");
    ReleaseWatched(block, bytes);
}

// A resource build over host-imported guest memory notes its in-place reads when its writes are
// marked, so a CPU copy into that memory is refused until the batch ran.
// Per-batch store runs (Recorder::RecordStore): the stores of a batch queue until Submit, a
// later writer or reader of a queued store's bytes has the run recorded in place first, and the
// stores land in the import's memory once the batch ran. With APS5_LABEL_RUNS_INLINE=1 or
// APS5_NO_LABEL_RUNS=1 (run the binary with either set) nothing queues and the stores land alike.
void storeRunTests(const Device& device, Recorder& recorder) {
    const auto& context = device.GetContext();
    if (context.hostImportAlignment == 0) {
        std::cout << "host imports unavailable: store runs not tested\n";
        return;
    }
    constexpr std::size_t bytes = 65536;
#ifdef _WIN32
    void* block = VirtualAlloc(nullptr, bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
#else
    void* block = std::aligned_alloc(65536, bytes);
#endif
    Require(block != nullptr, "cannot allocate the store test block");
    std::memset(block, 0, bytes);
    const auto address = reinterpret_cast<std::uint64_t>(block);
    {
        GuestAllocations::Mutation mutation;
        mutation.Add(block, bytes, true, true);
    }
    const auto* import = HostImportFor(context, address, bytes);
    if (import == nullptr) {
        std::cout << "host import of the store test block refused: store runs not tested\n";
        return;
    }
    const bool perBatch = std::getenv("APS5_LABEL_RUNS_INLINE") == nullptr && std::getenv("APS5_NO_LABEL_RUNS") == nullptr && std::getenv("APS5_NO_LABEL_BATCHING") == nullptr;
    auto* words = static_cast<volatile std::uint32_t*>(block);
    const auto store = [&](std::size_t word, std::uint32_t value) {
        const std::array<std::byte, 4> value4{std::byte{static_cast<unsigned char>(value)}, std::byte{static_cast<unsigned char>(value >> 8u)}, std::byte{static_cast<unsigned char>(value >> 16u)}, std::byte{static_cast<unsigned char>(value >> 24u)}};
        recorder.RecordStore(import->buffer, address + word * 4 - import->base, value4, address + word * 4);
    };
    Require(recorder.Idle() && !recorder.HasQueuedStores(), "a fresh recorder has queued stores");
    store(0, 1);
    store(1, 2);
    store(4, 3);
    store(4, 4);
    Require(recorder.Recording(), "a store did not open a batch");
    Require(recorder.HasQueuedStores() == perBatch, "per-batch runs do not queue (or inline runs do)");
    Require(recorder.QueuedStoreOverlaps(address, 8) == perBatch && recorder.QueuedStoreOverlaps(address + 16, 4) == perBatch && !recorder.QueuedStoreOverlaps(address + 8, 8), "queued store ranges are wrong");
    const auto before = Recorder::StoreCounts();
    // A reader or writer of untouched bytes forces nothing; one over a queued store records the run.
    recorder.FlushStoresOverlapping(address + 8, 8);
    Require(recorder.HasQueuedStores() == perBatch, "a disjoint range flushed the run");
    recorder.FlushStoresOverlapping(address + 16, 4);
    Require(!recorder.HasQueuedStores() && Recorder::StoreCounts().runsForced == before.runsForced + (perBatch ? 1 : 0), "an overlapping writer did not record the run in place");
    store(2, 5);
    Require(recorder.HasQueuedStores() == perBatch, "a store after a forced run did not start a new run");
    recorder.Submit();
    Require(!recorder.HasQueuedStores() && Recorder::StoreCounts().runsAtSubmit == before.runsAtSubmit + (perBatch ? 1 : 0), "Submit did not record the run");
    recorder.Sync();
    Require(words[0] == 1 && words[1] == 2 && words[2] == 5 && words[4] == 4 && words[3] == 0, "the stores did not land (or the later store of a dword lost)");
    const auto after = Recorder::StoreCounts();
    Require(after.stores == before.stores + 1 && after.replaced == before.replaced + 0 && after.joined == before.joined + 0, "store counters are off");
    // The covered-access mask: reported once by the next Commands() call, then cleared.
    VkAccessFlags covered = 0xffffffffu;
    recorder.Commands(&covered);
    Require(covered == 0, "a new batch starts covered");
    recorder.MarkCovered(VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT);
    recorder.Commands(&covered);
    Require(covered == (VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT), "the covered mask is not reported");
    recorder.Commands(&covered);
    Require(covered == 0, "the covered mask survived a Commands() call");
    recorder.Sync();
    // The hazard tracker (counting mode only): accesses are noted without effect on the batch.
    const std::pair<std::uint64_t, std::uint64_t> range{address, address + 64};
    recorder.NoteAccess(Recorder::CommandClass::Fill, Recorder::Access{{}, std::span(&range, 1), {}, VK_PIPELINE_STAGE_TRANSFER_BIT});
    recorder.NoteAccess(Recorder::CommandClass::DispatchLeading, Recorder::Access{std::span(&range, 1), {}, {}, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT});
    Require(recorder.Recording() == Recorder::BarrierValidate(), "a noted access opened a batch with the tracker off (or none with it on)");
    recorder.Sync();
    {
        GuestAllocations::Mutation mutation;
        mutation.Remove(block);
    }
    HostImportFor(context, address, bytes);
}

void remappedImportTests(const Device& device) {
    const auto& context = device.GetContext();
    if (context.hostImportAlignment == 0) {
        std::cout << "host imports unavailable: remapped imports not tested\n";
        return;
    }
    constexpr std::size_t bytes = 65536;
#ifdef _WIN32
    void* block = VirtualAlloc(nullptr, bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
#else
    void* block = std::aligned_alloc(65536, bytes);
#endif
    Require(block != nullptr, "cannot allocate the remap test block");
    const auto address = reinterpret_cast<std::uint64_t>(block);
    {
        GuestAllocations::Mutation mutation;
        mutation.Add(block, bytes, true, true);
    }
    if (HostImportFor(context, address, bytes) == nullptr) {
        std::cout << "host import of the remap test block refused: remapped imports not tested\n";
        return;
    }
    const auto first = HostImportSerial(context, address, bytes, true);
    Require(first != 0 && HostImportSerial(context, address, bytes, true) == first, "an unchanged range lost its import");
    {
        GuestAllocations::Mutation mutation;
        mutation.Remove(block);
        mutation.Add(block, bytes, true, true);
    }
    Require(HostImportFor(context, address, bytes) != nullptr, "the remapped range was not imported again");
    const auto second = HostImportSerial(context, address, bytes, true);
    Require(second != 0 && second != first, "the import of a range mapped again was kept");
    {
        GuestAllocations::Mutation mutation;
        mutation.Remove(block);
    }
    HostImportFor(context, address, bytes);
}

// HostImportsFor (the fast dispatch's imports under one hold of the registry's lock): each range
// receives what HostImportFor returns for it, in order (null outside every registered allocation),
// and a visitor that stops leaves the ranges after it unlooked-up, so not imported.
void batchedImportTests(const Device& device) {
    const auto& context = device.GetContext();
    if (context.hostImportAlignment == 0) {
        std::cout << "host imports unavailable: batched imports not tested\n";
        return;
    }
    constexpr std::size_t bytes = 65536;
    // The third block is registered later (a registry change).
    std::array<void*, 3> blocks{};
    for (std::size_t i = 0; i < blocks.size(); ++i) {
        auto& block = blocks[i];
#ifdef _WIN32
        block = VirtualAlloc(nullptr, bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
#else
        block = std::aligned_alloc(65536, bytes);
#endif
        Require(block != nullptr, "cannot allocate a batched import test block");
        if (i == 2) continue;
        GuestAllocations::Mutation mutation;
        mutation.Add(block, bytes, true, true);
    }
    const auto first = reinterpret_cast<std::uint64_t>(blocks[0]);
    const auto second = reinterpret_cast<std::uint64_t>(blocks[1]);
    static std::array<std::uint64_t, 8> outside{};
    const auto unregistered = reinterpret_cast<std::uint64_t>(outside.data());
    const std::array<std::pair<std::uint64_t, std::uint64_t>, 3> ranges{{{first + 0x100, first + 0x200}, {unregistered, unregistered + 16}, {second + 0x40, second + 0x1000}}};
    struct Seen {
        std::size_t stopAt;
        std::vector<const HostImport*> imports;
    };
    const HostImportVisitor visit = [](void* user, std::size_t index, const HostImport* import) {
        auto& seen = *static_cast<Seen*>(user);
        seen.imports.push_back(import);
        return index != seen.stopAt;
    };
    Seen stopped{0, {}};
    HostImportsFor(context, ranges, visit, &stopped);
    if (stopped.imports.size() == 1 && stopped.imports[0] == nullptr) {
        std::cout << "host import of the batched import test block refused: batched imports not tested\n";
        return;
    }
    Require(stopped.imports.size() == 1 && stopped.imports[0] != nullptr && stopped.imports[0] == HostImportFor(context, first + 0x100, 0x100), "the first range did not receive HostImportFor's import");
    Require(!HostImportCovers(context, second + 0x40, 0xfc0), "a range after the visitor stopped was imported");
    Seen all{ranges.size(), {}};
    HostImportsFor(context, ranges, visit, &all);
    Require(all.imports.size() == 3 && all.imports[0] == stopped.imports[0] && all.imports[1] == nullptr && all.imports[2] != nullptr, "the ranges did not receive their imports in order");
    Require(all.imports[2] == HostImportFor(context, second + 0x40, 0xfc0) && all.imports[2]->base == second, "the last range's import is not HostImportFor's");
    // Hits only: answered under the one hold, the same imports.
    const std::array<std::pair<std::uint64_t, std::uint64_t>, 2> known{{ranges[0], ranges[2]}};
    Seen hits{known.size(), {}};
    HostImportsFor(context, known, visit, &hits);
    Require(hits.imports.size() == 2 && hits.imports[0] == all.imports[0] && hits.imports[1] == all.imports[2], "ranges an import covers did not receive it");
    // A registry change since the last reconcile: the first range is no hit under the hold, so
    // every range takes HostImportFor, which reconciles; the imports of unchanged registrations stay.
    {
        GuestAllocations::Mutation mutation;
        mutation.Add(blocks[2], bytes, true, true);
    }
    Seen stale{known.size(), {}};
    HostImportsFor(context, known, visit, &stale);
    Require(stale.imports.size() == 2 && stale.imports[0] == all.imports[0] && stale.imports[1] == all.imports[2], "a stale registry changed the ranges' imports");
    // A miss after a hit: the new registration is imported in order, between the two hits.
    const auto third = reinterpret_cast<std::uint64_t>(blocks[2]);
    const std::array<std::pair<std::uint64_t, std::uint64_t>, 3> mixed{{ranges[0], {third + 0x10, third + 0x20}, ranges[2]}};
    Seen created{mixed.size(), {}};
    HostImportsFor(context, mixed, visit, &created);
    Require(created.imports.size() == 3 && created.imports[0] == all.imports[0] && created.imports[1] != nullptr && created.imports[1] == HostImportFor(context, third + 0x10, 0x10) && created.imports[2] == all.imports[2], "a miss after a hit did not import the new registration in order");
    Seen none{0, {}};
    HostImportsFor(context, std::span<const std::pair<std::uint64_t, std::uint64_t>>{}, visit, &none);
    Require(none.imports.empty(), "an empty range list was visited");
    for (auto* block : blocks) {
        GuestAllocations::Mutation mutation;
        mutation.Remove(block);
    }
    HostImportFor(context, first, bytes);
}

// The fast paths' import memo against HostImportFor on a real registry (s53-fast-cost-b-fix, review
// F5a): at every step HostImportMemoized answers what HostImportFor answers; a lookup in an
// unchanged registry is a memo hit, and a range mapped again at another size or removed is never
// answered from an entry noted before.
void importMemoTests(const Device& device) {
    const auto& context = device.GetContext();
    if (context.hostImportAlignment == 0) {
        std::cout << "host imports unavailable: the import memo not tested\n";
        return;
    }
    constexpr std::size_t bytes = 131072;
#ifdef _WIN32
    void* block = VirtualAlloc(nullptr, bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
#else
    void* block = std::aligned_alloc(65536, bytes);
#endif
    Require(block != nullptr, "cannot allocate the import memo test block");
    const auto address = reinterpret_cast<std::uint64_t>(block);
    const bool memoOn = std::getenv("APS5_NO_IMPORT_MEMO") == nullptr;
    auto& memo = ThreadHostImportMemo();
    // The memo's answer first (a hit must not depend on HostImportFor's reconcile), then HostImportFor's.
    const auto same = [&](std::uint64_t at, std::size_t size, const char* what) {
        const auto* memoized = HostImportMemoized(context, at, size);
        const auto* direct = HostImportFor(context, at, size);
        Require(memoized == direct, what);
        return direct;
    };
    {
        GuestAllocations::Mutation mutation;
        mutation.Add(block, bytes, true, true);
    }
    struct Unregister {
        const Context& context;
        void* block;
        std::uint64_t address;
        bool registered = true;
        ~Unregister() {
            if (registered) {
                GuestAllocations::Mutation mutation;
                mutation.Remove(block);
            }
            HostImportFor(context, address, bytes);
        }
    } unregister{context, block, address};
    const auto* first = same(address, bytes, "import memo: the first lookup differs from HostImportFor");
    if (first == nullptr) {
        std::cout << "host import of the import memo test block refused: the import memo not tested\n";
        return;
    }
    auto hits = memo.hits;
    Require(HostImportMemoized(context, address + 4096, 256) == first && HostImportMemoized(context, address, bytes) == first, "import memo: a range of the import in an unchanged registry was not answered");
    Require(!memoOn || memo.hits == hits + 2, "import memo: lookups in an unchanged registry were not memo hits");
    Require(HostImportMemoized(context, address + bytes - 4, 8) == HostImportFor(context, address + bytes - 4, 8), "import memo: a range past the import's end differs from HostImportFor");
    // Mapped again at half the size: the generation moves and the reconcile retires the import.
    {
        GuestAllocations::Mutation mutation;
        mutation.Remove(block);
        mutation.Add(block, bytes / 2, true, true);
    }
    const auto misses = memo.misses;
    const auto* shrunk = same(address + 4096, 256, "import memo: a range of a range mapped again differs from HostImportFor");
    Require(!memoOn || memo.misses == misses + 1, "import memo: an entry noted before the registry changed was not a miss");
    Require(shrunk != nullptr && shrunk->bytes == bytes / 2, "import memo: a range mapped again was not imported again at its new size");
    Require(same(address, bytes, "import memo: the old whole range differs from HostImportFor after the range shrank") == nullptr, "import memo: the old whole range was answered after the range shrank");
    hits = memo.hits;
    Require(HostImportMemoized(context, address, bytes / 2) == shrunk, "import memo: the new import was not answered");
    Require(!memoOn || memo.hits == hits + 1, "import memo: the new import was not noted");
    // Removed: nothing answers any more, the memo's entry for the new import included.
    {
        GuestAllocations::Mutation mutation;
        mutation.Remove(block);
    }
    unregister.registered = false;
    Require(same(address + 4096, 256, "import memo: a removed range differs from HostImportFor") == nullptr, "import memo: a removed range was answered");
}

// The S# memo against the sampler cache (s53-fast-cost-b-fix, reviews F2 and F5b): GetMemoized
// answers what Get answers through evictions; a memo hit keeps its sampler as recent as a Get hit
// would (the eviction takes the least recently used sampler); an eviction makes the memo miss, and
// a cache made at a destroyed one's address is not answered from the old cache's entries.
void samplerMemoTests(const Device& device) {
    const auto& context = device.GetContext();
    // GuestSamplerResource's captured 2D sampler, and the same with another maximum LOD.
    const std::array<std::uint32_t, 4> wordsA{0u, 0x00fff000u, 0x09000000u, 0u};
    const std::array<std::uint32_t, 4> wordsB{0u, 0x00800000u, 0x09000000u, 0u};
    std::optional<SamplerCache> cache;
    cache.emplace(2);
    const auto a = cache->GetMemoized(context, wordsA, false);
    Require(a != nullptr && cache->Get(context, wordsA, false) == a, "sampler memo: the first lookup differs from Get");
    const auto b = cache->GetMemoized(context, wordsA, true);
    Require(b != nullptr && b != a, "sampler memo: the compare sampler is the plain one");
    // A memo hit on `a` (nothing evicted since it was noted): `a` is now more recent than `b`.
    Require(cache->GetMemoized(context, wordsA, false) == a, "sampler memo: an unchanged cache's sampler was not answered");
    const auto removals = cache->Removals();
    const auto c = cache->Get(context, wordsB, false);
    Require(c != nullptr && cache->Removals() == removals + 1, "sampler memo: a third sampler in a cache of two evicted nothing");
    Require(cache->Get(context, wordsA, false) == a, "sampler memo: a memo hit left its sampler the least recently used one (evicted before an older one)");
    // `b` was evicted (the test still holds it): its memo entry, noted before, must not answer.
    const auto again = cache->GetMemoized(context, wordsA, true);
    Require(again != b && again == cache->Get(context, wordsA, true), "sampler memo: an evicted sampler was answered from the memo");
    Require(cache->GetMemoized(context, wordsB, false) == cache->Get(context, wordsB, false), "sampler memo: a lookup after evictions differs from Get");
    // Another cache, possibly at the same address, with no removal yet (as the old one had when `a`
    // was noted): the instance tells them apart.
    const auto instance = cache->Instance();
    cache.reset();
    cache.emplace(2);
    Require(cache->Instance() != instance, "sampler memo: a new cache has the old one's instance");
    const auto fresh = cache->GetMemoized(context, wordsA, false);
    Require(fresh != a && fresh == cache->Get(context, wordsA, false), "sampler memo: a new cache was answered from the old cache's memo");
}

// Out of video memory (AllocateDeviceMemory): a device whose allocations past a budget fail with
// VK_ERROR_OUT_OF_DEVICE_MEMORY, as the GPU's did at the Boletaria load (t421-t424: textures
// refused for minutes while the buffer pool retained ~2 GiB of released device buffers). A new
// size must not fail while released buffers hold the memory (the pool releases them and the
// allocation is made on one more try), and with nothing to release the refusal comes after one
// try. APS5_NO_OOM_RECLAIM=1: refused at once, the pool kept, as before.
struct FakeVideoMemory {
    PFN_vkGetDeviceProcAddr real = nullptr;
    PFN_vkAllocateMemory allocateMemory = nullptr;
    PFN_vkFreeMemory freeMemory = nullptr;
    VkDeviceSize budget = ~VkDeviceSize{0};
    VkDeviceSize live = 0;
    std::uint64_t tries = 0;
    std::map<VkDeviceMemory, VkDeviceSize> sizes;
};

FakeVideoMemory& fakeVideoMemory() {
    static FakeVideoMemory memory;
    return memory;
}

VKAPI_ATTR VkResult VKAPI_CALL fakeAllocateMemory(VkDevice device, const VkMemoryAllocateInfo* info, const VkAllocationCallbacks* callbacks, VkDeviceMemory* memory) {
    auto& fake = fakeVideoMemory();
    ++fake.tries;
    if (info->allocationSize > fake.budget - std::min(fake.budget, fake.live)) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    const auto result = fake.allocateMemory(device, info, callbacks, memory);
    if (result == VK_SUCCESS) {
        fake.live += info->allocationSize;
        fake.sizes[*memory] = info->allocationSize;
    }
    return result;
}

VKAPI_ATTR void VKAPI_CALL fakeFreeMemory(VkDevice device, VkDeviceMemory memory, const VkAllocationCallbacks* callbacks) {
    auto& fake = fakeVideoMemory();
    if (const auto found = fake.sizes.find(memory); found != fake.sizes.end()) {
        fake.live -= found->second;
        fake.sizes.erase(found);
    }
    fake.freeMemory(device, memory, callbacks);
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL fakeVideoDeviceProc(VkDevice device, const char* name) {
    if (std::strcmp(name, "vkAllocateMemory") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&fakeAllocateMemory);
    if (std::strcmp(name, "vkFreeMemory") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&fakeFreeMemory);
    return fakeVideoMemory().real(device, name);
}

void outOfVideoMemoryTests(const Device& device) {
    auto& fake = fakeVideoMemory();
    // A context of its own whose buffer pool frees through the fake too.
    Context context = device.GetContext();
    fake.real = context.deviceProc;
    fake.allocateMemory = context.Function<PFN_vkAllocateMemory>("vkAllocateMemory");
    fake.freeMemory = context.Function<PFN_vkFreeMemory>("vkFreeMemory");
    context.deviceProc = &fakeVideoDeviceProc;
    context.functions = nullptr;
    context.bufferPool.reset();
    const bool reclaim = std::getenv("APS5_NO_OOM_RECLAIM") == nullptr;
    constexpr VkBufferUsageFlags usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    constexpr std::size_t mib = std::size_t{1} << 20u;
    {
        // Four buffers of 2 MiB, then released: the pool's device tier retains their memory.
        std::vector<std::unique_ptr<DeviceBuffer>> released;
        for (int i = 0; i < 4; ++i) released.push_back(std::make_unique<DeviceBuffer>(context, 2 * mib, usage));
    }
    Require(fake.live >= 8 * mib, "out of video memory: the released device buffers were not retained");
    // 1 MiB left: a 3 MiB buffer (no retained one has its size) needs the retained memory.
    fake.budget = fake.live + mib;
    const auto before = BufferPool::OutOfMemory();
    const auto triesBefore = fake.tries;
    std::unique_ptr<DeviceBuffer> made;
    std::string error;
    try {
        made = std::make_unique<DeviceBuffer>(context, 3 * mib, usage);
    } catch (const std::exception& thrown) {
        error = thrown.what();
    }
    const auto after = BufferPool::OutOfMemory();
    Require(after.refused == before.refused + 1, "out of video memory: the refusal was not counted");
    if (reclaim) {
        Require(made != nullptr, "out of video memory: an allocation failed while the pool retained released device memory (" + error + ")");
        Require(after.reclaims == before.reclaims + 1 && after.reclaimedBytes >= before.reclaimedBytes + 8 * mib && after.madeAfter == before.madeAfter + 1, "out of video memory: the release or the allocation after it was not counted");
        Require(fake.tries == triesBefore + 2, "out of video memory: not exactly one more try after the release");
    } else {
        Require(made == nullptr && error.find("vkAllocateMemory device buffer") != std::string::npos, "out of video memory: APS5_NO_OOM_RECLAIM=1 did not fail as before");
        Require(after.reclaims == before.reclaims && fake.tries == triesBefore + 1, "out of video memory: APS5_NO_OOM_RECLAIM=1 released the pool");
    }
    // No room and nothing the pool could release (with the reclaim it was emptied above): refused
    // after one try.
    fake.budget = fake.live;
    const auto again = BufferPool::OutOfMemory();
    const auto triesAgain = fake.tries;
    bool refused = false;
    try {
        DeviceBuffer extra(context, 5 * mib, usage);
    } catch (const std::exception&) {
        refused = true;
    }
    Require(refused && fake.tries == triesAgain + 1 && BufferPool::OutOfMemory().reclaims == again.reclaims, "out of video memory: a refusal with nothing retained was tried again");
    made.reset();
    fake.budget = ~VkDeviceSize{0};
}

void movedMetadataTests(const Device& device, Recorder& recorder) {
    const auto& base = device.GetContext();
    if (base.hostImportAlignment == 0) {
        std::cout << "host imports unavailable: moved DCC metadata not tested\n";
        return;
    }
    constexpr std::uint32_t side = 256;
    constexpr std::size_t surfaceBytes = side * side * 4;
    constexpr std::size_t keyCount = surfaceBytes / 256;
    constexpr std::size_t bytes = surfaceBytes + 65536;
#ifdef _WIN32
    void* block = VirtualAlloc(nullptr, bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
#else
    void* block = std::aligned_alloc(65536, bytes);
#endif
    Require(block != nullptr, "cannot allocate the moved metadata block");
    auto* texels = static_cast<std::uint8_t*>(block);
    auto* firstKeys = texels + surfaceBytes;
    auto* secondKeys = firstKeys + 4096;
    const auto address = reinterpret_cast<std::uint64_t>(block);
    {
        GuestAllocations::Mutation mutation;
        mutation.Add(block, bytes, true, true);
    }
    struct Unregister {
        const Context& context;
        void* block;
        std::uint64_t address;
        ~Unregister() {
            ClearCachedTextures(context.device);
            {
                GuestAllocations::Mutation mutation;
                mutation.Remove(block);
            }
            HostImportFor(context, address, bytes);
        }
    } unregister{base, block, address};
    if (HostImportFor(base, address, bytes) == nullptr) {
        std::cout << "host import of the moved metadata block refused: moved DCC metadata not tested\n";
        return;
    }
    TextureDetiler detiler(base);
    auto context = base;
    context.detiler = &detiler;
    GuestTextureResource resource{};
    resource.baseAddress = address;
    resource.width = side;
    resource.height = side;
    resource.mipCount = 1;
    resource.tileMode = TextureTileMode::kR64KBX;
    resource.dimension = TextureDimension::k2D;
    resource.format = 56;
    resource.dstSelX = 4;
    resource.dstSelY = 5;
    resource.dstSelZ = 6;
    resource.dstSelW = 7;
    Require(DescribeSurface(resource).guestBytes == surfaceBytes, "the moved metadata surface has an unexpected size");
    const auto first = address + surfaceBytes;
    const auto second = first + 4096;
    const auto withKeys = [&](std::uint64_t keys) {
        auto described = resource;
        described.dccAddress = keys;
        return described;
    };
    const auto holds = [&](const StorageTexture& image, std::array<std::uint8_t, 4> texel) {
        Buffer readback(context, surfaceBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
        const auto commands = recorder.Commands();
        RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
        VkBufferImageCopy copy{};
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.imageExtent = {side, side, 1};
        context.Function<PFN_vkCmdCopyImageToBuffer>("vkCmdCopyImageToBuffer")(commands, image.Image(), VK_IMAGE_LAYOUT_GENERAL, readback.Handle(), 1, &copy);
        RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT);
        recorder.Submit();
        device.WaitQueue();
        recorder.Sync();
        const auto pixels = readback.Bytes();
        for (std::size_t i = 0; i < pixels.size(); ++i) {
            if (std::to_integer<std::uint8_t>(pixels[i]) != texel[i % 4]) return false;
        }
        return true;
    };
    std::memset(texels, 0x55, surfaceBytes);
    std::memset(firstKeys, 0xff, keyCount);
    std::memset(secondKeys, 0x00, keyCount);
    const auto original = CachedStorageSurface(context, withKeys(first));
    Require(original->Descriptor().dccAddress == first && holds(*original, {0x55, 0x55, 0x55, 0x55}), "the first image does not hold the stored texels");
    const auto moved = CachedStorageSurface(context, withKeys(second));
    Require(moved->Descriptor().dccAddress == second, "the storage image kept the keys the surface no longer names");
    Require(!StorageImageCached(context, original.get()), "the image of the old keys is still the surface's");
    Require(holds(*moved, {0, 0, 0, 0}), "a fast clear of the moved keys did not reach the image");
    Require(CachedStorageSurface(context, resource) == moved && moved->Descriptor().dccAddress == second, "a descriptor without metadata replaced the image");
    const auto back = CachedStorageSurface(context, withKeys(first));
    Require(back != moved && back->Descriptor().dccAddress == first, "the keys moving back did not remake the image");
    Require(holds(*back, {0x55, 0x55, 0x55, 0x55}), "the image remade under the first keys does not hold the stored texels");
}

void resourceReadTests(const Device& device, Recorder& recorder) {
    // The draw-snapshot tests below exercise the snapshot path (read once by
    // ShaderResources::InPlaceBindings, before its first PrepareDrawBindings call).
    _putenv_s("APS5_NO_INPLACE_BINDINGS", "1");
    const auto& context = device.GetContext();
    if (context.hostImportAlignment == 0) {
        std::cout << "host imports unavailable: resource read notes not tested\n";
        return;
    }
    constexpr std::size_t bytes = 65536;
#ifdef _WIN32
    void* block = VirtualAlloc(nullptr, bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
#else
    void* block = std::aligned_alloc(65536, bytes);
#endif
    Require(block != nullptr, "cannot allocate the test block");
    std::memset(block, 0x11, bytes);
    const auto address = reinterpret_cast<std::uint64_t>(block);
    {
        GuestAllocations::Mutation mutation;
        mutation.Add(block, bytes, true, true);
    }
    if (HostImportFor(context, address, bytes) == nullptr) {
        std::cout << "host import of the test block refused: resource read notes not tested\n";
        return;
    }
    const auto element = address + 4096;
    constexpr std::size_t elementBytes = 1024;
    {
        GuestBufferMemory memory(context);
        memory.AddReadable(element, elementBytes);
        memory.Upload(false);
        const auto reads = memory.InPlaceReads();
        Require(reads.size() == 1 && reads[0].first == element && reads[0].second == element + elementBytes, "an imported region is not an in-place read");
        Require(memory.Writes().empty(), "a readable element counts as written");
    }
    ShaderRecompiler::RecompileResult program;
    ShaderRecompiler::DescriptorBinding binding;
    binding.kind = ShaderRecompiler::DescriptorKind::StorageBuffer;
    binding.role = ShaderRecompiler::DescriptorRole::GuestBuffers;
    binding.descriptorSet = 0;
    binding.binding = 0;
    binding.count = 1;
    binding.guestDescriptor = {static_cast<std::uint32_t>(element), static_cast<std::uint32_t>(element >> 32u) & 0xffffu, elementBytes, 0x31000000u};
    binding.bufferWritten = {false};
    program.bindings.push_back(binding);
    const CompiledShader compute{ShaderRecompiler::ShaderStage::Compute, &program, 0};
    {
        ShaderResources resources(context, compute);
        Require(recorder.Idle() && !recorder.PendingReadOverlaps(element, 16), "the build itself noted a read");
        resources.MarkGpuWrites(recorder);
        Require(recorder.PendingReadOverlaps(element, 16) && recorder.PendingReadOverlaps(element + elementBytes - 4, 4) && !recorder.PendingReadOverlaps(element + elementBytes, 16), "the build's in-place read was not noted");
        Require(!recorder.PendingWriteOverlaps(element, elementBytes), "a read-only element was noted as written");
        const auto info = recorder.DescribePendingRead(element, 16);
        Require(info.has_value() && info->kind == Kind::DispatchElement && info->open, "the build's read has the wrong kind");
        recorder.Submit();
        device.WaitQueue();
        Require(!recorder.PendingReadOverlaps(element, 16), "the build's read refuses after its batch ran");
        recorder.Sync();
    }
    std::weak_ptr<ShaderResources::DrawBindings> snapshotLifetime;
    {
        auto snapshotContext = context;
        DescriptorCache cache(snapshotContext);
        snapshotContext.descriptorCache = &cache;
        Recorder snapshotRecorder(snapshotContext);
        snapshotRecorder.Activate();
        ShaderResources resources(snapshotContext, compute);
        auto first = resources.PrepareDrawBindings(snapshotRecorder);
        Require(first != nullptr && first->snapshots.size() == 1, "read-only draw input was not snapshotted");
        std::memset(reinterpret_cast<void*>(element), 0x22, elementBytes);
        auto second = resources.PrepareDrawBindings(snapshotRecorder);
        Require(second != nullptr && second->snapshots.size() == 1, "cached draw input was not snapshotted again");
        Require(first->allocation.set != second->allocation.set, "in-flight draws share mutable descriptor bindings");
        std::memset(reinterpret_cast<void*>(element), 0x33, elementBytes);
        auto downloaded = std::make_shared<Buffer>(snapshotContext, elementBytes * 2, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
        const auto commands = snapshotRecorder.Commands();
        RecordMemoryBarrier(snapshotContext, commands, VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_HOST_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
        const auto copy = snapshotContext.Function<PFN_vkCmdCopyBuffer>("vkCmdCopyBuffer");
        VkBufferCopy region{0, 0, elementBytes};
        copy(commands, first->snapshots[0].buffer->Handle(), downloaded->Handle(), 1, &region);
        region.dstOffset = elementBytes;
        copy(commands, second->snapshots[0].buffer->Handle(), downloaded->Handle(), 1, &region);
        RecordMemoryBarrier(snapshotContext, commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT);
        snapshotLifetime = first;
        first.reset();
        second.reset();
        Require(!snapshotLifetime.expired(), "draw snapshot was released before its GPU batch");
        snapshotRecorder.Sync();
        const auto contents = downloaded->Bytes();
        Require(std::all_of(contents.begin(), contents.begin() + elementBytes, [](std::byte value) { return value == std::byte{0x11}; }), "first draw observed overwritten input");
        Require(std::all_of(contents.begin() + elementBytes, contents.end(), [](std::byte value) { return value == std::byte{0x22}; }), "second draw observed overwritten input");
        snapshotRecorder.NotePendingWrite(element, elementBytes);
        Require(resources.PrepareDrawBindings(snapshotRecorder) == nullptr, "GPU-produced input was replaced with stale CPU memory");
        snapshotRecorder.Sync();
    }
    Require(snapshotLifetime.expired(), "draw snapshots outlived their recorder");
    recorder.Activate();
    {
        GuestAllocations::Mutation mutation;
        mutation.Remove(block);
    }
    // The import is retired by the next reconcile; the block itself is left to the process.
    HostImportFor(context, address, bytes);
}

void misalignedSnapshotTests(const Device& device, Recorder& recorder) {
    _putenv_s("APS5_NO_INPLACE_BINDINGS", "1");
    const auto& context = device.GetContext();
    const auto alignment = context.limits.minStorageBufferOffsetAlignment;
    if (context.hostImportAlignment == 0) {
        std::cout << "host imports unavailable: misaligned draw snapshots not tested\n";
        return;
    }
    if (alignment < 8 || alignment > 256) {
        std::cout << "storage buffer offset alignment " << alignment << ": misaligned draw snapshots not tested\n";
        return;
    }
    constexpr std::size_t bytes = 65536;
#ifdef _WIN32
    void* block = VirtualAlloc(nullptr, bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
#else
    void* block = std::aligned_alloc(65536, bytes);
#endif
    Require(block != nullptr, "cannot allocate the misaligned snapshot block");
    auto* guest = static_cast<std::uint8_t*>(block);
    for (std::size_t at = 0; at < bytes; ++at) guest[at] = static_cast<std::uint8_t>(at * 7u + 3u);
    const auto address = reinterpret_cast<std::uint64_t>(block);
    {
        GuestAllocations::Mutation mutation;
        mutation.Add(block, bytes, true, true);
    }
    struct Unregister {
        const Context& context;
        void* block;
        ~Unregister() {
            {
                GuestAllocations::Mutation mutation;
                mutation.Remove(block);
            }
            HostImportFor(context, reinterpret_cast<std::uint64_t>(block), bytes);
        }
    } unregister{context, block};
    if (HostImportFor(context, address, bytes) == nullptr) {
        std::cout << "host import of the test block refused: misaligned draw snapshots not tested\n";
        return;
    }
    constexpr std::uint32_t outer = 4096;
    constexpr std::uint32_t offset = outer + 4;
    constexpr std::size_t outerBytes = 128;
    constexpr std::size_t elementBytes = 64;
    const auto words = [](std::uint64_t at, std::size_t size) { return std::array<std::uint32_t, 4>{static_cast<std::uint32_t>(at), static_cast<std::uint32_t>(at >> 32u) & 0xffffu, static_cast<std::uint32_t>(size), 0x31000000u}; };
    ShaderRecompiler::RecompileResult program;
    ShaderRecompiler::DescriptorBinding binding;
    binding.kind = ShaderRecompiler::DescriptorKind::StorageBuffer;
    binding.role = ShaderRecompiler::DescriptorRole::GuestBuffers;
    binding.descriptorSet = 0;
    binding.binding = 0;
    binding.count = 2;
    for (const auto word : words(address + outer, outerBytes)) binding.guestDescriptor.push_back(word);
    for (const auto word : words(address + offset, elementBytes)) binding.guestDescriptor.push_back(word);
    binding.bufferWritten = {false, false};
    program.bindings.push_back(binding);
    program.pushConstants.resize(16);
    program.memoryOffsetDword = 0;
    const CompiledShader compute{ShaderRecompiler::ShaderStage::Compute, &program, 0};
    {
        auto snapshotContext = context;
        DescriptorCache cache(snapshotContext);
        snapshotContext.descriptorCache = &cache;
        Recorder snapshotRecorder(snapshotContext);
        snapshotRecorder.Activate();
        ShaderResources resources(snapshotContext, compute);
        std::array<std::byte, PipelinePushConstantBytes> push{};
        resources.PatchPushConstants(push);
        const auto adjustment = std::to_integer<std::uint32_t>(push[1]);
        Require(push[0] == std::byte{0} && adjustment == offset % alignment, "the inner view's shader offset is not its distance from the binding");
        const auto bindings = resources.PrepareDrawBindings(snapshotRecorder);
        Require(bindings != nullptr && bindings->snapshots.size() == 2, "read-only draw inputs were not snapshotted");
        const auto outerContents = bindings->snapshots[0].buffer->Bytes();
        Require(outerContents.size() >= outerBytes && std::memcmp(outerContents.data(), guest + outer, outerBytes) == 0, "an aligned draw snapshot misses its view's bytes");
        const auto contents = bindings->snapshots[1].buffer->Bytes();
        Require(contents.size() >= adjustment + elementBytes, "a draw snapshot ends before the bytes the shader reads");
        Require(std::memcmp(contents.data() + adjustment, guest + offset, elementBytes) == 0, "the shader's offset into a draw snapshot misses the view's bytes");
        snapshotRecorder.Sync();
    }
    recorder.Activate();
}

void drawSnapshotReuseTests(const Device& device, Recorder& recorder) {
    _putenv_s("APS5_NO_INPLACE_BINDINGS", "1");
    const auto& context = device.GetContext();
    constexpr std::size_t bytes = 65536;
    void* block = context.hostImportAlignment != 0 ? AllocateWatched(bytes, 65536) : nullptr;
    if (block == nullptr) {
        std::cout << "host imports or write watching unavailable: draw snapshot reuse not tested\n";
        return;
    }
    struct Release {
        void* block;
        ~Release() { ReleaseWatched(block, bytes); }
    } release{block};
    std::memset(block, 0x11, bytes);
    const auto address = reinterpret_cast<std::uint64_t>(block);
    {
        GuestAllocations::Mutation mutation;
        mutation.Add(block, bytes, true, true);
    }
    struct Unregister {
        const Context& context;
        void* block;
        ~Unregister() {
            {
                GuestAllocations::Mutation mutation;
                mutation.Remove(block);
            }
            HostImportFor(context, reinterpret_cast<std::uint64_t>(block), bytes);
        }
    } unregister{context, block};
    if (HostImportFor(context, address, bytes) == nullptr) {
        std::cout << "host import of the watched block refused: draw snapshot reuse not tested\n";
        return;
    }
    const auto element = address + 4096;
    constexpr std::size_t elementBytes = 1024;
    ShaderRecompiler::RecompileResult program;
    ShaderRecompiler::DescriptorBinding binding;
    binding.kind = ShaderRecompiler::DescriptorKind::StorageBuffer;
    binding.role = ShaderRecompiler::DescriptorRole::GuestBuffers;
    binding.descriptorSet = 0;
    binding.binding = 0;
    binding.count = 1;
    binding.guestDescriptor = {static_cast<std::uint32_t>(element), static_cast<std::uint32_t>(element >> 32u) & 0xffffu, elementBytes, 0x31000000u};
    binding.bufferWritten = {false};
    program.bindings.push_back(binding);
    const CompiledShader compute{ShaderRecompiler::ShaderStage::Compute, &program, 0};
    {
        auto snapshotContext = context;
        DescriptorCache cache(snapshotContext);
        snapshotContext.descriptorCache = &cache;
        Recorder snapshotRecorder(snapshotContext);
        snapshotRecorder.Activate();
        ShaderResources resources(snapshotContext, compute);
        const auto snapshot = [&](std::byte expected) {
            const auto bindings = resources.PrepareDrawBindings(snapshotRecorder);
            Require(bindings != nullptr && bindings->snapshots.size() == 1, "read-only draw input was not snapshotted");
            const auto buffer = bindings->snapshots[0].buffer;
            const auto contents = buffer->Bytes();
            Require(contents.size() == elementBytes && std::all_of(contents.begin(), contents.end(), [&](std::byte value) { return value == expected; }), "a draw snapshot does not hold the guest bytes of its draw");
            return buffer;
        };
        const auto first = snapshot(std::byte{0x11});
        Require(snapshot(std::byte{0x11}) == first, "an unchanged draw input was copied again");
        std::memset(reinterpret_cast<void*>(element), 0x22, elementBytes);
        const auto afterCpu = snapshot(std::byte{0x22});
        Require(afterCpu != first, "a draw snapshot outlived a CPU store to its range");
        Require(snapshot(std::byte{0x22}) == afterCpu, "the recopied draw input was not kept");
        std::memset(reinterpret_cast<void*>(element), 0x33, elementBytes);
        AgcDriver::GuestMemory::MarkWritten(element, 4);
        const auto afterStore = snapshot(std::byte{0x33});
        Require(afterStore != afterCpu, "a draw snapshot outlived a driver store to its range");
        {
            GuestAllocations::Mutation mutation;
        }
        Require(snapshot(std::byte{0x33}) != afterStore, "a draw snapshot outlived a registry mutation");
        snapshotRecorder.Sync();
    }
    recorder.Activate();
}

void drawSnapshotEvictionTests(const Device& device) {
    using namespace AgcDriver::GuestMemory;
    constexpr std::size_t bytes = 65536;
    void* block = WriteWatched() ? AllocateWatched(bytes, 65536) : nullptr;
    if (block == nullptr) {
        std::cout << "write watching unavailable: draw snapshot eviction not tested\n";
        return;
    }
    struct Release {
        void* block;
        ~Release() { ReleaseWatched(block, bytes); }
    } release{block};
    const auto address = reinterpret_cast<std::uint64_t>(block);
    Recorder cache(device.GetContext());
    const auto buffer = std::make_shared<Buffer>(device.GetContext(), 16, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    const auto registry = GuestAllocations::GuestAllocationsGeneration_nid_postfix();
    const auto generation = CollectWrites(address, 4096);
    Require(generation != 0, "the watched block has no generation");
    constexpr auto cap = Recorder::DrawSnapshotEntries;
    cache.KeepDrawSnapshot(address, 16, generation, registry, buffer, Recorder::SnapshotUse::Vertex);
    for (std::size_t size = 1; size <= cap; ++size) cache.KeepDrawSnapshot(address, size, generation, registry, buffer);
    Require(cache.ReusableDrawSnapshot(address, 1) == buffer, "a kept snapshot is not reusable");
    cache.KeepDrawSnapshot(address, cap + 1, generation, registry, buffer);
    Require(cache.ReusableDrawSnapshot(address, 2) == nullptr, "the least recently used snapshot survived the cap");
    Require(cache.ReusableDrawSnapshot(address, 1) == buffer && cache.ReusableDrawSnapshot(address, 3) == buffer && cache.ReusableDrawSnapshot(address, cap + 1) == buffer, "eviction dropped a more recently used snapshot");
    cache.KeepDrawSnapshot(address, cap + 2, generation, registry, buffer);
    Require(cache.ReusableDrawSnapshot(address, 4) == nullptr && cache.ReusableDrawSnapshot(address, 1) == buffer, "the second eviction did not take the next oldest");
    Require(cache.ReusableDrawSnapshot(address, 16, Recorder::SnapshotUse::Vertex) == buffer, "storage snapshots evicted a vertex snapshot");
    cache.KeepDrawSnapshot(address, 1, generation, registry, buffer);
    Require(cache.ReusableDrawSnapshot(address, 1) == buffer && cache.ReusableDrawSnapshot(address, 5) == buffer, "replacing a kept snapshot evicted another");
    cache.KeepDrawSnapshot(address + 8192, Recorder::DrawSnapshotBudget, generation, registry, buffer);
    Require(cache.ReusableDrawSnapshot(address, 1) == nullptr && cache.ReusableDrawSnapshot(address, 5) == nullptr, "the byte budget did not evict the older snapshots");
    cache.KeepDrawSnapshot(address, 16, generation, registry, buffer);
    std::memset(block, 0x5a, 16);
    CollectWrites(address, 16);
    Require(cache.ReusableDrawSnapshot(address, 16) == nullptr && cache.ReusableDrawSnapshot(address, 16) == nullptr, "a snapshot outlived a CPU store");
}

void drawInputReuseTests(const Device& device, Recorder& recorder) {
    using namespace AgcDriver::GuestMemory;
    using Use = Recorder::SnapshotUse;
    constexpr std::size_t bytes = 65536;
    void* block = WriteWatched() ? AllocateWatched(bytes, 65536) : nullptr;
    if (block == nullptr) {
        std::cout << "write watching unavailable: draw input reuse not tested\n";
        return;
    }
    struct Release {
        void* block;
        ~Release() { ReleaseWatched(block, bytes); }
    } release{block};
    const auto& context = device.GetContext();
    auto* words = static_cast<std::uint32_t*>(block);
    for (std::uint32_t i = 0; i < bytes / sizeof(std::uint32_t); ++i) words[i] = i * 3;
    const auto address = reinterpret_cast<std::uint64_t>(block);
    constexpr std::size_t size = 256;
    const auto equalsGuest = [&](const DrawInputCopy& copy) {
        const auto contents = copy.buffer->Bytes();
        return contents.size() == size && std::memcmp(contents.data(), block, size) == 0;
    };
    const auto first = CopyDrawInput(context, &recorder, address, size, 4, Use::Index32);
    Require(!first.reused && first.generation != 0 && equalsGuest(first), "the first draw input copy is wrong");
    KeepDrawInput(&recorder, address, first, Use::Index32, 189);
    const auto second = CopyDrawInput(context, &recorder, address, size, 4, Use::Index32);
    Require(second.reused && second.buffer == first.buffer && second.derived == 189, "an unchanged draw input was copied again");
    const auto vertex = CopyDrawInput(context, &recorder, address, size, 1, Use::Vertex);
    const auto narrow = CopyDrawInput(context, &recorder, address, size, 2, Use::Index16);
    Require(!vertex.reused && !narrow.reused && equalsGuest(vertex) && equalsGuest(narrow), "a draw input reused another use's copy");
    KeepDrawInput(&recorder, address, vertex, Use::Vertex, 0);
    Require(CopyDrawInput(context, &recorder, address, size, 1, Use::Vertex).buffer == vertex.buffer && CopyDrawInput(context, &recorder, address, size, 4, Use::Index32).buffer == first.buffer, "the uses' copies displaced each other");
    Require(!CopyDrawInput(context, nullptr, address, size, 4, Use::Index32).reused, "a draw input was reused without a recorder");
    words[5] = 0xdead;
    const auto stored = CopyDrawInput(context, &recorder, address, size, 4, Use::Index32);
    Require(!stored.reused && stored.buffer != first.buffer && equalsGuest(stored), "a draw input outlived a CPU store");
    KeepDrawInput(&recorder, address, stored, Use::Index32, 0xdead);
    Require(CopyDrawInput(context, &recorder, address, size, 4, Use::Index32).derived == 0xdead, "the new copy was not kept");
    MarkWritten(address + 128, 4);
    const auto marked = CopyDrawInput(context, &recorder, address, size, 4, Use::Index32);
    Require(!marked.reused && equalsGuest(marked), "a draw input outlived a stamped GPU store");
    KeepDrawInput(&recorder, address, marked, Use::Index32, 1);
    Require(CopyDrawInput(context, &recorder, address, size, 4, Use::Index32).reused, "the copy after the GPU store was not kept");
    alignas(64) static std::byte other[64];
    {
        GuestAllocations::Mutation mutation;
        mutation.Add(other, sizeof(other), true, true);
    }
    {
        GuestAllocations::Mutation mutation;
        mutation.Remove(other);
    }
    Require(!CopyDrawInput(context, &recorder, address, size, 4, Use::Index32).reused, "a draw input outlived a registry mutation");
    const auto pool = address + 8192;
    const auto whole = CopyDrawInput(context, &recorder, pool, 12288, 1, Use::Vertex);
    Require(!whole.reused && std::memcmp(whole.buffer->Bytes().data(), reinterpret_cast<const void*>(pool), 12288) == 0, "the vertex pool copy is wrong");
    KeepDrawInput(&recorder, pool, whole, Use::Vertex, 0);
    const auto prefix = CopyDrawInput(context, &recorder, pool, 4096, 1, Use::Vertex);
    Require(prefix.reused && prefix.buffer == whole.buffer, "a shorter vertex read did not reuse the longer snapshot");
    Require(!CopyDrawInput(context, &recorder, pool, 16384, 1, Use::Vertex).reused, "a longer vertex read reused a shorter snapshot");
    Require(!CopyDrawInput(context, &recorder, pool + 4, 4096, 1, Use::Vertex).reused, "a vertex read at another address reused the snapshot");
    Require(!CopyDrawInput(context, &recorder, pool, 4096, 4, Use::Index32).reused, "an index read reused a longer vertex snapshot");
    const auto shorter = CopyDrawInput(context, &recorder, pool, 8192, 4, Use::Index32);
    KeepDrawInput(&recorder, pool, shorter, Use::Index32, 3);
    Require(!CopyDrawInput(context, &recorder, pool, 4096, 4, Use::Index32).reused, "an index read reused a longer index snapshot");
    words[(8192 + 8192 + 16) / 4] = 0xbeef;
    const auto prefixAfter = CopyDrawInput(context, &recorder, pool, 4096, 1, Use::Vertex);
    Require(std::memcmp(prefixAfter.buffer->Bytes().data(), reinterpret_cast<const void*>(pool), 4096) == 0, "a shorter vertex read after a store got other bytes");
    const auto after = CopyDrawInput(context, &recorder, pool, 12288, 1, Use::Vertex);
    Require(!after.reused && std::memcmp(after.buffer->Bytes().data(), reinterpret_cast<const void*>(pool), 12288) == 0, "a vertex read over the store reused the old bytes");
    KeepDrawInput(&recorder, pool, after, Use::Vertex, 0);
    const auto small = CopyDrawInput(context, &recorder, pool, 4096, 1, Use::Vertex);
    Require(small.reused && small.buffer == after.buffer, "the new vertex snapshot does not serve shorter reads");
    recorder.Sync();
}

// Unit shadows (UnitShadow.hpp) over a host import of write-watched arena memory: a retile piece's
// slab destination and its seeds, freshness from the tracker (a publish never stamps, a CPU write
// makes the unit stale), the scopes, the slab boundary, and the retire publish. With
// APS5_UNIT_SHADOW_MIB=8 (one slab) the second slab's allocation evicts the first with a publish;
// with APS5_NO_UNIT_SHADOW=1 every primitive is inert.
void unitShadowTests(const Device& device, Recorder& recorder) {
    using namespace AgcDriver::GuestMemory;
    const auto& context = device.GetContext();
    if (!UnitShadowEnabled()) {
        Require(!AnyShadowedOverlaps(0x10000, 16) && PublishShadow(0x10000, 16, PublishScope::Whole, PublishReason::Hook) == 0, "unit shadows are off but not inert");
        std::cout << "unit shadows off (APS5_NO_UNIT_SHADOW, or no write watching): primitives inert\n";
        return;
    }
    if (context.hostImportAlignment == 0) {
        std::cout << "host imports unavailable: unit shadows not tested\n";
        return;
    }
    constexpr std::uint64_t unit = 65536;
    // Three slabs at the default 8 MiB: units 0..127, 128..255, 256..271.
    constexpr std::size_t bytes = (17u << 20u);
    void* block = AllocateWatched(bytes, 65536);
    Require(block != nullptr, "unit shadows are on without write watching");
    struct Release {
        void* block;
        ~Release() { ReleaseWatched(block, bytes); }
    } release{block};
    std::memset(block, 0x11, bytes);
    const auto address = reinterpret_cast<std::uint64_t>(block);
    {
        GuestAllocations::Mutation mutation;
        mutation.Add(block, bytes, true, true);
    }
    struct Unregister {
        void* block;
        bool armed = true;
        ~Unregister() {
            if (!armed) return;
            GuestAllocations::Mutation mutation;
            mutation.Remove(block);
        }
    } unregister{block};
    const auto* import = HostImportFor(context, address, bytes);
    if (import == nullptr) {
        std::cout << "host import of the shadow test block refused: unit shadows not tested\n";
        return;
    }
    if (!Watched(address, bytes)) {
        std::cout << "host imports are compared, not watched: unit shadows not tested\n";
        return;
    }
    Require(import->base == address && import->bytes == bytes, "the import does not cover the block");
    CollectWritesUncached(address, bytes);
    const auto* words = static_cast<const std::uint8_t*>(block);
    const auto unit3 = address + 3 * unit;
    const auto unit4 = unit3 + unit;
    const auto unit5 = unit4 + unit;
    // A half-unit piece seeds its unit from the import; a whole-unit piece does not; both land in
    // one slab at one offset.
    auto half = ShadowDestinationFor(context, *import, unit3, unit3 + unit / 2);
    Require(half.has_value() && half->seedUnits.size() == 1 && half->seedUnits[0].first == unit3 && half->seedUnits[0].second == unit4, "a half-unit retile piece does not seed its unit");
    auto whole = ShadowDestinationFor(context, *import, unit3, unit4);
    Require(whole.has_value() && whole->seedUnits.empty() && whole->slab == half->slab && whole->offset == half->offset && whole->buffer == half->buffer, "a whole-unit retile piece seeds, or lands elsewhere");
    Require(!AnyShadowedOverlaps(unit3, unit), "a destination alone counts as shadowed");
    const auto firstSlab = half->slab;
    Require(firstSlab->pins.load() == 2 && half->pin != nullptr, "destinations do not pin their slab");
    // The retile as writeBackWindows records it: the seed, then the piece's bytes over its half.
    const auto copyBuffer = context.Function<PFN_vkCmdCopyBuffer>("vkCmdCopyBuffer");
    const auto retile = [&](const ShadowDestination& destination, std::uint64_t begin, std::uint64_t length, std::uint8_t value, bool seed) {
        auto pattern = std::make_shared<Buffer>(context, static_cast<std::size_t>(length), VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
        std::memset(pattern->Bytes().data(), value, pattern->Bytes().size());
        const auto commands = recorder.Commands();
        recorder.Keep(pattern);
        recorder.Keep(destination.slab);
        RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_HOST_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT);
        if (seed) {
            const VkBufferCopy seedCopy{begin - import->base, SlabOffset(*import, *destination.slab, begin), unit};
            copyBuffer(commands, import->buffer, destination.buffer, 1, &seedCopy);
            RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT);
        }
        const VkBufferCopy piece{0, SlabOffset(*import, *destination.slab, begin), length};
        copyBuffer(commands, pattern->Handle(), destination.buffer, 1, &piece);
        RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT);
        recorder.MarkCovered(VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT);
        const ShadowedRange range{begin & ~(unit - 1), (begin & ~(unit - 1)) + unit, destination.slab};
        MarkShadowed(*import, std::span(&range, 1), TrackerGeneration());
    };
    retile(*half, unit3, unit / 2, 0x22, true);
    Require(AnyShadowedOverlaps(unit3, 1) && AnyShadowedOverlaps(unit4 - 1, 1) && AnyShadowedOverlaps(unit3 + 1000, 64), "a fresh unit is not seen as shadowed");
    Require(!AnyShadowedOverlaps(unit3 - 1, 1) && !AnyShadowedOverlaps(unit4, unit) && !AnyShadowedOverlaps(address, 3 * unit), "a range outside the unit is seen as shadowed");
    {
        // An upload's runs over units 2..4 split at unit 3's freshness; the fresh piece reads the slab.
        const std::pair<std::uint64_t, std::uint64_t> run{2 * unit, 5 * unit};
        const auto sources = ShadowSources(context, *import, address, std::span(&run, 1), {});
        Require(sources.size() == 3, "an upload run is not split at the fresh unit");
        Require(!sources[0].shadow && sources[0].begin == 2 * unit && sources[0].end == 3 * unit && sources[0].buffer == import->buffer && sources[0].offset == 2 * unit, "the import piece before the fresh unit is wrong");
        Require(sources[1].shadow && sources[1].begin == 3 * unit && sources[1].end == 4 * unit && sources[1].buffer == half->buffer && sources[1].offset == half->offset && sources[1].slab == half->slab, "the fresh unit's piece does not read the slab");
        Require(!sources[2].shadow && sources[2].begin == 4 * unit && sources[2].end == 5 * unit, "the import piece after the fresh unit is wrong");
    }
    // A whole publish: the untouched half keeps the seed (the import's bytes), the other the
    // retile's; the copy stamps nothing, and a second publish copies nothing.
    const auto pre = CollectWritesUncached(address, bytes);
    Require(PublishShadow(unit3, unit, PublishScope::Whole, PublishReason::Hook) == 1, "the whole publish did not copy the unit");
    Require(recorder.PendingWriteOverlaps(unit3, unit), "the publish noted no pending write");
    recorder.Sync();
    Require(words[3 * unit] == 0x22 && words[3 * unit + unit / 2 - 1] == 0x22 && words[3 * unit + unit / 2] == 0x11 && words[4 * unit - 1] == 0x11 && words[3 * unit - 1] == 0x11 && words[4 * unit] == 0x11, "the published bytes are wrong");
    Require(!AnyShadowedOverlaps(unit3, unit), "a published unit still counts as shadowed");
    Require(PublishShadow(unit3, unit, PublishScope::Whole, PublishReason::Hook) == 0, "a second publish copied the unit again");
    Require(UnchangedSince(unit3, unit, pre), "the publish stamped the unit");
    {
        // The published unit still reads from the slab (both hold the bytes).
        const std::pair<std::uint64_t, std::uint64_t> run{3 * unit, 4 * unit};
        const auto sources = ShadowSources(context, *import, address, std::span(&run, 1), {});
        Require(sources.size() == 1 && sources[0].shadow, "a published unit does not read from the slab");
    }
    // Partial-unit scope over unit 3 (partly, fresh again) and unit 4 (wholly, fresh): unit 3 only.
    retile(*whole, unit3, unit, 0x33, false);
    auto four = ShadowDestinationFor(context, *import, unit4, unit5);
    Require(four.has_value() && four->seedUnits.empty() && four->slab == half->slab, "unit 4's destination is not in the same slab");
    retile(*four, unit4, unit, 0x44, false);
    Require(AnyShadowedOverlaps(unit3, unit) && AnyShadowedOverlaps(unit4, unit), "re-marked units are not shadowed");
    Require(PublishShadow(unit3 + 100, static_cast<std::size_t>(2 * unit - 100), PublishScope::PartialUnits, PublishReason::Fill) == 1, "the partial publish did not copy exactly the partly covered unit");
    Require(!AnyShadowedOverlaps(unit3, unit) && AnyShadowedOverlaps(unit4, unit), "the partial publish copied the wrong unit");
    recorder.Sync();
    Require(words[3 * unit] == 0x33 && words[4 * unit - 1] == 0x33 && words[4 * unit] == 0x11, "the partial publish's bytes are wrong");
    // A CPU write into a page of unit 4 makes it stale: the publish collects the unit itself (no
    // walk saw the store before it), copies nothing and drops it.
    static_cast<void>(CollectWritesUncached(unit4, unit));
    *static_cast<volatile std::uint8_t*>(static_cast<void*>(static_cast<std::uint8_t*>(block) + 4 * unit + 4096)) = 0x55;
    Require(PublishShadow(unit4, unit, PublishScope::Whole, PublishReason::Hook) == 0, "a stale unit was published");
    Require(!AnyShadowedOverlaps(unit4, unit), "a stale unit still counts as shadowed after its publish");
    recorder.Sync();
    Require(words[4 * unit + 4096] == 0x55 && words[4 * unit] == 0x11, "a stale unit's publish overwrote the CPU's bytes");
    // The slab boundary: a piece over units 127 and 128 is refused, each alone lands in its slab.
    const auto unit127 = address + 127 * unit;
    const auto unit128 = unit127 + unit;
    Require(SlabBoundary(*import, unit127) == unit128 && SlabBoundary(*import, address) == unit128 && SlabBoundary(*import, unit128) == unit128 + 128 * unit, "the slab boundary is wrong");
    Require(!ShadowDestinationFor(context, *import, unit127, unit128 + unit).has_value(), "a piece across a slab boundary was accepted");
    auto low = ShadowDestinationFor(context, *import, unit127, unit128);
    Require(low.has_value() && low->slab == firstSlab && low->offset == 127 * unit, "unit 127 does not land in the first slab");
    retile(*low, unit127, unit, 0x66, false);
    auto high = ShadowDestinationFor(context, *import, unit128, unit128 + unit);
    const char* budgetText = std::getenv("APS5_UNIT_SHADOW_MIB");
    const auto budgetMiB = budgetText != nullptr ? std::strtoull(budgetText, nullptr, 10) : 1024ull;
    if (budgetMiB < 16) {
        // One slab fits: while destinations pin the first slab the second is refused (a
        // write-back in progress must keep its slab); with the pins released, the second slab's
        // allocation evicts the first after publishing its fresh unit 127 (the eviction publish is
        // recorded, so the bytes land at the sync).
        Require(!high.has_value(), "a pinned slab was evicted for a second slab");
        half.reset();
        whole.reset();
        four.reset();
        low.reset();
        Require(firstSlab->pins.load() == 0, "a released destination left its pin");
        high = ShadowDestinationFor(context, *import, unit128, unit128 + unit);
        Require(high.has_value() && high->slab != firstSlab && high->offset == 0, "unit 128 does not land in a second slab under the one-slab budget");
        recorder.Sync();
        Require(words[127 * unit] == 0x66 && !AnyShadowedOverlaps(unit127, unit), "the evicted slab's fresh unit was not published");
        std::cout << "unit shadow eviction under APS5_UNIT_SHADOW_MIB=" << budgetMiB << " verified\n";
    } else {
        Require(high.has_value() && high->slab != firstSlab && high->offset == 0 && AnyShadowedOverlaps(unit127, unit), "unit 128 does not land in a second slab beside the first");
        half.reset();
        whole.reset();
        four.reset();
        low.reset();
    }
    retile(*high, unit128, unit, 0x77, false);
    Require(AnyShadowedOverlaps(unit128, unit), "the second slab's unit is not shadowed");
    high.reset();
    {
        // RecordFastDispatch's flush of its in-place ranges (FlushFastDispatchRanges): ranges clear
        // of every fresh unit and storage result skip their FlushPending per dispatch (one scan of
        // each registry), not per element; with a fresh unit under two of three ranges the scan
        // hits and every range takes FlushPending, so both units are published (an "imported
        // buffer region" publishes the whole range). Under the one-slab budget unit 6's slab
        // evicts the second slab first (unit 128 published).
        const auto unit6 = unit5 + unit;
        const auto unit7 = unit6 + unit;
        retile(*ShadowDestinationFor(context, *import, unit6, unit7), unit6, unit, 0xaa, false);
        retile(*ShadowDestinationFor(context, *import, unit7, unit7 + unit), unit7, unit, 0xbb, false);
        const std::array<std::pair<std::uint64_t, std::uint64_t>, 2> clear{{{unit3, unit3 + 64}, {unit4 + 64, unit4 + 128}}};
        Require(FlushFastDispatchRanges(clear, true) && !FlushFastDispatchRanges(clear, false), "ranges clear of every shadow did not skip their flush per dispatch, or skipped it per element");
        const std::array<std::pair<std::uint64_t, std::uint64_t>, 3> hit{{{unit3, unit3 + 64}, {unit6 + 64, unit6 + 128}, {unit7, unit7 + 16}}};
        Require(AnyShadowedOverlaps(unit6, unit) && AnyShadowedOverlaps(unit7, unit), "units 6 and 7 are not shadowed");
        Require(!FlushFastDispatchRanges(hit, true), "a range over a fresh unit did not make the ranges take FlushPending");
        Require(!AnyShadowedOverlaps(unit6, unit) && !AnyShadowedOverlaps(unit7, unit), "a range's fresh unit was not published: not every range took FlushPending");
        recorder.Sync();
        Require(words[6 * unit] == 0xaa && words[7 * unit - 1] == 0xaa && words[7 * unit] == 0xbb && words[8 * unit - 1] == 0xbb, "the flushed ranges' units did not reach the import");
    }
    // Retire through the registry: the block is re-registered as its first five units, so the next
    // lookup reconciles and retires the import; the fresh units still inside a registered range
    // are published into its (kept) buffer, the rest (memory the title took back) are dropped.
    // Under the one-slab budget the second slab was evicted above, by unit 6's (unit 128 published).
    retile(*ShadowDestinationFor(context, *import, unit5, unit5 + unit), unit5, unit, 0x88, false);
    retile(*ShadowDestinationFor(context, *import, address + 2 * unit, unit3), address + 2 * unit, unit, 0x99, false);
    Require(AnyShadowedOverlaps(unit5, unit) && AnyShadowedOverlaps(address + 2 * unit, unit), "units 2 and 5 are not shadowed before the retire");
    {
        GuestAllocations::Mutation mutation;
        mutation.Remove(block);
        mutation.Add(block, static_cast<std::size_t>(5 * unit), true, true);
    }
    Require(HostImportFor(context, address, bytes) == nullptr, "a shrunk registration still imports the old range");
    Require(!AnyShadowedOverlaps(address, bytes), "the retired import's shadow survived");
    recorder.Sync();
    Require(words[2 * unit] == 0x99 && words[3 * unit - 1] == 0x99, "the retire did not publish the unit still registered");
    Require(words[5 * unit] == 0x11 && words[5 * unit + unit - 1] == 0x11, "the retire published a unit whose memory is no longer registered");
    if (budgetMiB >= 16) Require(words[127 * unit] == 0x11 && words[128 * unit] == 0x11, "the retire published the second slab's units outside the registration");
    else Require(words[128 * unit] == 0x77, "the eviction before the retire did not publish unit 128");
}

void storageRefreshTests(const Device& device, Recorder& recorder, bool watched) {
    const auto& base = device.GetContext();
    if (base.hostImportAlignment == 0) {
        std::cout << "host imports unavailable: storage refresh not tested\n";
        return;
    }
    constexpr std::uint32_t side = 256;
    constexpr std::size_t surfaceBytes = side * side * 4;
    constexpr std::size_t keyCount = surfaceBytes / 256;
    constexpr std::size_t bytes = surfaceBytes + 65536;
    void* block = nullptr;
    if (watched) {
        block = AllocateWatched(bytes, 65536);
        if (block == nullptr) {
            std::cout << "no write watching: storage refresh in watched memory not tested\n";
            return;
        }
    } else {
#ifdef _WIN32
        block = VirtualAlloc(nullptr, bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
#else
        block = std::aligned_alloc(65536, bytes);
#endif
    }
    Require(block != nullptr, "cannot allocate the storage refresh block");
    auto* texels = static_cast<std::uint8_t*>(block);
    auto* keys = texels + surfaceBytes;
    std::memset(texels, 0x55, surfaceBytes);
    std::memset(keys, 0x00, keyCount);
    const auto address = reinterpret_cast<std::uint64_t>(block);
    {
        GuestAllocations::Mutation mutation;
        mutation.Add(block, bytes, true, true);
    }
    struct Unregister {
        const Context& context;
        void* block;
        std::uint64_t address;
        ~Unregister() {
            {
                GuestAllocations::Mutation mutation;
                mutation.Remove(block);
            }
            HostImportFor(context, address, bytes);
        }
    } unregister{base, block, address};
    const auto* import = HostImportFor(base, address, bytes);
    if (import == nullptr) {
        std::cout << "host import of the storage refresh block refused: storage refresh not tested\n";
        return;
    }
    if (watched && !AgcDriver::GuestMemory::Watched(address, bytes)) std::cout << "host imports are compared, not watched: storage refresh in watched memory runs as unwatched\n";
    watched = watched && AgcDriver::GuestMemory::Watched(address, bytes);
    Require(watched || !ShadowDestinationFor(base, *import, address, address + 65536).has_value(), "a unit shadow was offered for memory the write tracker does not watch");
    if (!AgcDriver::GuestMemory::WriteWatched()) Require(!UnitShadowEnabled(), "unit shadows are on without write watching");
    TextureDetiler detiler(base);
    auto context = base;
    context.detiler = &detiler;
    GuestTextureResource resource{};
    resource.baseAddress = address;
    resource.width = side;
    resource.height = side;
    resource.mipCount = 1;
    resource.tileMode = TextureTileMode::kR64KBX;
    resource.dimension = TextureDimension::k2D;
    resource.format = 56;
    resource.dstSelX = 4;
    resource.dstSelY = 5;
    resource.dstSelZ = 6;
    resource.dstSelW = 7;
    resource.dccAddress = address + surfaceBytes;
    Require(DescribeSurface(resource).guestBytes == surfaceBytes, "the test surface has an unexpected size");
    DccKeyProof proof;
    Require(ProvedClearKeys(resource, surfaceBytes, proof) == DccKeys::Clear0000, "0x00 keys are not a 0000 clear");
    {
        auto image = std::make_shared<StorageTexture>(context, detiler, resource, 0);
        const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        const auto draw = [&](VkClearColorValue value) {
            const auto commands = recorder.Commands();
            recorder.Keep(image);
            RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
            context.Function<PFN_vkCmdClearColorImage>("vkCmdClearColorImage")(commands, image->Image(), VK_IMAGE_LAYOUT_GENERAL, &value, 1, &range);
            RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT);
            image->MarkDirty();
        };
        const auto holds = [&](std::array<std::uint8_t, 4> texel) {
            Buffer readback(context, surfaceBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
            const auto commands = recorder.Commands();
            RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
            VkBufferImageCopy copy{};
            copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            copy.imageExtent = {side, side, 1};
            context.Function<PFN_vkCmdCopyImageToBuffer>("vkCmdCopyImageToBuffer")(commands, image->Image(), VK_IMAGE_LAYOUT_GENERAL, readback.Handle(), 1, &copy);
            RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT);
            recorder.Submit();
            device.WaitQueue();
            recorder.Sync();
            const auto pixels = readback.Bytes();
            for (std::size_t i = 0; i < pixels.size(); ++i) {
                if (std::to_integer<std::uint8_t>(pixels[i]) != texel[i % 4]) return false;
            }
            return true;
        };
        const auto memoryHolds = [&](std::array<std::uint8_t, 4> texel, std::size_t first = 0, std::uint8_t other = 0) {
            std::vector<std::byte> read(surfaceBytes);
            if (watched) AgcDriver::GuestMemory::Read(address, read);
            else std::memcpy(read.data(), texels, surfaceBytes);
            for (std::size_t i = 0; i < surfaceBytes; ++i) {
                if (std::to_integer<std::uint8_t>(read[i]) != (i < first ? other : texel[i % 4])) return false;
            }
            return true;
        };
        const auto holdsMixed = [&](std::array<std::uint8_t, 4> texel, std::size_t count, std::uint8_t other) {
            Buffer readback(context, surfaceBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
            const auto commands = recorder.Commands();
            RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
            VkBufferImageCopy copy{};
            copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            copy.imageExtent = {side, side, 1};
            context.Function<PFN_vkCmdCopyImageToBuffer>("vkCmdCopyImageToBuffer")(commands, image->Image(), VK_IMAGE_LAYOUT_GENERAL, readback.Handle(), 1, &copy);
            RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT);
            recorder.Submit();
            device.WaitQueue();
            recorder.Sync();
            const auto pixels = readback.Bytes();
            std::size_t others = 0;
            for (std::size_t i = 0; i < pixels.size(); i += 4) {
                bool isOther = true, isTexel = true;
                for (std::size_t c = 0; c < 4; ++c) {
                    const auto value = std::to_integer<std::uint8_t>(pixels[i + c]);
                    isOther = isOther && value == other;
                    isTexel = isTexel && value == texel[c];
                }
                if (isOther) ++others;
                else if (!isTexel) return false;
            }
            return others == count;
        };
        Require(holds({0, 0, 0, 0}), "a surface under 0000 clear keys was not cleared");
        draw({{1.0f, 0.0f, 0.0f, 1.0f}});
        image->Refresh();
        Require(holds({255, 0, 0, 255}), "a refresh under the image's own clear keys dropped the draw's results");
        Require(memoryHolds({255, 0, 0, 255}), "the refresh's store of the results did not reach guest memory");
        if (watched) {
            draw({{0.0f, 0.0f, 1.0f, 1.0f}});
            std::memset(texels, 0x33, 65536);
            image->Refresh();
            Require(holdsMixed({0, 0, 255, 255}, 65536 / 4, 0x33), "a CPU store into a unit with results pending was not seen by the refresh");
            Require(memoryHolds({0, 0, 255, 255}, 65536, 0x33), "guest memory lost the CPU store or the other units' results");
            image->Refresh();
            Require(holdsMixed({0, 0, 255, 255}, 65536 / 4, 0x33), "an unchanged surface changed at a refresh");
        }
        draw({{0.0f, 1.0f, 0.0f, 1.0f}});
        std::memset(keys, 0x40, keyCount);
        image->Refresh();
        Require(holds({0, 0, 0, 255}), "new 0001 clear keys did not clear the image");
        Require(watched ? memoryHolds({0, 0, 255, 255}, 65536, 0x33) : memoryHolds({255, 0, 0, 255}), "results dead under new clear keys were stored");
    }
    recorder.Sync();
}

void importWatchTests(const Device& device) {
    using namespace AgcDriver::GuestMemory;
    const auto& context = device.GetContext();
#ifdef _WIN32
    static_cast<void>(context);
    std::cout << "import watch decisions: Linux write watch only\n";
#else
    if (context.hostImportAlignment == 0 || !WriteWatched()) {
        std::cout << "host imports or write watching unavailable: import watch decisions not tested\n";
        return;
    }
    const auto probe = ProbeImportWriteProtection(context);
    Require(probe.failure == nullptr, std::string("(u) the import probe failed at ") + (probe.failure != nullptr ? probe.failure : "") + " (" + std::to_string(static_cast<int>(probe.result)) + ")");
    std::cout << "import probe: " << probe.writtenAfterSubmit << " of " << probe.pages << " scratch pages written after a GPU read, " << probe.writtenAtImport << " after the import\n";
    const auto decided = PrepareImportWatch(context);
    struct Restore {
        const Context& context;
        ImportWatch decided;
        ~Restore() { SetImportWatch(context, decided); }
    } restore{context, decided};
    constexpr std::size_t unit = 65536;
    const auto remap = [](void* block, std::size_t bytes) {
        munmap(block, bytes);
        GuestWriteWatch::GuestWriteWatchUnregister_nid_postfix(block, bytes);
        Require(mmap(block, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) == block, "(u) cannot map the test block again");
        GuestWriteWatch::GuestWriteWatchRegister_nid_postfix(block, bytes);
    };
    const auto registerRange = [](void* block, std::size_t bytes) {
        GuestAllocations::Mutation mutation;
        mutation.Add(block, bytes, true, true);
    };
    const auto unregisterRange = [](void* block) {
        GuestAllocations::Mutation mutation;
        mutation.Remove(block);
    };
    SetImportWatch(context, ImportWatch::Unwatch);
    {
        constexpr std::size_t bytes = 3 * unit;
        void* block = AllocateWatched(bytes, unit);
        const auto address = reinterpret_cast<std::uint64_t>(block);
        std::memset(block, 0x11, bytes);
        const auto before = CollectWrites(address, bytes);
        Require(before != 0, "(u) a watched block is not collected");
        BumpCollectEpoch();
        Require(CollectWrites(address, 2 * unit) != 0, "(u) the memoized collect of a watched block failed");
        registerRange(block, 2 * unit);
        const auto* import = HostImportFor(context, address, 2 * unit);
        Require(import != nullptr && import->unwatched, "(u) an import made under the unwatch decision is not marked unwatched");
        Require(!Watched(address, 2 * unit) && !Watched(address + unit, 4096), "(u) an imported range stays watched");
        Require(CollectWrites(address, 2 * unit) == 0 && CollectWrites(address + 4096, 4096) == 0, "(u) a collect memoized before the import answers for the unwatched range");
        Require(CollectWrites(address + 2 * unit - 4096, 8192) == 0, "(u) a range partly imported is collected");
        Require(Watched(address + 2 * unit, unit), "(u) the unimported neighbour left the watch");
        const auto neighbour = CollectWrites(address + 2 * unit, unit);
        Require(neighbour != 0, "(u) the unimported neighbour is not collected");
        static_cast<volatile std::uint8_t*>(block)[4096] = 0x22;
        Require(!UnchangedSince(address, 2 * unit, before) && !UnchangedSince(address, 4096, TrackerGeneration()) && !UnchangedSinceCollected(address, 4096, TrackerGeneration()), "(u) an unwatched range reports unchanged");
        const std::array<std::uint64_t, 2> generations{before, TrackerGeneration()};
        std::array<std::uint8_t, 2> changed{};
        std::array<std::uint8_t, 2> cpu{};
        Require(!ChangedBlocks(address, 2 * unit, generations, changed, cpu) && changed[0] != 0 && changed[1] != 0, "(u) the stamps of an unwatched range are offered as tracked");
        Require(MarkWritten(address, unit) == 0, "(u) a GPU write into an unwatched range claims a generation");
        Require(MarkWritten(address + 2 * unit, 4096) != 0 && !UnchangedSince(address + 2 * unit, unit, neighbour), "(u) a GPU write into the watched neighbour is not stamped");
        unregisterRange(block);
        remap(block, 2 * unit);
        Require(Watched(address, 2 * unit), "(u) a remapped range is not watched");
        registerRange(block, 2 * unit);
        const auto* kept = HostImportFor(context, address, 2 * unit);
        Require(kept != nullptr && kept->unwatched && !Watched(address, 2 * unit) && CollectWrites(address, 2 * unit) == 0, "(u) an import kept over a remap left the range watched");
        unregisterRange(block);
        remap(block, 2 * unit);
        registerRange(block, unit);
        const auto* again = HostImportFor(context, address, unit);
        Require(again != nullptr && again->unwatched && again->bytes == unit && !Watched(address, unit), "(u) a re-import after an unmap did not follow the decision");
        Require(Watched(address + unit, unit) && CollectWrites(address + unit, unit) != 0, "(u) the unregistered rest of a remapped range is not watched");
        unregisterRange(block);
        Require(HostImportFor(context, address, unit) == nullptr, "(u) an unregistered range still imports");
        ReleaseWatched(block, bytes);
    }
    SetImportWatch(context, ImportWatch::Watch);
    {
        constexpr std::size_t bytes = unit;
        void* block = AllocateWatched(bytes, unit);
        const auto address = reinterpret_cast<std::uint64_t>(block);
        registerRange(block, bytes);
        const auto* import = HostImportFor(context, address, bytes);
        Require(import != nullptr && !import->unwatched && Watched(address, bytes), "(u) an import made under the watch decision left the watch");
        const auto generation = CollectWrites(address, bytes);
        Require(generation != 0 && UnchangedSince(address, bytes, generation), "(u) a watched import is not collected");
        Require(MarkWritten(address, 4096) != 0 && !UnchangedSince(address, bytes, generation), "(u) a GPU write into a watched import is not stamped");
        unregisterRange(block);
        HostImportFor(context, address, bytes);
        ReleaseWatched(block, bytes);
    }
#endif
}

void staleGenerationTests(const Device& device, Recorder& recorder) {
    const auto& base = device.GetContext();
#ifdef _WIN32
    static_cast<void>(recorder);
    static_cast<void>(base);
    std::cout << "a range leaving the watch: Linux write watch only\n";
#else
    if (base.hostImportAlignment == 0) {
        std::cout << "host imports unavailable: a range leaving the watch not tested\n";
        return;
    }
    constexpr std::uint32_t side = 256;
    constexpr std::size_t surfaceBytes = side * side * 4;
    constexpr std::size_t keyCount = surfaceBytes / 256;
    constexpr std::size_t bytes = surfaceBytes + 65536;
    void* block = AllocateWatched(bytes, 65536);
    if (block == nullptr) {
        std::cout << "no write watching: a range leaving the watch not tested\n";
        return;
    }
    struct Release {
        void* block;
        ~Release() { ReleaseWatched(block, bytes); }
    } release{block};
    const auto decided = PrepareImportWatch(base);
    SetImportWatch(base, ImportWatch::Unwatch);
    struct Restore {
        const Context& context;
        ImportWatch decided;
        ~Restore() { SetImportWatch(context, decided); }
    } restore{base, decided};
    auto* texels = static_cast<std::uint8_t*>(block);
    std::memset(texels, 0x55, surfaceBytes);
    std::memset(texels + surfaceBytes, 0x00, keyCount);
    const auto address = reinterpret_cast<std::uint64_t>(block);
    {
        GuestAllocations::Mutation mutation;
        mutation.Add(block, bytes, true, true);
    }
    struct Unregister {
        const Context& context;
        void* block;
        std::uint64_t address;
        ~Unregister() {
            {
                GuestAllocations::Mutation mutation;
                mutation.Remove(block);
            }
            HostImportFor(context, address, bytes);
        }
    } unregister{base, block, address};
    TextureDetiler detiler(base);
    auto context = base;
    context.detiler = &detiler;
    GuestTextureResource resource{};
    resource.baseAddress = address;
    resource.width = side;
    resource.height = side;
    resource.mipCount = 1;
    resource.tileMode = TextureTileMode::kR64KBX;
    resource.dimension = TextureDimension::k2D;
    resource.format = 56;
    resource.dstSelX = 4;
    resource.dstSelY = 5;
    resource.dstSelZ = 6;
    resource.dstSelW = 7;
    resource.dccAddress = address + surfaceBytes;
    {
        auto image = std::make_shared<StorageTexture>(context, detiler, resource, 0);
        Require(AgcDriver::GuestMemory::Watched(address, bytes), "(s) the storage image imported its memory before a generation was taken");
        const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        const auto commands = recorder.Commands();
        recorder.Keep(image);
        const VkClearColorValue red{{1.0f, 0.0f, 0.0f, 1.0f}};
        RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
        context.Function<PFN_vkCmdClearColorImage>("vkCmdClearColorImage")(commands, image->Image(), VK_IMAGE_LAYOUT_GENERAL, &red, 1, &range);
        RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT);
        image->MarkDirty();
        const auto* import = HostImportFor(base, address, bytes);
        if (import == nullptr) {
            std::cout << "host import of the stale generation block refused: a range leaving the watch not tested\n";
            recorder.Sync();
            return;
        }
        Require(!AgcDriver::GuestMemory::Watched(address, bytes), "(s) the imported range stayed watched");
        image->Refresh();
        Buffer readback(context, surfaceBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
        const auto copyCommands = recorder.Commands();
        RecordMemoryBarrier(context, copyCommands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
        VkBufferImageCopy copy{};
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.imageExtent = {side, side, 1};
        context.Function<PFN_vkCmdCopyImageToBuffer>("vkCmdCopyImageToBuffer")(copyCommands, image->Image(), VK_IMAGE_LAYOUT_GENERAL, readback.Handle(), 1, &copy);
        RecordMemoryBarrier(context, copyCommands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT);
        recorder.Submit();
        device.WaitQueue();
        recorder.Sync();
        const auto pixels = readback.Bytes();
        constexpr std::array<std::uint8_t, 4> expected{255, 0, 0, 255};
        for (std::size_t i = 0; i < pixels.size(); ++i) Require(std::to_integer<std::uint8_t>(pixels[i]) == expected[i % 4], "(s) results pending when their memory left the watch were dropped");
        for (std::size_t i = 0; i < surfaceBytes; ++i) Require(texels[i] == expected[i % 4], "(s) results pending when their memory left the watch did not reach it");
    }
    recorder.Sync();
#endif
}

void importWindowTests(const Device& device, Recorder& recorder) {
    using namespace AgcDriver::GuestMemory;
    const auto& base = device.GetContext();
#ifdef _WIN32
    static_cast<void>(recorder);
    static_cast<void>(base);
    std::cout << "the import window: Linux write watch only\n";
#else
    if (base.hostImportAlignment == 0) {
        std::cout << "host imports unavailable: the import window not tested\n";
        return;
    }
    constexpr std::uint32_t side = 256;
    constexpr std::size_t surfaceBytes = side * side * 4;
    constexpr std::size_t keyCount = surfaceBytes / 256;
    constexpr std::size_t bytes = surfaceBytes + 65536;
    void* block = AllocateWatched(bytes, 65536);
    if (block == nullptr) {
        std::cout << "no write watching: the import window not tested\n";
        return;
    }
    struct Release {
        void* block;
        ~Release() { ReleaseWatched(block, bytes); }
    } release{block};
    const auto probe = ProbeImportWriteProtection(base);
    Require(probe.failure == nullptr, "(w) the import probe failed");
    const bool importWrites = probe.writtenAtImport != 0;
    const auto decided = PrepareImportWatch(base);
    SetImportWatch(base, ImportWatch::Watch);
    struct Restore {
        const Context& context;
        ImportWatch decided;
        ~Restore() { SetImportWatch(context, decided); }
    } restore{base, decided};
    auto* texels = static_cast<std::uint8_t*>(block);
    std::memset(texels, 0x55, surfaceBytes);
    std::memset(texels + surfaceBytes, 0x00, keyCount);
    const auto address = reinterpret_cast<std::uint64_t>(block);
    {
        GuestAllocations::Mutation mutation;
        mutation.Add(block, bytes, true, true);
    }
    struct Unregister {
        const Context& context;
        void* block;
        std::uint64_t address;
        ~Unregister() {
            {
                GuestAllocations::Mutation mutation;
                mutation.Remove(block);
            }
            HostImportFor(context, address, bytes);
        }
    } unregister{base, block, address};
    TextureDetiler detiler(base);
    auto context = base;
    context.detiler = &detiler;
    GuestTextureResource resource{};
    resource.baseAddress = address;
    resource.width = side;
    resource.height = side;
    resource.mipCount = 1;
    resource.tileMode = TextureTileMode::kR64KBX;
    resource.dimension = TextureDimension::k2D;
    resource.format = 56;
    resource.dstSelX = 4;
    resource.dstSelY = 5;
    resource.dstSelZ = 6;
    resource.dstSelW = 7;
    resource.dccAddress = address + surfaceBytes;
    {
        auto image = std::make_shared<StorageTexture>(context, detiler, resource, 0);
        Require(!HostImportCovers(base, address, bytes), "(w) the storage image imported its memory before a generation was taken");
        const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        const auto commands = recorder.Commands();
        recorder.Keep(image);
        const VkClearColorValue red{{1.0f, 0.0f, 0.0f, 1.0f}};
        RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
        context.Function<PFN_vkCmdClearColorImage>("vkCmdClearColorImage")(commands, image->Image(), VK_IMAGE_LAYOUT_GENERAL, &red, 1, &range);
        RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT);
        image->MarkDirty();
        const auto cached = CollectWritesUncached(address, surfaceBytes);
        Require(cached != 0 && UnchangedSince(address, surfaceBytes, cached), "(w) the cache's generation is not current before the import");
        const auto* import = HostImportFor(base, address, bytes);
        if (import == nullptr) {
            std::cout << "host import of the import window block refused: the import window not tested\n";
            recorder.Sync();
            return;
        }
        Require(!import->unwatched && Watched(address, bytes), "(w) an import under the watch decision left the watch");
        CollectWritesUncached(address, surfaceBytes);
        if (importWrites) {
            Require(!UnchangedSince(address, surfaceBytes, cached), "(w) a cache over the pages the import reported sees no change");
            std::array<std::uint8_t, surfaceBytes / 65536> changed{};
            const std::array<std::uint64_t, surfaceBytes / 65536> generations{cached, cached, cached, cached};
            Require(ChangedBlocks(address, surfaceBytes, generations, changed) && std::all_of(changed.begin(), changed.end(), [](std::uint8_t value) { return value == BlockMaybeWritten; }), "(w) the pages the import reported read as written by the CPU");
            Require(!WrittenSince(address, surfaceBytes, cached), "(w) the import window reads as a CPU store");
        } else {
            std::cout << "the import reports no pages here: only the pending results of the import window checked\n";
        }
        const auto after = CollectWritesUncached(address, surfaceBytes);
        Require(after != 0 && UnchangedSince(address, surfaceBytes, after), "(w) the imported range is not watched again after the import window");
        image->Refresh();
        Buffer readback(context, surfaceBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
        const auto copyCommands = recorder.Commands();
        RecordMemoryBarrier(context, copyCommands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
        VkBufferImageCopy copy{};
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.imageExtent = {side, side, 1};
        context.Function<PFN_vkCmdCopyImageToBuffer>("vkCmdCopyImageToBuffer")(copyCommands, image->Image(), VK_IMAGE_LAYOUT_GENERAL, readback.Handle(), 1, &copy);
        RecordMemoryBarrier(context, copyCommands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT);
        recorder.Submit();
        device.WaitQueue();
        recorder.Sync();
        const auto pixels = readback.Bytes();
        constexpr std::array<std::uint8_t, 4> expected{255, 0, 0, 255};
        for (std::size_t i = 0; i < pixels.size(); ++i) Require(std::to_integer<std::uint8_t>(pixels[i]) == expected[i % 4], "(w) results pending over the import window were dropped");
        std::vector<std::byte> memory(surfaceBytes);
        Read(address, memory);
        for (std::size_t i = 0; i < surfaceBytes; ++i) Require(std::to_integer<std::uint8_t>(memory[i]) == expected[i % 4], "(w) results pending over the import window did not reach guest memory");
        const auto stored = CollectWritesUncached(address, surfaceBytes);
        texels[65536 + 8] = 0x77;
        CollectWritesUncached(address, surfaceBytes);
        Require(WrittenSince(address + 65536, 65536, stored) && !WrittenSince(address, 65536, stored), "(w) a CPU store after the import window is not reported as one");
    }
    recorder.Sync();
#endif
}

void pendingKeyStoreTests(const Device& device, Recorder& recorder) {
    const auto& context = device.GetContext();
    if (context.hostImportAlignment == 0) {
        std::cout << "host imports unavailable: pending key stores not tested\n";
        return;
    }
    constexpr std::size_t surfaceBytes = 65536;
    constexpr std::size_t keyCount = surfaceBytes / 256;
    constexpr std::size_t bytes = 65536;
#ifdef _WIN32
    void* block = VirtualAlloc(nullptr, bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
#else
    void* block = std::aligned_alloc(65536, bytes);
#endif
    Require(block != nullptr, "cannot allocate the pending key store block");
    auto* keys = static_cast<std::uint8_t*>(block);
    const auto address = reinterpret_cast<std::uint64_t>(block);
    {
        GuestAllocations::Mutation mutation;
        mutation.Add(block, bytes, true, true);
    }
    struct Unregister {
        const Context& context;
        void* block;
        std::uint64_t address;
        ~Unregister() {
            {
                GuestAllocations::Mutation mutation;
                mutation.Remove(block);
            }
            HostImportFor(context, address, bytes);
        }
    } unregister{context, block, address};
    if (HostImportFor(context, address, bytes) == nullptr) {
        std::cout << "host import of the pending key store block refused: pending key stores not tested\n";
        return;
    }
    recorder.Sync();
    std::memset(keys, 0x00, keyCount);
    MarkDccUncompressed(context, address, surfaceBytes);
    Require(recorder.PendingWriteOverlaps(address, keyCount) && keys[0] == 0x00, "the uncompressed key store did not stay pending");
    Require(CurrentDccKeys(address, surfaceBytes) == DccKeys::Uncompressed, "the keys after a pending uncompressed store do not read as uncompressed");
    Require(recorder.PendingWriteOverlaps(address, keyCount), "reading keys the driver's own pending store wrote waited for the GPU");
    Require(CurrentDccKeys(address + 16, surfaceBytes / 2) == DccKeys::Uncompressed, "a key range inside the pending store does not read as uncompressed");
    recorder.NotePendingWrite(address + 16, 16);
    Require(CurrentDccKeys(address, surfaceBytes) == DccKeys::Uncompressed && !recorder.PendingWriteOverlaps(address, keyCount), "a key read with a later writer over the pending store did not wait for it");
    Require(std::all_of(keys, keys + keyCount, [](std::uint8_t key) { return key == 0xff; }), "the uncompressed key store did not land");
    recorder.NotePendingWrite(address, keyCount);
    NoteKeysFillOnGpu(address, keyCount, DccKeys::Clear0001);
    Require(CurrentDccKeys(address, surfaceBytes) == DccKeys::Clear0001 && recorder.PendingWriteOverlaps(address, keyCount), "the keys of a pending fill did not read as its keys without a wait");
    recorder.NotePendingWrite(address, 2 * keyCount);
    Require(CurrentDccKeys(address, surfaceBytes) == DccKeys::Uncompressed && !recorder.PendingWriteOverlaps(address, keyCount), "a later wider writer over a pending fill was not waited for");
    recorder.Sync();
}

void metadataPassTests(const Device& device, Recorder& recorder) {
    const auto& base = device.GetContext();
    if (base.hostImportAlignment == 0) {
        std::cout << "host imports unavailable: resident metadata passes not tested\n";
        return;
    }
    constexpr std::uint32_t side = 256;
    constexpr std::size_t surfaceBytes = side * side * 4;
    constexpr std::size_t keyCount = surfaceBytes / 256;
    constexpr std::size_t bytes = surfaceBytes + 65536;
#ifdef _WIN32
    void* block = VirtualAlloc(nullptr, bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
#else
    void* block = std::aligned_alloc(65536, bytes);
#endif
    Require(block != nullptr, "cannot allocate the metadata pass block");
    auto* texels = static_cast<std::uint8_t*>(block);
    auto* keys = texels + surfaceBytes;
    const auto address = reinterpret_cast<std::uint64_t>(block);
    {
        GuestAllocations::Mutation mutation;
        mutation.Add(block, bytes, true, true);
    }
    struct Unregister {
        const Context& context;
        void* block;
        std::uint64_t address;
        ~Unregister() {
            ClearCachedTextures(context.device);
            {
                GuestAllocations::Mutation mutation;
                mutation.Remove(block);
            }
            HostImportFor(context, address, bytes);
        }
    } unregister{base, block, address};
    if (HostImportFor(base, address, bytes) == nullptr) {
        std::cout << "host import of the metadata pass block refused: resident metadata passes not tested\n";
        return;
    }
    TextureDetiler detiler(base);
    auto context = base;
    context.detiler = &detiler;
    ColorTarget color{};
    color.address = address;
    color.extent = {side, side};
    color.format = VK_FORMAT_R8G8B8A8_UNORM;
    color.bytes = surfaceBytes;
    color.componentMapping = 0xe4u;
    color.tileMode = ColorTileMode::RenderTarget;
    color.elementBytes = 4;
    color.dccAddress = address + surfaceBytes;
    color.clearWords = {0x80402010u, 0};
    const ColorMetadataPass pass{ColorMetadataPass::Mode::EliminateFastClear, {color}};
    const auto memoryHolds = [&](std::array<std::uint8_t, 4> texel) {
        StorageTexture::FlushPending(address, surfaceBytes, nullptr, "test");
        recorder.Submit();
        device.WaitQueue();
        recorder.Sync();
        std::vector<std::uint8_t> stored(surfaceBytes);
        AgcDriver::GuestMemory::Read(address, std::as_writable_bytes(std::span(stored)), 1);
        for (std::size_t i = 0; i < surfaceBytes; ++i) {
            if (stored[i] != texel[i % 4]) return false;
        }
        return true;
    };
    const auto keysUncompressed = [&] { return ReadDccKeys(color.dccAddress, surfaceBytes) == DccKeys::Uncompressed; };
    std::memset(texels, 0x55, surfaceBytes);
    std::memset(keys, 0x20, keyCount);
    RunColorMetadataPass(context, pass);
    Require(StorageTexture::FindPending(address, surfaceBytes) != nullptr && texels[0] == 0x55, "the register fast clear eliminate did not stay in the resident image");
    Require(keysUncompressed(), "the register fast clear eliminate left the keys compressed");
    Require(memoryHolds({0x10, 0x20, 0x40, 0x80}), "the register fast clear eliminate did not store CB_COLOR_CLEAR_WORD");
    Require(std::all_of(keys, keys + keyCount, [](std::uint8_t key) { return key == 0xff; }), "the stored keys are not uncompressed");
    std::memset(keys, 0xc0, keyCount);
    RunColorMetadataPass(context, {ColorMetadataPass::Mode::DccDecompress, {color}});
    Require(StorageTexture::FindPending(address, surfaceBytes) != nullptr, "the DCC decompress did not stay in the resident image");
    Require(keysUncompressed(), "the DCC decompress left the keys compressed");
    Require(memoryHolds({0xff, 0xff, 0xff, 0xff}), "the DCC decompress did not store the 1111 value");
    {
        const auto* import = HostImportFor(base, address, bytes);
        Require(import != nullptr, "the metadata pass block lost its import");
        const auto commands = recorder.Commands();
        context.Function<PFN_vkCmdFillBuffer>("vkCmdFillBuffer")(commands, import->buffer, color.dccAddress - import->base, keyCount, 0x20202020u);
        RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT | VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_HOST_READ_BIT);
        recorder.NotePendingWrite(color.dccAddress, keyCount);
        Require(keys[0] == 0xff, "the recorded key fill landed before its batch ran");
        Require(CurrentDccKeys(color.dccAddress, surfaceBytes) == DccKeys::ClearRegister, "the keys read past a pending fast-clear fill");
    }
    RunColorMetadataPass(context, pass);
    Require(keysUncompressed() && memoryHolds({0x10, 0x20, 0x40, 0x80}), "a fast clear recorded before the pass was not eliminated");
    std::memset(texels, 0x66, surfaceBytes);
    RunColorMetadataPass(context, pass);
    Require(memoryHolds({0x66, 0x66, 0x66, 0x66}), "a pass over uncompressed keys changed the texels");
    ClearCachedTextures(context.device);
    GuestTextureResource plain{};
    plain.baseAddress = address;
    plain.width = side;
    plain.height = side;
    plain.mipCount = 1;
    plain.tileMode = TextureTileMode::kR64KBX;
    plain.dimension = TextureDimension::k2D;
    plain.format = 56;
    plain.dstSelX = 4;
    plain.dstSelY = 5;
    plain.dstSelZ = 6;
    plain.dstSelW = 7;
    const auto unkeyed = CachedStorageSurface(context, plain);
    std::memset(keys, 0xc0, keyCount);
    RunColorMetadataPass(context, pass);
    const auto keyed = StorageTexture::FindPending(address, surfaceBytes);
    Require(keyed != nullptr && keyed != unkeyed && keyed->Descriptor().dccAddress == color.dccAddress, "a pass over a resident image under other keys did not remake it under the target's keys");
    Require(keysUncompressed() && memoryHolds({0xff, 0xff, 0xff, 0xff}), "a pass over a remade resident image did not store the 1111 value");
}

// The data word positions of a dispatch-cache variant (Driver.cpp's data-only hits): leaves
// located among the runs' words, aliased, unaligned, out-of-run and mismatched ones skipped.
void dataWordPositionsTests() {
    using AgcDriver::DataWordPositions;
    const std::vector<std::pair<std::uint64_t, std::uint64_t>> runs{{0x1000, 0x1010}, {0x2000, 0x2008}};
    const std::vector<std::uint32_t> words{1, 2, 3, 4, 5, 6};
    const std::vector<std::uint32_t> flattened{2, 6, 3};
    std::vector<std::uint32_t> positions, slots;
    {
        const std::vector<std::pair<std::uint32_t, std::uint64_t>> leaves{{1, 0x2004}, {0, 0x1004}, {2, 0x1008}};
        const std::vector<std::uint64_t> otherReads{0x1008};
        const auto counts = DataWordPositions(runs, leaves, otherReads, words, flattened, positions, slots);
        Require(positions == std::vector<std::uint32_t>{1, 5} && slots == std::vector<std::uint32_t>{0, 1}, "data word positions are wrong");
        Require(counts.aliased == 1 && counts.unmapped == 0 && counts.mismatched == 0, "an aliased leaf was not counted");
    }
    {
        const std::vector<std::pair<std::uint32_t, std::uint64_t>> leaves{{0, 0x1006}, {1, 0x3000}, {1, 0x2008}, {2, 0x1000}};
        const auto counts = DataWordPositions(runs, leaves, {}, words, flattened, positions, slots);
        Require(positions.empty() && slots.empty(), "a skipped leaf produced a position");
        Require(counts.unmapped == 3 && counts.mismatched == 1 && counts.aliased == 0, "skipped leaves were counted wrongly");
    }
    {
        // Two pure slots over one dword: both positions kept, in position order.
        const std::vector<std::pair<std::uint32_t, std::uint64_t>> leaves{{2, 0x1008}, {1, 0x2004}, {0, 0x1004}};
        static_cast<void>(DataWordPositions(runs, leaves, {}, words, flattened, positions, slots));
        Require(positions == std::vector<std::uint32_t>{1, 2, 5} && slots == std::vector<std::uint32_t>{0, 2, 1}, "positions are not sorted");
    }
}

// The don't-care bits of a variant (IgnoredWordBits): a sampled-image T# located among the words
// yields its word 5 / word 6 masks; a T# whose words are not consecutive in address, a storage
// image's and one with a word at a data position yield none; adjacent runs count as consecutive.
void ignoredWordBitsTests() {
    using AgcDriver::IgnoredWordBits;
    using AgcDriver::IgnoredMaskAt;
    using AgcDriver::WordsEqualIgnoring;
    using AgcDriver::SameDescriptorIgnoringTsharpBits;
    using AgcDriver::TsharpWord5IgnoredBits;
    using AgcDriver::TsharpWord6IgnoredBits;
    using Bits = std::vector<std::pair<std::uint32_t, std::uint32_t>>;
    const std::vector<std::uint32_t> first{0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18};
    const std::vector<std::uint32_t> second{0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x28};
    ShaderRecompiler::DescriptorBinding sampled;
    sampled.kind = ShaderRecompiler::DescriptorKind::SampledImage;
    sampled.role = ShaderRecompiler::DescriptorRole::GuestImages;
    sampled.count = 2;
    sampled.guestDescriptor = first;
    sampled.guestDescriptor.insert(sampled.guestDescriptor.end(), second.begin(), second.end());
    // Words: two fillers, the first T# (positions 2-9), two fillers, the second T# (12-19) split
    // over the last two runs.
    std::vector<std::uint32_t> words{0xa, 0xb};
    words.insert(words.end(), first.begin(), first.end());
    words.insert(words.end(), {0xc, 0xd});
    words.insert(words.end(), second.begin(), second.end());
    Bits ignored;
    {
        const std::vector<std::pair<std::uint64_t, std::uint64_t>> runs{{0x1000, 0x1030}, {0x2000, 0x2010}, {0x3000, 0x3010}};
        const auto located = IgnoredWordBits(runs, words, std::span(&sampled, 1), {}, ignored);
        Require(located == 1 && ignored == Bits{{7, TsharpWord5IgnoredBits}, {8, TsharpWord6IgnoredBits}}, "a T# among the words was not located, or a split one was");
    }
    {
        const std::vector<std::pair<std::uint64_t, std::uint64_t>> runs{{0x1000, 0x1030}, {0x2000, 0x2010}, {0x2010, 0x2020}};
        const auto located = IgnoredWordBits(runs, words, std::span(&sampled, 1), {}, ignored);
        Require(located == 2 && ignored == Bits{{7, TsharpWord5IgnoredBits}, {8, TsharpWord6IgnoredBits}, {17, TsharpWord5IgnoredBits}, {18, TsharpWord6IgnoredBits}}, "a T# over adjacent runs was not located");
        const std::vector<std::uint32_t> dataPositions{8};
        Require(IgnoredWordBits(runs, words, std::span(&sampled, 1), dataPositions, ignored) == 1 && ignored == Bits{{17, TsharpWord5IgnoredBits}, {18, TsharpWord6IgnoredBits}}, "a T# with a word at a data position was kept");
        auto storage = sampled;
        storage.kind = ShaderRecompiler::DescriptorKind::StorageImage;
        Require(IgnoredWordBits(runs, words, std::span(&storage, 1), {}, ignored) == 0 && ignored.empty(), "a storage image produced don't-care bits");
    }
    {
        const Bits bits{{7, TsharpWord5IgnoredBits}, {8, TsharpWord6IgnoredBits}};
        Require(IgnoredMaskAt(bits, 8) == TsharpWord6IgnoredBits && IgnoredMaskAt(bits, 9) == 0, "the mask lookup is wrong");
        auto toggled = words;
        toggled[7] ^= TsharpWord5IgnoredBits;
        toggled[8] = (toggled[8] & ~TsharpWord6IgnoredBits) | 0x73u;
        Require(WordsEqualIgnoring(words, toggled, bits), "words differing only in the don't-care bits compared unequal");
        toggled[8] ^= 0x100u;
        Require(!WordsEqualIgnoring(words, toggled, bits), "a difference outside the don't-care bits compared equal");
        Require(!WordsEqualIgnoring(words, std::span(toggled).first(words.size() - 1), bits), "sequences of different sizes compared equal");
        auto descriptor = sampled.guestDescriptor;
        descriptor[5] ^= TsharpWord5IgnoredBits;
        descriptor[8 + 6] ^= 0x73u;
        Require(SameDescriptorIgnoringTsharpBits(sampled, sampled.guestDescriptor, descriptor), "descriptors differing only in the don't-care bits compared unequal");
        descriptor[8 + 6] ^= 0x100u;
        Require(!SameDescriptorIgnoringTsharpBits(sampled, sampled.guestDescriptor, descriptor), "a descriptor difference outside the don't-care bits compared equal");
        auto storage = sampled;
        storage.kind = ShaderRecompiler::DescriptorKind::StorageImage;
        descriptor[8 + 6] ^= 0x100u;
        Require(!SameDescriptorIgnoringTsharpBits(storage, storage.guestDescriptor, descriptor), "a storage image's descriptor was compared through the T# mask");
    }
}

// The buffer base slots of a variant (BufferBaseWords): a read-only guest-buffer V# whose base
// (word 0, the low half of word 1) is located once among the words, consecutive in address and
// read by the walk, yields its base words' slots (word 1 masked to the base bits), whatever the
// shader made of its other words; a V# the walk did not read, one whose base is split over
// non-adjacent runs, one with a base word at a data position, a written element's and one located
// twice yield none.
void bufferBaseWordsTests() {
    using AgcDriver::BufferBaseWords;
    using AgcDriver::PatchMaskAt;
    using AgcDriver::WordPatchSlot;
    using AgcDriver::VsharpWord1BaseBits;
    using Slots = std::vector<WordPatchSlot>;
    using Reads = std::vector<std::uint64_t>;
    using Runs = std::vector<std::pair<std::uint64_t, std::uint64_t>>;
    const std::vector<std::uint32_t> first{0x4ee7fc40, 0x00040002, 0x10, 0x16204};
    const std::vector<std::uint32_t> second{0x5ee7fc40, 0x00040002, 0x20, 0x16204};
    ShaderRecompiler::DescriptorBinding buffers;
    buffers.kind = ShaderRecompiler::DescriptorKind::StorageBuffer;
    buffers.role = ShaderRecompiler::DescriptorRole::GuestBuffers;
    buffers.count = 2;
    buffers.guestDescriptor = first;
    buffers.guestDescriptor.insert(buffers.guestDescriptor.end(), second.begin(), second.end());
    buffers.bufferWritten = {false, false};
    // Words: two fillers, the first V# (positions 2-5), two fillers, the second V# (8-11).
    std::vector<std::uint32_t> words{0xa, 0xb};
    words.insert(words.end(), first.begin(), first.end());
    words.insert(words.end(), {0xc, 0xd});
    words.insert(words.end(), second.begin(), second.end());
    const Runs runs{{0x1000, 0x1030}};
    const auto same = [](const Slots& slots, const Slots& expected) {
        if (slots.size() != expected.size()) return false;
        for (std::size_t i = 0; i < slots.size(); ++i) {
            if (slots[i].position != expected[i].position || slots[i].binding != expected[i].binding || slots[i].word != expected[i].word || slots[i].mask != expected[i].mask) return false;
        }
        return true;
    };
    Slots slots;
    {
        const Reads reads{0x1008, 0x1020};
        const auto counts = BufferBaseWords(runs, words, std::span(&buffers, 1), {}, reads, slots);
        Require(counts.located == 2 && counts.written + counts.unlocated + counts.unread + counts.ambiguous + counts.data == 0 && same(slots, {{2, 0, 0, 0xffffffffu}, {3, 0, 1, VsharpWord1BaseBits}, {8, 0, 4, 0xffffffffu}, {9, 0, 5, VsharpWord1BaseBits}}), "the V#s among the words were not located");
        Require(PatchMaskAt(slots, 3) == VsharpWord1BaseBits && PatchMaskAt(slots, 8) == 0xffffffffu && PatchMaskAt(slots, 4) == 0, "the slot mask lookup is wrong");
    }
    {
        // Only a V# the walk read counts; a data position at a base word and a written element skip.
        const Reads reads{0x1008};
        auto counts = BufferBaseWords(runs, words, std::span(&buffers, 1), {}, reads, slots);
        Require(counts.located == 1 && counts.unread == 1 && slots.size() == 2 && slots[0].position == 2, "a V# the walk did not read was located");
        const std::vector<std::uint32_t> dataPositions{3};
        counts = BufferBaseWords(runs, words, std::span(&buffers, 1), dataPositions, reads, slots);
        Require(counts.located == 0 && counts.data == 1 && slots.empty(), "a V# with a base word at a data position was kept");
        const Reads none;
        counts = BufferBaseWords(runs, words, std::span(&buffers, 1), {}, none, slots);
        Require(counts.located == 0 && counts.unread == 2 && slots.empty(), "V#s were located without a read trace");
        auto written = buffers;
        written.bufferWritten = {true, false};
        const Reads both{0x1008, 0x1020};
        counts = BufferBaseWords(runs, words, std::span(&written, 1), {}, both, slots);
        Require(counts.located == 1 && counts.written == 1 && slots.size() == 2 && slots[0].position == 8, "a written element's V# was located");
        written.bufferWritten.clear();
        Require(BufferBaseWords(runs, words, std::span(&written, 1), {}, both, slots).written == 2, "an element beyond bufferWritten was treated as read-only");
        // The pair in the flat SRT too: slots into it as well; twice there: the element is skipped.
        std::array<ShaderRecompiler::DescriptorBinding, 2> withFlat{buffers, buffers};
        withFlat[1].role = ShaderRecompiler::DescriptorRole::FlattenedSrt;
        withFlat[1].guestDescriptor = {0x1, 0x2, 0x5ee7fc40, 0x00040002, 0x3, 0x4ee7fc40, 0x00040002, 0x4};
        counts = BufferBaseWords(runs, words, withFlat, {}, both, slots);
        Require(counts.located == 2 && same(slots, {{2, 0, 0, 0xffffffffu}, {2, 1, 5, 0xffffffffu}, {3, 0, 1, VsharpWord1BaseBits}, {3, 1, 6, VsharpWord1BaseBits}, {8, 0, 4, 0xffffffffu}, {8, 1, 2, 0xffffffffu}, {9, 0, 5, VsharpWord1BaseBits}, {9, 1, 3, VsharpWord1BaseBits}}), "the flat SRT copies of the bases got no slots");
        withFlat[1].guestDescriptor = {0x4ee7fc40, 0x00040002, 0x5ee7fc40, 0x00040002, 0x3, 0x4ee7fc40, 0x00040002, 0x4};
        counts = BufferBaseWords(runs, words, withFlat, {}, both, slots);
        Require(counts.located == 1 && counts.ambiguous == 1 && slots.size() == 4 && slots[0].position == 8, "a base the flat SRT holds twice was kept");
        // The shader patched the stride (word 1 high half), the record count and word 3: the base
        // still locates; a base that differs does not.
        auto patched = buffers;
        patched.guestDescriptor = {0x4ee7fc40, 0x00080002, 0x40, 0x12345, 0x5ee7fc44, 0x00040002, 0x20, 0x16204};
        counts = BufferBaseWords(runs, words, std::span(&patched, 1), {}, both, slots);
        Require(counts.located == 1 && counts.unlocated == 1 && slots.size() == 2 && slots[0].position == 2 && slots[1].mask == VsharpWord1BaseBits, "a V# with shader-patched words 1-3 was not located by its base, or a moved base was");
    }
    {
        // Split over non-adjacent runs: not located; over adjacent runs: located; twice among the
        // words: ambiguous, skipped.
        const Runs split{{0x1000, 0x100c}, {0x2000, 0x2024}};
        const Reads reads{0x1008, 0x2014};
        const auto counts = BufferBaseWords(split, words, std::span(&buffers, 1), {}, reads, slots);
        Require(counts.located == 1 && counts.unlocated == 1 && slots.size() == 2 && slots[0].position == 8, "a V# whose base is split over non-adjacent runs was located");
        const Runs adjacent{{0x1000, 0x100c}, {0x100c, 0x1030}};
        const Reads both{0x1008, 0x1020};
        Require(BufferBaseWords(adjacent, words, std::span(&buffers, 1), {}, both, slots).located == 2, "a V# over adjacent runs was not located");
        auto twice = words;
        twice.insert(twice.end(), first.begin(), first.end());
        const Runs longer{{0x1000, 0x1040}};
        const Reads all{0x1008, 0x1020, 0x1030};
        const auto ambiguous = BufferBaseWords(longer, twice, std::span(&buffers, 1), {}, all, slots);
        Require(ambiguous.located == 1 && ambiguous.ambiguous == 1 && slots.size() == 2 && slots[0].position == 8, "a V# located twice was kept");
    }
}

// A template's data buffers refreshed by words from a patched compiled result (a data-only hit)
// and back: DataWordsHash() follows the buffers exactly, so a later recipe hit's hash compare
// (RecordedDispatch::DataRefresh::Hash) decides correctly in both directions.
void dataRefreshTests(const Device& device, Recorder& recorder) {
    const auto& context = device.GetContext();
    ShaderRecompiler::RecompileResult program;
    ShaderRecompiler::DescriptorBinding binding;
    binding.kind = ShaderRecompiler::DescriptorKind::StorageBuffer;
    binding.role = ShaderRecompiler::DescriptorRole::FlattenedSrt;
    binding.descriptorSet = 0;
    binding.binding = 0;
    binding.count = 1;
    binding.guestDescriptor = {1, 2, 3, 4};
    program.bindings.push_back(binding);
    auto patched = program;
    patched.bindings[0].guestDescriptor[2] = 0x33;
    const CompiledShader original{ShaderRecompiler::ShaderStage::Compute, &program, 0};
    const CompiledShader live{ShaderRecompiler::ShaderStage::Compute, &patched, 0};
    ShaderResources resources(context, original);
    Require(resources.DataWordsHash() == ShaderResources::DataWordsHash(original) && resources.DataWordsHash() != ShaderResources::DataWordsHash(live), "a fresh template's data hash is not its words'");
    Require(!resources.DataWordsDiffer(original) && resources.DataWordsDiffer(live), "the per-word compare disagrees with the words");
    Require(resources.RefreshData(recorder.Commands(), live, &recorder), "a refresh with different words recorded nothing");
    Require(resources.DataWordsHash() == ShaderResources::DataWordsHash(live) && !resources.DataWordsDiffer(live), "the refreshed template's hash is not the patched words'");
    Require(resources.RefreshData(recorder.Commands(), original, &recorder), "the refresh back recorded nothing");
    Require(resources.DataWordsHash() == ShaderResources::DataWordsHash(original), "the refreshed template's hash is not the original words'");
    Require(!resources.RefreshData(recorder.Commands(), original, &recorder), "a refresh with equal words recorded");
    recorder.Submit();
    device.WaitQueue();
    recorder.Sync();
}

}

class SampleProgram {
public:
    SampleProgram(const Context& context, Recorder& recorder) : context(context), recorder(recorder), result(context, sizeof(float) * 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT) {
        VkDescriptorSetLayoutBinding bindings[2]{};
        bindings[0] = {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
        bindings[1] = {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
        VkDescriptorSetLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        layoutInfo.bindingCount = 2;
        layoutInfo.pBindings = bindings;
        Check(context.Function<PFN_vkCreateDescriptorSetLayout>("vkCreateDescriptorSetLayout")(context.device, &layoutInfo, nullptr, &setLayout), "vkCreateDescriptorSetLayout");
        const VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(float)};
        VkPipelineLayoutCreateInfo pipelineLayoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pipelineLayoutInfo.setLayoutCount = 1;
        pipelineLayoutInfo.pSetLayouts = &setLayout;
        pipelineLayoutInfo.pushConstantRangeCount = 1;
        pipelineLayoutInfo.pPushConstantRanges = &push;
        Check(context.Function<PFN_vkCreatePipelineLayout>("vkCreatePipelineLayout")(context.device, &pipelineLayoutInfo, nullptr, &pipelineLayout), "vkCreatePipelineLayout");
        VkShaderModuleCreateInfo moduleInfo{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        moduleInfo.codeSize = sizeof(SAMPLE_LOD_SPV);
        moduleInfo.pCode = SAMPLE_LOD_SPV;
        Check(context.Function<PFN_vkCreateShaderModule>("vkCreateShaderModule")(context.device, &moduleInfo, nullptr, &module), "vkCreateShaderModule");
        VkComputePipelineCreateInfo pipelineInfo{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        pipelineInfo.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, module, "main", nullptr};
        pipelineInfo.layout = pipelineLayout;
        Check(context.Function<PFN_vkCreateComputePipelines>("vkCreateComputePipelines")(context.device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &pipeline), "vkCreateComputePipelines");
        VkSamplerCreateInfo samplerInfo{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        samplerInfo.magFilter = VK_FILTER_NEAREST;
        samplerInfo.minFilter = VK_FILTER_NEAREST;
        samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        samplerInfo.addressModeU = samplerInfo.addressModeV = samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.maxLod = VK_LOD_CLAMP_NONE;
        Check(context.Function<PFN_vkCreateSampler>("vkCreateSampler")(context.device, &samplerInfo, nullptr, &sampler), "vkCreateSampler");
        const VkDescriptorPoolSize sizes[2]{{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1}, {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1}};
        VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
        poolInfo.maxSets = 1;
        poolInfo.poolSizeCount = 2;
        poolInfo.pPoolSizes = sizes;
        Check(context.Function<PFN_vkCreateDescriptorPool>("vkCreateDescriptorPool")(context.device, &poolInfo, nullptr, &pool), "vkCreateDescriptorPool");
    }
    ~SampleProgram() {
        context.Function<PFN_vkDestroyDescriptorPool>("vkDestroyDescriptorPool")(context.device, pool, nullptr);
        context.Function<PFN_vkDestroySampler>("vkDestroySampler")(context.device, sampler, nullptr);
        context.Function<PFN_vkDestroyPipeline>("vkDestroyPipeline")(context.device, pipeline, nullptr);
        context.Function<PFN_vkDestroyShaderModule>("vkDestroyShaderModule")(context.device, module, nullptr);
        context.Function<PFN_vkDestroyPipelineLayout>("vkDestroyPipelineLayout")(context.device, pipelineLayout, nullptr);
        context.Function<PFN_vkDestroyDescriptorSetLayout>("vkDestroyDescriptorSetLayout")(context.device, setLayout, nullptr);
    }
    SampleProgram(const SampleProgram&) = delete;
    SampleProgram& operator=(const SampleProgram&) = delete;

    float Red(VkImageView view, VkImageLayout layout, float lod) {
        VkDescriptorSetAllocateInfo allocateInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        allocateInfo.descriptorPool = pool;
        allocateInfo.descriptorSetCount = 1;
        allocateInfo.pSetLayouts = &setLayout;
        VkDescriptorSet set = VK_NULL_HANDLE;
        Check(context.Function<PFN_vkAllocateDescriptorSets>("vkAllocateDescriptorSets")(context.device, &allocateInfo, &set), "vkAllocateDescriptorSets");
        const VkDescriptorImageInfo image{sampler, view, layout};
        const VkDescriptorBufferInfo buffer{result.Handle(), 0, VK_WHOLE_SIZE};
        VkWriteDescriptorSet writes[2]{{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}, {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}};
        writes[0].dstSet = set;
        writes[0].dstBinding = 0;
        writes[0].descriptorCount = 1;
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[0].pImageInfo = &image;
        writes[1].dstSet = set;
        writes[1].dstBinding = 1;
        writes[1].descriptorCount = 1;
        writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[1].pBufferInfo = &buffer;
        context.Function<PFN_vkUpdateDescriptorSets>("vkUpdateDescriptorSets")(context.device, 2, writes, 0, nullptr);
        const auto commands = recorder.Commands();
        RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
        context.Function<PFN_vkCmdBindPipeline>("vkCmdBindPipeline")(commands, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
        context.Function<PFN_vkCmdBindDescriptorSets>("vkCmdBindDescriptorSets")(commands, VK_PIPELINE_BIND_POINT_COMPUTE, pipelineLayout, 0, 1, &set, 0, nullptr);
        context.Function<PFN_vkCmdPushConstants>("vkCmdPushConstants")(commands, pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(lod), &lod);
        context.Function<PFN_vkCmdDispatch>("vkCmdDispatch")(commands, 1, 1, 1);
        RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT);
        recorder.Submit();
        recorder.Sync();
        Check(context.Function<PFN_vkFreeDescriptorSets>("vkFreeDescriptorSets")(context.device, pool, 1, &set), "vkFreeDescriptorSets");
        float texel[4];
        std::memcpy(texel, result.Bytes().data(), sizeof(texel));
        return texel[0];
    }

private:
    const Context& context;
    Recorder& recorder;
    Buffer result;
    VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
    VkShaderModule module = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkSampler sampler = VK_NULL_HANDLE;
    VkDescriptorPool pool = VK_NULL_HANDLE;
};

void expectRed(float got, float want, const char* what) {
    if (std::abs(got - want) > 1.5f / 255.0f) throw std::runtime_error(std::string(what) + ": read " + std::to_string(got) + ", expected " + std::to_string(want));
}

void minLodTests(const Device& device, Recorder& recorder) {
    const auto& context = device.GetContext();
    if (!context.imageViewMinLod) {
        std::cout << "VK_EXT_image_view_min_lod unavailable: minimum LOD clamp not tested\n";
        return;
    }
    TextureDetiler detiler(context);
    GuestTextureResource resource{};
    resource.baseAddress = 0x100000;
    resource.width = 8;
    resource.height = 8;
    resource.mipCount = 4;
    resource.baseLevel = 0;
    resource.lastLevel = 3;
    resource.tileMode = TextureTileMode::kLinear;
    resource.dimension = TextureDimension::k2D;
    resource.format = 56;
    resource.dstSelX = 4;
    resource.dstSelY = 5;
    resource.dstSelZ = 6;
    resource.dstSelW = 7;
    const auto geometry = DescribeSurface(resource);
    Require(geometry.mips.size() == 4, "the min LOD test surface has an unexpected mip count");
    const std::array<std::uint8_t, 4> levels{0x00, 0x40, 0x80, 0xff};
    std::vector<std::byte> snapshot(static_cast<std::size_t>(geometry.guestBytes));
    for (std::size_t level = 0; level < levels.size(); ++level) {
        const auto& mip = geometry.mips[level];
        std::fill_n(snapshot.begin() + static_cast<std::ptrdiff_t>(mip.tiledOffset), static_cast<std::size_t>(mip.tiledSize), std::byte{levels[level]});
    }
    SampleProgram program(context, recorder);
    const VkComponentMapping identity{VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY};
    const auto sample = [&](std::uint32_t minLod, float lod, std::uint32_t baseLevel = 0) {
        auto described = resource;
        described.minLod = minLod;
        described.baseLevel = baseLevel;
        Texture texture(context, detiler, described, identity, snapshot);
        return program.Red(texture.View(), texture.Layout(), lod);
    };
    const auto unorm = [&](std::size_t level) { return levels[level] / 255.0f; };
    expectRed(sample(0, 0.0f), unorm(0), "minimum LOD clamp: no clamp reads level 0");
    expectRed(sample(0x100, 0.0f), unorm(1), "minimum LOD clamp: MIN_LOD 1 reads level 1 at LOD 0");
    expectRed(sample(0x180, 0.0f), (unorm(1) + unorm(2)) / 2.0f, "minimum LOD clamp: MIN_LOD 1.5 blends levels 1 and 2");
    expectRed(sample(0x100, 2.0f), unorm(2), "minimum LOD clamp: MIN_LOD 1 lowered LOD 2");
    expectRed(sample(0xfff, 0.0f), unorm(3), "minimum LOD clamp: MIN_LOD past the last level reads the last level");
    expectRed(sample(0x200, 0.0f, 1), unorm(2), "minimum LOD clamp: MIN_LOD 2 over a view from level 1 reads level 2");
    expectRed(sample(0x100, 0.0f, 1), unorm(1), "minimum LOD clamp: MIN_LOD at the view's base level reads its base level");
}

void firstLayerViewTests(const Device& device, Recorder& recorder) {
    const auto& context = device.GetContext();
    TextureDetiler detiler(context);
    auto withDetiler = context;
    withDetiler.detiler = &detiler;
    GuestTextureResource resource{};
    resource.width = 64;
    resource.height = 4;
    resource.depthOrLastArray = 2;
    resource.baseArray = 1;
    resource.mipCount = 1;
    resource.tileMode = TextureTileMode::kLinear;
    resource.dimension = TextureDimension::k2DArray;
    resource.format = 56;
    resource.dstSelX = 4;
    resource.dstSelY = 5;
    resource.dstSelZ = 6;
    resource.dstSelW = 7;
    const auto geometry = DescribeSurface(resource);
    std::vector<std::uint8_t> memory(static_cast<std::size_t>(geometry.guestBytes) + 256);
    auto* surface = reinterpret_cast<std::uint8_t*>((reinterpret_cast<std::uintptr_t>(memory.data()) + 255) & ~std::uintptr_t{255});
    for (std::uint32_t layer = 0; layer < 3; ++layer) std::memset(surface + geometry.GuestLayerOffset(layer), 0x20 * (layer + 1), static_cast<std::size_t>(geometry.layerBytes));
    resource.baseAddress = reinterpret_cast<std::uint64_t>(surface);
    auto image = std::make_shared<StorageTexture>(withDetiler, detiler, resource, 0);
    recorder.Keep(image);
    SampleProgram program(context, recorder);
    expectRed(program.Red(image->FirstLayerView(0), VK_IMAGE_LAYOUT_GENERAL, 0.0f), 0x40 / 255.0f, "a first-layer view does not read the BASE_ARRAY layer");
    Require(image->FirstLayerView(0) == image->FirstLayerView(0), "first-layer views are not reused");
    const VkComponentMapping identity{VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY};
    Texture snapshotTexture(context, detiler, resource, identity, std::span<const std::byte>(reinterpret_cast<const std::byte*>(surface), static_cast<std::size_t>(geometry.guestBytes)));
    Require(snapshotTexture.FirstLayerView() != VK_NULL_HANDLE, "a sampled 2D array texture has no first-layer view");
    expectRed(program.Red(snapshotTexture.FirstLayerView(), snapshotTexture.Layout(), 0.0f), 0x40 / 255.0f, "a sampled first-layer view does not read the BASE_ARRAY layer");
    Texture storageView(context, image, resource, identity);
    Require(storageView.FirstLayerView() != VK_NULL_HANDLE, "a sampled view of a 2D array storage image has no first-layer view");
    expectRed(program.Red(storageView.FirstLayerView(), storageView.Layout(), 0.0f), 0x40 / 255.0f, "a sampled first-layer view of a storage image does not read the BASE_ARRAY layer");
    auto flat = resource;
    flat.dimension = TextureDimension::k2D;
    flat.depthOrLastArray = 0;
    flat.baseArray = 0;
    Texture flatTexture(context, detiler, flat, identity, std::span<const std::byte>(reinterpret_cast<const std::byte*>(surface), static_cast<std::size_t>(DescribeSurface(flat).guestBytes)));
    Require(flatTexture.FirstLayerView() == VK_NULL_HANDLE, "a 2D texture has a first-layer view");
    expectRed(program.Red(flatTexture.View(), flatTexture.Layout(), 0.0f), 0x20 / 255.0f, "a 2D texture over the surface does not read its first layer");
}

void keysFillTests(const Device& device, Recorder& recorder) {
    const auto& base = device.GetContext();
    if (base.hostImportAlignment == 0) {
        std::cout << "host imports unavailable: DCC key fills not tested\n";
        return;
    }
    constexpr std::uint32_t side = 256;
    constexpr std::size_t surfaceBytes = side * side * 4;
    constexpr std::size_t keyCount = surfaceBytes / 256;
    constexpr std::size_t bytes = surfaceBytes + 65536;
#ifdef _WIN32
    void* block = VirtualAlloc(nullptr, bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
#else
    void* block = std::aligned_alloc(65536, bytes);
#endif
    Require(block != nullptr, "cannot allocate the key fill block");
    auto* texels = static_cast<std::uint8_t*>(block);
    auto* keys = texels + surfaceBytes;
    std::memset(texels, 0x55, surfaceBytes);
    std::memset(keys, 0x00, keyCount);
    const auto address = reinterpret_cast<std::uint64_t>(block);
    const auto keysAddress = address + surfaceBytes;
    {
        GuestAllocations::Mutation mutation;
        mutation.Add(block, bytes, true, true);
    }
    struct Unregister {
        const Context& context;
        void* block;
        std::uint64_t address;
        ~Unregister() {
            {
                GuestAllocations::Mutation mutation;
                mutation.Remove(block);
            }
            HostImportFor(context, address, bytes);
        }
    } unregister{base, block, address};
    if (HostImportFor(base, address, bytes) == nullptr) {
        std::cout << "host import of the key fill block refused: DCC key fills not tested\n";
        return;
    }
    TextureDetiler detiler(base);
    auto context = base;
    context.detiler = &detiler;
    GuestTextureResource resource{};
    resource.baseAddress = address;
    resource.width = side;
    resource.height = side;
    resource.mipCount = 1;
    resource.tileMode = TextureTileMode::kR64KBX;
    resource.dimension = TextureDimension::k2D;
    resource.format = 56;
    resource.dstSelX = 4;
    resource.dstSelY = 5;
    resource.dstSelZ = 6;
    resource.dstSelW = 7;
    resource.dccAddress = keysAddress;
    Require(DescribeSurface(resource).guestBytes == surfaceBytes, "the key fill surface has an unexpected size");
    {
        auto image = std::make_shared<StorageTexture>(context, detiler, resource, 0);
        const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        const auto draw = [&](VkClearColorValue value) {
            const auto commands = recorder.Commands();
            recorder.Keep(image);
            RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
            context.Function<PFN_vkCmdClearColorImage>("vkCmdClearColorImage")(commands, image->Image(), VK_IMAGE_LAYOUT_GENERAL, &value, 1, &range);
            RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT);
            image->MarkDirty();
        };
        const auto holds = [&](std::array<std::uint8_t, 4> texel) {
            Buffer readback(context, surfaceBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
            const auto commands = recorder.Commands();
            RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
            VkBufferImageCopy copy{};
            copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            copy.imageExtent = {side, side, 1};
            context.Function<PFN_vkCmdCopyImageToBuffer>("vkCmdCopyImageToBuffer")(commands, image->Image(), VK_IMAGE_LAYOUT_GENERAL, readback.Handle(), 1, &copy);
            RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT);
            recorder.Submit();
            device.WaitQueue();
            recorder.Sync();
            const auto pixels = readback.Bytes();
            for (std::size_t i = 0; i < pixels.size(); ++i) {
                if (std::to_integer<std::uint8_t>(pixels[i]) != texel[i % 4]) return false;
            }
            return true;
        };
        Require(holds({0, 0, 0, 0}), "a surface under 0000 keys was not cleared");
        draw({{1.0f, 0.0f, 0.0f, 1.0f}});
        Require(StorageTexture::NoteKeysFill(keysAddress, keyCount, 0x00) == 1, "a 0000 key fill did not cover the surface");
        Require(image->FilledKeys() == DccKeys::Clear0000, "a key fill over pending results was not recorded");
        image->Refresh();
        Require(holds({0, 0, 0, 0}), "a key fill did not clear the results made before it at the next refresh");
        Require(image->FilledKeys() == DccKeys::Uncompressed, "the image cleared by a refresh still holds the fill");
        draw({{0.0f, 0.0f, 1.0f, 1.0f}});
        Require(StorageTexture::NoteKeysFill(keysAddress, keyCount, 0x00) == 1, "a second 0000 key fill did not cover the surface");
#ifdef _WIN32
        _putenv_s("APS5_KEYS_FILL_CLEAR", "1");
#else
        setenv("APS5_KEYS_FILL_CLEAR", "1", 1);
#endif
        Require(StorageTexture::ClearByKeysFill(keysAddress, keyCount, 0x00) == 1, "a 0000 key fill did not clear the surface at once");
        Require(holds({0, 0, 0, 0}), "a key fill cleared at once left results made before it");
        draw({{0.0f, 1.0f, 0.0f, 1.0f}});
        Require(image->FilledKeys() == DccKeys::Uncompressed, "results drawn after a key fill cleared at once are held under the fill's code");
        Require(holds({0, 255, 0, 255}), "results drawn after a key fill cleared at once were lost");
    }
    recorder.Sync();
}

// Copy-back coalescing (APS5_COALESCE_COPY_BACKS=1, read by a recorder made with it set): a later
// deferred copy drops the bytes it stores again from the queued ones (splitting one around it, its
// offsets moved alike); queued copies stay past CommandsKeepingCopyBacks, a flush over other bytes
// and their own claimant's flush, and land at an overlapping flush, at Commands() and at Submit,
// each byte from its last copy.
void coalesceCopyBackTests(const Device& device, Recorder& recorder) {
    const auto& context = device.GetContext();
    if (context.hostImportAlignment == 0) {
        std::cout << "host imports unavailable: copy-back coalescing not tested\n";
        return;
    }
    constexpr std::size_t bytes = 65536;
#ifdef _WIN32
    void* block = VirtualAlloc(nullptr, bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
#else
    void* block = std::aligned_alloc(65536, bytes);
#endif
    Require(block != nullptr, "cannot allocate the copy-back test block");
    std::memset(block, 0, bytes);
    const auto address = reinterpret_cast<std::uint64_t>(block);
    {
        GuestAllocations::Mutation mutation;
        mutation.Add(block, bytes, true, true);
    }
    const auto* import = HostImportFor(context, address, bytes);
    if (import == nullptr) {
        std::cout << "host import of the copy-back test block refused: copy-back coalescing not tested\n";
        return;
    }
    {
        _putenv_s("APS5_COALESCE_COPY_BACKS", "1");
        Recorder coalescing(context);
        _putenv_s("APS5_COALESCE_COPY_BACKS", "");
        Require(coalescing.CoalescesCopyBacks() && coalescing.DefersCopyBacks(), "APS5_COALESCE_COPY_BACKS=1 did not turn coalescing on");
        coalescing.Activate();
        constexpr std::size_t sourceBytes = 8192;
        auto first = std::make_shared<Buffer>(context, sourceBytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
        auto second = std::make_shared<Buffer>(context, sourceBytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
        const auto firstByte = [](std::size_t at) { return static_cast<unsigned char>(at * 7 + 1); };
        const auto secondByte = [](std::size_t at) { return static_cast<unsigned char>(at * 13 + 5); };
        for (std::size_t at = 0; at < sourceBytes; ++at) {
            first->Bytes()[at] = std::byte{firstByte(at)};
            second->Bytes()[at] = std::byte{secondByte(at)};
        }
        // The shadow's byte `at` goes to the block's byte `at`, as a staged region's copy-back.
        const auto copyOf = [&](const std::shared_ptr<Buffer>& source, std::uint64_t at, std::uint64_t count) {
            return std::vector<Recorder::DeferredCopy>{Recorder::DeferredCopy{source, source.get(), source->Handle(), import->buffer, at, address + at - import->base, count, address + at}};
        };
        using Reason = Recorder::FlushReason;
        const auto before = Recorder::CopyBackCounts();
        coalescing.DeferCopies(copyOf(first, 0, 1024));
        coalescing.DeferCopies(copyOf(second, 512, 1024));
        Require(coalescing.DeferredCopyBytes() == 1536, "a later copy did not drop the bytes it stores again");
        coalescing.DeferCopies(copyOf(first, 768, 256));
        Require(coalescing.DeferredCopyBytes() == 1536, "a copy inside a queued one did not split it");
        auto counts = Recorder::CopyBackCounts();
        Require(counts.deferred - before.deferred == 3 && counts.overwrittenBytes - before.overwrittenBytes == 768 && counts.overwritten == before.overwritten, "coalescing counters are off");
        // Queued: first [0, 512), second [512, 768), first [768, 1024), second [1024, 1536).
        static_cast<void>(coalescing.CommandsKeepingCopyBacks());
        coalescing.FlushDeferredOverlapping(address + 8192, 64, nullptr, Reason::CopyIn);
        Require(coalescing.HasDeferredCopies() && coalescing.DeferredCopyBytes() == 1536, "copies were recorded by a command or flush that does not touch them");
        coalescing.FlushDeferredOverlapping(address + 100, 4, nullptr, Reason::CopyIn);
        Require(coalescing.DeferredCopyBytes() == 1024, "an overlapping flush did not record exactly the copy over its bytes");
        coalescing.ClaimDeferredCopies(second.get());
        coalescing.FlushDeferredOverlapping(address + 600, 4, second.get(), Reason::CopyIn);
        Require(coalescing.DeferredCopyBytes() == 1024, "a build's own claimed copy was recorded before its copy-back");
        coalescing.FlushDeferredOverlapping(address + 600, 4, nullptr, Reason::CopyIn);
        Require(coalescing.DeferredCopyBytes() == 768, "a claimed copy over another range's copy-in was not recorded");
        coalescing.ReleaseClaims();
        static_cast<void>(coalescing.Commands());
        Require(!coalescing.HasDeferredCopies(), "Commands() left queued copies");
        coalescing.DeferCopies(copyOf(second, 4096, 256));
        coalescing.Submit();
        Require(!coalescing.HasDeferredCopies(), "Submit left queued copies");
        coalescing.Sync();
        counts = Recorder::CopyBackCounts();
        const auto flushes = [&](Reason reason) { return counts.flushes[static_cast<std::size_t>(reason)] - before.flushes[static_cast<std::size_t>(reason)]; };
        Require(counts.passes - before.passes == 4 && flushes(Reason::CopyIn) == 2 && flushes(Reason::Command) == 1 && flushes(Reason::Submit) == 1 && counts.recorded - before.recorded == 5 && counts.recordedBytes - before.recordedBytes == 1536 + 256, "copy-back pass counters are off");
        const auto* landed = static_cast<const unsigned char*>(block);
        for (std::size_t at = 0; at < 8192; ++at) {
            const bool fromFirst = at < 512 || (at >= 768 && at < 1024);
            const bool fromSecond = (at >= 512 && at < 768) || (at >= 1024 && at < 1536) || (at >= 4096 && at < 4352);
            const unsigned char want = fromFirst ? firstByte(at) : fromSecond ? secondByte(at) : 0;
            if (landed[at] != want) throw std::runtime_error("a coalesced copy-back stored the wrong byte at offset " + std::to_string(at));
        }
    }
    recorder.Activate();
    {
        GuestAllocations::Mutation mutation;
        mutation.Remove(block);
    }
    HostImportFor(context, address, bytes);
}

// Sets (or, with an empty value, clears) a switch a recorder or guard reads when it is made.
void setSwitch(const char* name, const char* value) {
#ifdef _WIN32
    _putenv_s(name, value);
#else
    if (*value != '\0') setenv(name, value, 1);
    else unsetenv(name);
#endif
}

// Narrow copy-backs (APS5_NARROW_COPY_BACKS=1, here with coalescing): a queued copy with a baseline
// (Recorder::DeferredCopy::baselineOffset) stores into the import only the dwords whose source
// differs from the baseline, and sets the baseline to them; the partial dwords at its ends are
// stored whole and mirrored into the baseline. Adjacent queued copies land in the one pass that
// records them, each narrow; a later copy over part of a queued one leaves each byte to its last
// copy and moves the trimmed copy's baseline with its offsets (the dropped bytes' baseline stays
// as it was); a copy whose source and destination disagree modulo 4 falls back to a whole copy
// (mirrored), and one without a baseline (an untrusted shadow) copies its whole range as before.
void narrowCopyBackTests(const Device& device, Recorder& recorder) {
    const auto& context = device.GetContext();
    if (context.hostImportAlignment == 0 || !context.bufferDeviceAddress) {
        std::cout << "host imports or buffer device addresses unavailable: narrow copy-backs not tested\n";
        return;
    }
    // The defaults: coalescing and narrow copy-backs on; APS5_NO_<name>=1 or APS5_<name>=0 turns
    // each off, APS5_<name>=1 forces it on over the opt-out. The caller's switches are restored.
    {
        const auto saved = [](const char* name) {
            const char* value = std::getenv(name);
            return std::string(value != nullptr ? value : "");
        };
        const std::string coalesce = saved("APS5_COALESCE_COPY_BACKS"), narrow = saved("APS5_NARROW_COPY_BACKS");
        const std::string noCoalesce = saved("APS5_NO_COALESCE_COPY_BACKS"), noNarrow = saved("APS5_NO_NARROW_COPY_BACKS");
        const auto made = [&](const char* on, const char* off) {
            setSwitch("APS5_COALESCE_COPY_BACKS", on);
            setSwitch("APS5_NARROW_COPY_BACKS", on);
            setSwitch("APS5_NO_COALESCE_COPY_BACKS", off);
            setSwitch("APS5_NO_NARROW_COPY_BACKS", off);
            Recorder made(context);
            return std::pair{made.CoalescesCopyBacks(), made.NarrowsCopyBacks()};
        };
        Require(made("", "") == std::pair{true, true}, "coalescing and narrow copy-backs are not on by default");
        Require(made("", "1") == std::pair{false, false}, "APS5_NO_*_COPY_BACKS=1 did not turn them off");
        Require(made("0", "") == std::pair{false, false}, "APS5_*_COPY_BACKS=0 did not turn them off");
        Require(made("1", "1") == std::pair{true, true}, "APS5_*_COPY_BACKS=1 did not force them on");
        Require(made("", "0") == std::pair{true, true}, "APS5_NO_*_COPY_BACKS=0 turned them off");
        setSwitch("APS5_COALESCE_COPY_BACKS", coalesce.c_str());
        setSwitch("APS5_NARROW_COPY_BACKS", narrow.c_str());
        setSwitch("APS5_NO_COALESCE_COPY_BACKS", noCoalesce.c_str());
        setSwitch("APS5_NO_NARROW_COPY_BACKS", noNarrow.c_str());
    }
    constexpr std::size_t bytes = 65536;
    constexpr unsigned char sentinel = 0x5A;
#ifdef _WIN32
    void* block = VirtualAlloc(nullptr, bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
#else
    void* block = std::aligned_alloc(65536, bytes);
#endif
    Require(block != nullptr, "cannot allocate the narrow copy-back test block");
    std::memset(block, sentinel, bytes);
    const auto address = reinterpret_cast<std::uint64_t>(block);
    {
        GuestAllocations::Mutation mutation;
        mutation.Add(block, bytes, true, true);
    }
    const auto* import = HostImportFor(context, address, bytes);
    if (import == nullptr || import->address == 0) {
        std::cout << "host import of the narrow copy-back test block refused or not addressable: narrow copy-backs not tested\n";
        return;
    }
    {
        setSwitch("APS5_COALESCE_COPY_BACKS", "1");
        setSwitch("APS5_NARROW_COPY_BACKS", "1");
        Recorder narrowing(context);
        setSwitch("APS5_COALESCE_COPY_BACKS", "");
        setSwitch("APS5_NARROW_COPY_BACKS", "");
        Require(narrowing.NarrowsCopyBacks() && narrowing.CoalescesCopyBacks(), "APS5_NARROW_COPY_BACKS=1 did not turn narrow copy-backs on");
        narrowing.Activate();
        // Each source buffer: the shadow's bytes in [0, span), its baseline in [span, 2 * span).
        constexpr std::size_t span = 16384;
        const auto usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
        auto first = std::make_shared<Buffer>(context, 2 * span, usage);
        auto second = std::make_shared<Buffer>(context, 2 * span, usage);
        const auto shadowByte = [](unsigned seed, std::size_t at) { return static_cast<unsigned char>(at * 7 + seed); };
        // Every third dword differs from its baseline: the shader "changed" it.
        const auto changed = [](std::size_t at) { return (at / 4) % 3 == 0; };
        for (std::size_t at = 0; at < span; ++at) {
            first->Bytes()[at] = std::byte{shadowByte(1, at)};
            first->Bytes()[span + at] = std::byte{changed(at) ? static_cast<unsigned char>(~shadowByte(1, at)) : shadowByte(1, at)};
            second->Bytes()[at] = std::byte{shadowByte(5, at)};
            second->Bytes()[span + at] = std::byte{changed(at) ? static_cast<unsigned char>(~shadowByte(5, at)) : shadowByte(5, at)};
        }
        // The shadow's byte `source` goes to the block's byte `guest`.
        const auto copyOf = [&](const std::shared_ptr<Buffer>& buffer, std::uint64_t source, std::uint64_t guest, std::uint64_t count, bool narrow) {
            Recorder::DeferredCopy copy{buffer, buffer.get(), buffer->Handle(), import->buffer, source, address + guest - import->base, count, address + guest};
            if (narrow) {
                copy.sourceAddress = buffer->DeviceAddress();
                copy.destinationAddress = import->address;
                copy.baselineOffset = span;
            }
            return std::vector<Recorder::DeferredCopy>{copy};
        };
        const auto before = Recorder::CopyBackCounts();
        // Aligned: only the changed dwords.
        narrowing.DeferCopies(copyOf(first, 0, 0, 4096, true));
        // Unaligned at both ends: three bytes whole at each.
        narrowing.DeferCopies(copyOf(first, 4097, 4097, 1034, true));
        // Adjacent copies of two shadows.
        narrowing.DeferCopies(copyOf(first, 8192, 8192, 1024, true));
        narrowing.DeferCopies(copyOf(second, 9216, 9216, 1024, true));
        // A later copy over the second half of a queued one.
        narrowing.DeferCopies(copyOf(first, 12288, 12288, 1024, true));
        narrowing.DeferCopies(copyOf(second, 12800, 12800, 1024, true));
        Require(narrowing.DeferredCopyBytes() == 4096 + 1034 + 2048 + 512 + 1024, "a narrow copy over a queued one did not trim it");
        // Source and destination a byte apart modulo 4: whole, mirrored.
        narrowing.DeferCopies(copyOf(first, 14337, 14336, 512, true));
        // No baseline: whole, as before.
        narrowing.DeferCopies(copyOf(second, 15360, 15360, 1024, false));
        static_cast<void>(narrowing.Commands());
        Require(!narrowing.HasDeferredCopies(), "Commands() left narrow copies queued");
        narrowing.Submit();
        narrowing.Sync();
        const auto counts = Recorder::CopyBackCounts();
        Require(counts.passes - before.passes == 1, "the queued narrow and plain copies were not recorded in one pass");
        // Seven copies with a baseline, none back to back with another of its shadow: seven spans,
        // six compared (one dispatch each), the misaligned one whole.
        Require(counts.narrow - before.narrow == 7 && counts.narrowSpans - before.narrowSpans == 7 && counts.narrowBytes - before.narrowBytes == 4096 + 1034 + 1024 + 1024 + 512 + 1024 && counts.narrowWhole - before.narrowWhole == 1 && counts.narrowDispatches - before.narrowDispatches == 6, "narrow copy-back counters are off");
        const auto* landed = static_cast<const unsigned char*>(block);
        const auto want = [&](std::size_t at) -> unsigned char {
            const auto narrow = [&](unsigned seed) { return changed(at) ? shadowByte(seed, at) : sentinel; };
            if (at < 4096) return narrow(1);
            if (at >= 4097 && at < 4100) return shadowByte(1, at);
            if (at >= 4100 && at < 5128) return narrow(1);
            if (at >= 5128 && at < 5131) return shadowByte(1, at);
            if (at >= 8192 && at < 9216) return narrow(1);
            if (at >= 9216 && at < 10240) return narrow(5);
            if (at >= 12288 && at < 12800) return narrow(1);
            if (at >= 12800 && at < 13824) return narrow(5);
            if (at >= 14336 && at < 14848) return shadowByte(1, at + 1);
            if (at >= 15360 && at < 16384) return shadowByte(5, at);
            return sentinel;
        };
        for (std::size_t at = 0; at < span + 4096; ++at) {
            if (landed[at] != want(at)) throw std::runtime_error("a narrow copy-back stored the wrong byte at offset " + std::to_string(at) + ": " + std::to_string(landed[at]) + ", expected " + std::to_string(want(at)));
        }
        // The baselines now stand for what the import holds where the copies went, and are left
        // alone elsewhere (the dropped part of the trimmed copy too).
        const auto baseline = [&](const std::shared_ptr<Buffer>& buffer, unsigned seed, std::size_t at) {
            const auto was = changed(at) ? static_cast<unsigned char>(~shadowByte(seed, at)) : shadowByte(seed, at);
            const bool firstUpdated = at < 4096 || (at >= 4097 && at < 5131) || (at >= 8192 && at < 9216) || (at >= 12288 && at < 12800) || (at >= 14337 && at < 14849);
            const bool secondUpdated = (at >= 9216 && at < 10240) || (at >= 12800 && at < 13824);
            const bool updated = seed == 1 ? firstUpdated : secondUpdated;
            return std::to_integer<unsigned char>(buffer->Bytes()[span + at]) == (updated ? shadowByte(seed, at) : was);
        };
        for (std::size_t at = 0; at < span; ++at) {
            if (!baseline(first, 1, at) || !baseline(second, 5, at)) throw std::runtime_error("a narrow copy-back left a wrong baseline byte at offset " + std::to_string(at));
        }
        // Written ranges back to back in one shadow and its import (two V#s over adjacent
        // sub-ranges, a use's range and the range of a copy it took over) join into one compare
        // span, recorded in one dispatch, whatever order they were queued in; ranges back to back
        // in the shadow but not in the import stay apart. Each byte still lands as its own copy
        // would land it.
        auto third = std::make_shared<Buffer>(context, 2 * span, usage);
        for (std::size_t at = 0; at < span; ++at) {
            third->Bytes()[at] = std::byte{shadowByte(9, at)};
            third->Bytes()[span + at] = std::byte{changed(at) ? static_cast<unsigned char>(~shadowByte(9, at)) : shadowByte(9, at)};
        }
        std::memset(block, sentinel, span);
        const auto joined = Recorder::CopyBackCounts();
        narrowing.DeferCopies(copyOf(third, 1024, 1024, 1024, true));
        narrowing.DeferCopies(copyOf(third, 0, 0, 1024, true));
        narrowing.DeferCopies(copyOf(third, 2048, 2048, 2048, true));
        // Back to back in the shadow, 4 KiB apart in the import.
        narrowing.DeferCopies(copyOf(third, 8192, 12288, 512, true));
        narrowing.DeferCopies(copyOf(third, 8704, 8704, 512, true));
        static_cast<void>(narrowing.Commands());
        narrowing.Submit();
        narrowing.Sync();
        const auto afterJoin = Recorder::CopyBackCounts();
        Require(afterJoin.narrow - joined.narrow == 5 && afterJoin.narrowSpans - joined.narrowSpans == 3 && afterJoin.narrowDispatches - joined.narrowDispatches == 3 && afterJoin.narrowWhole == joined.narrowWhole && afterJoin.narrowBytes - joined.narrowBytes == 4096 + 1024, "back-to-back narrow copies of one shadow did not join into one span");
        const auto wantJoined = [&](std::size_t at) -> unsigned char {
            const auto narrow = [&](std::size_t from) { return changed(from) ? shadowByte(9, from) : sentinel; };
            if (at < 4096) return narrow(at);
            if (at >= 12288 && at < 12800) return changed(at - 4096) ? shadowByte(9, at - 4096) : sentinel;
            if (at >= 8704 && at < 9216) return narrow(at);
            return sentinel;
        };
        for (std::size_t at = 0; at < span; ++at) {
            if (landed[at] != wantJoined(at)) throw std::runtime_error("a joined narrow copy-back stored the wrong byte at offset " + std::to_string(at) + ": " + std::to_string(landed[at]) + ", expected " + std::to_string(wantJoined(at)));
        }
    }
    // APS5_NARROW_VERIFY=1: a dword the shadow left equal to its baseline is read back from the
    // import; one the import holds otherwise (a stale baseline: a narrow pass alone would leave
    // it) is stored as a whole copy would store it and counted. Every seventh dword differs from
    // its baseline (stored as always), every fifth is stale in the import.
    {
        setSwitch("APS5_COALESCE_COPY_BACKS", "1");
        setSwitch("APS5_NARROW_COPY_BACKS", "1");
        setSwitch("APS5_NARROW_VERIFY", "1");
        Recorder verifying(context);
        setSwitch("APS5_COALESCE_COPY_BACKS", "");
        setSwitch("APS5_NARROW_COPY_BACKS", "");
        setSwitch("APS5_NARROW_VERIFY", "");
        verifying.Activate();
        constexpr std::size_t span = 4096;
        const auto usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
        auto shadow = std::make_shared<Buffer>(context, 2 * span, usage);
        const auto shadowByte = [](std::size_t at) { return static_cast<unsigned char>(at * 13 + 3); };
        const auto changed = [](std::size_t at) { return (at / 4) % 7 == 0; };
        const auto stale = [](std::size_t at) { return (at / 4) % 5 == 0; };
        auto* guest = static_cast<unsigned char*>(block);
        std::size_t staleWords = 0;
        for (std::size_t at = 0; at < span; ++at) {
            shadow->Bytes()[at] = std::byte{shadowByte(at)};
            shadow->Bytes()[span + at] = std::byte{changed(at) ? static_cast<unsigned char>(~shadowByte(at)) : shadowByte(at)};
            guest[at] = stale(at) ? static_cast<unsigned char>(~shadowByte(at)) : shadowByte(at);
            if (at % 4 == 0 && stale(at) && !changed(at)) ++staleWords;
        }
        Recorder::DeferredCopy copy{shadow, shadow.get(), shadow->Handle(), import->buffer, 0, address - import->base, span, address};
        copy.sourceAddress = shadow->DeviceAddress();
        copy.destinationAddress = import->address;
        copy.baselineOffset = span;
        const auto before = Recorder::CopyBackCounts();
        verifying.DeferCopies({copy});
        static_cast<void>(verifying.Commands());
        verifying.Submit();
        verifying.Sync();
        const auto after = Recorder::CopyBackCounts();
        for (std::size_t at = 0; at < span; ++at) {
            if (guest[at] != shadowByte(at)) throw std::runtime_error("APS5_NARROW_VERIFY left a stale byte in the import at offset " + std::to_string(at) + ": " + std::to_string(guest[at]) + ", expected " + std::to_string(shadowByte(at)));
            if (std::to_integer<unsigned char>(shadow->Bytes()[span + at]) != shadowByte(at)) throw std::runtime_error("APS5_NARROW_VERIFY left a wrong baseline byte at offset " + std::to_string(at));
        }
        Require(after.narrowStale - before.narrowStale == staleWords, "APS5_NARROW_VERIFY counted " + std::to_string(after.narrowStale - before.narrowStale) + " stale dwords, expected " + std::to_string(staleWords));
    }
    recorder.Activate();
    {
        GuestAllocations::Mutation mutation;
        mutation.Remove(block);
    }
    HostImportFor(context, address, bytes);
}

// The page guards resident buffers use (GuestPageGuard*): a fault runs the resolver, a guard it
// leaves is released by force, and a page two guards hold stays guarded until both went.
std::uint64_t guardTestIds[2]{};
std::atomic<int> guardTestFaults{0};

bool releaseFirstGuard(std::uintptr_t) {
    guardTestFaults.fetch_add(1);
    GuestWriteWatch::GuestPageGuardRelease_nid_postfix(guardTestIds[0]);
    return true;
}

void pageGuardTests() {
    constexpr std::size_t page = 4096;
#ifdef _WIN32
    auto* block = static_cast<unsigned char*>(VirtualAlloc(nullptr, 4 * page, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
#else
    void* mapped = mmap(nullptr, 4 * page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    auto* block = static_cast<unsigned char*>(mapped == MAP_FAILED ? nullptr : mapped);
#endif
    Require(block != nullptr, "cannot allocate the page guard test block");
    const auto byteAt = [](std::size_t at) { return static_cast<unsigned char>(at % 251); };
    for (std::size_t at = 0; at < 4 * page; ++at) block[at] = byteAt(at);
    const auto base = reinterpret_cast<std::uintptr_t>(block);
    GuestWriteWatch::GuestPageGuardInstall_nid_postfix(&releaseFirstGuard);
    guardTestIds[0] = GuestWriteWatch::GuestPageGuardProtect_nid_postfix(base, base + 2 * page);
    guardTestIds[1] = GuestWriteWatch::GuestPageGuardProtect_nid_postfix(base + page, base + 3 * page);
    if (guardTestIds[0] == 0 || guardTestIds[1] == 0) {
        GuestWriteWatch::GuestPageGuardRelease_nid_postfix(guardTestIds[0]);
        GuestWriteWatch::GuestPageGuardRelease_nid_postfix(guardTestIds[1]);
        std::cout << "page guards unavailable: not tested\n";
        return;
    }
    Require(GuestWriteWatch::GuestPageGuardProtect_nid_postfix(base + 16, base + page) == 0, "a guard off whole pages was taken");
    std::uint64_t faults = 0, forced = 0, guards = 0;
    GuestWriteWatch::GuestPageGuardCounts_nid_postfix(&faults, &forced, nullptr);
    const volatile unsigned char* read = block;
    Require(read[7] == byteAt(7) && guardTestFaults.load() == 1, "a read of a guarded page did not run the resolver once");
    Require(GuestWriteWatch::GuestPageGuardCovers_nid_postfix(base + page), "the page the second guard holds too was released with the first");
    Require(read[page + 3] == byteAt(page + 3) && guardTestFaults.load() == 2, "a read of a page two guards held did not fault");
    Require(read[2 * page + 5] == byteAt(2 * page + 5) && read[3 * page] == byteAt(3 * page) && guardTestFaults.load() == 2, "a page no guard holds faulted");
    std::uint64_t faultsAfter = 0, forcedAfter = 0;
    GuestWriteWatch::GuestPageGuardCounts_nid_postfix(&faultsAfter, &forcedAfter, &guards);
    Require(faultsAfter - faults == 2 && forcedAfter - forced == 1 && guards == 0 && !GuestWriteWatch::GuestPageGuardCovers_nid_postfix(base), "page guard counts are off");
    // Host I/O over guarded pages (GuestArena::HostWrite, the kernel's file writes) resolves them
    // first, as a fault would; over pages no guard holds it does nothing.
    guardTestIds[0] = GuestWriteWatch::GuestPageGuardProtect_nid_postfix(base + page, base + 3 * page);
    Require(guardTestIds[0] != 0, "a guard over released pages was refused");
    GuestWriteWatch::GuestPageGuardTouch_nid_postfix(base, 16);
    Require(guardTestFaults.load() == 2 && GuestWriteWatch::GuestPageGuardCovers_nid_postfix(base + page), "touching a page no guard holds resolved a guard");
    GuestWriteWatch::GuestPageGuardTouch_nid_postfix(base + 2 * page + 100, 8);
    Require(guardTestFaults.load() == 3 && !GuestWriteWatch::GuestPageGuardCovers_nid_postfix(base + page), "touching a guarded page did not resolve its guard");
    block[page] = 1;
    // A hold (a mapping change, host I/O: GuestPageGuardHold) resolves the guards over its range
    // and refuses new ones there until it ends; a range beside it is still guarded.
    guardTestIds[0] = GuestWriteWatch::GuestPageGuardProtect_nid_postfix(base, base + page);
    Require(guardTestIds[0] != 0, "a guard over a released page was refused");
    {
        const GuestWriteWatch::PageGuardHold hold(block + 100, 8);
        Require(guardTestFaults.load() == 4 && !GuestWriteWatch::GuestPageGuardCovers_nid_postfix(base), "a hold did not resolve the guard over its range");
        Require(GuestWriteWatch::GuestPageGuardProtect_nid_postfix(base, base + page) == 0, "a guard over a held range was taken");
        const auto beside = GuestWriteWatch::GuestPageGuardProtect_nid_postfix(base + page, base + 2 * page);
        Require(beside != 0, "a guard beside a held range was refused");
        GuestWriteWatch::GuestPageGuardRelease_nid_postfix(beside);
    }
    guardTestIds[0] = GuestWriteWatch::GuestPageGuardProtect_nid_postfix(base, base + page);
    Require(guardTestIds[0] != 0, "a guard over a range no longer held was refused");
    GuestWriteWatch::GuestPageGuardRelease_nid_postfix(guardTestIds[0]);
    GuestWriteWatch::GuestPageGuardInstall_nid_postfix(nullptr);
#ifdef _WIN32
    VirtualFree(block, 0, MEM_RELEASE);
#else
    munmap(block, 4 * page);
#endif
}

// Page guards over shared views (Windows, WindowsMappings; APS5_GUARD_SHARED_VIEWS=1): refused
// without the switch, as before. With it a guard holds exactly its 4 KiB parts of a 16 KiB view
// page; the write tracking's arming (a collect) leaves those parts no-access and arms the others; a
// read of a guarded part runs the resolver once and the release gives the parts the armed
// protection, so a store after it faults into the write tracking (not the guard) and is collected;
// a page mapped at a second guest address is refused as aliased; and a release after the view was
// mapped again leaves the new mapping's protection alone.
void sharedViewGuardTests() {
#ifdef _WIN32
    if (!AgcDriver::GuestMemory::WriteWatched()) {
        std::cout << "no write-watched guest arena: shared view guards not tested\n";
        return;
    }
    constexpr std::size_t viewPage = 16384;
    constexpr std::size_t part = 4096;
    constexpr std::size_t bytes = 4 * viewPage;
    HANDLE section = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_EXECUTE_READWRITE, 0, static_cast<DWORD>(bytes), nullptr);
    Require(section != nullptr, "cannot create the shared view test section");
    void* block = GuestArena::GuestArenaAllocate_nid_postfix(2 * bytes, 65536);
    Require(block != nullptr, "cannot reserve the shared view test range");
    GuestArena::GuestArenaMap_nid_postfix(block, bytes, section, 0, PAGE_READWRITE);
    auto* guest = static_cast<volatile unsigned char*>(block);
    const auto byteAt = [](std::size_t at) { return static_cast<unsigned char>(at % 253); };
    for (std::size_t at = 0; at < bytes; ++at) guest[at] = byteAt(at);
    const auto base = reinterpret_cast<std::uintptr_t>(block);
    const auto protection = [](std::uintptr_t at) {
        MEMORY_BASIC_INFORMATION info{};
        Require(VirtualQuery(reinterpret_cast<void*>(at), &info, sizeof(info)) == sizeof(info), "cannot query a shared view test page");
        return info.Protect;
    };
    const auto refusedAs = [](const char* name) {
        std::array<std::uint64_t, 16> counts{};
        GuestWriteWatch::GuestPageGuardRefusals_nid_postfix(counts.data(), counts.size());
        for (std::size_t i = 0; i < counts.size(); ++i) {
            const char* reason = GuestWriteWatch::GuestPageGuardRefusalName_nid_postfix(i);
            if (reason == nullptr) break;
            if (std::strcmp(reason, name) == 0) return counts[i];
        }
        throw std::runtime_error(std::string("no page guard refusal named ") + name);
    };
    GuestWriteWatch::GuestPageGuardInstall_nid_postfix(&releaseFirstGuard);
    setSwitch("APS5_GUARD_SHARED_VIEWS", "");
    const auto sharedBefore = refusedAs("shared view");
    Require(GuestWriteWatch::GuestPageGuardProtect_nid_postfix(base + part, base + 3 * part) == 0 && refusedAs("shared view") == sharedBefore + 1, "a guard over a shared view was taken without APS5_GUARD_SHARED_VIEWS");
    setSwitch("APS5_GUARD_SHARED_VIEWS", "1");
    const auto faults = guardTestFaults.load();
    guardTestIds[0] = GuestWriteWatch::GuestPageGuardProtect_nid_postfix(base + part, base + 3 * part);
    Require(guardTestIds[0] != 0, "a guard over two parts of a shared view was refused");
    Require(protection(base) == PAGE_READWRITE && protection(base + part) == PAGE_NOACCESS && protection(base + 2 * part) == PAGE_NOACCESS && protection(base + 3 * part) == PAGE_READWRITE, "the guard did not hold exactly its parts of the view page");
    Require(guest[5] == byteAt(5) && guest[3 * part + 5] == byteAt(3 * part + 5) && guardTestFaults.load() == faults, "a part of the view page no guard holds faulted");
    // The write tracking arms the page: the guarded parts stay no-access, the others turn read-only.
    std::array<void*, 16> pages{};
    std::size_t count = pages.size();
    Require(GuestArena::GuestArenaCollectWrites_nid_postfix(base, viewPage, pages.data(), &count, true), "the shared view test page could not be collected");
    Require(protection(base) == PAGE_READONLY && protection(base + part) == PAGE_NOACCESS && protection(base + 2 * part) == PAGE_NOACCESS && protection(base + 3 * part) == PAGE_READONLY, "arming the write tracking lifted the guard or skipped the parts no guard holds");
    // A read of a guarded part lands it once; its release arms the parts read-only.
    Require(guest[part + 7] == byteAt(part + 7) && guardTestFaults.load() == faults + 1, "a read of a guarded part of a view did not run the resolver once");
    Require(protection(base + part) == PAGE_READONLY && protection(base + 2 * part) == PAGE_READONLY, "the release did not give the guarded parts the armed protection");
    count = pages.size();
    Require(GuestArena::GuestArenaCollectWrites_nid_postfix(base, viewPage, pages.data(), &count, true) && count == 0, "an armed view page nobody wrote reported writes");
    // A store after the release goes to the write tracking (not the guard) and is collected.
    guest[part + 9] = 0x42;
    Require(guest[part + 9] == 0x42 && guardTestFaults.load() == faults + 1 && protection(base + part) == PAGE_READWRITE, "a store after the guard's release did not go through the write tracking");
    count = pages.size();
    Require(GuestArena::GuestArenaCollectWrites_nid_postfix(base, viewPage, pages.data(), &count, false) && count != 0, "a store after a guard's release was not collected");
    // A page of the section mapped at a second guest address: refused as aliased.
    GuestArena::GuestArenaMap_nid_postfix(static_cast<char*>(block) + bytes, viewPage, section, 0, PAGE_READWRITE);
    const auto aliasedBefore = refusedAs("aliased view");
    Require(GuestWriteWatch::GuestPageGuardProtect_nid_postfix(base, base + part) == 0 && refusedAs("aliased view") == aliasedBefore + 1, "a guard over a view page mapped twice was taken");
    // A view mapped again while guarded keeps its new protection when the guard goes.
    guardTestIds[1] = GuestWriteWatch::GuestPageGuardProtect_nid_postfix(base + 2 * viewPage, base + 3 * viewPage);
    Require(guardTestIds[1] != 0 && protection(base + 2 * viewPage) == PAGE_NOACCESS, "a guard over a whole view page was refused");
    GuestArena::GuestArenaMap_nid_postfix(reinterpret_cast<void*>(base + 2 * viewPage), viewPage, section, 2 * viewPage, PAGE_READONLY);
    GuestWriteWatch::GuestPageGuardRelease_nid_postfix(guardTestIds[1]);
    Require(protection(base + 2 * viewPage) == PAGE_READONLY && guest[2 * viewPage + 3] == byteAt(2 * viewPage + 3), "a guard's release changed a view mapped again since");
    GuestWriteWatch::GuestPageGuardInstall_nid_postfix(nullptr);
    setSwitch("APS5_GUARD_SHARED_VIEWS", "");
    GuestArena::GuestArenaReset_nid_postfix(block, 2 * bytes);
    GuestArena::GuestArenaRelease_nid_postfix(block, 2 * bytes);
    CloseHandle(section);
#endif
}

// The fast ring's reclaim without a GPU wait (ReclaimFastRing; the fast draw and the fast dispatch
// share one ring): under the GPU mutex a reaped batch's kept retirement is released only at this
// thread's unlock, so a reap alone leaves the ring full; the reclaim completes the serials of the
// batches no longer in flight and the ring serves again. A batch still in flight keeps its region.
void fastRingReclaimTests(const Device& device, Recorder& recorder) {
    using AgcDriver::Graphics::FastRing;
    recorder.Sync();
    FastRing ring(device.GetContext(), 1u << 16u);
    const auto capacity = ring.Capacity();
    const auto serial = recorder.Submissions() + 1;
    Require(ring.Allocate(capacity, serial).has_value(), "the fast ring refused its whole capacity");
    recorder.Keep(ring.Retirement(serial));
    recorder.Submit();
    Require(recorder.Submissions() == serial, "the batch holding the ring region was not submitted");
    device.WaitQueue();
    Require(!ring.Allocate(capacity, serial + 1), "the fast ring served a region of a batch nobody reaped");
    recorder.Reap();
    Require(recorder.InFlightBatches() == 0, "the finished batch was not reaped");
    // APS5_RELEASE_UNDER_LOCK releases kept objects at the reap: the reap alone would do then.
    if (std::getenv("APS5_RELEASE_UNDER_LOCK") == nullptr) Require(!ring.Allocate(capacity, serial + 1), "the reap released the kept retirement under the mutex");
    AgcDriver::Graphics::ReclaimFastRing(ring, recorder);
    Require(ring.Allocate(capacity, serial + 1).has_value(), "the fast ring did not serve again after the reclaim of a finished batch");
    // The region of the open batch (serial + 1) stays held: a reclaim with that batch in flight
    // frees nothing.
    recorder.Keep(ring.Retirement(serial + 1));
    recorder.Submit();
    AgcDriver::Graphics::ReclaimFastRing(ring, recorder);
    if (recorder.InFlightBatches() != 0) Require(!ring.Allocate(capacity, serial + 2), "the fast ring served a region of a batch still in flight");
    recorder.Sync();
}

// tests/shaders/DataSlot.comp (results[words[1]] = words[0]) on a template's set layout, and its
// dispatch as recordDispatch records one: everything earlier visible to the dispatch (a refresh's
// transfer included), its stores to everything after; the template's use is noted as
// recordDispatch notes it (ShaderResources::NoteRecorded, which ForkData's adoption reads).
class DataSlotPipeline {
public:
    DataSlotPipeline(const Context& context, VkDescriptorSetLayout setLayout) : context(context) {
        VkPipelineLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        layoutInfo.setLayoutCount = 1;
        layoutInfo.pSetLayouts = &setLayout;
        Check(context.Function<PFN_vkCreatePipelineLayout>("vkCreatePipelineLayout")(context.device, &layoutInfo, nullptr, &pipelineLayout), "vkCreatePipelineLayout");
        VkShaderModuleCreateInfo moduleInfo{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        moduleInfo.codeSize = sizeof(DATA_SLOT_SPV);
        moduleInfo.pCode = DATA_SLOT_SPV;
        Check(context.Function<PFN_vkCreateShaderModule>("vkCreateShaderModule")(context.device, &moduleInfo, nullptr, &module), "vkCreateShaderModule");
        VkComputePipelineCreateInfo pipelineInfo{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        pipelineInfo.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, module, "main", nullptr};
        pipelineInfo.layout = pipelineLayout;
        Check(context.Function<PFN_vkCreateComputePipelines>("vkCreateComputePipelines")(context.device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &pipeline), "vkCreateComputePipelines");
    }
    ~DataSlotPipeline() {
        context.Function<PFN_vkDestroyPipeline>("vkDestroyPipeline")(context.device, pipeline, nullptr);
        context.Function<PFN_vkDestroyShaderModule>("vkDestroyShaderModule")(context.device, module, nullptr);
        context.Function<PFN_vkDestroyPipelineLayout>("vkDestroyPipelineLayout")(context.device, pipelineLayout, nullptr);
    }
    DataSlotPipeline(const DataSlotPipeline&) = delete;
    DataSlotPipeline& operator=(const DataSlotPipeline&) = delete;

    // Binds `set` (a ForkData set) or, when null, the template's own.
    void Dispatch(Recorder& recorder, ShaderResources& resources, VkDescriptorSet set) const {
        const auto commands = recorder.Commands();
        RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_HOST_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
        context.Function<PFN_vkCmdBindPipeline>("vkCmdBindPipeline")(commands, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
        if (set != VK_NULL_HANDLE) resources.Bind(commands, VK_PIPELINE_BIND_POINT_COMPUTE, pipelineLayout, set);
        else resources.Bind(commands, VK_PIPELINE_BIND_POINT_COMPUTE, pipelineLayout);
        context.Function<PFN_vkCmdDispatch>("vkCmdDispatch")(commands, 1, 1, 1);
        RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_HOST_READ_BIT);
        resources.NoteRecorded(recorder);
    }

private:
    Context context;
    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
    VkShaderModule module = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
};

// The words a template's data binding holds (BoundDescriptors: the buffer's bytes).
template <std::size_t Count>
std::array<std::uint32_t, Count> HeldWords(const ShaderResources& resources, std::size_t binding) {
    const auto bound = resources.BoundDescriptors();
    Require(binding < bound.size() && bound[binding].elements.size() == 1 && bound[binding].elements[0].data.size() >= Count * sizeof(std::uint32_t), "the template's data binding has no host bytes");
    std::array<std::uint32_t, Count> words{};
    std::memcpy(words.data(), bound[binding].elements[0].data.data(), sizeof(words));
    return words;
}

// The template refresh through the ring (APS5_TEMPLATE_REFRESH_RING, ShaderResources::ForkData) on
// a real device, with tests/shaders/DataSlot.comp (binding 0 the template's flattened-SRT words,
// binding 1 a second data buffer the results land in): forks and in-stream refreshes (RefreshData)
// interleaved in one batch each read their own words while the template's buffer keeps its words
// (and DataWordsHash) through a fork; equal words fork nothing; the batch's last fork's words bind
// that fork again (Reused) until an in-stream refresh drops it; a template whose last use finished
// takes the words into its own buffer on the CPU (Adopted), one used in the open batch forks;
// without a ring nothing is made; a full ring refuses; a batch needing more sets than one transient
// pool takes another, and a pool is reset and reused once its batch completed.
void templateRefreshRingTests(const Device& device, Recorder& recorder) {
    using Outcome = ShaderResources::ForkOutcome;
    recorder.Sync();
    auto context = device.GetContext();
    auto cache = std::make_unique<DescriptorCache>(context);
    auto ring = std::make_unique<FastRing>(context, 1u << 20u);
    context.descriptorCache = cache.get();
    context.fastRing = ring.get();
    ShaderRecompiler::RecompileResult program;
    ShaderRecompiler::DescriptorBinding words;
    words.kind = ShaderRecompiler::DescriptorKind::StorageBuffer;
    words.role = ShaderRecompiler::DescriptorRole::FlattenedSrt;
    words.descriptorSet = 0;
    words.binding = 0;
    words.count = 1;
    words.guestDescriptor = {7, 0};
    auto results = words;
    results.role = ShaderRecompiler::DescriptorRole::ShaderData;
    results.binding = 1;
    results.guestDescriptor.assign(8, 0u);
    program.bindings = {words, results};
    const auto withWords = [&](std::uint32_t value, std::uint32_t slot) {
        auto copy = program;
        copy.bindings[0].guestDescriptor = {value, slot};
        return copy;
    };
    const auto firstProgram = withWords(42, 1);
    const auto secondProgram = withWords(9, 2);
    const auto thirdProgram = withWords(5, 3);
    const auto fourthProgram = withWords(11, 4);
    const CompiledShader original{ShaderRecompiler::ShaderStage::Compute, &program, 0};
    const CompiledShader first{ShaderRecompiler::ShaderStage::Compute, &firstProgram, 0};
    const CompiledShader second{ShaderRecompiler::ShaderStage::Compute, &secondProgram, 0};
    const CompiledShader third{ShaderRecompiler::ShaderStage::Compute, &thirdProgram, 0};
    const CompiledShader fourth{ShaderRecompiler::ShaderStage::Compute, &fourthProgram, 0};
    // Without a ring (or a descriptor cache) nothing is made: the caller refreshes in-stream. A
    // template no batch recorded (no NoteRecorded) never adopts.
    {
        ShaderResources plain(device.GetContext(), original);
        const auto fork = plain.ForkData(first, recorder);
        Require(fork.outcome == Outcome::NoRing && fork.set == VK_NULL_HANDLE, "a fork without a ring made a set");
        Require(plain.ForkData(original, recorder).outcome == Outcome::Same, "the template's own words did not compare equal");
    }
    {
        ShaderResources resources(context, original);
        const DataSlotPipeline slots(context, resources.Layout());
        const auto dispatch = [&](VkDescriptorSet set) { slots.Dispatch(recorder, resources, set); };
        const auto builtHash = resources.DataWordsHash();
        // 1. The template's own words: nothing differs, its set is bound.
        auto fork = resources.ForkData(original, recorder);
        Require(fork.outcome == Outcome::Same && fork.set == VK_NULL_HANDLE, "the template's own words forked");
        dispatch(VK_NULL_HANDLE);
        // 2-3. Two forks: each binds its own copy, the template keeps its words. The first one's
        // words again in the batch bind its set again and make nothing.
        fork = resources.ForkData(first, recorder);
        Require(fork.outcome == Outcome::Forked && fork.set != VK_NULL_HANDLE && fork.bytes == 2 * sizeof(std::uint32_t), "the first fork was not made");
        dispatch(fork.set);
        const auto setsMade = cache->TransientCounters().sets;
        const auto again = resources.ForkData(first, recorder);
        Require(again.outcome == Outcome::Reused && again.set == fork.set && cache->TransientCounters().sets == setsMade, "the batch's last fork's words did not bind its set again");
        dispatch(again.set);
        const auto secondFork = resources.ForkData(second, recorder);
        Require(secondFork.outcome == Outcome::Forked && secondFork.set != VK_NULL_HANDLE && secondFork.set != fork.set, "the second fork was not a set of its own");
        dispatch(secondFork.set);
        Require(resources.DataWordsHash() == builtHash && !resources.DataWordsDiffer(original) && resources.DataWordsDiffer(first), "a fork changed the template's words");
        // 4. An in-stream refresh after the forks, in the same batch: the forks keep their words,
        // and the last fork is not bound again (its copied bindings read the refreshed buffers).
        Require(resources.RefreshData(recorder.Commands(), third, &recorder), "the in-stream refresh recorded nothing");
        dispatch(VK_NULL_HANDLE);
        const auto afterRefresh = resources.ForkData(second, recorder);
        Require(afterRefresh.outcome == Outcome::Forked && afterRefresh.set != secondFork.set, "an in-stream refresh did not drop the batch's last fork");
        dispatch(afterRefresh.set);
        // 5. Back to the built words through the ring: the template holds the refreshed ones now.
        Require(resources.ForkData(third, recorder).outcome == Outcome::Same, "the refreshed words forked");
        fork = resources.ForkData(original, recorder);
        Require(fork.outcome == Outcome::Forked, "a fork back to the built words was not made");
        dispatch(fork.set);
        Require(cache->TransientCounters().pools == 1 && cache->TransientCounters().sets == 4, "the batch's forks did not share one transient pool");
        const auto serial = recorder.Submissions() + 1;
        recorder.Submit();
        Require(recorder.Submissions() == serial, "the forks' batch was not submitted");
        device.WaitQueue();
        recorder.Sync();
        const auto landed = HeldWords<5>(resources, 1);
        Require(landed[0] == 7 && landed[1] == 42 && landed[2] == 9 && landed[3] == 5, "a dispatch did not read its own words: results " + std::to_string(landed[0]) + " " + std::to_string(landed[1]) + " " + std::to_string(landed[2]) + " " + std::to_string(landed[3]));
        Require(HeldWords<2>(resources, 0) == std::array<std::uint32_t, 2>{5, 3} && !resources.DataWordsDiffer(third), "the template's buffer does not hold the in-stream refresh's words");
        // 6. The template's last use finished: the words go into its own buffer on the CPU, its own
        // set is bound, nothing is made or recorded for them.
        const auto setsBefore = cache->TransientCounters().sets;
        const auto adopted = resources.ForkData(fourth, recorder);
        Require(adopted.outcome == Outcome::Adopted && adopted.set == VK_NULL_HANDLE && adopted.bytes == 2 * sizeof(std::uint32_t) && cache->TransientCounters().sets == setsBefore, "an idle template did not adopt the words");
        Require(HeldWords<2>(resources, 0) == std::array<std::uint32_t, 2>{11, 4} && !resources.DataWordsDiffer(fourth) && resources.DataWordsHash() == ShaderResources::DataWordsHash(fourth), "the adopted words are not the template's");
        dispatch(VK_NULL_HANDLE);
        // The batch's pool goes back once the batch's keeps are released (after this thread's
        // unlock, on the release thread); the next fork takes it again. The template is used in the
        // open batch now: it forks, it does not adopt.
        GpuMutex().unlock();
        for (int wait = 0; wait < 400 && cache->TransientCounters().resets == 0; ++wait) std::this_thread::sleep_for(std::chrono::milliseconds(5));
        GpuMutex().lock();
        Require(cache->TransientCounters().resets == 1, "the transient pool was not reset after its batch completed");
        Require(resources.ForkData(first, recorder).outcome == Outcome::Forked && cache->TransientCounters().pools == 1, "the next batch's fork did not reuse the reset pool");
        // More forks in one batch than one pool holds (alternating words, so none is the last
        // fork's): another pool, every fork served.
        for (int index = 0; index < 300; ++index) {
            const auto made = resources.ForkData(index % 2 == 0 ? second : first, recorder);
            Require(made.outcome == Outcome::Forked && made.set != VK_NULL_HANDLE, "a fork past one pool's sets was not made");
        }
        // A driver may serve sets past maxSets (the spec lets the allocation fail there, it need not):
        // then the one pool held them all.
        Require(cache->TransientCounters().pools <= 2 && cache->TransientCounters().refused == 0, "a batch past one pool's sets did not take a second pool");
        recorder.Submit();
        device.WaitQueue();
        recorder.Sync();
        Require(HeldWords<5>(resources, 1)[4] == 11, "the dispatch after the adoption did not read the adopted words");
        // A full ring refuses (the caller then refreshes in-stream). Alternating words: a repeat of
        // the last fork's would bind it again and take no ring space.
        auto small = context;
        FastRing tiny(context, 256);
        small.fastRing = &tiny;
        ShaderResources squeezed(small, original);
        auto outcome = Outcome::Forked;
        for (int index = 0; index < 512 && outcome == Outcome::Forked; ++index) outcome = squeezed.ForkData(index % 2 == 0 ? first : second, recorder).outcome;
        Require(outcome == Outcome::RingFull, "a fork into a full ring was not refused as ring full");
        recorder.Submit();
        device.WaitQueue();
        recorder.Sync();
    }
    // The cache goes before the batches that keep its pools are released: their returns find it gone.
    cache.reset();
    ring.reset();
}

// The data patch through the template refresh (s53-gpu2-tmpl review, equivalence finding 2): a
// read-only V# (element 1) 4 bytes past the aligned start of the region it shares with element 0,
// in a shader without push constants, puts its adjustment into byte memoryOffsetDword * 4 + 1 of
// the shader data (here bits 8-15 of words[0], DataSlot.comp's value). A fork (the ring's copy), an
// in-stream refresh (RefreshData) and an adoption (the template's buffer, by the CPU) must each give
// the dispatch the patched word, results[slot] = value | adjustment << 8, and the [dispatch-io]
// fork words show it.
void templateRefreshPatchTests(const Device& device, Recorder& recorder) {
    using Outcome = ShaderResources::ForkOutcome;
    auto context = device.GetContext();
    const auto alignment = context.limits.minStorageBufferOffsetAlignment;
    if (context.hostImportAlignment == 0) {
        std::cout << "host imports unavailable: the template refresh's data patch not tested\n";
        return;
    }
    if (alignment < 8 || alignment > 256) {
        std::cout << "storage buffer offset alignment " << alignment << ": the template refresh's data patch not tested\n";
        return;
    }
    constexpr std::size_t bytes = 65536;
#ifdef _WIN32
    void* block = VirtualAlloc(nullptr, bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
#else
    void* block = std::aligned_alloc(65536, bytes);
#endif
    Require(block != nullptr, "cannot allocate the data patch block");
    std::memset(block, 0x5a, bytes);
    const auto address = reinterpret_cast<std::uint64_t>(block);
    {
        GuestAllocations::Mutation mutation;
        mutation.Add(block, bytes, true, true);
    }
    struct Unregister {
        const Context& context;
        void* block;
        ~Unregister() {
            {
                GuestAllocations::Mutation mutation;
                mutation.Remove(block);
            }
            HostImportFor(context, reinterpret_cast<std::uint64_t>(block), bytes);
        }
    } unregister{context, block};
    if (HostImportFor(context, address, bytes) == nullptr) {
        std::cout << "host import of the test block refused: the template refresh's data patch not tested\n";
        return;
    }
    recorder.Sync();
    auto cache = std::make_unique<DescriptorCache>(context);
    auto ring = std::make_unique<FastRing>(context, 1u << 20u);
    context.descriptorCache = cache.get();
    context.fastRing = ring.get();
    const auto outer = address + 4096;
    const auto element = outer + 4;
    ShaderRecompiler::RecompileResult program;
    ShaderRecompiler::DescriptorBinding words;
    words.kind = ShaderRecompiler::DescriptorKind::StorageBuffer;
    words.role = ShaderRecompiler::DescriptorRole::ShaderData;
    words.descriptorSet = 0;
    words.binding = 0;
    words.count = 1;
    words.guestDescriptor = {0x110000, 1};
    auto results = words;
    results.role = ShaderRecompiler::DescriptorRole::FlattenedSrt;
    results.binding = 1;
    results.guestDescriptor.assign(8, 0u);
    ShaderRecompiler::DescriptorBinding guest;
    guest.kind = ShaderRecompiler::DescriptorKind::StorageBuffer;
    guest.role = ShaderRecompiler::DescriptorRole::GuestBuffers;
    guest.descriptorSet = 0;
    guest.binding = 2;
    guest.count = 2;
    guest.guestDescriptor = {static_cast<std::uint32_t>(outer), static_cast<std::uint32_t>(outer >> 32u) & 0xffffu, 64u, 0x31000000u, static_cast<std::uint32_t>(element), static_cast<std::uint32_t>(element >> 32u) & 0xffffu, 16u, 0x31000000u};
    guest.bufferWritten = {false, false};
    program.bindings = {words, results, guest};
    program.memoryOffsetDword = 0;
    const auto withWords = [&](std::uint32_t value, std::uint32_t slot) {
        auto copy = program;
        copy.bindings[0].guestDescriptor = {value, slot};
        return copy;
    };
    const auto forkedProgram = withWords(0x220000, 2);
    const auto refreshedProgram = withWords(0x330000, 3);
    const auto adoptedProgram = withWords(0x440000, 4);
    const CompiledShader original{ShaderRecompiler::ShaderStage::Compute, &program, 0};
    const CompiledShader forked{ShaderRecompiler::ShaderStage::Compute, &forkedProgram, 0};
    const CompiledShader refreshed{ShaderRecompiler::ShaderStage::Compute, &refreshedProgram, 0};
    const CompiledShader adopted{ShaderRecompiler::ShaderStage::Compute, &adoptedProgram, 0};
    {
        ShaderResources resources(context, original);
        const auto adjustment = (HeldWords<1>(resources, 0)[0] >> 8u) & 0xffu;
        if (adjustment == 0) {
            // Element 1 was not bound off an aligned offset (its region copied from its own start).
            std::cout << "no data patch in the built template: the template refresh's data patch not tested\n";
        } else {
            Require(adjustment == 4, "the data patch is not the V#'s distance from its aligned offset");
            const DataSlotPipeline slots(context, resources.Layout());
            slots.Dispatch(recorder, resources, VK_NULL_HANDLE);
            const auto fork = resources.ForkData(forked, recorder);
            Require(fork.outcome == Outcome::Forked, "the patched template did not fork");
            Require(resources.DescribeForkedWords(forked).find("00220400 00000002") != std::string::npos, "the fork's trace words are not the patched words: " + resources.DescribeForkedWords(forked));
            slots.Dispatch(recorder, resources, fork.set);
            Require(resources.RefreshData(recorder.Commands(), refreshed, &recorder), "the patched template's refresh recorded nothing");
            slots.Dispatch(recorder, resources, VK_NULL_HANDLE);
            recorder.Submit();
            device.WaitQueue();
            recorder.Sync();
            const auto made = resources.ForkData(adopted, recorder);
            Require(made.outcome == Outcome::Adopted, "the idle patched template did not adopt");
            Require(HeldWords<2>(resources, 0) == std::array<std::uint32_t, 2>{0x440400, 4}, "the adopted words are not patched");
            slots.Dispatch(recorder, resources, VK_NULL_HANDLE);
            recorder.Submit();
            device.WaitQueue();
            recorder.Sync();
            const auto landed = HeldWords<5>(resources, 1);
            Require(landed[1] == 0x110400 && landed[2] == 0x220400 && landed[3] == 0x330400 && landed[4] == 0x440400, "a dispatch did not read its patched words: results " + std::to_string(landed[1]) + " " + std::to_string(landed[2]) + " " + std::to_string(landed[3]) + " " + std::to_string(landed[4]));
            std::cout << "template refresh data patch (adjustment " << adjustment << "): the build, a fork, an in-stream refresh and an adoption read the patched word\n";
        }
    }
    cache.reset();
    ring.reset();
}

// Resident buffers (APS5_RESIDENT_BUFFERS=1): the whole pages of a copy-back stay resident past
// Submit (the partial ones land with it), a CPU read of one lands its bytes, a write after that
// survives, a command records a resident copy, and a label store over one records it first.
void residentBufferTests(const Device& device, Recorder& recorder) {
    const auto& context = device.GetContext();
    if (context.hostImportAlignment == 0) {
        std::cout << "host imports unavailable: resident buffers not tested\n";
        return;
    }
    constexpr std::size_t bytes = 65536;
#ifdef _WIN32
    void* block = VirtualAlloc(nullptr, bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
#else
    void* block = std::aligned_alloc(65536, bytes);
#endif
    Require(block != nullptr, "cannot allocate the resident buffer test block");
    std::memset(block, 0, bytes);
    const auto address = reinterpret_cast<std::uint64_t>(block);
    {
        GuestAllocations::Mutation mutation;
        mutation.Add(block, bytes, true, true);
    }
    const auto* import = HostImportFor(context, address, bytes);
    if (import == nullptr) {
        std::cout << "host import of the resident buffer test block refused: resident buffers not tested\n";
        return;
    }
    {
#ifdef _WIN32
        _putenv_s("APS5_RESIDENT_BUFFERS", "1");
#else
        setenv("APS5_RESIDENT_BUFFERS", "1", 1);
#endif
        Recorder resident(context);
#ifdef _WIN32
        _putenv_s("APS5_RESIDENT_BUFFERS", "");
#else
        unsetenv("APS5_RESIDENT_BUFFERS");
#endif
        Require(resident.KeepsResidentBuffers() && resident.CoalescesCopyBacks(), "APS5_RESIDENT_BUFFERS=1 did not turn resident buffers and coalescing on");
        resident.Activate();
        constexpr std::size_t sourceBytes = 32768;
        auto source = std::make_shared<Buffer>(context, sourceBytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
        const auto sourceByte = [](std::size_t at) { return static_cast<unsigned char>(at * 7 + 3); };
        for (std::size_t at = 0; at < sourceBytes; ++at) source->Bytes()[at] = std::byte{sourceByte(at)};
        // `target`: the range's import (a new one after the range is registered again, below).
        const HostImport* target = import;
        const auto copyFrom = [&](std::uint64_t from, std::uint64_t at, std::uint64_t count) {
            return std::vector<Recorder::DeferredCopy>{Recorder::DeferredCopy{source, source.get(), source->Handle(), target->buffer, from, address + at - target->base, count, address + at}};
        };
        const auto copyOf = [&](std::uint64_t at, std::uint64_t count) { return copyFrom(at, at, count); };
        const volatile unsigned char* landed = static_cast<const unsigned char*>(block);
        const auto before = Recorder::ResidentCounts();
        // [100, 12388): pages 1 and 2 stay resident, [100, 4096) and [12288, 12388) land at Submit.
        resident.DeferCopies(copyOf(100, 12288));
        resident.Submit();
        Require(resident.HasDeferredCopies() && resident.ResidentCopyBytes() == 8192, "Submit did not keep the whole pages of a copy-back resident");
        // Guarded, the pages are still mapped to the guest: the page queries (each asks the host here,
        // the block is outside the arena's page-state cache) must not read them as holes.
        Require(AgcDriver::GuestMemory::Accessible(static_cast<char*>(block) + 4096, 8192, true) && AgcDriver::GuestMemory::DescribeCommitted(address, bytes, true).whole, "a page a resident copy's guard holds read as inaccessible");
        resident.Sync();
        Require(landed[100] == sourceByte(100) && landed[4095] == sourceByte(4095) && landed[12387] == sourceByte(12387) && landed[12388] == 0 && landed[99] == 0, "the partial pages of a resident copy did not land with its batch");
        Require(landed[5000] == sourceByte(5000) && landed[9000] == sourceByte(9000), "a read of a resident page did not land its bytes");
        Require(Recorder::ResidentCounts().resolved - before.resolved == 1, "a fault on a resident page was not resolved once");
        static_cast<unsigned char*>(block)[6000] = 0xEE;
        static_cast<void>(resident.Commands());
        Require(!resident.HasDeferredCopies(), "a command left a landed resident copy queued");
        resident.Submit();
        resident.Sync();
        Require(landed[6000] == 0xEE, "a resident copy a fault landed was recorded again over a later CPU write");
        // Recorded by a command (no fault): the bytes land with that batch.
        resident.DeferCopies(copyOf(16384, 8192));
        resident.Submit();
        Require(resident.ResidentCopyBytes() == 8192, "a page-aligned copy-back was not kept resident");
        static_cast<void>(resident.Commands());
        Require(!resident.HasDeferredCopies(), "a command did not record a resident copy");
        resident.Submit();
        resident.Sync();
        for (std::size_t at = 16384; at < 16384 + 8192; at += 509) {
            if (landed[at] != sourceByte(at)) throw std::runtime_error("a resident copy recorded by a command stored the wrong byte at offset " + std::to_string(at));
        }
        // A label store over a resident copy records the copy ahead of it.
        resident.DeferCopies(copyOf(28672, 4096));
        resident.Submit();
        Require(resident.ResidentCopyBytes() == 4096, "a one-page copy-back was not kept resident");
        const std::array<std::byte, 4> label{std::byte{0xA1}, std::byte{0xB2}, std::byte{0xC3}, std::byte{0xD4}};
        resident.RecordStore(import->buffer, address + 28680 - import->base, label, address + 28680);
        resident.Submit();
        Require(!resident.HasDeferredCopies(), "a label store over a resident copy left it queued");
        resident.Sync();
        Require(landed[28680] == 0xA1 && landed[28683] == 0xD4 && landed[28679] == sourceByte(28679) && landed[28684] == sourceByte(28684), "a label store over a resident copy did not land after it");
        const auto counts = Recorder::ResidentCounts();
        Require(counts.made - before.made == 3 && counts.madeBytes - before.madeBytes == 8192 + 8192 + 4096 && counts.skipped - before.skipped == 1 && counts.recorded - before.recorded == 2, "resident buffer counters are off");
        constexpr std::size_t page = 4096;
        // A no-access page the guest made and guarded pages after it in one host region (one
        // VirtualQuery region, one /proc/self/maps line): the queries tell them apart, the guest's
        // page a hole, the guarded ones mapped (before, a query from the guest's page read all of
        // them as holes).
        {
#ifdef _WIN32
            auto* pages = static_cast<unsigned char*>(VirtualAlloc(nullptr, 3 * page, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
            DWORD previous = 0;
            const bool hidden = pages != nullptr && VirtualProtect(pages, page, PAGE_NOACCESS, &previous) != 0;
#else
            void* mapped = mmap(nullptr, 3 * page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
            auto* pages = static_cast<unsigned char*>(mapped == MAP_FAILED ? nullptr : mapped);
            const bool hidden = pages != nullptr && mprotect(pages, page, PROT_NONE) == 0;
#endif
            Require(hidden, "cannot make the guest-no-access test page");
            const auto first = reinterpret_cast<std::uintptr_t>(pages);
            const auto id = GuestWriteWatch::GuestPageGuardProtect_nid_postfix(first + page, first + 3 * page);
            Require(id != 0, "a guard beside a no-access page was refused");
            const auto ranges = AgcDriver::GuestMemory::CommittedRanges(first, 3 * page, true);
            GuestWriteWatch::GuestPageGuardRelease_nid_postfix(id);
            Require(ranges.size() == 1 && ranges[0].first == first + page && ranges[0].second == first + 3 * page, "guarded pages after a no-access page the guest made read as holes (or the guest's page as mapped)");
#ifdef _WIN32
            VirtualFree(pages, 0, MEM_RELEASE);
#else
            munmap(pages, 3 * page);
#endif
        }
        // An owner fault (a thread holding the GPU mutex: this one) on a guard made at a label of the
        // open batch is given up (forced: the batch cannot be submitted under its recording thread)
        // and the guard is released by force. The copy, unguarded now, is recorded at the next
        // decision, so it lands with this batch (before, Submit kept it resident without a guard:
        // a read after the batch's labels landed saw the import's old bytes, and its late recording
        // could undo a CPU write).
        {
            const auto forcedBefore = Recorder::ResidentCounts();
            resident.DeferCopies(copyOf(24576, page));
            const std::array<std::byte, 4> unrelated{std::byte{0x11}, std::byte{0x22}, std::byte{0x33}, std::byte{0x44}};
            resident.RecordStore(import->buffer, address + 10 * page - import->base, unrelated, address + 10 * page);
            resident.FlushStores();
            Require(resident.ResidentCopyBytes() == page, "a copy-back was not kept resident at a label");
            static_cast<void>(landed[24576 + 5]);
            Require(Recorder::ResidentCounts().forced - forcedBefore.forced == 1, "an owner fault on a guard of the open batch was not given up");
            resident.Submit();
            Require(resident.ResidentCopyBytes() == 0 && Recorder::ResidentCounts().unguarded - forcedBefore.unguarded == 1, "a resident copy whose guard was released by force was kept past Submit");
            resident.Sync();
            Require(landed[24576 + 5] == sourceByte(24576 + 5) && landed[24576 + page - 1] == sourceByte(24576 + page - 1) && landed[10 * page] == 0x11, "a resident copy whose guard was released by force did not land with its batch");
        }
        // A driver store from a thread without the GPU mutex (a queue worker's CPU-executed
        // WRITE_DATA) into a resident page, while the mutex's holder (this thread) waits for the
        // write tracker: the store resolves the guard before it takes the tracker (before, it faulted
        // under the tracker; the resolver waited for the GPU mutex, its holder for the tracker: all
        // hung). The stored bytes land after the GPU's.
        {
            resident.DeferCopies(copyFrom(0, 9 * page, page));
            resident.Submit();
            Require(resident.ResidentCopyBytes() == page, "a copy-back was not kept resident for the store test");
            Require(GpuMutex().DepthOnThisThread() == 1, "the resident store test expects one hold of the GPU mutex");
            const std::array<std::byte, 4> word{std::byte{0x5A}, std::byte{0x6B}, std::byte{0x7C}, std::byte{0x8D}};
            std::atomic<bool> stored{false};
            std::thread writer([&] {
                AgcDriver::GuestMemory::Write(address + 9 * page + 64, word, 4);
                stored.store(true);
            });
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            std::atomic<bool> collected{false};
            std::thread watchdog([&] {
                for (int waited = 0; waited < 300 && !collected.load(); ++waited) std::this_thread::sleep_for(std::chrono::milliseconds(100));
                if (!collected.load()) {
                    std::cerr << "a driver store into a resident page held the write tracker while its guard waited for the GPU mutex (deadlock)\n";
                    std::_Exit(3);
                }
            });
            static_cast<void>(AgcDriver::GuestMemory::CollectWritesUncached(address + 9 * page, page));
            collected.store(true);
            watchdog.join();
            const bool early = stored.load();
            GpuMutex().unlock();
            writer.join();
            GpuMutex().lock();
            Require(!early, "a driver store into a resident page did not wait for its guard's landing");
            Require(landed[9 * page + 64] == 0x5A && landed[9 * page + 67] == 0x8D && landed[9 * page + 63] == sourceByte(63) && landed[9 * page + 68] == sourceByte(68) && landed[10 * page - 1] == sourceByte(page - 1), "a driver store into a resident page did not land after the GPU's bytes");
        }
        // A new import of the range (registered again) while a guard holds one of its plain pages:
        // none for now (Windows: a plain page is pinned through the guest address), but not refused
        // for good: made once no guard holds part of it. The resident copy into the retired import
        // still lands.
        {
            resident.DeferCopies(copyFrom(page, 11 * page, page));
            resident.Submit();
            Require(resident.ResidentCopyBytes() == page, "a copy-back was not kept resident for the import test");
            {
                GuestAllocations::Mutation mutation;
                mutation.Remove(block);
            }
            {
                GuestAllocations::Mutation mutation;
                mutation.Add(block, bytes, true, true);
            }
#ifdef _WIN32
            Require(HostImportFor(context, address, bytes) == nullptr, "a plain range was imported over a guarded page");
            Require(HostImportFor(context, address, bytes) == nullptr, "a plain range was imported over a page a guard still holds");
#else
            static_cast<void>(HostImportFor(context, address, bytes));
#endif
            resident.Submit();
            resident.Sync();
            Require(landed[11 * page + 9] == sourceByte(page + 9) && landed[12 * page - 1] == sourceByte(2 * page - 1), "a resident copy into a retired plain import did not land");
            target = HostImportFor(context, address, bytes);
            Require(target != nullptr, "the import of a plain range was refused for good after a guard held one of its pages");
            Require(HostImportFor(context, address, bytes) == target, "the import made after the guard went did not stay");
            // The new import changed the mapping generation: the first decision after it records
            // the copy into the new import instead of keeping it.
            resident.DeferCopies(copyFrom(2 * page, 13 * page, page));
            resident.Submit();
            Require(resident.ResidentCopyBytes() == 0, "a copy-back queued after a mapping change was kept resident");
            resident.Sync();
            Require(landed[13 * page + 5] == sourceByte(2 * page + 5), "a copy-back into the new plain import did not land");
        }
        // A resident copy still queued when its recorder goes lands with the teardown.
        resident.DeferCopies(copyOf(12288, 4096));
        resident.Submit();
        Require(resident.ResidentCopyBytes() == 4096, "a copy-back over a landed page was not kept resident");
    }
    Require(static_cast<const volatile unsigned char*>(block)[13000] == static_cast<unsigned char>(13000 * 7 + 3), "a resident copy queued at the recorder's teardown was lost");
    recorder.Activate();
    {
        GuestAllocations::Mutation mutation;
        mutation.Remove(block);
    }
    HostImportFor(context, address, bytes);
}

// Resident read-only copies (APS5_RESIDENT_READS=1, GuestBufferMemory.cpp "Resident reads"): a
// dispatch build's element it only reads, inside a watched host import, binds a device-local copy
// (not the import, and not among the in-place reads) holding the guest bytes; another build of the
// range takes the copy as it stands; and every kind of write over the range makes the next use
// refresh it from the import first: a CPU store (write watch), a GPU write the recorder notes (a
// fill into the import, noted only), a queued label store (landed before the refresh copies), an
// address-based use whose BDA table may store over the range while its batch is in flight (once
// it finished, only its stamped stores count), a failed write-watch collect, a batch submitted
// between the check and the record (RecheckResidentReads), and a changed import. Writes elsewhere
// leave it standing; a reused build refreshes into the buffer its descriptor names; a written
// element, a refused copy and a recorder without the switch bind in place; a range refreshed at
// most uses is demoted to in place; APS5_RESIDENT_READS_VERIFY compares a standing copy with its
// import and catches a write that escaped every tracker; no copy outlives its builds and the cache.
void residentReadTests(const Device& device, Recorder& recorder) {
    const auto& context = device.GetContext();
    if (context.hostImportAlignment == 0) {
        std::cout << "host imports unavailable: resident reads not tested\n";
        return;
    }
    constexpr std::size_t unit = 65536;
    constexpr std::size_t bytes = 4 * unit;
    void* block = AllocateWatched(bytes, unit);
    if (block == nullptr) {
        std::cout << "no write watching: resident reads not tested\n";
        return;
    }
    struct Release {
        void* block;
        ~Release() { ReleaseWatched(block, bytes); }
    } release{block};
    auto* guest = static_cast<volatile unsigned char*>(block);
    for (std::size_t at = 0; at < bytes; ++at) guest[at] = static_cast<unsigned char>(at * 11 + 7);
    const auto address = reinterpret_cast<std::uint64_t>(block);
    {
        GuestAllocations::Mutation mutation;
        mutation.Add(block, bytes, true, true);
    }
    struct Unregister {
        const Context& context;
        void* block;
        std::uint64_t address;
        ~Unregister() {
            ClearResidentReads();
            {
                GuestAllocations::Mutation mutation;
                mutation.Remove(block);
            }
            HostImportFor(context, address, bytes);
        }
    } unregister{context, block, address};
    const auto* import = HostImportFor(context, address, bytes);
    if (import == nullptr || import->unwatched || !AgcDriver::GuestMemory::Watched(address, bytes)) {
        std::cout << "host import of the resident read block refused or unwatched: resident reads not tested\n";
        return;
    }
    const auto build = [&](std::size_t at, std::size_t count, bool written) {
        auto memory = std::make_unique<GuestBufferMemory>(context);
        memory->AllowDeviceStaging();
        if (written) memory->AddWritable(address + at, count);
        else memory->AddReadable(address + at, count);
        memory->UploadPrepare(false);
        memory->UploadFinish(false);
        return memory;
    };
    const auto fill = [&](Recorder& target, std::size_t at, std::uint32_t value) {
        const auto commands = target.Commands();
        RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_HOST_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
        context.Function<PFN_vkCmdFillBuffer>("vkCmdFillBuffer")(commands, import->buffer, address + at - import->base, 16, value);
        RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT);
    };
    // What the build binds for [at, at + count), copied to the host after everything recorded.
    const auto bound = [&](Recorder& target, const GuestBufferMemory& memory, std::size_t at, std::size_t count) {
        std::uint32_t adjustment = 0;
        const auto info = memory.Descriptor(address + at, count, adjustment);
        auto readback = std::make_shared<Buffer>(context, count, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
        const auto commands = target.Commands();
        RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_HOST_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
        CopyBuffer(context, commands, info.buffer, info.offset + adjustment, readback->Handle(), 0, count);
        RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT);
        target.Keep(readback);
        target.Submit();
        target.Sync();
        const auto copied = readback->Bytes();
        for (std::size_t index = 0; index < count; ++index) {
            if (std::to_integer<unsigned char>(copied[index]) != guest[at + index]) throw std::runtime_error("(r) the bound copy of the range differs from guest memory at offset " + std::to_string(at + index));
        }
    };
    const auto bufferOf = [&](const GuestBufferMemory& memory, std::size_t at, std::size_t count) {
        std::uint32_t adjustment = 0;
        return memory.Descriptor(address + at, count, adjustment).buffer;
    };
    constexpr std::size_t at = unit + 4096;
    constexpr std::size_t span = 8192;
    // Without the switch: in place.
    {
        auto plain = build(at, span, false);
        Require(plain->ServedInPlace(address + at) && !plain->ServedResident(address + at) && bufferOf(*plain, at, span) == import->buffer, "(r) a recorder without APS5_RESIDENT_READS bound a resident copy");
        recorder.Sync();
    }
    setSwitch("APS5_RESIDENT_READS", "1");
    setSwitch("APS5_RESIDENT_READS_MIN_KIB", "0");
    Recorder resident(context);
    setSwitch("APS5_RESIDENT_READS_MIB", "0");
    Recorder refusing(context);
    setSwitch("APS5_RESIDENT_READS_MIB", "");
    setSwitch("APS5_RESIDENT_READS_VERIFY", "1");
    Recorder verifying(context);
    setSwitch("APS5_RESIDENT_READS_VERIFY", "");
    setSwitch("APS5_RESIDENT_READS", "");
    setSwitch("APS5_RESIDENT_READS_MIN_KIB", "");
    Require(resident.KeepsResidentReads() && resident.ResidentReads().verifyEvery == 0 && verifying.ResidentReads().verifyEvery == 1 && refusing.ResidentReads().limitBytes == 0 && !recorder.KeepsResidentReads(), "APS5_RESIDENT_READS did not set up the recorders");
    resident.Activate();
    auto counts = ResidentReadCounts();
    const auto expect = [&](std::uint64_t ResidentReadStatistics::*field, std::uint64_t delta, const char* what) {
        const auto now = ResidentReadCounts();
        if (now.*field - counts.*field != delta) throw std::runtime_error(std::string("(r) ") + what + ": counted " + std::to_string(now.*field - counts.*field) + ", expected " + std::to_string(delta));
    };
    // The first build makes the copy and fills it.
    auto first = build(at, span, false);
    Require(first->ServedResident(address + at) && !first->ServedInPlace(address + at) && first->InPlaceReads().empty(), "(r) a read-only element was not bound from a resident copy");
    const auto copyBuffer = bufferOf(*first, at, span);
    Require(copyBuffer != import->buffer, "(r) the resident copy is the import");
    Require(first->DirectRegions().has_value() && first->DirectRegions()->size() == 1, "(r) a resident region is not keyed by its import for reuse");
    expect(&ResidentReadStatistics::made, 1, "copies made");
    expect(&ResidentReadStatistics::refreshFirst, 1, "first refreshes");
    bound(resident, *first, at, span);
    counts = ResidentReadCounts();
    // Unchanged: the next build takes the copy as it stands.
    auto second = build(at, span, false);
    Require(bufferOf(*second, at, span) == copyBuffer, "(r) a second build of the range made another copy");
    expect(&ResidentReadStatistics::hits, 1, "uses as they stood");
    expect(&ResidentReadStatistics::made, 0, "copies made by a reuse");
    // A CPU store into the range: refreshed.
    counts = ResidentReadCounts();
    guest[at + 100] = 0xEE;
    auto third = build(at, span, false);
    expect(&ResidentReadStatistics::refreshStamp, 1, "refreshes after a CPU store");
    bound(resident, *third, at, span);
    // A CPU store in another block and a noted GPU write elsewhere: standing.
    counts = ResidentReadCounts();
    guest[3 * unit + 5] = static_cast<unsigned char>(guest[3 * unit + 5] ^ 1u);
    resident.NotePendingWrite(address + 3 * unit + 64, 16, Recorder::WriteKind::Fill);
    auto fourth = build(at, span, false);
    expect(&ResidentReadStatistics::hits, 1, "uses as they stood after writes elsewhere");
    resident.Sync();
    // A GPU write the recorder notes (here a fill noted only, no stamp): refreshed after it.
    counts = ResidentReadCounts();
    fill(resident, at + 256, 0xA5A5A5A5u);
    resident.NotePendingWrite(address + at + 256, 16, Recorder::WriteKind::Fill);
    auto fifth = build(at, span, false);
    expect(&ResidentReadStatistics::refreshNoted, 1, "refreshes after a noted GPU write");
    bound(resident, *fifth, at, span);
    Require(guest[at + 256] == 0xA5 && guest[at + 271] == 0xA5, "(r) the noted fill did not land");
    // A queued label store over the range: it lands before the refresh copies the range.
    counts = ResidentReadCounts();
    const std::array<std::byte, 4> label{std::byte{0x11}, std::byte{0x22}, std::byte{0x33}, std::byte{0x44}};
    resident.RecordStore(import->buffer, address + at + 512 - import->base, label, address + at + 512);
    resident.NotePendingWrite(address + at + 512, 4, Recorder::WriteKind::Label);
    auto sixth = build(at, span, false);
    expect(&ResidentReadStatistics::refreshNoted, 1, "refreshes after a label store");
    Require(!resident.QueuedStoreOverlaps(address + at + 512, 4), "(r) a refresh left a queued label store over its range");
    bound(resident, *sixth, at, span);
    Require(guest[at + 512] == 0x11 && guest[at + 515] == 0x44, "(r) the label did not land");
    // An address-based use whose table may store over the range (here its store is a fill neither
    // noted nor stamped, as a BDA store until its batch completed): refreshed; one elsewhere: standing.
    counts = ResidentReadCounts();
    fill(resident, at + 1024, 0x5A5A5A5Au);
    NoteResidentReadsAddressWriter(std::make_shared<const std::vector<std::pair<std::uint64_t, std::uint64_t>>>(std::vector<std::pair<std::uint64_t, std::uint64_t>>{{address, address + 64}, {address + at + 1024, address + at + 1040}}), &resident);
    auto seventh = build(at, span, false);
    expect(&ResidentReadStatistics::refreshWriter, 1, "refreshes after an address-based writer");
    bound(resident, *seventh, at, span);
    counts = ResidentReadCounts();
    NoteResidentReadsAddressWriter(std::make_shared<const std::vector<std::pair<std::uint64_t, std::uint64_t>>>(std::vector<std::pair<std::uint64_t, std::uint64_t>>{{address + 3 * unit, address + 4 * unit}}), &resident);
    auto eighth = build(at, span, false);
    expect(&ResidentReadStatistics::hits, 1, "uses as they stood after an address-based writer elsewhere");
    // A reused build (Revalidate): a CPU store, then its own copy refreshed in place.
    counts = ResidentReadCounts();
    guest[at + 2000] = 0x77;
    Require(first->RecordResidentReads(resident), "(r) a reused build's copy was taken as demoted");
    expect(&ResidentReadStatistics::refreshStamp, 1, "refreshes of a reused build");
    Require(bufferOf(*first, at, span) == copyBuffer, "(r) a reused build's copy moved");
    bound(resident, *first, at, span);
    counts = ResidentReadCounts();
    Require(first->RecordResidentReads(resident), "(r) a reused build's copy was taken as demoted");
    expect(&ResidentReadStatistics::hits, 1, "reused builds' uses as they stood");
    // An address-based writer's note lasts while its batch is in flight: once the batch finished,
    // its stores are stamps (BdaResources::CheckFault marks the pages each use stored to). One
    // that stored elsewhere leaves the copy standing; one that stored over the range refreshes it
    // as a stamp, not as a writer.
    resident.Sync();
    Require(ResidentReadCounts().writers == 0, "(r) address-based writer notes outlived their batches");
    counts = ResidentReadCounts();
    const auto overRange = std::make_shared<const std::vector<std::pair<std::uint64_t, std::uint64_t>>>(std::vector<std::pair<std::uint64_t, std::uint64_t>>{{address, address + bytes}});
    RecordMemoryBarrier(context, resident.Commands(), VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
    NoteResidentReadsAddressWriter(overRange, &resident);
    NoteResidentReadsAddressWriter(overRange, &resident);
    Require(ResidentReadCounts().writers == 1, "(r) an address-based writer in flight was not listed once");
    resident.Submit();
    resident.Sync();
    Require(ResidentReadCounts().writers == 0, "(r) an address-based writer's note outlived its batch");
    auto ninth = build(at, span, false);
    expect(&ResidentReadStatistics::hits, 1, "uses as they stood after a finished address-based writer that stored elsewhere");
    counts = ResidentReadCounts();
    fill(resident, at + 1536, 0x3C3C3C3Cu);
    NoteResidentReadsAddressWriter(overRange, &resident);
    const auto storedPage = (address + at + 1536) & ~std::uint64_t{4095};
    resident.OnComplete([storedPage] { AgcDriver::GuestMemory::MarkWritten(storedPage, 4096); });
    resident.Submit();
    resident.Sync();
    auto tenth = build(at, span, false);
    expect(&ResidentReadStatistics::refreshStamp, 1, "refreshes after a finished address-based writer that stored over the range");
    expect(&ResidentReadStatistics::refreshWriter, 0, "writer refreshes after the writer's batch finished");
    bound(resident, *tenth, at, span);
    // A failed write-watch collect over the range (its dirty bits may be gone without stamps):
    // refreshed.
    counts = ResidentReadCounts();
    ResidentReadsFailCollectForTests(true);
    std::unique_ptr<GuestBufferMemory> eleventh;
    try {
        eleventh = build(at, span, false);
    } catch (...) {
        ResidentReadsFailCollectForTests(false);
        throw;
    }
    ResidentReadsFailCollectForTests(false);
    expect(&ResidentReadStatistics::refreshUnwatched, 1, "refreshes after a failed collect");
    bound(resident, *eleventh, at, span);
    // A batch submitted between a build's check and its record, whose completion stored into the
    // range: the record checks again and refreshes into the open batch; with no submit since, the
    // record checks nothing.
    counts = ResidentReadCounts();
    auto twelfth = build(at, span, false);
    expect(&ResidentReadStatistics::hits, 1, "uses as they stood before a recheck");
    twelfth->RecheckResidentReads(resident);
    expect(&ResidentReadStatistics::rechecks, 0, "rechecks with no submit since the check");
    const auto completionStore = at + 3000;
    resident.OnComplete([guest, completionStore] { guest[completionStore] = 0x5D; });
    resident.Submit();
    resident.Sync();
    Require(guest[at + 3000] == 0x5D, "(r) the completion store did not land");
    twelfth->RecheckResidentReads(resident);
    expect(&ResidentReadStatistics::rechecks, 1, "rechecks after a submit");
    expect(&ResidentReadStatistics::recheckRefreshes, 1, "refreshes of a recheck after a completion store");
    expect(&ResidentReadStatistics::refreshStamp, 1, "the recheck's refresh reason");
    expect(&ResidentReadStatistics::uses, 1, "uses counted by a recheck");
    bound(resident, *twelfth, at, span);
    // A written element, and a copy the cap refuses: in place.
    {
        auto written = build(2 * unit, 4096, true);
        Require(written->ServedInPlace(address + 2 * unit) && !written->ServedResident(address + 2 * unit), "(r) a written element was bound from a resident copy");
        resident.Sync();
        refusing.Activate();
        counts = ResidentReadCounts();
        auto refused = build(2 * unit + 8192, 4096, false);
        Require(refused->ServedInPlace(address + 2 * unit + 8192), "(r) a copy over the cap was bound");
        expect(&ResidentReadStatistics::refused, 1, "refused copies");
        refusing.Sync();
        resident.Activate();
    }
    // Refreshed at every use: demoted, the next build binds in place.
    counts = ResidentReadCounts();
    constexpr std::size_t churn = 2 * unit + 16384;
    std::unique_ptr<GuestBufferMemory> churned;
    for (int round = 0; round < 8; ++round) {
        guest[churn + static_cast<std::size_t>(round)] = static_cast<unsigned char>(round);
        churned = build(churn, 4096, false);
        Require(churned->ServedResident(address + churn), "(r) a churning range was not bound from its copy before its demotion");
        resident.Sync();
    }
    expect(&ResidentReadStatistics::demoted, 1, "demoted ranges");
    // A reused build holding the demoted copy fails its proof (the rebuild binds in place).
    Require(!churned->RecordResidentReads(resident), "(r) a reused build over a demoted copy was not rebuilt");
    expect(&ResidentReadStatistics::rebuiltDemoted, 1, "reused builds rebuilt over a demoted copy");
    churned.reset();
    {
        auto demoted = build(churn, 4096, false);
        Require(demoted->ServedInPlace(address + churn), "(r) a demoted range was bound from a copy");
    }
    // Verify: a standing copy compares equal; a write no tracker saw is caught.
    resident.Sync();
    verifying.Activate();
    counts = ResidentReadCounts();
    {
        auto checked = build(at, span, false);
        verifying.Submit();
        verifying.Sync();
        expect(&ResidentReadStatistics::verified, 1, "verified uses");
        expect(&ResidentReadStatistics::mismatched, 0, "mismatched uses of a current copy");
        counts = ResidentReadCounts();
        fill(verifying, at + 4096, 0xC3C3C3C3u);
        auto escaped = build(at, span, false);
        verifying.Submit();
        verifying.Sync();
        expect(&ResidentReadStatistics::mismatched, 1, "mismatched uses after an untracked write");
        // Tracked from here on: the next use refreshes.
        verifying.NotePendingWrite(address + at + 4096, 16, Recorder::WriteKind::Fill);
        counts = ResidentReadCounts();
        auto refreshed = build(at, span, false);
        expect(&ResidentReadStatistics::refreshNoted, 1, "refreshes after the noted write");
        bound(verifying, *refreshed, at, span);
    }
    resident.Activate();
    // A changed import (the range registered again): refreshed from the new import.
    resident.Sync();
    {
        GuestAllocations::Mutation mutation;
        mutation.Remove(block);
    }
    HostImportFor(context, address, bytes);
    {
        GuestAllocations::Mutation mutation;
        mutation.Add(block, bytes, true, true);
    }
    import = HostImportFor(context, address, bytes);
    Require(import != nullptr, "(r) the block was not imported again");
    counts = ResidentReadCounts();
    auto remapped = build(at, span, false);
    Require(remapped->ServedResident(address + at), "(r) the range was not bound from its copy after the import changed");
    expect(&ResidentReadStatistics::refreshImport, 1, "refreshes after a changed import");
    bound(resident, *remapped, at, span);
    std::cout << "resident read tests passed\n";
    resident.Sync();
    remapped.reset();
    first.reset();
    second.reset();
    third.reset();
    fourth.reset();
    fifth.reset();
    sixth.reset();
    seventh.reset();
    eighth.reset();
    ninth.reset();
    tenth.reset();
    eleventh.reset();
    twelfth.reset();
    // Nothing holds a copy now: dropping the cache's references frees them all (the device
    // teardown's ClearResidentReads).
    const auto left = ClearResidentReads();
    const auto after = ResidentReadCounts();
    if (left != 0 || after.liveBytes != 0 || after.entries != 0 || after.bytes != 0) throw std::runtime_error("(r) resident copies outlived their builds and the cache: " + std::to_string(left) + " live, " + std::to_string(after.liveBytes) + " live bytes");
    recorder.Activate();
}

// SpirvMayStoreThroughBda: a store or atomic through a physical storage buffer pointer counts,
// whatever the names; loads through one and stores to other storage classes do not; a store
// through a pointer the scan cannot type counts.
void bdaStoreScanTests() {
    constexpr std::uint32_t uint32 = 1, uint64 = 2, pointer = 3, zero = 4, seven = 5, voidType = 6, functionType = 7, function = 8, label = 9, address = 10, loaded = 11, scope = 12, semantics = 13, added = 14;
    const auto op = [](std::uint32_t opcode, std::uint32_t words) { return (words << 16u) | opcode; };
    // body: the instructions inside the function, after `address` was made from pointer type `storage`.
    const auto module = [&](std::uint32_t storage, bool fromVariable, std::initializer_list<std::uint32_t> body) {
        std::vector<std::uint32_t> words{0x07230203u, 0x00010500u, 0, 32, 0};
        words.insert(words.end(), {op(17, 2), 5347});  // OpCapability PhysicalStorageBufferAddresses
        words.insert(words.end(), {op(21, 4), uint32, 32, 0, op(21, 4), uint64, 64, 0});
        words.insert(words.end(), {op(32, 4), pointer, storage, uint32});
        words.insert(words.end(), {op(43, 5), uint64, zero, 0, 0, op(43, 4), uint32, seven, 7, op(43, 4), uint32, scope, 1, op(43, 4), uint32, semantics, 0});
        words.insert(words.end(), {op(19, 2), voidType, op(33, 3), functionType, voidType, op(54, 5), voidType, function, 0, functionType, op(248, 2), label});
        if (fromVariable) words.insert(words.end(), {op(59, 4), pointer, address, storage});
        else words.insert(words.end(), {op(120, 4), pointer, address, zero});
        words.insert(words.end(), body);
        words.insert(words.end(), {op(253, 1), op(56, 1)});
        return words;
    };
    constexpr std::uint32_t PhysicalStorageBuffer = 5349, FunctionStorage = 7;
    Require(SpirvMayStoreThroughBda(module(PhysicalStorageBuffer, false, {op(62, 5), address, seven, 2, 4})), "(s) a store through a physical pointer was not seen");
    Require(SpirvMayStoreThroughBda(module(PhysicalStorageBuffer, false, {op(234, 7), uint32, added, address, scope, semantics, seven})), "(s) an atomic through a physical pointer was not seen");
    Require(!SpirvMayStoreThroughBda(module(PhysicalStorageBuffer, false, {op(61, 6), uint32, loaded, address, 2, 4})), "(s) a load through a physical pointer counted as a store");
    Require(!SpirvMayStoreThroughBda(module(FunctionStorage, true, {op(62, 3), address, seven})), "(s) a store to a function variable counted as a BDA store");
    Require(SpirvMayStoreThroughBda(module(FunctionStorage, true, {op(62, 3), 31, seven})), "(s) a store through an untyped pointer did not count");
    Require(SpirvMayStoreThroughBda(std::vector<std::uint32_t>{0x07230203u}), "(s) a truncated module did not count as storing");
    std::cout << "BDA store scan tests passed\n";
}

// Sets (or, for an empty value, removes) an environment variable.
void setEnvironment(const char* name, const char* value) {
#ifdef _WIN32
    _putenv_s(name, value);
#else
    if (*value != '\0') setenv(name, value, 1);
    else unsetenv(name);
#endif
}

// Resident buffers over a shared view, the title's case (Windows, APS5_RESIDENT_BUFFERS=1 with
// APS5_GUARD_SHARED_VIEWS=1): the guest's GPU memory is a shared section mapped at a guest address,
// and its host import is a second, read-write mapping of the same pages (the alias, WindowsMappings::
// MapAlias). A resident copy's guard makes the guest's view of its pages no-access and leaves the
// alias alone, so the GPU's import still reads and writes them. To the guest the pages stay mapped:
// the driver's page queries (Accessible, DescribeCommitted, ReadCommitted: the page-state cache
// forgotten, as after any mapping change) and a new import of the range must see them so, and a read
// lands the GPU's bytes, visible through both mappings. t419: the queries read guarded pages as
// holes (zeros, skipped write-backs) and refused their imports for good.
void residentSharedViewTests(const Device& device, Recorder& recorder) {
#ifdef _WIN32
    const auto& context = device.GetContext();
    if (context.hostImportAlignment == 0 || !AgcDriver::GuestMemory::WriteWatched()) {
        std::cout << "host imports or the write-watched arena unavailable: resident shared views not tested\n";
        return;
    }
    constexpr std::size_t bytes = 65536;
    constexpr std::size_t page = 4096;
    HANDLE section = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_EXECUTE_READWRITE, 0, static_cast<DWORD>(bytes), nullptr);
    Require(section != nullptr, "cannot create the resident shared view test section");
    void* block = GuestArena::GuestArenaAllocate_nid_postfix(bytes, 65536);
    Require(block != nullptr, "cannot reserve the resident shared view test range");
    GuestArena::GuestArenaMap_nid_postfix(block, bytes, section, 0, PAGE_READWRITE);
    auto* guest = static_cast<volatile unsigned char*>(block);
    for (std::size_t at = 0; at < bytes; ++at) guest[at] = 0;
    const auto address = reinterpret_cast<std::uint64_t>(block);
    {
        GuestAllocations::Mutation mutation;
        mutation.Add(block, bytes, true, true);
    }
    const auto* import = HostImportFor(context, address, bytes);
    if (import == nullptr) {
        std::cout << "host import of the resident shared view test range refused: resident shared views not tested\n";
        {
            GuestAllocations::Mutation mutation;
            mutation.Remove(block);
        }
        GuestArena::GuestArenaReset_nid_postfix(block, bytes);
        GuestArena::GuestArenaRelease_nid_postfix(block, bytes);
        CloseHandle(section);
        return;
    }
    Require(import->alias != nullptr, "a shared view was imported without its read-write alias");
    setSwitch("APS5_GUARD_SHARED_VIEWS", "1");
    {
        setEnvironment("APS5_RESIDENT_BUFFERS", "1");
        Recorder resident(context);
        setEnvironment("APS5_RESIDENT_BUFFERS", "");
        resident.Activate();
        constexpr std::size_t sourceBytes = 65536;
        auto source = std::make_shared<Buffer>(context, sourceBytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
        const auto sourceByte = [](std::size_t at) { return static_cast<unsigned char>(at * 7 + 3); };
        for (std::size_t at = 0; at < sourceBytes; ++at) source->Bytes()[at] = std::byte{sourceByte(at)};
        const auto copyInto = [&](const HostImport* into, std::uint64_t at, std::uint64_t count) {
            return std::vector<Recorder::DeferredCopy>{Recorder::DeferredCopy{source, source.get(), source->Handle(), into->buffer, at, address + at - into->base, count, address + at}};
        };
        auto* guarded = static_cast<char*>(block) + page;
        // The page states are known (cached) before the guard, as the title's are.
        Require(AgcDriver::GuestMemory::Accessible(block, bytes, true), "the shared view test range is not accessible");
        // [100, 12388): pages 1 and 2 stay resident under a guard of the view, the rest land at Submit.
        resident.DeferCopies(copyInto(import, 100, 12288));
        resident.Submit();
        Require(resident.ResidentCopyBytes() == 2 * page, "Submit did not keep the whole pages of a shared view's copy-back resident (guard refused)");
        resident.Sync();
        const auto* alias = static_cast<const volatile unsigned char*>(import->alias);
        Require(guest[100] == sourceByte(100) && guest[12387] == sourceByte(12387) && alias[100] == sourceByte(100), "the partial pages of a shared view's resident copy did not land with its batch");
        Require(alias[page + 904] == 0, "the alias held a resident copy's bytes before anything landed them");
        // A mapping change elsewhere makes the page states be asked again: guarded, they are what the
        // guest maps (read-write), not holes.
        GuestAllocations::GuestAllocationsInvalidate_nid_postfix(address, bytes);
        // The answers before the fix (debug aid APS5_GUARD_PAGES_AS_HOLES=1): holes (t419).
        AgcDriver::GuestMemory::SetGuardedPagesAsHoles(true);
        const bool holes = !AgcDriver::GuestMemory::Accessible(guarded, 2 * page, true) && !AgcDriver::GuestMemory::DescribeCommitted(address, bytes, true).whole;
        AgcDriver::GuestMemory::SetGuardedPagesAsHoles(false);
        Require(holes, "APS5_GUARD_PAGES_AS_HOLES=1 did not read the guarded pages as holes");
        const auto queriesBefore = AgcDriver::GuestMemory::GuardedPageQueries();
        Require(AgcDriver::GuestMemory::Accessible(guarded, 2 * page, true), "a guarded page of a shared view read as inaccessible");
        Require(AgcDriver::GuestMemory::GuardedPageQueries() > queriesBefore, "the guarded page query was not counted");
        Require(AgcDriver::GuestMemory::DescribeCommitted(address, bytes, true).whole && AgcDriver::GuestMemory::CommittedWhole(address, bytes), "a shared view range with a guarded page read as sparse");
        // A read through the queries lands the bytes (it faults into the resolver) instead of zeros.
        std::vector<std::byte> read(2 * page);
        AgcDriver::GuestMemory::ReadCommitted(address + page, read);
        Require(read[904] == std::byte{sourceByte(page + 904)} && read[2 * page - 1] == std::byte{sourceByte(3 * page - 1)}, "a committed read of a guarded shared view page did not land the GPU's bytes");
        Require(alias[page + 904] == sourceByte(page + 904), "the landed bytes are not seen through the import's alias");
        // A write after the landing survives the next command, through both mappings.
        guest[page + 7] = 0xEE;
        static_cast<void>(resident.Commands());
        resident.Submit();
        resident.Sync();
        Require(guest[page + 7] == 0xEE && alias[page + 7] == 0xEE, "a resident copy a read landed was recorded again over a later CPU write");
        // The first decision with a copy queued after a mapping change records it (an import it
        // stores into may retire), so nothing stays resident then; the bytes land with that batch.
        const auto settle = [&](const HostImport* into, std::uint64_t at) {
            resident.DeferCopies(copyInto(into, at, page));
            resident.Submit();
            Require(resident.ResidentCopyBytes() == 0, "a copy-back queued after a mapping change was kept resident");
            resident.Sync();
            Require(guest[at + 5] == sourceByte(at + 5), "a copy-back recorded after a mapping change did not land");
        };
        settle(import, 12 * page);
        // A new import of the range (its registration changed) while a guard holds pages of it: the
        // alias import is made (before, it was refused for good), and the copy into the retired
        // import still lands.
        resident.DeferCopies(copyInto(import, 4 * page, 2 * page));
        resident.Submit();
        Require(resident.ResidentCopyBytes() == 2 * page, "a page-aligned copy-back of a shared view was not kept resident");
        {
            GuestAllocations::Mutation mutation;
            mutation.Remove(block);
        }
        {
            GuestAllocations::Mutation mutation;
            mutation.Add(block, bytes, true, true);
        }
        const auto* renewed = HostImportFor(context, address, bytes);
        Require(renewed != nullptr && renewed->alias != nullptr, "the import of a shared view with a guarded page was refused");
        resident.Submit();
        resident.Sync();
        Require(HostImportFor(context, address, bytes) == renewed, "the import made over a guarded page did not stay");
        for (std::size_t at = 4 * page; at < 6 * page; at += 509) {
            if (guest[at] != sourceByte(at)) throw std::runtime_error("a resident copy into a retired shared view import stored the wrong byte at offset " + std::to_string(at));
        }
        settle(renewed, 13 * page);
        // Recorded into the new import by a command: the bytes land through its alias.
        resident.DeferCopies(copyInto(renewed, 8 * page, 2 * page));
        resident.Submit();
        Require(resident.ResidentCopyBytes() == 2 * page, "a copy-back into the new import was not kept resident");
        static_cast<void>(resident.Commands());
        resident.Submit();
        resident.Sync();
        const auto* renewedAlias = static_cast<const volatile unsigned char*>(renewed->alias);
        Require(guest[8 * page + 11] == sourceByte(8 * page + 11) && renewedAlias[10 * page - 1] == sourceByte(10 * page - 1), "a resident copy recorded into a shared view's import did not land");
    }
    setSwitch("APS5_GUARD_SHARED_VIEWS", "");
    recorder.Activate();
    {
        GuestAllocations::Mutation mutation;
        mutation.Remove(block);
    }
    HostImportFor(context, address, bytes);
    GuestArena::GuestArenaReset_nid_postfix(block, bytes);
    GuestArena::GuestArenaRelease_nid_postfix(block, bytes);
    CloseHandle(section);
#endif
}

bool lineHas(const std::vector<std::string>& lines, const std::string& prefix, const std::string& part) {
    return std::any_of(lines.begin(), lines.end(), [&](const std::string& line) { return line.rfind(prefix, 0) == 0 && line.find(part) != std::string::npos; });
}

// The [gputime] digest (APS5_PROFILE_GPU) on synthetic ranges: the in-place byte counts, the
// program, class, queue and pass totals, the rows and the union, without a device.
void gpuTimingDigestTests() {
    using Ranges = std::vector<std::pair<std::uint64_t, std::uint64_t>>;
    constexpr std::uint64_t MiB = 1048576;
    const Ranges reads{{0x1000, 0x3000}, {0x2000, 0x4000}, {0x8000, 0x9000}};
    const Ranges writes{{0x3800, 0x8800}};
    const Ranges inputs{{0x0, 0x100}, {0x80, 0x200}};
    auto use = Recorder::InPlaceUseOf(reads, writes, inputs);
    Require(use.read == 0x4000 && use.written == 0x1000 && use.inputs == 0x200 && !use.leased, "InPlaceUseOf: wrong union or written bytes");
    use = Recorder::InPlaceUseOf(reads, writes, inputs, true);
    Require(use.read == 0 && use.written == 0 && use.inputs == 0x200 && use.leased, "InPlaceUseOf: an address-based build kept its reads");
    using Kind = Recorder::TimingKind;
    using Class = Recorder::CommandClass;
    const auto range = [](std::uint64_t key, Kind kind, std::uint32_t queue, std::uint64_t begin, std::uint64_t end) {
        Recorder::TimedRange timed;
        timed.key = key;
        timed.kind = kind;
        timed.queue = queue;
        timed.begin = begin;
        timed.end = end;
        return timed;
    };
    std::vector<Recorder::TimedRange> ranges;
    ranges.push_back(range(Recorder::BatchTimingKey, Kind::Batch, 0, 0, 10000));
    ranges.push_back(range(Recorder::ClassKey(Class::DispatchLeading), Kind::Class, 0, 0, 100));
    auto program = range(0x248994d00, Kind::Program, 0, 100, 2100);
    program.inPlaceRead = MiB;
    program.inPlaceWritten = MiB / 2;
    ranges.push_back(program);
    auto copyBack = range(Recorder::ClassKey(Class::StagingOut), Kind::Class, 0, 2100, 5100);
    copyBack.bytes = 2 * MiB;
    ranges.push_back(copyBack);
    ranges.push_back(range(Recorder::ClassKey(Class::Copy), Kind::Class, 1, 5100, 6100));
    auto transfer = range(0x1234000, Kind::Transfer, 1, 5200, 6000);
    transfer.bytes = 4096;
    ranges.push_back(transfer);
    auto pass = range(Recorder::ClassKey(Class::Draw), Kind::Class, 1, 6100, 9100);
    pass.target = 0x50000;
    pass.draws = 3;
    pass.inPlaceInputs = MiB;
    pass.inPlaceRead = 2 * MiB;
    ranges.push_back(pass);
    // Ended by Submit (a throw between Begin and End): it spans the rest of the batch.
    auto open = range(Recorder::ClassKey(Class::DccClear), Kind::Class, 1, 9100, 10000);
    open.leftOpen = true;
    ranges.push_back(open);
    GpuTimingDigest digest;
    // One tick is a microsecond: 1000 ticks are a millisecond.
    digest.AddBatch(ranges, 1000.0);
    digest.AddClass(Class::PresentBlit, 700000.0, 4096, 0xffffffffu);
    const auto approx = [](double value, double want) { return std::abs(value - want) < 1e-6; };
    Require(digest.Batches() == 1 && approx(digest.BatchMs(), 10) && approx(digest.DispatchMs(), 2) && approx(digest.ProgramMs(), 2.8) && approx(digest.ClassMs(), 7.8) && approx(digest.UnionMs(), 9.1), "digest: wrong sums");
    Require(digest.Dispatches().count == 1 && digest.Dispatches().read == MiB && digest.Dispatches().written == MiB / 2, "digest: wrong dispatch in-place bytes");
    Require(digest.Transfers().count == 1 && approx(digest.Transfers().ms, 0.8) && digest.Programs().count(0x1234000) == 1 && digest.Programs().count(0x248994d00) == 1, "digest: wrong program keys");
    Require(digest.Class(Class::StagingOut).bytes == 2 * MiB && approx(digest.Class(Class::StagingOut).ms, 3), "digest: wrong copy-back class");
    const auto& queues = digest.Queues();
    Require(queues.count(0) == 1 && queues.count(1) == 1 && queues.count(0xffffffffu) == 1, "digest: missing queues");
    Require(approx(queues.at(0).timedMs, 5.1) && approx(queues.at(0).programMs, 2) && approx(queues.at(0).classMs, 3.1) && approx(queues.at(0).batchMs, 10) && queues.at(0).batches == 1, "digest: wrong queue 0 split");
    Require(approx(queues.at(1).timedMs, 4) && approx(queues.at(1).programMs, 0) && approx(queues.at(1).classMs, 4) && queues.at(1).ranges == 3, "digest: wrong queue 1 split (a transfer counted as a dispatch?)");
    Require(digest.Passes().count(0x50000) == 1 && digest.Passes().at(0x50000).draws == 3 && digest.Passes().at(0x50000).inputs == MiB && approx(digest.Passes().at(0x50000).ms, 3), "digest: wrong draw pass");
    Require(digest.LeftOpen() == 1 && digest.Class(Class::DccClear).count == 0, "digest: a range left open entered the class totals");
    const auto lines = digest.Report(2, 4, 512, 3);
    Require(lines.size() == 15, "digest: wrong report line count");
    Require(lines.front().rfind("[gputime] 3 ms of GPU time in 1 batches", 0) == 0 && lines.front().find(" 0x248994d00 x1 2ms r1.0/w0.5MiB") != std::string::npos, "digest: wrong program line");
    Require(lineHas(lines, "[gputime] row busy:", "5.00 ms per present") && lineHas(lines, "[gputime] row programs:", "1.00 ms per present (20.0% of busy)"), "digest: wrong busy or programs row");
    Require(lines.front().find("4 ranges dropped at the 512 cap; 1 left open, ended at submit and not counted; 3 refused inside a render pass)") != std::string::npos, "digest: wrong dropped, left-open or refused counts");
    Require(lineHas(lines, "[gputime] row busy:", "untimed holds the work of x2.0 ranges dropped at the 512 cap and x0.5 left open"), "digest: the busy row lost the dropped ranges");
    Require(lineHas(lines, "[gputime] row copy-back:", "1.50 ms per present (30.0% of busy), x0.5, 1.00 MiB"), "digest: wrong copy-back row");
    Require(lineHas(lines, "[gputime] row barriers:", "0.05 ms per present") && lineHas(lines, "[gputime] row guest-transfers:", "0.50 ms per present") && lineHas(lines, "[gputime] row untimed:", "0.45 ms per present"), "digest: wrong barrier, guest or untimed row");
    Require(lineHas(lines, "[gputime] row present-blit:", "0.35 ms per present"), "digest: wrong present-blit row");
    Require(lineHas(lines, "[gputime] by queue", "0x0 timed 2.55") && lineHas(lines, "[gputime] by queue", "0x1 timed 2.00") && lineHas(lines, "[gputime] by queue", "untagged timed 0.35"), "digest: wrong queue line");
    Require(lineHas(lines, "[gputime] draw passes by target", "0x50000 1.50ms x0.5 1.5d 0.5/1.0MiB"), "digest: wrong draw pass line");
    digest.Clear();
    Require(digest.Batches() == 0 && digest.Programs().empty() && digest.Queues().empty() && digest.Passes().empty() && digest.Class(Class::StagingOut).count == 0, "digest: Clear left totals");
    std::cout << "gpu timing digest checked\n";
}

// APS5_PROFILE_GPU=1 runs (the timing switch is read once): a timed range leaves the queued
// copy-backs queued and the barrier-merge state as it was, and the ranges reach the digest with
// their queue tag, in-place bytes, pass target and kind.
void gpuTimingRecorderTests(const Device& device, Recorder& recorder) {
    if (!Recorder::GpuTimingEnabled()) {
        std::cout << "gpu timing off: recorded ranges not tested (run with APS5_PROFILE_GPU=1)\n";
        return;
    }
    const auto& context = device.GetContext();
    if (context.hostImportAlignment == 0) {
        std::cout << "host imports unavailable: gpu timing under coalescing not tested\n";
        return;
    }
    constexpr std::size_t bytes = 65536;
#ifdef _WIN32
    void* block = VirtualAlloc(nullptr, bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
#else
    void* block = std::aligned_alloc(65536, bytes);
#endif
    Require(block != nullptr, "cannot allocate the gpu timing test block");
    std::memset(block, 0, bytes);
    const auto address = reinterpret_cast<std::uint64_t>(block);
    {
        GuestAllocations::Mutation mutation;
        mutation.Add(block, bytes, true, true);
    }
    const auto* import = HostImportFor(context, address, bytes);
    if (import == nullptr) {
        std::cout << "host import of the gpu timing test block refused: gpu timing under coalescing not tested\n";
        return;
    }
    {
        setEnvironment("APS5_COALESCE_COPY_BACKS", "1");
        Recorder coalescing(context);
        setEnvironment("APS5_COALESCE_COPY_BACKS", "");
        Require(coalescing.CoalescesCopyBacks(), "APS5_COALESCE_COPY_BACKS=1 did not turn coalescing on");
        coalescing.Activate();
        auto source = std::make_shared<Buffer>(context, 4096, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
        for (std::size_t at = 0; at < 4096; ++at) source->Bytes()[at] = std::byte{static_cast<unsigned char>(at * 5 + 3)};
        using Class = Recorder::CommandClass;
        using Reason = Recorder::FlushReason;
        const auto before = Recorder::CopyBackCounts();
        coalescing.DeferCopies({Recorder::DeferredCopy{source, source.get(), source->Handle(), import->buffer, 0, address - import->base, 1024, address}});
        static_cast<void>(coalescing.CommandsKeepingCopyBacks());
        // A staging copy-in's range (GuestBufferMemory::recordGpuCopies times its pass this way).
        const auto stagingIn = coalescing.BeginGpuTiming(Class::StagingIn);
        Require(stagingIn != Recorder::NoTiming, "a timed range was not begun");
        Require(coalescing.HasDeferredCopies() && coalescing.DeferredCopyBytes() == 1024, "a timed range recorded the queued copy-backs");
        coalescing.EndGpuTiming(stagingIn, 64);
        // A program range on queue 7 with its in-place bytes.
        const auto tag = AgcDriver::GuestMemory::GpuLockThreadTag();
        AgcDriver::GuestMemory::TagGpuLockThread(7);
        const auto program = coalescing.BeginGpuTiming(0xabc000);
        AgcDriver::GuestMemory::TagGpuLockThread(tag);
        const std::vector<std::pair<std::uint64_t, std::uint64_t>> reads{{0x10000, 0x18000}}, written{{0x14000, 0x20000}};
        coalescing.NoteInPlace(program, Recorder::InPlaceUseOf(reads, written));
        coalescing.EndGpuTiming(program);
        // The barrier-merge state survives a range begun after the command that set it.
        constexpr VkAccessFlags marked = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
        coalescing.MarkCovered(marked);
        const auto fill = coalescing.BeginGpuTiming(Class::Fill);
        VkAccessFlags covered = 0;
        static_cast<void>(coalescing.CommandsKeepingCopyBacks(&covered));
        Require(covered == marked, "a timed range cleared the barrier-merge state");
        coalescing.EndGpuTiming(fill);
        // A pass of two draws, named by its first draw's target; a copy's transfer.
        const auto pass = coalescing.BeginGpuTiming(Class::Draw);
        Recorder::InPlaceUse drawUse;
        drawUse.inputs = 4096;
        coalescing.NoteDrawInPass(pass, 0x70000, drawUse);
        coalescing.NoteDrawInPass(pass, 0x80000, drawUse);
        coalescing.EndGpuTiming(pass);
        const auto transfer = coalescing.BeginTransferTiming(0xdef000);
        coalescing.EndGpuTiming(transfer, 256);
        Require(coalescing.HasDeferredCopies(), "the timed ranges recorded the queued copy-back");
        // A range left open (a throw between Begin and End): Submit ends it, or the reap would
        // wait for its query forever.
        static_cast<void>(coalescing.BeginGpuTiming(Class::DccClear));
        coalescing.Submit();
        coalescing.Sync();
        const auto counts = Recorder::CopyBackCounts();
        const auto flushes = [&](Reason reason) { return counts.flushes[static_cast<std::size_t>(reason)] - before.flushes[static_cast<std::size_t>(reason)]; };
        Require(flushes(Reason::Command) == 0 && flushes(Reason::Submit) == 1 && counts.passes - before.passes == 1, "profiling changed the copy-back passes");
        Require(static_cast<const volatile unsigned char*>(block)[100] == static_cast<unsigned char>(100 * 5 + 3), "the queued copy-back did not land at Submit");
        const auto totals = Recorder::GpuTimingTotals();
        Require(totals.Batches() >= 1 && totals.BatchMs() > 0, "the batch range was not read");
        Require(totals.Class(Class::StagingIn).count >= 1 && totals.Class(Class::StagingIn).bytes >= 64, "the staging-in range was not read");
        Require(totals.Programs().count(0xabc000) == 1 && totals.Programs().at(0xabc000).read == 0x8000 && totals.Programs().at(0xabc000).written == 0x4000, "the program range lost its in-place bytes");
        Require(totals.Queues().count(7) == 1 && totals.Queues().at(7).ranges >= 1, "the program range lost its queue tag");
        Require(totals.Passes().count(0x70000) == 1 && totals.Passes().at(0x70000).draws == 2 && totals.Passes().at(0x70000).inputs == 8192, "the draw pass lost its target or draws");
        Require(totals.Transfers().count >= 1 && totals.Programs().count(0xdef000) == 1, "the transfer range was not read as a transfer");
        Require(totals.LeftOpen() >= 1 && totals.Class(Class::DccClear).count == 0, "the range left open was not ended at Submit, or entered the class totals");
        // More ranges in one batch than the old cap of 512: none dropped.
        const auto cap = Recorder::GpuTimingRangeCap();
        if (cap >= 700) {
            const auto fills = totals.Class(Class::Fill).count;
            static_cast<void>(coalescing.Commands());
            for (int i = 0; i < 600; ++i) coalescing.EndGpuTiming(coalescing.BeginGpuTiming(Class::Fill));
            coalescing.Submit();
            coalescing.Sync();
            Require(Recorder::GpuTimingTotals().Class(Class::Fill).count == fills + 600, "ranges past 512 in one batch were dropped");
        } else {
            std::cout << "APS5_PROFILE_GPU_RANGES=" << cap << ": the large batch not tested\n";
        }
        // A range begun while a render pass is open (no caller does this) is refused and counted;
        // the pass ends at Submit as usual.
        if (!Recorder::GpuTimingFlushes()) {
            VkSubpassDescription subpass{};
            subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
            VkRenderPassCreateInfo passInfo{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
            passInfo.subpassCount = 1;
            passInfo.pSubpasses = &subpass;
            VkRenderPass renderPass = VK_NULL_HANDLE;
            Check(context.Function<PFN_vkCreateRenderPass>("vkCreateRenderPass")(context.device, &passInfo, nullptr, &renderPass), "vkCreateRenderPass");
            VkFramebufferCreateInfo framebufferInfo{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
            framebufferInfo.renderPass = renderPass;
            framebufferInfo.width = 1;
            framebufferInfo.height = 1;
            framebufferInfo.layers = 1;
            VkFramebuffer framebuffer = VK_NULL_HANDLE;
            Check(context.Function<PFN_vkCreateFramebuffer>("vkCreateFramebuffer")(context.device, &framebufferInfo, nullptr, &framebuffer), "vkCreateFramebuffer");
            const auto commands = coalescing.Commands();
            VkRenderPassBeginInfo begin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
            begin.renderPass = renderPass;
            begin.framebuffer = framebuffer;
            begin.renderArea.extent = {1, 1};
            context.Function<PFN_vkCmdBeginRenderPass>("vkCmdBeginRenderPass")(commands, &begin, VK_SUBPASS_CONTENTS_INLINE);
            coalescing.LeaveRenderPassOpen(0x5000, Recorder::NoTiming, true);
            const auto refused = Recorder::GpuTimingRefusedInPass();
            Require(coalescing.BeginGpuTiming(Class::Fill) == Recorder::NoTiming && Recorder::GpuTimingRefusedInPass() == refused + 1, "a range begun inside a render pass was timed");
            Require(coalescing.RenderPassOpen(), "a refused range ended the render pass");
            coalescing.Submit();
            coalescing.Sync();
            context.Function<PFN_vkDestroyFramebuffer>("vkDestroyFramebuffer")(context.device, framebuffer, nullptr);
            context.Function<PFN_vkDestroyRenderPass>("vkDestroyRenderPass")(context.device, renderPass, nullptr);
        }
    }
    recorder.Activate();
    {
        GuestAllocations::Mutation mutation;
        mutation.Remove(block);
    }
    HostImportFor(context, address, bytes);
    std::cout << "gpu timing under coalescing checked\n";
}

int main() {
    try {
        gpuTimingDigestTests();
        Device device;
        std::lock_guard gpu(GpuMutex());
        std::cout << "host imports " << (PrepareImportWatch(device.GetContext()) == ImportWatch::Unwatch ? "are compared" : "stay watched") << '\n';
        Recorder recorder(device.GetContext());
        recorder.Activate();
        // First: the 10 s [gputime] report clears the totals it reads.
        gpuTimingRecorderTests(device, recorder);
        readTrackingTests(device, recorder);
        writeSettledTests(device, recorder);
        pendingBlockTests(device, recorder);
        fastReaderPendingTests(recorder);
        fastReaderKnownValueTests(recorder);
        completionCountTests(device, recorder);
        afterRecordedWorkTests(device, recorder);
        batchStampTests(recorder);
        labelTests(recorder);
        lateLabelTests(recorder);
        unchangedSinceTests();
        closeRaceTests(device, recorder);
        keyProofTests(device, recorder);
        resourceReadTests(device, recorder);
        misalignedSnapshotTests(device, recorder);
        drawSnapshotReuseTests(device, recorder);
        drawSnapshotEvictionTests(device);
        drawInputReuseTests(device, recorder);
        RunResidentPresentTests(device.GetContext());
        storeRunTests(device, recorder);
        remappedImportTests(device);
        batchedImportTests(device);
        importMemoTests(device);
        samplerMemoTests(device);
        outOfVideoMemoryTests(device);
        movedMetadataTests(device, recorder);
        keysFillTests(device, recorder);
        unitShadowTests(device, recorder);
        storageRefreshTests(device, recorder, false);
        storageRefreshTests(device, recorder, true);
        importWatchTests(device);
        staleGenerationTests(device, recorder);
        importWindowTests(device, recorder);
        dataWordPositionsTests();
        ignoredWordBitsTests();
        bufferBaseWordsTests();
        dataRefreshTests(device, recorder);
        minLodTests(device, recorder);
        firstLayerViewTests(device, recorder);
        metadataPassTests(device, recorder);
        pendingKeyStoreTests(device, recorder);
        coalesceCopyBackTests(device, recorder);
        narrowCopyBackTests(device, recorder);
        pageGuardTests();
        sharedViewGuardTests();
        residentBufferTests(device, recorder);
        residentSharedViewTests(device, recorder);
        residentReadTests(device, recorder);
        bdaStoreScanTests();
        fastRingReclaimTests(device, recorder);
        templateRefreshRingTests(device, recorder);
        templateRefreshPatchTests(device, recorder);
        std::cout << "Recorder read tracking and label tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
