#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_GUESTBUFFERMEMORY_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_GUESTBUFFERMEMORY_HPP

#include "prx/libSceAgcDriver/Graphics/include/Resources.hpp"
#include "prx/libSceAgcDriver/Graphics/include/VideoMemory.hpp"
#include "BdaAbi.hpp"
#include "prx/libc/include/GuestAllocations.hpp"
#include <array>
#include <memory>
#include <cstdint>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace AgcDriver::Graphics {

void ShutdownGuestBufferWorkers();

struct GuestMemorySnapshot {
    std::uint64_t address;
    std::span<const std::byte> bytes;
};

// AddSnapshot's mismatch, with the old text: a captured region disagrees with the region serving
// it. `live`: the owner reads guest memory in place, so the memory changed since the capture (made
// before the device lock); otherwise two snapshots of one snapshot-backed range disagree.
// `pending`: recorded GPU work or a label still writes the range. Driver::draw catches it and runs
// the packet again (Draw.cpp, snapshotRetry); APS5_NO_SNAPSHOT_RETRY=1 drops the draw as before.
struct SnapshotStale : std::runtime_error {
    SnapshotStale(bool live, bool pending) : std::runtime_error("AGC graphics: guest snapshot differs from registered memory"), live(live), pending(pending) {}
    bool live;
    bool pending;
};

// Set on a thread, its AddSnapshot binds a mismatching range as it is (the live region, or the
// range's first snapshot) instead of throwing SnapshotStale: Driver::draw's last retry, which
// renders with the memory as it is, as hardware would.
bool& ThreadServesStaleSnapshots();

// APS5_PROFILE_DRAW: a draw packet run again after SnapshotStale, by the retry's level (1 a fresh
// capture, 2 the capture under the GPU mutex, 3 the mismatch served); `hit` when the attempt that
// threw bound a draw-cache hit's stage bytes. Counted on the [address-sync] line.
void CountSnapshotRetry(std::uint32_t level, bool hit);

// A guest allocation imported with VK_EXT_external_memory_host, usable by the GPU in place.
struct HostImport {
    std::uint64_t base;
    std::uint64_t bytes;
    VkBuffer buffer;
    VkDeviceMemory memory;
    VkDeviceAddress address;
    void* alias = nullptr;
    std::weak_ptr<const GuestAllocations::Range> range {};
    // Identity for the life of this import (see HostImportSerial); 0 until first asked for.
    std::uint64_t serial = 0;
    bool unwatched = false;
    // A span import (over several adjoining registered ranges): not in the per-range registry, so
    // no serial names it and recipes do not key on it.
    bool span = false;
    // The memory type it was imported into (~0u: unknown; the [retile] line names it).
    std::uint32_t memoryType = ~0u;
};

enum class ImportWatch : std::uint8_t { Watch, Unwatch };

struct ImportProbe {
    const char* failure = nullptr;
    VkResult result = VK_SUCCESS;
    std::uint32_t pages = 0;
    std::uint32_t writtenAtImport = 0;
    std::uint32_t writtenAfterSubmit = 0;
};

ImportProbe ProbeImportWriteProtection(const Context& context);
ImportWatch PrepareImportWatch(const Context& context);
void SetImportWatch(const Context& context, ImportWatch watch);

// The host import of the registered allocation containing [address, address + bytes), made on demand
// (alignment and budget permitting), or null. Bytes at `address` are at `address - import->base` in
// the import's buffer.
const HostImport* HostImportFor(const Context& context, std::uint64_t address, std::size_t bytes);
// HostImportFor of each [begin, end) of `ranges` in order. The leading ranges an existing import
// answers (registry unchanged since the last reconcile) are looked up under one hold of the import
// registry's lock, with no other thread's lookup, reconcile or retire between two of them; from the
// first miss or stale registry on, each range takes HostImportFor itself (its own hold, so a
// reconcile or an import's creation holds the lock as long as before). `visit(user, index, import)`
// receives what HostImportFor would return for range `index` and returns whether to go on: no range
// after the one it stops at is looked up (or imported). The visitor may run under the registry's
// lock, so it must not take it (no HostImport* call) and should be short.
using HostImportVisitor = bool (*)(void* user, std::size_t index, const HostImport* import);
void HostImportsFor(const Context& context, std::span<const std::pair<std::uint64_t, std::uint64_t>> ranges, HostImportVisitor visit, void* user);

// A thread's memo of the imports HostImportFor answered it last (docs/design/draw-fastpath.md 2.5,
// s53-fastdraw-diag step 2c). An entry answers a range inside its import while the registry is as
// it was when the entry was noted: the same device, the same registry generation
// (GuestAllocationsGeneration) and the same import epoch (bumped by every retire and by a device
// change). HostImportFor would then find that very import without reconciling: the imports of one
// generation are of disjoint registered ranges (an import whose range changed was retired when
// the generation was reconciled) and none was dropped since. Entries are noted only after a
// lookup that left the registry reconciled for the device.
struct HostImportMemo {
    struct Entry {
        VkDevice device = VK_NULL_HANDLE;
        std::uint64_t generation = 0;
        std::uint64_t epoch = 0;
        std::uint64_t begin = 0;
        std::uint64_t end = 0;
        const HostImport* import = nullptr;
    };
    static constexpr std::size_t Entries = 4;
    std::array<Entry, Entries> entries{};
    std::size_t next = 0;
    std::uint64_t hits = 0;
    std::uint64_t misses = 0;
    // The noted import holding [address, address + bytes) under exactly (device, generation,
    // epoch), or null.
    const HostImport* Find(VkDevice device, std::uint64_t generation, std::uint64_t epoch, std::uint64_t address, std::uint64_t bytes) const;
    // Remembers `import` as found under (device, generation, epoch), over the oldest entry.
    void Note(VkDevice device, std::uint64_t generation, std::uint64_t epoch, const HostImport& import);
};
HostImportMemo& ThreadHostImportMemo();
// HostImportFor through the calling thread's HostImportMemo: a hit takes no registry mutex. The
// fast draw (its bindings through HostImportResolver, its in-place inputs and indirect records
// through CopyDrawInput's and indirectPathFor's flags) and the fast dispatch's records (and its
// elements with APS5_FAST_DISPATCH_PER_ELEMENT=1; batched, they take HostImportsFor) use it; Draw
// keeps HostImportFor. APS5_NO_IMPORT_MEMO=1 calls HostImportFor.
// A hit reads the registry generation and the published epoch without the registry mutex, so a
// caller must be serialized with every reconcile and retire (GuestMemory::GpuMutex, held by every
// caller), as any user of a HostImportFor pointer already must be: an epoch bump that follows an
// erase on another thread would not stop a hit on the erased import.
const HostImport* HostImportMemoized(const Context& context, std::uint64_t address, std::size_t bytes);

// Whether an existing import covers [address, address + bytes), without reconciling the imports
// with the registry or making one (HostImportFor may take a registry lease): a hint for choices
// made outside the device lock (a sampled texture's path, a dispatch's pre-sync); the path taken
// re-checks with HostImportFor when it binds the import.
bool HostImportCovers(const Context& context, std::uint64_t address, std::size_t bytes);
// The existing import covering the range, as HostImportCovers finds it (nothing reconciled or
// made): its buffer and base, copied under the registry lock.
bool HostImportExisting(const Context& context, std::uint64_t address, std::size_t bytes, VkBuffer& buffer, std::uint64_t& base);
// The import registry's epoch, bumped by every retire (an atomic load, no lock).
std::uint64_t HostImportsEpoch();
// Whether a readable registered allocation contains [address, address + bytes) right now (one
// registry lease): a storage image whose memory was freed or re-registered has nothing to store to.
bool RegisteredReadableCovers(std::uint64_t address, std::size_t bytes);
// Whether a dispatch build would stage a written (or atomic) element of `bytes` in device-local
// memory (the written-shadow window, the atomic cap; see GuestBufferMemory.cpp) instead of binding
// it in place: what the fast dispatch declines until it stages itself (docs/design/draw-fastpath.md
// 2.9). A merged region's size can differ from one element's, so this is the element's answer.
bool DeviceStagingWanted(std::uint64_t bytes, bool atomic);

// A persistent device copy of one registered range of the main guest image (which cannot be host
// imported); see GuestBufferMemory.cpp.
struct ImageMirror;
class Recorder;

// Identity of the import serving [address, address + bytes): a serial unique for the life of one
// import, 0 when no import serves the range. An equal serial later means the same VkBuffer still
// backs the range (a re-import gets a new serial), which is what a descriptor set written against it
// needs to stay valid (see the ShaderResources cache). With `reconcile`, imports whose registered
// range changed are dropped first, as an upload does; without it (right after an upload, before the
// work using the import is recorded) the imports are left as they are.
std::uint64_t HostImportSerial(const Context& context, std::uint64_t address, std::size_t bytes, bool reconcile);

// Resident read-only copies (Recorder::KeepsResidentReads, APS5_RESIDENT_READS=1; see
// GuestBufferMemory.cpp "Resident reads"). A copy stands for its range while nothing wrote the
// range since its last refresh: no CPU store (write watch, CollectWrites/UnchangedSince; a failed
// collect counts as one), no driver or GPU write stamped (MarkWritten), no GPU write the recorder
// noted (every shader write, fill, copy/DMA, label, texture store, shadow publish and DCC key
// store: the hook below) and no address-based use still in flight that may store through its BDA
// table over it (the writer list; once its batch finished, its stores are stamped).
struct ResidentCopy;
struct ResidentReadStatistics {
    // Uses of a resident region; served by the copy as it stood, or after a refresh by reason
    // (`refreshUnwatched`: the range's write-watch collect failed, so a CPU store may be unseen).
    std::uint64_t uses = 0, hits = 0;
    std::uint64_t refreshFirst = 0, refreshStamp = 0, refreshNoted = 0, refreshImport = 0, refreshWriter = 0, refreshUnwatched = 0;
    std::uint64_t refreshedBytes = 0, servedBytes = 0;
    // Copies made, evicted from the cache (a holder keeps using its own), refused by the device or
    // the cap, regions left in place (demoted ranges whose copies were refreshed far more often
    // than used as they stood); noted GPU writes and address-based uses that staled a copy.
    std::uint64_t made = 0, madeBytes = 0, evicted = 0, refused = 0, demoted = 0, inPlace = 0;
    std::uint64_t notedStale = 0, notedVisited = 0, writerUses = 0, rebuiltDemoted = 0;
    // Checks made again because the batch changed between a build's check and its dispatch's
    // record (a sync in between, RecheckResidentReads), and the refreshes they recorded.
    std::uint64_t rechecks = 0, recheckRefreshes = 0;
    // APS5_RESIDENT_READS_VERIFY: uses compared on completion, and those whose copy differed
    // from the import.
    std::uint64_t verified = 0, mismatched = 0;
    // The video memory guard (VideoMemory, APS5_VRAM_GUARD): copies refused for the budget, copies
    // evicted by its trims that no build held (and their bytes) and that builds held (their builds
    // are rebuilt), cached copies made before its last recycle and made anew, and reused builds
    // rebuilt over a recycled copy, over a trimmed one, and after an episode ended that refused
    // them a copy.
    std::uint64_t refusedBudget = 0, trimmed = 0, trimmedBytes = 0, trimmedHeld = 0, trimmedHeldBytes = 0, recycled = 0;
    std::uint64_t rebuiltRecycled = 0, rebuiltTrimmed = 0, rebuiltRefused = 0;
    // The cache now: copies and their allocated bytes (BufferPool::Capacity); the bytes of every
    // live copy (cached, or evicted and still held by a build); address-based writers in flight.
    std::uint64_t entries = 0, bytes = 0, liveBytes = 0, writers = 0;
};
ResidentReadStatistics ResidentReadCounts();
// Whether any resident copy was made (one relaxed load): the recorder's write notes call
// NoteResidentReadsWrite only then.
bool ResidentReadsLive();
// A GPU write over [begin, end) the recorder noted: every cached copy over it is stale.
void NoteResidentReadsWrite(std::uint64_t begin, std::uint64_t end);
// An address-based use, recorded by `recorder` into its open batch (serial Submissions() + 1),
// that may store anywhere in `ranges` (sorted, disjoint [begin, end); null: anywhere). Its stores
// are stamped only when that batch's completions ran (BdaResources::CheckFault), so until then a
// copy over the ranges is refreshed at its next use (after it, in queue order); the note is
// dropped when the batch finished (NoteResidentReadsFinished). Kept by reference: the same list
// of the same recorder again updates its entry.
void NoteResidentReadsAddressWriter(std::shared_ptr<const std::vector<std::pair<std::uint64_t, std::uint64_t>>> ranges, const Recorder* recorder);
// Recorder::finish: every batch of recorder `recorderId` up to `serial` finished, completions
// included, so the address-based writers recorded into them are stamped.
void NoteResidentReadsFinished(std::uint64_t recorderId, std::uint64_t serial);
// Drops every cached copy (a holder keeps its own): tests, and the device teardown. Returns the
// copies still alive (held by builds).
std::size_t ClearResidentReads();
// Video-memory budget ("resident" reclaimer, a safe point): drops the cached copies nothing else
// holds (no build), least recently used first, until about `want` bytes went; by try_lock.
VkDeviceSize TrimResidentReads(VkDeviceSize want, std::uint64_t& evicted);
// Video-memory budget ("shadows" reclaimer, any thread): drops staging-chain registry entries
// whose shadow nothing else holds (no build, no batch, no queued copy-back), so the shadow goes
// back to the pool; the next use of such a range copies from its import, as after the registry's
// own reset. By try_lock.
VkDeviceSize TrimStagedShadows(VkDeviceSize want, std::uint64_t& evicted);
VramCacheCensus StagedShadowCensus();
// The device teardown: drops the staging-chain registry's entries whose shadows belong to
// `device` (they go back to its pool before the pool and the device go).
void ClearStagedShadows(VkDevice device);
// Tests: the resident checks take every write-watch collect as failed.
void ResidentReadsFailCollectForTests(bool fail);
// The video memory guard's trim (VideoMemory::SetResidentTrimmer, registered with the first copy):
// see VideoMemory::ResidentTrimmer. A held copy it evicts is marked trimmed, so a reused build
// holding it is rebuilt (RecordResidentReads) and its memory goes with the last holder.
VideoMemory::ResidentTrim TrimResidentReads(std::uint64_t bytes);

// The import table's identity (the fast Revalidate): serials proved under one identity stand
// while the table still has it, i.e. the same device, the same epoch (bumped by every retire)
// and the registry generation it was reconciled with, which must still be the live one.
struct HostImportsProof {
    VkDevice device = VK_NULL_HANDLE;
    std::uint64_t epoch = 0;
    std::uint64_t refreshedGeneration = 0;
};
// Whether the table still matches `proof` (one lock); false for an empty or stale proof.
bool HostImportsUnchanged(const Context& context, const HostImportsProof& proof);
// The table's identity when it is reconciled with the live registry (one lock), else empty.
HostImportsProof HostImportsIdentity(const Context& context);

// Identity of the read-only image mirror serving [address, address + bytes) (the counterpart of
// HostImportSerial for main-image ranges, which no import serves): a serial unique for the life of
// one mirror with its top bit set, 0 when no read-only mirror whose registered range still exists
// covers the range. A read-only mirror's bytes and VkBuffer never change, so a descriptor set written
// against it stays valid while the serial is unchanged.
std::uint64_t ImageMirrorSerial(const Context& context, std::uint64_t address, std::size_t bytes);

// Deferred lease release (see GuestBufferMemory.cpp). An address-based build leases every readable
// registered allocation until its write-back, which runs when its batch completed; the guest's
// unmap/mprotect/free of a leased allocation waits for that through the registry's pin waiter, which
// submits the recorder and finishes its batches up to the newest one recorded with a lease. SyncLeaseWork
// says whether the build's work must instead be synced as soon as it is recorded
// (APS5_SYNC_LEASE_DISPATCH=1, the behaviour before deferral). The driver and the draw path count
// their choice with CountLeaseOutcome right after the lease-holding resources were kept by the open
// batch: `batchSerial` is that batch's serial (Recorder::Submissions() + 1 under the device lock), 0
// when synced, and the waiter targets the newest of them. Under APS5_PROFILE_DRAW CountLeaseOutcome
// prints the [address-sync] leases line every 10 s from LeaseCounters (cumulative).
bool SyncLeaseWork();
void CountLeaseOutcome(bool synced, std::uint64_t batchSerial);
struct LeaseStats {
    std::uint64_t deferred = 0;
    std::uint64_t synced = 0;
    // Pin-contention waits made by guest threads (the waiter calls), how many of them finished the
    // newest lease batch, how many had to drain the whole recorder instead (no lease batch noted),
    // how many only dropped the cached address space (no GPU wait, see AddressSpaceStats), and
    // their total time.
    std::uint64_t contentionWaits = 0;
    std::uint64_t contentionSyncs = 0;
    std::uint64_t contentionDrains = 0;
    std::uint64_t cacheDrops = 0;
    double contentionMs = 0;
};
LeaseStats LeaseCounters();

// The cached address space of address-based builds (see GuestBufferMemory.cpp): builds served by
// the cached space, rebuilds by reason, builds whose space could not be published (an import
// retired while it was built) and the pin waiter's drops (cumulative). `enabled` is false under
// APS5_NO_ADDRESS_SPACE_CACHE=1, when every build takes the per-build path.
struct AddressSpaceStats {
    bool enabled = true;
    std::uint64_t hits = 0;
    std::uint64_t rebuiltFirst = 0;
    std::uint64_t rebuiltGeneration = 0;
    std::uint64_t rebuiltEpoch = 0;
    std::uint64_t rebuiltDevice = 0;
    std::uint64_t rebuiltWaiterDrop = 0;
    std::uint64_t unpublished = 0;
    // Builds that left the space for the per-build path: a region partially overlapping one of
    // its regions, or an import retired under it before the upload.
    std::uint64_t dissolvedOverlap = 0;
    std::uint64_t dissolvedImports = 0;
    std::uint64_t waiterDrops = 0;
};
AddressSpaceStats AddressSpaceCounters();
// Lease reuse outcomes (ShaderResources::RearmLease), printed in the [address] line.
// Rebased: a rearm that moved read-only V#s (counted with Rearmed too); Rebase: a rearm refused
// because a moved V# left the space (ShaderResources::rebaseLease).
enum class LeaseReuse { Rearmed, Busy, Space, Proof, Rebase, Rebased };
void CountLeaseReuse(LeaseReuse outcome);

struct MirrorStats {
    std::uint64_t heapMirrors = 0;
    std::uint64_t heapBytes = 0;
    std::uint64_t rebuilds = 0;
    std::uint64_t blocksCopied = 0;
    std::uint64_t heapRefills = 0;
};
MirrorStats MirrorCounters();
void ClearImageMirrors(VkDevice device);

struct AddressCopy {
    std::uint64_t begin;
    std::uint64_t end;
    std::uint64_t committed;
    const char* reason;
};
std::string AddressCopyOverflow(std::vector<AddressCopy> copies, std::uint64_t limit);

class GuestBufferMemory {
public:
    explicit GuestBufferMemory(const Context& context);
    ~GuestBufferMemory();
    void AcquireRegistered();
    // APS5_PROFILE_DRAW: the parts of one address-based build ([address] line every 10 s): the
    // registry lease, the imports pass, the mirror preparation and the mirror compare (blocks and
    // copied blocks) are timed by AcquireRegistered on the calling thread; the caller adds the
    // snapshot compares it made after it (ShaderResources::prepareAddressBindings).
    static void CountAddressBuild(double snapshotsUs);
    // Narrow copy-backs' baselines (APS5_NARROW_COPY_BACKS; counted under APS5_PROFILE_DRAW), totals
    // since start: refreshed at a copy-in from the import, inherited along the staging chain (and
    // of those untrusted), copy-backs recorded narrow and whole, and trust lost to a claim broken
    // since the chain (Recorder::ClaimBreaks).
    struct NarrowTrustStatistics {
        std::uint64_t refreshed = 0, inherited = 0, inheritedUntrusted = 0, narrow = 0, whole = 0, claimBroken = 0;
    };
    static NarrowTrustStatistics NarrowTrustCounts();
    // A guest range bound through a descriptor. Both read live guest memory at upload and bind the
    // same way (a storage buffer); AddWritable also notes the range in Writes(), so it gets the
    // write-back's reference copy, a write-back, the recorder's pending-write note and the
    // direct-write marks. AddReadable is for an element the shader is proved never to store to
    // (DescriptorBinding::bufferWritten): the CPU never has to wait for it. `atomic` marks an
    // element the shader updates atomically (DescriptorBinding::bufferAtomic), staged in device
    // memory whatever its size (see AllowDeviceStaging).
    void AddWritable(std::uint64_t address, std::size_t bytes, bool atomic = false);
    void AddReadable(std::uint64_t address, std::size_t bytes);
    // Device-local staging of written and atomic elements inside host imports (see
    // GuestBufferMemory.cpp): allowed only for a build whose every use records its work and then
    // calls RecordCopyBacks (a dispatch), since a staged region's results reach guest memory by
    // that copy alone. Call before Upload.
    void AllowDeviceStaging() { stagingAllowed = true; }
    // Records the copy-in of every staged region anew for another use of this upload (a resource
    // cache hit, from ShaderResources::Revalidate, under GuestMemory::GpuMutex, once the imports
    // were confirmed unchanged): the previous use's copy-back left the shadow behind, and the next
    // RecordCopyBacks copies it back again. Nothing to do without staged regions.
    void RecordStagingCopies(Recorder& recorder);
    // Resident read-only copies (Recorder::KeepsResidentReads): checks every region this upload
    // binds from a resident copy for another use (ShaderResources::Revalidate, after the imports
    // were confirmed unchanged, under GuestMemory::GpuMutex) and records the refresh of each stale
    // one from its import into the open batch, ahead of the work. Nothing without such regions.
    // False (nothing recorded) when a copy it binds was demoted since: the caller rebuilds, and
    // the new build binds that range in place.
    bool RecordResidentReads(Recorder& recorder);
    // The work is about to be recorded (VulkanDevice::recordDispatch): when `recorder` submitted
    // since the last check (a sync between the build or proof and the record, as an indirect
    // dispatch's CPU path makes, whose completions may have stored into a range), every resident
    // region is checked again and a stale copy refreshed into the open batch. Nothing otherwise.
    void RecheckResidentReads(Recorder& recorder);
    // An address-based use is about to be recorded by `recorder` (ShaderResources::Bind): the
    // ranges its BDA table lets it store to stale the resident copies over them while it is in
    // flight (NoteResidentReadsAddressWriter). The list is built once per build.
    void NoteAddressWriter(const Recorder* recorder) const;
    // Whether the region owning `address` is bound from a resident read-only copy.
    bool ServedResident(std::uint64_t address) const;
    void AddSnapshot(const GuestMemorySnapshot& snapshot);
    // Upload is the two stages below back to back. UploadPrepare needs no device lock: it merges the
    // regions, binds the image mirrors and host imports that already serve them (an import pointer is
    // re-checked against the registry epoch later) and copies read-only regions. UploadFinish runs
    // under GuestMemory::GpuMutex: it reconciles and makes imports (retiring one hands its buffer to
    // the recorder), refreshes writable mirrors and flushes pending results (both may wait for
    // recorded work) and copies the regions a descriptor may write, so the window between that copy
    // and the recorder's pending-write note stays closed.
    void Upload(bool addressable);
    void UploadPrepare(bool addressable);
    void UploadFinish(bool addressable);
    VkDescriptorBufferInfo Descriptor(std::uint64_t address, std::size_t bytes, std::uint32_t& adjustment) const;
    // Whether the region owning `address` is read in place from its host import (Descriptor binds
    // the import), not a copy or a mirror.
    bool ServedInPlace(std::uint64_t address) const;
    std::vector<ShaderRecompiler::BdaAbi::Range> AddressRanges() const;
    // The BDA table of the cached address space when it serves this upload alone (an address-based
    // build with no region outside it): its ranges, immutable while the space lives, and the
    // space's serial, by which BdaResources reuses the table buffer built for it without a
    // compare. Nothing when a region of this build is not in the space (AddressRanges then merges).
    struct CachedTable {
        std::uint64_t serial;
        const std::vector<ShaderRecompiler::BdaAbi::Range>* ranges;
    };
    std::optional<CachedTable> CachedAddressTable() const;
    // The registered memory an address-based build maps, shared by consecutive builds (see
    // GuestBufferMemory.cpp): its lease and the sorted, immutable regions of the ranges served in
    // place by imports and mirrors. Opaque outside GuestBufferMemory.cpp.
    struct AddressSpace;
    void WriteBack();
    bool WritesOverlap(std::uint64_t address, std::size_t bytes) const;
    // Guest ranges the shader may write through descriptors, as [begin, end).
    const std::vector<std::pair<std::uint64_t, std::uint64_t>>& Writes() const { return writes; }
    // Whether any written range lives in a copied buffer, so a write-back must run once the GPU is done.
    bool HasCopiedWrites() const;
    // Reports writes into host-imported memory (made by the GPU in place, or copied back into it by
    // RecordCopyBacks) to the write tracking now.
    void MarkDirectWrites() const;
    // Records, into the recorder's open batch, the copy of every written sub-range of a region the
    // GPU copied out of a host import (see Region::gpuCopy) back into the import: what the shader
    // wrote lands in guest memory by the GPU, ordered after the recorded work, so the region needs
    // no CPU write-back (HasCopiedWrites no longer counts it) and the caller notes the ranges as
    // pending writes and marks them as MarkDirectWrites does. Called right after the work using the
    // regions was recorded (ShaderResources::MarkGpuWrites), under GuestMemory::GpuMutex. A use that
    // never calls it (a synchronous draw) stores the staging bytes from the CPU in WriteBack.
    void RecordCopyBacks(Recorder& recorder);
    // Whether registered allocations are pinned until write-back (address-based shaders): by this
    // build's own lease, or by the cached address space it holds.
    bool HoldsLease() const { return !lease.empty() || space != nullptr; }
    // Lease reuse (ShaderResources::RearmLease): a committed build of shape 0 (LeaseShape) takes
    // the cached space it was built over again for another use while that space is still the
    // current one (the same serial, registry generation, imports epoch and device, no import
    // stale): its descriptors and BDA table then name the same imports and nothing is uploaded.
    // False leaves the object as it was. DropRearmed undoes a rearm whose proof failed (the object
    // is not recorded); SpaceSerial is the held space's serial (0: none); SpaceEpochCurrent checks
    // the held space's imports epoch again (last, after the proof's flushes, as UploadFinish does).
    bool RearmSpace(std::uint64_t serial);
    void DropRearmed();
    std::uint64_t SpaceSerial() const;
    bool SpaceEpochCurrent() const;
    // Rebased lease templates (default; APS5_NO_LEASE_REBASE=1 off): the descriptor a read-only V# moved to
    // [address, address + bytes) gets in the rearmed space, as Descriptor gives it, when the range
    // lies inside one base range served in place or by a fixed mirror (shape 0); false (nothing
    // changed) otherwise, without throwing.
    bool RebasedDescriptor(std::uint64_t address, std::size_t bytes, VkDescriptorBufferInfo& info, std::uint32_t& adjustment) const;
    // APS5_PROFILE_DRAW: how far an uploaded address-based build is from one a later dispatch could
    // share: 0 the cached space alone, every base range served in place or by a fixed mirror; 1 the
    // same plus per-build regions (V#s, snapshots, copied ranges); 2 a writable or heap mirror in the
    // space; 3 no cached space (the build's own lease).
    int LeaseShape() const;
    void traceDissolve(const char* kind, std::uint64_t begin, std::uint64_t end) const;
    // Every uploaded region as [begin, end) when all of them are served by host imports, in place or
    // through a device-local staging copy of the import (nothing was copied through the CPU, so the
    // upload can serve a later identical build), else nothing.
    std::optional<std::vector<std::pair<std::uint64_t, std::uint64_t>>> DirectRegions() const;
    // Every region the recorded work reads in place through a host import, as [begin, end): the
    // regions bound in place (`direct`), an address-based build's leased heaps included. A region
    // the GPU copies out of an import (gpuCopy) notes its read itself when the copy is recorded.
    // For the recorder's read tracking (ShaderResources::MarkGpuWrites); nothing once committed.
    std::vector<std::pair<std::uint64_t, std::uint64_t>> InPlaceReads() const;
    // The same ranges listed into `out` (cleared first, keeping its capacity), as a span over it:
    // a per-draw query lists them into a thread's scratch instead of a fresh vector.
    std::span<const std::pair<std::uint64_t, std::uint64_t>> InPlaceReads(std::vector<std::pair<std::uint64_t, std::uint64_t>>& out) const;
    // The in-place reads split for the recorder's read sets (Recorder::NotePendingReadSet): the
    // build's own regions listed into `out` as InPlaceReads does, and the shared address space's
    // ranges (an address-based build: sorted and merged, immutable, the same list for every build
    // of the space) returned by reference, or null without a space. Nothing once committed.
    std::shared_ptr<const std::vector<std::pair<std::uint64_t, std::uint64_t>>> InPlaceReadSet(std::vector<std::pair<std::uint64_t, std::uint64_t>>& out) const;
    // The device-local bytes of the regions' staging shadows: alone, those nothing but this
    // object holds; shared, those one other holder also holds (see
    // ShaderResources::DeviceBytesHeldAlone).
    VramHeld DeviceBytesHeldAlone() const;

private:
    struct Region {
        std::uint64_t begin;
        std::uint64_t end;
        bool writable;
        std::vector<std::byte> snapshot;
        // Shared so a recorded GPU copy into it (see gpuCopy) can keep it in the batch itself, before
        // the caller keeps the whole resources: a throw between the two would otherwise return it to
        // the pool under an unsubmitted copy command.
        std::shared_ptr<Buffer> buffer;
        // Guest bytes as uploaded; write-back only stores bytes the GPU changed.
        std::vector<std::byte> uploaded {};
        // Registered allocation that is imported: its bytes are read from live guest memory, not a snapshot.
        bool hostBacked = false;
        // Set when the region is served by an imported allocation; nothing is copied or written back.
        const HostImport* direct = nullptr;
        // Set when the region covers uncommitted pages: only the `backed` parts (possibly none) are guest
        // memory; the rest reads as zeros and is never stored.
        bool sparse = false;
        std::vector<std::pair<std::uint64_t, std::uint64_t>> backed {};
        // Set when the region is served by an image mirror: nothing is copied; writable mirrors are
        // written back by comparing with the mirror's shadow. Kept alive here for recorded work.
        std::shared_ptr<ImageMirror> mirror {};
        // The mirror is a descriptor sub-range one found by UploadPrepare (not a lease mirror), which
        // UploadFinish confirms still has its Range before the region binds it.
        bool subrangeMirror = false;
        // Set by UploadPrepare when UploadFinish still has device-lock work for the region: a copy a
        // descriptor writes, a writable mirror's refresh, an import to reconcile or make, or the
        // pending-results flush of an import it took.
        bool pending = false;
        // The region lies inside a host import but is misaligned for binding in place: `buffer` is
        // filled by a vkCmdCopyBuffer from the import recorded into the batch (no CPU read, so no
        // flush-hook wait), from `copySource` (the import's VkBuffer, alive until the batch
        // completed, with the guest address of its first byte). `copiedBack` once RecordCopyBacks
        // recorded the written sub-ranges' copies back into the import.
        bool gpuCopy = false;
        VkBuffer copySource = VK_NULL_HANDLE;
        std::uint64_t copySourceBase = 0;
        bool copiedBack = false;
        // Staging chain: the tracker generation at this use's copy-in (after collecting the
        // range), against which MarkDirectWrites proves nothing else wrote the range during the
        // use before it registers the shadow.
        std::uint64_t chainGeneration = 0;
        // Deferred copy-backs: the shadow whose deferred copy-backs this use claimed (it took that
        // shadow through the staging chain); its copy-back takes them over (RecordCopyBacks).
        const void* claimedShadow = nullptr;
        // An element the shader updates atomically lies inside (AddWritable's `atomic`).
        bool atomic = false;
        // The gpuCopy buffer is a device-local staging shadow (see stagingEligible): no host
        // mapping, so nothing is ever stored from it by the CPU, and the region is taken even when
        // the import could bind it in place.
        bool deviceLocal = false;
        // The device refused the shadow (memory or allocations exhausted): the region takes the
        // path it would take without staging, in both upload stages.
        bool unstaged = false;
        // Narrow copy-backs (Recorder::NarrowsCopyBacks): a staging shadow made with the switch on
        // keeps a baseline of its range `baselineDelta` bytes behind its first byte (0: none), which
        // stands for what the import holds of the range: refreshed from the shadow after every
        // copy-in from the import, copied along with a chained shadow, and kept so by every copy-back
        // (the compare pass, or a whole copy that mirrors its bytes into it). `baselineTrusted` while
        // it does (a chained shadow passes its trust on); `baselineInherited` when it came with a
        // chained shadow of another buffer whose copies this use claimed at `claimBreaks`
        // (Recorder::ClaimBreaks): a claimed copy recorded meanwhile updated that buffer's baseline,
        // not this one's, so the copy-back then copies whole and the trust is gone until the next
        // copy-in from the import. `copySourceAddress`: the import's device address of
        // `copySourceBase` (0: unknown, copied whole).
        VkDeviceSize baselineDelta = 0;
        bool baselineTrusted = false;
        bool baselineInherited = false;
        std::uint64_t claimBreaks = 0;
        VkDeviceAddress copySourceAddress = 0;
        // A descriptor crossing back-to-back imported ranges of the address space (see spannable):
        // it overlaps the space's base ranges, so it stays out of the BDA table (the base imports
        // serve those addresses) and binds one span import over the same host pages.
        bool span = false;
        // Resident reads (Recorder::KeepsResidentReads): the device-local copy Descriptor binds in
        // the import's place (`direct` stays the import: the copy's source, and the identity the
        // region is reused under), the import's serial when it was taken, and its VkBuffer and
        // base in `copySource`/`copySourceBase`.
        std::shared_ptr<ResidentCopy> resident {};
        std::uint64_t residentSerial = 0;
    };

    // How [begin, end) lies against the space's base regions.
    enum class BaseOverlap { None, Inside, Partial };
    BaseOverlap baseOverlap(std::uint64_t begin, std::uint64_t end, const Region** owner) const;
    // Whether a descriptor partially overlapping the base regions can be served by a span import
    // instead of dissolving the space: it starts inside a base region and every base region it
    // crosses is imported and adjoins the next (APS5_NO_SPAN_IMPORT=1 dissolves as before).
    bool spannable(std::uint64_t begin, std::uint64_t end) const;
    // Copies the space's base regions into this build's own regions (today's per-build form) and
    // drops the space: for a region that partially overlaps a base region, and for a build whose
    // imports changed under the space (`resolve`: the direct regions are re-resolved by UploadFinish).
    void dissolveSpace(bool resolve);
    // The region containing `address`: this build's own regions (sorted after UploadPrepare) or
    // the space's base regions, or null.
    const Region* owner(std::uint64_t address) const;
    // Builds this build's regions from a lease as the per-build path always did: `base` gets the
    // ranges served in place (imports, mirrors), `copied` the ones copied per build.
    struct CopiedRange {
        std::uint64_t begin;
        std::uint64_t end;
        bool writable;
    };
    void addCopiedRange(const CopiedRange& range);
    // The BDA table entry of an uploaded region.
    static ShaderRecompiler::BdaAbi::Range addressRange(const Region& region);
    void validate(std::uint64_t address, std::size_t bytes) const;
    // The region of a descriptor-bound range (AddWritable/AddReadable), committed pages only. A
    // range inside a base region of the space adds nothing: the region serves it, as today's merge
    // of the two did.
    void addDescriptorRegion(std::uint64_t address, std::size_t bytes, bool atomic);
    // Gives a region a buffer of its own with its bytes (guest memory for host-backed and writable
    // ranges, plus the write-back's reference copy for a range a descriptor writes; else its snapshot).
    void copyRegion(Region& region, bool addressable);
    // A staged region's device-local shadow (null when the device refuses it), with a baseline
    // behind its bytes when the active recorder narrows copy-backs (Region::baselineDelta).
    std::shared_ptr<Buffer> stagingShadow(Region& region, bool addressable) const;
    // Whether a region inside a host import that cannot be bound in place is copied by the GPU
    // instead of the CPU (see Region::gpuCopy): live guest bytes, not sparse, at most the size
    // APS5_GPU_COPY_MAX_KIB allows, and APS5_CPU_COPIES unset.
    bool gpuCopyEligible(const Region& region) const;
    // Whether a region inside a host import is staged in device memory instead of bound in place
    // (see GuestBufferMemory.cpp): staging allowed, not address-based, a written element inside,
    // and an atomic element or a size within the written-shadow window. Independent of the import,
    // so UploadPrepare and UploadFinish decide alike.
    bool stagingEligible(const Region& region, bool addressable) const;
    // Records the import-to-buffer copies of the given gpuCopy regions into the open batch, with
    // the barriers that order them after earlier recorded writes and before the shaders reading them.
    void recordGpuCopies(std::span<Region* const> copies, bool addressable);
    // Resident reads: whether a region UploadFinish binds in place from `entry` may take a
    // resident copy instead (switch on, a dispatch build (AllowDeviceStaging) not address-based,
    // no descriptor of the build writes it, a watched non-span import, within the size window; the
    // caller checked the alignment and the write watch), and the check-and-refresh of the given
    // resident regions (RecordResidentReads).
    bool residentEligible(const Region& region, const HostImport& entry, bool addressable, const Recorder* recorder) const;
    void recordResidentReads(Recorder& recorder, std::span<Region* const> resident, bool recheck = false);
    void allocateRegionBuffer(Region& region, bool addressable);
    void takeHeapReferences();
    Context context;
    bool stagingAllowed = false;
    GuestAllocations::Lease lease;
    // The cached address space this build maps through (its lease pins the ranges); `regions` then
    // holds only the regions outside it (V#s, snapshots, ranges copied per build).
    std::shared_ptr<const AddressSpace> space;
    // Import registry epoch when `direct` pointers were taken at acquire time; they are reused while
    // no import was destroyed since.
    std::uint64_t importsEpoch = 0;
    std::vector<Region> regions;
    // Whether `regions` is in ascending address order (true right after AcquireRegistered, whose
    // regions follow the registry's order), so AddSnapshot can search instead of scanning.
    bool regionsSorted = false;
    // Some region is bound from a resident read-only copy (RecordResidentReads has work), and the
    // recorder and its submission count at the last check (RecheckResidentReads).
    bool residentRegions = false;
    // A resident copy was refused for the video memory budget: VideoMemory::Rounds() + 1 at the
    // refusal (0: none). Once an episode ended since, the reused build is rebuilt to take copies.
    std::uint64_t residentBudgetRound = 0;
    const Recorder* residentCheckedBy = nullptr;
    std::uint64_t residentCheckedAt = 0;
    // NoteAddressWriter's list for this build (the space's writable ranges, merged with the
    // build's own when it has any), made at its first use after an upload, for the space it held.
    mutable std::shared_ptr<const std::vector<std::pair<std::uint64_t, std::uint64_t>>> addressWriterRanges;
    mutable const AddressSpace* addressWriterSpace = nullptr;
    mutable bool addressWriterKnown = false;
    // Some region of `regions` is a span (Region::span): owner() prefers the span holding an address
    // over the base range that starts later, so a view or write-back crossing base ranges finds it.
    bool spansHeld = false;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> writes;
    std::vector<std::pair<std::uint64_t, std::vector<std::byte>>> heapReferences;
    // UploadPrepare ran (regions are frozen); `uploaded` once UploadFinish ran.
    bool prepared = false;
    bool uploaded = false;
    bool committed = false;
};

}

#endif
