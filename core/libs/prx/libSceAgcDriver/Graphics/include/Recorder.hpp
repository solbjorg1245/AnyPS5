#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_RECORDER_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_RECORDER_HPP

#include "prx/libSceAgcDriver/Graphics/include/Context.hpp"
#include "prx/libc/include/HostMutex.hpp"
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <array>
#include <deque>
#include <mutex>
#include <functional>
#include <list>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace AgcDriver::Graphics {

class Buffer;
class GpuTimingDigest;

// Accumulates GPU work across guest commands so the CPU does not wait for each one. Dispatches and the
// copies that feed them record into one open batch; Submit sends it to the queue without waiting and
// Sync waits for every batch, then runs its completion actions (write-backs) in order. Objects handed
// to Keep live until the batch that recorded them completed. Guest memory ranges the recorded work
// will write are noted, so a CPU access to such a range (the GuestMemory flush hook) syncs first.
// Every call happens under GuestMemory::GpuMutex, except WaitSerial and the static lock-free readers
// below. The noted ranges are also published as an immutable snapshot (rebuilt under the mutex at
// every note and after a batch's completions ran) that the flush hook reads without the mutex, so
// accesses overlapping nothing never wait behind device work.
//
// Submissions are numbered (serials) and, when the device has timeline semaphores, every Submit
// signals a timeline semaphore with its serial: a thread can then wait for "everything submitted
// up to serial S" WITHOUT the mutex (WaitSerial), because the monotonic value cannot be reset or
// recycled the way the pooled batch fences are, and afterwards run the completions under the mutex
// (FinishUpTo). Without timeline semaphores callers use the locked Sync as before.
class Recorder {
public:
    // `timelineSemaphores`: the device enabled VK_KHR_timeline_semaphore (WaitSerial is usable).
    explicit Recorder(const Context& context, bool timelineSemaphores = false);
    ~Recorder();
    Recorder(const Recorder&) = delete;
    Recorder& operator=(const Recorder&) = delete;

    // The open batch's command buffer, starting a batch when none is open. `coveredAccess`, when
    // given, receives the destination access mask of the last recorded command's trailing barrier
    // when that barrier had ALL_COMMANDS as its destination stage (MarkCovered), else 0: every
    // write recorded so far is then available and visible to those accesses of any later stage,
    // so a dispatch, fill or copy whose leading barrier asks for a subset of them may skip it
    // (the [barriers] line counts the merges). The call clears the mask (whatever the caller
    // records is assumed uncovered until it marks); a new batch starts uncovered.
    VkCommandBuffer Commands(VkAccessFlags* coveredAccess = nullptr);
    // After a trailing barrier with ALL_COMMANDS as its destination stage: `access` is its
    // destination access mask.
    void MarkCovered(VkAccessFlags access);
    void MarkShaderReadsCovered() { MarkCovered(VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT); }
    // A recorded draw's render pass (Draw.cpp) is left open after the draw: the next draw of the
    // same attachments (`key`: the views and the extent) continues it when nothing was recorded in
    // between and the earlier draw allowed it (`continuable`: it wrote nothing but its
    // attachments, so no barrier is owed inside the pass), and anything else recorded first ends
    // it (vkCmdEndRenderPass, one trailing barrier for the pass, the end of the draw class range
    // `timing`): Commands(), RecordStore and Submit end it; CommandsInRenderPass hands a
    // continuing draw the command buffer without ending it.
    bool ContinuesRenderPass(std::uint64_t key) const;
    // The serial of the render pass currently left open (0: none). While it stays the same, only
    // draws of that pass were recorded (anything else ends it): StorageTexture::Refresh's memo.
    std::uint64_t OpenRenderPassSerial() const { return open != nullptr && open->renderPass.open ? open->renderPass.serial : 0; }
    // The active recorder's, for a caller holding the GPU mutex (0 otherwise, or without a recorder).
    static std::uint64_t ActiveOpenRenderPassSerial();
    VkCommandBuffer CommandsInRenderPass();
    void LeaveRenderPassOpen(std::uint64_t key, std::uint32_t timing, bool continuable);
    // DCC "uncompressed" key stores (DccMetadata.cpp StoreUncompressedOnGpu): queued on the open
    // batch and recorded as one run (one barrier pair for every queued fill) at Submit, before a
    // label store (RecordStore), or before a command that writes or reads a queued range (the
    // fill must precede a later key writer or reader in the batch: FlushKeyStores by the callers
    // that know their ranges). `seed` (kept) supplies an
    // unaligned head or tail; [begin, end) is the guest range. Debug aid: APS5_DCC_KEYS_EACH=1
    // records every store at once, as before (QueueKeyStore then records and returns).
    void QueueKeyStore(VkBuffer buffer, VkDeviceSize first, VkDeviceSize last, std::shared_ptr<void> seed, std::uint64_t begin, std::uint64_t end);
    bool HasQueuedKeyStores() const { return open != nullptr && !open->keyStores.empty(); }
    bool QueuedKeyStoreOverlaps(std::uint64_t address, std::size_t bytes) const;
    // Whether `overlaps(begin, end)` holds for a queued store's range.
    bool AnyQueuedKeyStore(const std::function<bool(std::uint64_t, std::uint64_t)>& overlaps) const;
    void FlushKeyStores();
    void FlushKeyStoresOverlapping(std::uint64_t address, std::size_t bytes) { if (HasQueuedKeyStores() && QueuedKeyStoreOverlaps(address, bytes)) FlushKeyStores(); }
    bool Recording() const { return open != nullptr; }
    bool RenderPassOpen() const { return open != nullptr && open->renderPass.open; }
    // Deferred copy-backs of staged buffers (GuestBufferMemory::RecordCopyBacks): a dispatch's
    // written sub-ranges go from its device-local shadow back into the host import, but the copies
    // are recorded only before the next other command (Commands()), store or key-store run, or at
    // Submit, so a label never lands before them and everything recorded later reads the import
    // after them. When the next staging build takes the same shadow (the staging chain), it takes
    // these copies over (TakeDeferredCopies) and copies the union back after its own work: the
    // ~970 back-to-back uses per frame of one 285 KiB buffer then cost one copy-back, not 970
    // (with two full barriers each). Never across batches: Submit records what is left.
    // Opt-in (APS5_DEFER_COPY_BACK=1) while a device loss seen with it on remains unexplained
    // (t175: 5 of 7 runs lost the device ~2 min into gameplay with it, none of 3 without); the
    // default records every copy-back at once, as before. It saves GPU time only (t166: staging
    // class time 1400 -> 870 ms per 10 s), and the frame is CPU-bound.
    struct ResidentGuard;
    struct DeferredCopy {
        std::shared_ptr<void> keep;
        const void* sourceKey = nullptr;
        VkBuffer source = VK_NULL_HANDLE;
        VkBuffer destination = VK_NULL_HANDLE;
        VkDeviceSize sourceOffset = 0;
        VkDeviceSize destinationOffset = 0;
        VkDeviceSize bytes = 0;
        // The guest range the copy stores.
        std::uint64_t address = 0;
        // A resident copy's guard over its host pages (KeepsResidentBuffers); empty otherwise.
        std::shared_ptr<ResidentGuard> guard {};
        // Narrow copy-backs (NarrowsCopyBacks): the device addresses of the source's and the
        // destination's first bytes, and the distance from a source byte to its baseline in the
        // same buffer (0: no baseline, a plain copy). A copy with a baseline stores only the dwords
        // whose source differs from the baseline, and sets the baseline to them (see
        // GuestBufferMemory's staging shadows); trimming moves both offsets alike, so the baseline
        // follows.
        VkDeviceAddress sourceAddress = 0;
        VkDeviceAddress destinationAddress = 0;
        VkDeviceSize baselineOffset = 0;
    };
    static bool DeferCopyBacks();
    // Copy-back coalescing (APS5_COALESCE_COPY_BACKS=1, read when the recorder is made; without it
    // everything here behaves as above). Copy-backs are deferred, and the queued ones stay queued
    // past the commands that do not touch their bytes, so only the last copy of a guest byte per
    // epoch (up to the next point that records them) lands: a copy deferred over bytes an earlier
    // queued copy stores drops them from that one (nothing read them in between, or the reader
    // would have recorded it first), so queued copies never overlap and record in any order.
    // What records them (docs/design/resident-memory.md, "Copy-back coalescing"):
    // - every queued copy, claimed ones too: Commands() (a caller that checks nothing), a label
    //   store or store run, a key-store run, Submit (before the batch's host barrier);
    // - those a dispatch reads or writes through the imports (bound in place, its indirect
    //   arguments; every one for an address-based build), before it takes CommandsKeepingCopyBacks
    //   (VulkanDevice recordDispatch);
    // - those over a range a staging build copies in from its import (all claims made first; a
    //   region's own claimed copies stay: it copies from that shadow), before its copy pass takes
    //   CommandsKeepingCopyBacks (GuestBufferMemory::recordGpuCopies).
    // The CPU reads a copied range only after its batch completed: the use's pending-write notes
    // make a CPU access sync the batch (Submit records the copies first), and the title's pollers
    // see a label only after the copies recorded ahead of its store.
    bool CoalescesCopyBacks() const { return coalesceCopyBacks; }
    bool DefersCopyBacks() const { return coalesceCopyBacks || DeferCopyBacks(); }
    // Narrow copy-backs (APS5_NARROW_COPY_BACKS=1, read when the recorder is made; off with the old
    // deferral alone, APS5_DEFER_COPY_BACK without coalescing, whose queued copies may overlap, and
    // on a device without buffer device addresses). A staged region's copy-back copies the whole
    // written element range (a V# the shader may write: ~285-360 KiB per use of the hot append
    // kernels, ~680 MiB per present before coalescing, 5.1-5.7 GB per 10 s after it), whatever
    // part of it the shader stored. With the switch a staging shadow keeps a baseline (what the
    // import holds of its range) behind its bytes, and its copy-back is a small compute pass that
    // compares the two on the device and stores over PCIe only the dwords that differ (updating the
    // baseline), so only the bytes the shader changed cross the bus (under profiling the
    // [barriers] digest's "narrow copy-backs ... MiB stored"). Unaligned edges, copies without a
    // usable baseline and a device without the pass copy whole as before (keeping the baseline).
    // The shadows' side is GuestBufferMemory's (allocation, baseline refresh at copy-in, trust).
    bool NarrowsCopyBacks() const { return narrowCopyBacks && !narrowFailed.load(std::memory_order_relaxed); }
    // Bumped whenever queued copies some build claimed are recorded or released before that build
    // took them over (TakeClaimedCopies): a shadow whose baseline was copied from a claimed
    // shadow no longer knows what the import holds then (GuestBufferMemory checks it).
    std::uint64_t ClaimBreaks() const { return claimBreaks; }
    // Records `copies` now as one pass (lead barrier, plain and narrow copies, trail barrier), as a
    // flush of deferred copies would: the immediate copy-back of a use whose regions copy narrow.
    void RecordCopyBacksNow(std::vector<DeferredCopy> copies);
    // The narrow copies of a pass as compare spans: copies back to back in their shadow and import
    // with the same baseline join (one compare dispatch instead of one per written range); the
    // rest stay as they are. Sorted by shadow, import and offset.
    static std::vector<DeferredCopy> NarrowSpans(std::vector<DeferredCopy> copies);
    // Resident buffers (APS5_RESIDENT_BUFFERS=1, which turns coalescing on; docs/design/
    // resident-memory.md S3): at Submit or a label, the whole host pages of a queued unclaimed
    // copy stay queued past the batch (its partial pages at either end are recorded as before):
    // they are made inaccessible (GuestPageGuard*) and wait for the next point that records them
    // (a command, a dispatch or copy-in over them, a label store over them, a changed mapping,
    // APS5_RESIDENT_BUFFER_BATCHES or the APS5_RESIDENT_BUFFER_MIB cap) or for the build that takes
    // their shadow over. A CPU access to such a page (the title's plain reads too) faults, and
    // ResolveResidentFault lands the bytes from the shadow with a submission of its own; a copy so
    // landed is not recorded again (the CPU may write the bytes after it). A guard stays until the
    // batch recording its copy completed.
    bool KeepsResidentBuffers() const { return residentBuffers; }
    // Resident read-only copies (APS5_RESIDENT_READS=1, read when the recorder is made; docs/design/
    // resident-memory.md S3, GuestBufferMemory.cpp "Resident reads"): a dispatch build binds an
    // element range it only reads, inside a host import, from a device-local copy instead of the
    // import over PCIe; the copy is refreshed from the import (recorded into the open batch) only
    // when guest memory changed under it. The settings: APS5_RESIDENT_READS_MIB (cap of the cached
    // copies, default 1024), APS5_RESIDENT_READS_MIN_KIB / APS5_RESIDENT_READS_MAX_KIB (the region
    // sizes taken, default 4 / 16384) and APS5_RESIDENT_READS_VERIFY=N (every Nth use served
    // without a refresh, 1: each one, also copies the import and the copy to the host; they are
    // compared when the batch completed).
    struct ResidentReadSettings {
        bool enabled = false;
        std::uint32_t verifyEvery = 0;
        std::uint64_t limitBytes = 0;
        std::uint64_t minBytes = 0;
        std::uint64_t maxBytes = 0;
    };
    bool KeepsResidentReads() const { return residentReads.enabled; }
    const ResidentReadSettings& ResidentReads() const { return residentReads; }
    // Whether any recorder was made with APS5_RESIDENT_READS (one relaxed load): what only the
    // resident reads use (an address space's writable ranges, a program's BDA store scan) is
    // prepared only then.
    static bool ResidentReadsConfigured();
    // This recorder's identity (never reused, unlike its address).
    std::uint64_t Id() const { return id; }
    std::uint64_t ResidentCopyBytes() const;
    static bool ResolveResidentFault(std::uintptr_t address);
    // ResolveResidentFault's work on the calling thread, under the GPU mutex it takes. The fault
    // handler's call runs it on the resolver thread unless the faulting thread holds the mutex
    // (the faulting stack may be a title's 16 KiB job fiber, too small for a submission).
    static bool ResolveResidentFaultHere(std::uintptr_t address);
    // Totals since start (relaxed): copies made resident, those recorded later and those dropped
    // after a fault, guards refused and flushes at a changed mapping; faults resolved (those under
    // the GPU lock, those that found every copy landed already) and given up.
    struct ResidentStatistics {
        std::uint64_t made, madeBytes, recorded, skipped, refused, remapFlushes, faults, underLock, resolved, landed, forced;
        // Faults on a guard whose copy a submitted batch still running records: waited for, not
        // copied again (a copy from the guard's shadow after that batch could undo the newer copy
        // the build that took the shadow over recorded there).
        std::uint64_t waitedLanding = 0;
    };
    static ResidentStatistics ResidentCounts();
    // Commands() that leaves the queued copy-backs alone under coalescing (the caller recorded
    // those its command touches); Commands() otherwise.
    VkCommandBuffer CommandsKeepingCopyBacks(VkAccessFlags* coveredAccess = nullptr);
    bool HasDeferredCopies() const { return !deferredCopies.empty() || !claimedCopies.empty(); }
    // Why queued copy-backs were recorded (the [barriers] digest under deferral).
    enum class FlushReason : std::uint8_t { Submit = 0, StoreRun, Command, Dispatch, DispatchLease, CopyIn, Count };
    static const char* FlushReasonName(FlushReason reason);
    // Records now the queued copies `selected` picks (`claimed`: from the claimed list).
    void FlushDeferredWhere(const std::function<bool(const DeferredCopy&, bool claimed)>& selected, FlushReason reason);
    // The queued copies over [address, address + bytes), except those claimed for `exceptSource`.
    void FlushDeferredOverlapping(std::uint64_t address, std::size_t bytes, const void* exceptSource, FlushReason reason);
    void FlushDeferredCopies(FlushReason reason) { flushDeferredCopies(true, reason); }
    // Bytes the queued copies (claimed ones too) would store.
    std::uint64_t DeferredCopyBytes() const;
    // Totals since start (relaxed): copies deferred, copies dropped whole and bytes dropped as
    // stored again by a later copy before any reader (coalescing only), copies recorded and the
    // passes that recorded them (two barriers each), by reason.
    struct CopyBackStatistics {
        std::uint64_t deferred, deferredBytes, overwritten, overwrittenBytes, recorded, recordedBytes, passes;
        std::array<std::uint64_t, static_cast<std::size_t>(FlushReason::Count)> flushes;
        // Narrow copy-backs: copies recorded with a baseline, the spans they made (NarrowSpans),
        // the bytes compared, spans copied whole (source and import off by bytes modulo 4, no
        // pass on the device), compare dispatches, and the bytes the passes stored (counted only
        // under APS5_PROFILE_DRAW or APS5_PROFILE_GPU; passes of batches still running count later).
        std::uint64_t narrow = 0, narrowSpans = 0, narrowBytes = 0, narrowWhole = 0, narrowDispatches = 0, narrowStoredBytes = 0;
        // APS5_NARROW_VERIFY: dwords whose import differed from a shadow equal to its baseline.
        std::uint64_t narrowStale = 0;
    };
    static CopyBackStatistics CopyBackCounts();
    void DeferCopies(std::vector<DeferredCopy> copies);
    // A staging build that takes the shadow `sourceKey` (staging chain) claims its deferred copies:
    // Commands() leaves claimed copies alone while the build records its work (a store run, a key
    // store run and Submit still record them, as does a later build staging an overlapping range:
    // FlushClaimedOverlapping), and the build's own copy-back takes them (TakeClaimedCopies) to
    // copy the union of both uses' written ranges back from its buffer.
    void ClaimDeferredCopies(const void* sourceKey);
    std::vector<DeferredCopy> TakeClaimedCopies(const void* sourceKey);
    void FlushClaimedOverlapping(std::uint64_t address, std::size_t bytes);
    // Claims the claiming use did not take (its copy-back skipped the region, it wrote nothing
    // back, or it failed before MarkGpuWrites) go back to the unclaimed copies, which the next
    // command records: held longer, every command after the use would read the imports without
    // the earlier use's results until Submit (t167-t170: device lost). Called after each use's
    // copy-backs and before a build claims.
    void ReleaseClaims();
    bool Idle() const { return open == nullptr && inFlight.empty(); }
    // Whether recorded work still has completion actions (write-backs the CPU must see) to run.
    bool HasCompletions() const;
    // The object lives at least until the batch open now completed; it is then destroyed AFTER the
    // finishing thread released GuestMemory::GpuMutex, on a release thread of its own (never under
    // a hold, see Recorder.cpp ReleaseDeferredKeeps; the finishing thread destroys it itself when
    // the thread's queue is full, APS5_RELEASE_QUEUE_MAX batches, or with APS5_RELEASE_ON_UNLOCK=1;
    // APS5_RELEASE_UNDER_LOCK=1 destroys it in the reap as before), so its destructor must need
    // neither the mutex nor the recorder nor a particular thread. ~Recorder joins the release
    // thread and waits for every release in progress before the device goes.
    void Keep(std::shared_ptr<void> object);
    enum class SnapshotUse : std::uint8_t { Storage, Vertex, Index16, Index32 };
    static constexpr std::size_t DrawSnapshotBudget = std::size_t{256} << 20u;
    static constexpr std::size_t DrawSnapshotEntries = 1024;
    static constexpr std::size_t DrawInputBudget = std::size_t{1024} << 20u;
    static constexpr std::size_t DrawInputEntries = 16384;
    std::shared_ptr<Buffer> ReusableDrawSnapshot(std::uint64_t address, std::size_t bytes, SnapshotUse use = SnapshotUse::Storage, std::uint32_t* derived = nullptr);
    void KeepDrawSnapshot(std::uint64_t address, std::size_t bytes, std::uint64_t generation, std::uint64_t registryGeneration, std::shared_ptr<Buffer> buffer, SnapshotUse use = SnapshotUse::Storage, std::uint32_t derived = 0);
    void OnComplete(std::function<void()> action);
    // What a noted pending write is, for the [hooksync] attribution (the writer of the range a
    // CPU access waited for, see DescribePendingWrite): a shader's writable element (a dispatch or
    // a draw, MarkGpuWrites), a buffer fill, a buffer copy (its destination) or a whole-surface
    // alias copy, a DCC key store, a storage texture's store into its import, a unit shadow's
    // publish, a GPU label (NoteLabel) or a label stored by a completion action (AfterCompletions).
    // Diagnostic only: the kind changes nothing about the wait.
    enum class WriteKind : std::uint8_t { Unknown = 0, ShaderWrite, Fill, Copy, AliasCopy, DccKeys, TextureStore, ShadowPublish, Label, CompletionLabel, Count };
    static const char* WriteKindName(WriteKind kind);
    void NotePendingWrite(std::uint64_t address, std::size_t bytes, WriteKind kind = WriteKind::Unknown);
    // Notes several [begin, end) ranges and publishes the snapshot once (a dispatch writes many buffers).
    void NotePendingWrites(std::span<const std::pair<std::uint64_t, std::uint64_t>> ranges, WriteKind kind = WriteKind::Unknown);
    bool PendingWriteOverlaps(std::uint64_t address, std::size_t bytes) const;
    // Whether no unfinished batch writes the range any more: no open-batch overlap, and every
    // overlapping in-flight batch's fence has signaled (its in-place GPU stores are final in host
    // memory; its completion stores are the caller's business, see VulkanDevice::CopyBuffer).
    bool PendingWriteSettled(std::uint64_t address, std::size_t bytes) const;
    std::uint64_t LastWriteNote(std::uint64_t address, std::size_t bytes) const;
    std::uint64_t NewestWriteNote(std::uint64_t address, std::size_t bytes) const;
    // Batch read tracking: the guest ranges the recorded work reads IN PLACE through a host import
    // and the GPU has not executed yet (a V# element bound in place, a region the GPU copies out of
    // an import, an address-based build's leased heaps, indirect arguments, a GPU-direct storage
    // upload, the copy HLE's source). A CPU store into such a range before the batch ran would be
    // seen by the reader early (WAR); VulkanDevice::CopyBuffer refuses its CPU path on a hit. Noted
    // on the open batch, no snapshot: the only reader holds the mutex. `kind` names the reader for
    // the [recorder] hit counters. APS5_COPY_READ_TRACKING=0 notes nothing (ReadTracking() is then
    // false and the CPU copy falls back to Idle()).
    enum class ReadKind : std::uint8_t { DispatchElement = 0, GpuCopy, AddressBased, Indirect, StorageUpload, CopySource, DrawInput, Count };
    static bool ReadTracking();
    void NotePendingRead(std::uint64_t address, std::size_t bytes, ReadKind kind);
    void NotePendingReads(std::span<const std::pair<std::uint64_t, std::uint64_t>> ranges, ReadKind kind);
    // Notes an immutable, sorted, merged list of in-place reads by reference: the open batch keeps
    // one reference per list, so a list every address-based dispatch of the batch notes (its shared
    // address space's ranges, thousands of them; t290: 44M ranges noted per 10 s) is referenced
    // once and never copied. Tested by PendingReadOverlaps like the noted ranges. Null or empty
    // notes nothing. APS5_NO_READ_SETS=1 copies the ranges as NotePendingReads does.
    void NotePendingReadSet(std::shared_ptr<const std::vector<std::pair<std::uint64_t, std::uint64_t>>> ranges, ReadKind kind);
    static bool ReadSets();
    // Whether an unexecuted batch (open, or in flight with its fence unsignaled) reads the range in
    // place. A hit in an in-flight batch whose fence signaled meanwhile is a miss (one status
    // query per hit). `ignoreSignaled` false: every in-flight batch counts, whatever its fence.
    bool PendingReadOverlaps(std::uint64_t address, std::size_t bytes, bool ignoreSignaled = true) const;
    // The newest batch whose noted read overlaps the range (for the [copyhle] trace and the verify
    // switch): its serial (the open batch reports the serial it will get), recording queue, reader
    // kind, and whether its fence has signaled. Nullopt: no overlap.
    struct PendingReadInfo {
        std::uint64_t serial;
        std::uint32_t queue;
        ReadKind kind;
        bool open;
        bool signaled;
    };
    std::optional<PendingReadInfo> DescribePendingRead(std::uint64_t address, std::size_t bytes) const;
    // Counters of the read tracking (cumulative, for the [recorder] line): ranges noted, overlap
    // queries, hits by reader kind, and hits ignored because the reader's fence had signaled.
    struct ReadStatistics {
        std::uint64_t noted, queries, staleIgnored;
        std::array<std::uint64_t, static_cast<std::size_t>(ReadKind::Count)> hits;
        // Read sets (NotePendingReadSet): lists referenced, notes of a list the batch already held,
        // and the ranges those repeat notes would have copied.
        std::uint64_t setsNoted, setNotesSkipped, setRangesSkipped;
    };
    static ReadStatistics ReadCounts();
    // Whether the OPEN (unsubmitted) batch writes the range: a wait on such a range must submit it.
    bool OpenWriteOverlaps(std::uint64_t address, std::size_t bytes) const;
    // What SyncThrough(address, bytes) would wait for, for the [hooksync] attribution: the newest
    // batch whose noted range overlaps the access (the open batch counts first, with the serial it
    // will get), whether that batch's fence has already signaled (its GPU work is done, so the
    // access's bytes are already final; never for the open batch), that noted range, and how many
    // batches the wait finishes. Nullopt: no overlap.
    struct PendingWriteInfo {
        std::uint64_t serial;
        bool open;
        bool signaled;
        std::uint64_t rangeBegin;
        std::uint64_t rangeEnd;
        std::size_t batchesToFinish;
        // The writer of that range (item 7's diagnostic): the GpuMutex queue tag of the thread that
        // opened the target batch, the PM4 packet (opcode and submission queue, GuestMemory::
        // CurrentPacket at the note; 0xfffffffe = no packet) the noting thread was executing, the
        // note's kind, whether the noted range covers the whole access (false: a partial overlap),
        // and, for the open batch, the draws and dispatches it had recorded when the wait flushed
        // it (RecordedWorkSinceSubmit; 0 for a batch in flight).
        std::uint32_t batchQueue = 0xffffffffu;
        std::uint32_t writerQueue = 0xffffffffu;
        std::uint32_t writerOpcode = 0xfffffffeu;
        WriteKind writerKind = WriteKind::Unknown;
        bool covers = false;
        std::uint64_t recordedWork = 0;
    };
    std::optional<PendingWriteInfo> DescribePendingWrite(std::uint64_t address, std::size_t bytes) const;
    // Ends and submits the open batch without waiting.
    void Submit();
    // Submits the open batch and returns the serial of the newest submitted batch (0 when nothing was
    // ever submitted); WaitSerial(serial) then covers all recorded work.
    std::uint64_t SubmitAndEpoch();
    bool HasTimeline() const { return timeline != VK_NULL_HANDLE; }
    // Waits until every batch up to `serial` completed on the GPU. Called WITHOUT GuestMemory::GpuMutex:
    // it touches only the immutable timeline semaphore (the caller keeps the device alive). Requires
    // HasTimeline(); the debug switch APS5_NO_TIMELINE makes the device create the recorder without one.
    void WaitSerial(std::uint64_t serial);
    // Under the mutex: finishes (completions, release) the in-flight batches up to `serial`, from the
    // front only; tolerates batches another thread finished meanwhile. Later batches stay in flight.
    void FinishUpTo(std::uint64_t serial);
    // Submits and waits for every batch, running completions in order.
    void Sync();
    void CountSamples();
    static std::uint64_t SamplesPassed();
    // Waits only for the batches up to the newest one that writes the range (submitting the open
    // batch when it is that one); later batches stay in flight. Fences of one queue signal in
    // submission order, so completions still run in order. Debug aid: APS5_NO_SYNC_THROUGH=1 syncs all.
    // `waitUnlocked` (the flush hook): when the caller's GpuMutex acquisition is the outermost one
    // on this thread and the device has a timeline, the GPU wait runs WITHOUT the mutex (released
    // and retaken here; the caller must hold nothing else that orders after it) and only the
    // completions run under it, so other queues do not queue behind a CPU read's wait. Inside a
    // batch's completion action no wait is made at all: every batch still in flight was recorded
    // after the completing one, and its store is that batch's in-order write (see Recorder.cpp).
    void SyncThrough(std::uint64_t address, std::size_t bytes, bool waitUnlocked = false);
    // Completes batches that already finished; returns whether nothing is in flight.
    bool Reap();
    std::uint64_t Submissions() const { return submissions; }
    // Batches submitted and not yet finished (what a full Sync() waits for); under GuestMemory::GpuMutex.
    std::size_t InFlightBatches() const { return inFlight.size(); }
    // Reaps that retired at least one batch so far (the [recorder] line's 'with work' count).
    static std::uint64_t ReapsWithWork();

    // A store of `bytes` into a host import (a label, or a COPY_DATA/DMA_DATA/DUMP_CONST_RAM store
    // of up to 64 KiB; `address` is its guest address) recorded into the open batch: ordered
    // after everything recorded before it and visible to the host and to later work. The stores
    // of a batch form one run recorded as the batch's last commands at Submit: one barrier
    // (everything so far -> transfer and host: every result of the batch reaches host memory
    // before any label of it lands, the order the title's pollers rely on), the
    // vkCmdUpdateBuffers, one transfer -> host barrier. A later command of the batch that writes
    // over a queued store's bytes or reads them in place must find the store recorded (the
    // callers that know their ranges call FlushStoresOverlapping, or FlushStores when they do
    // not, before recording): the run is then recorded in place, with a trailing barrier toward
    // the later work, and the next store starts a new run. Within a run a store contiguous with
    // the last queued one joins its vkCmdUpdateBuffer, one inside it replaces its bytes (the
    // later store wins, as in program order), and one overlapping a store queued earlier gets a
    // transfer-to-transfer barrier first (WAW). Debug aids: APS5_LABEL_RUNS_INLINE=1 records the
    // stores at once and closes the run at the next command (Commands()) or at Submit, as
    // before; APS5_NO_LABEL_RUNS=1 gives every store barriers of its own.
    void RecordStore(VkBuffer buffer, VkDeviceSize offset, std::span<const std::byte> bytes, std::uint64_t address);
    bool HasQueuedStores() const { return open != nullptr && !open->run.queued.empty(); }
    bool QueuedStoreOverlaps(std::uint64_t address, std::size_t bytes) const;
    // Whether `overlaps(begin, end)` holds for a queued store's guest range.
    bool AnyQueuedStore(const std::function<bool(std::uint64_t, std::uint64_t)>& overlaps) const;
    void FlushStores();
    void FlushStoresOverlapping(std::uint64_t address, std::size_t bytes) { if (HasQueuedStores() && QueuedStoreOverlaps(address, bytes)) FlushStores(); }

    // Pending-label table. A GPU label (RELEASE_MEM/WRITE_DATA recorded as a store into the batch)
    // is noted per 4-byte dword with the value it will store and a record-order stamp (the driver's
    // event serial, taken when the label is recorded). A WAIT_REG_MEM whose submission was received
    // (stamped) BEFORE the label was recorded may take the value from the table instead of waiting
    // for the GPU: any CPU store the game ordered before that submit call precedes the label in
    // program order exactly as on hardware, and later recorded work follows the label in queue
    // order. Entries recorded before the submission (the previous frame's label at the same address)
    // are trusted only under the late rule below (APS5_LABEL_TRUST_LATE=0 never trusts them).
    // Entries leave the table with their batch (a memory bound only). The note also enters the
    // label's range as a pending write of the open batch (the flush hook syncs CPU reads), marked
    // as the label's own so it does not count as an overwrite of the entry.
    void NoteLabel(std::uint64_t address, std::span<const std::byte> bytes, std::uint64_t stamp, std::uint32_t queue);
    // Late rule: an entry whose stamp is not newer than the wait's submission is still the value
    // memory will hold, unless a CPU store touched the dword since it was recorded (the table
    // cannot see CPU stores; the title recycles label slots by CPU writes) or a later recorded
    // non-label GPU write covers it. The table refuses a queued entry, one flagged `overwritten`
    // (noteWrite), and one whose record group has not closed (generation 0); the caller checks the
    // CPU side with GuestMemory::UnchangedSinceCollected against the hit's generation. Labels are
    // noted in groups (a worker's deferred labels recorded back to back, or one immediate label
    // or store): CloseLabelGroup, called by the recording thread after the group's last
    // MarkWritten, stamps the group's entries with the write tracker's generation at that point;
    // per-entry capture would fail, as the next label of the group stamps the shared 64 KiB block
    // newer. A generation of 0 leaves the entries unclosed (a failed record), as does a batch
    // already submitted at the close (its value may have landed before the generation was taken).
    static void CloseLabelGroup(std::uint64_t trackerGeneration);
    static bool LateTrust();
    struct LabelHit {
        std::uint64_t value;
        // The recording queue of the first dword, and its record stamp.
        std::uint32_t queue;
        std::uint64_t stamp;
        // Taken under the late rule: `generation` is the oldest group generation of the dwords.
        bool late;
        std::uint64_t generation;
    };
    // Why a lookup refused a late candidate (an entry with stamp <= afterStamp), for the trace.
    // BehindCompletion replaces the other reasons when the refused entry's value reaches memory
    // only through a completion action of its batch (AfterCompletions: the GPU has no view of the
    // range, or a write-back overwrote the GPU's store): a poller must reap that batch to converge.
    enum class LabelRefusal : std::uint8_t { None = 0, TrustOff, Queued, Overwritten, Unclosed, BehindCompletion };
    // Per calling thread (lookups run on the waiting worker): late candidates seen, in both modes,
    // and the refusals by reason ([packets] line deltas).
    struct LateStatistics {
        std::uint64_t candidates, queued, overwritten, unclosed;
    };
    static LateStatistics LateCounts();
    // The 4- or 8-byte value the table holds for `address` when every dword is present with a stamp
    // newer than `afterStamp`, or trustable under the late rule.
    std::optional<LabelHit> PendingLabel(std::uint64_t address, std::size_t bytes, std::uint64_t afterStamp, LabelRefusal* refusal = nullptr) const;
    std::size_t PendingLabels() const;
    // Whether any tracked label dword (recorded, or queued by a worker and not recorded yet) lies
    // inside [address, address + bytes): a range query for large ranges (a fill of megabytes),
    // where a per-dword lookup would not do.
    bool PendingLabelIn(std::uint64_t address, std::size_t bytes) const;
    // PendingLabel of the active recorder WITHOUT GuestMemory::GpuMutex: the table has a small mutex
    // of its own (every mutation holds both), so a WAIT_REG_MEM consults it without queueing behind
    // device work. Nothing is done under the table mutex but the lookup (it never takes the GPU mutex).
    static std::optional<LabelHit> LookupLabel(std::uint64_t address, std::size_t bytes, std::uint64_t afterStamp, LabelRefusal* refusal = nullptr);
    // The value alone, for callers asking whether any label is pending in a dword (afterStamp 0).
    static std::optional<std::uint64_t> LookupLabelValue(std::uint64_t address, std::size_t bytes, std::uint64_t afterStamp);
    // Whether LookupLabelValue(dword, 4, 0) has a value for any dword of [address & ~3, address +
    // bytes): one table lock and a range scan of the tracked dwords instead of a lookup per dword
    // (the fast reader's known-value range, Driver::fastPendingWord). The same answer: every
    // tracked dword in the range is looked up as LookupLabelValue looks it up, without counting a
    // queued label's hit as a same-queue wait hit.
    static bool LabelValueIn(std::uint64_t address, std::size_t bytes);
    // A label a queue worker decoded but has not recorded yet (Driver.cpp DeferredLabels): it
    // enters the table with no batch, and only a lookup made on the noting thread (a WAIT_REG_MEM
    // of the same queue) takes its value, since that queue's later work follows the label in queue
    // order whenever the group is recorded; other queues and the CPU see it once it is recorded.
    // The range is also kept per thread, so a CPU access of it by the same worker (the flush hook)
    // has the group recorded first through the function SetQueuedLabelRecorder installed. Takes
    // the table mutex only. The queued entries live apart from the recorded ones: a recorded
    // label of the same dword (an older store still pending on the GPU) keeps serving the other
    // queues' waits and the driver's pending-label checks until the queued one is recorded.
    // Debug aid: APS5_NO_SEPARATE_QUEUED_LABELS=1 lets a queued entry replace the recorded one.
    static void NoteQueuedLabel(std::uint64_t address, std::span<const std::byte> bytes, std::uint64_t stamp, std::uint32_t queue);
    // After the calling worker recorded (or dropped) its queued labels: its ranges are cleared and
    // the entries of its dwords still without a batch (stored by the CPU, or dropped) leave the table.
    static void ForgetQueuedLabels();
    // The function the flush hook calls, on the accessing thread (which may hold the GPU mutex),
    // when an access overlaps one of that thread's queued labels; it records them. Set once.
    static void SetQueuedLabelRecorder(void (*recorder)());
    // Whether the lock-free pending-write snapshot (open, in-flight and finishing batches) overlaps
    // the range: false means no recorded work writes it, so a wait on it has nothing to submit.
    static bool SnapshotWriteOverlaps(std::uint64_t address, std::size_t bytes);
    // Whether one of the calling worker's queued labels (NoteQueuedLabel, not recorded yet)
    // overlaps the range. The snapshot above does not see them, so a capture's page query over
    // such a label answered "nothing pending" and the whole-page read had the flush hook record
    // the group and sync on the open batch; the driver's page query asks this as well, so the
    // page is read word by word and only a read of the label's own dwords records and waits
    // (APS5_NO_WORDWISE_QUEUED_LABELS=1: the old path). Counts the true answers.
    static bool QueuedLabelOverlapsThisThread(std::uint64_t address, std::size_t bytes);
    // The same answer without the count: for the fast walks' direct reader (FastSrtRead), whose
    // word reads are not the old capture's page queries.
    static bool QueuedLabelOverlapsThisThreadUncounted(std::uint64_t address, std::size_t bytes);
    // Pending blocks (docs/design/draw-fastpath.md section 2.3, F2): per 64 KiB block of guest
    // memory, the serial of the newest batch of the active recorder that noted a pending write
    // into it (every noted range: NotePendingWrite(s), labels, completion stores), set beside the
    // snapshot publish. The block is pending while that serial is above CompletedSerial(), which
    // advances once a batch's completions ran. Lock-free, for the fast walk's direct reader. The
    // table is hashed: a block shares its slot with the blocks 16 GiB apart, so a collision reads
    // pending, never clear. Kept while APS5_FAST_WALK is set (nonzero) or after
    // TrackPendingBlocks(true); untracked, no block reads pending. The serials are offset per
    // recorder, so a replaced device's marks read completed once its successor is activated.
    static void TrackPendingBlocks(bool enabled);
    static bool PendingBlocksTracked();
    static bool BlockPending(std::uint64_t address);
    static std::uint64_t CompletedSerial();
    // The snapshot itself (the sorted, merged union of the pending ranges; null when none), for a
    // reader that tests many ranges against one loaded snapshot: one atomic shared_ptr load per
    // validation instead of one per run, and every test sees the same snapshot (design13 R1's p0).
    using WriteRanges = std::vector<std::pair<std::uint64_t, std::uint64_t>>;
    static std::shared_ptr<const WriteRanges> PendingWriteSnapshot();
    static bool SnapshotOverlaps(const WriteRanges* snapshot, std::uint64_t address, std::size_t bytes);
    // A label behind pending completions (CPU write-backs the label must not precede): stored to
    // guest memory by a completion action appended to the open batch, or to the newest in-flight
    // batch when none is open (no empty submission), so it runs after every earlier write-back in
    // submission order. The range is noted as a pending write of that batch so CPU reads through the
    // flush hook still sync, and the label enters the table like a GPU one.
    // `storedOnGpu`: the same bytes were also recorded into the open batch (a host import), so the
    // completion re-stores them only when a CPU write-back noted by NoteWrittenBack overlapped the
    // range since; the plain store of a label the GPU has no view of always runs. An unconditional
    // completion store would land on memory the game may have reused by then (a stale label value
    // over a fresh command buffer). Debug aid: APS5_LABEL_STORE_ALWAYS=1 stores unconditionally.
    void AfterCompletions(std::uint64_t address, std::span<const std::byte> bytes, std::uint64_t stamp, std::uint32_t queue, bool storedOnGpu);
    bool AfterRecordedWork(std::function<void()> action);
    // A CPU store of GPU results into guest memory (GuestBufferMemory::WriteBack): recorded so a
    // completion label store can tell whether its bytes were overwritten. Under GuestMemory::
    // GpuMutex (every write-back runs inside finish() or a synchronous draw's wait): the first
    // write-back overlapping a completion label stored on the GPU is what makes that label's
    // completion store necessary, so it is counted into PendingCompletionLabels here rather than
    // at registration (APS5_COUNT_ALL_COMPLETION_LABELS=1 counts every one at registration, as
    // before). A write-back over any tracked label dword is counted on the [recorder] line (must
    // stay 0: it would mean a CPU store landed over a label recorded on the GPU).
    static void NoteWrittenBack(std::uint64_t address, std::size_t bytes);

    // Lock-free readers for the queue workers' packet loops and WAIT_REG_MEM polls (values of the
    // active recorder; a torn-down recorder never publishes):
    // when the open batch received its first label (nullopt: none pending), so the label's
    // submission can be bounded (APS5_LABEL_FLUSH_US) without taking the mutex to look;
    static std::optional<std::chrono::steady_clock::time_point> PendingLabelSince();
    // bumped by every note of a pending write into the open batch, so a poller learns that the
    // producer may have recorded the label it waits for;
    static std::uint64_t WriteGeneration();
    // bumped right AFTER every store of the pending-write snapshot (the generation above moves
    // before the store), so a reader that loads it before and after a set of in-place reads knows
    // the snapshot it consulted in between was the one in force for all of them;
    static std::uint64_t PublishGeneration();
    // labels waiting in completion actions (the CPU must reap their batch for them to land);
    static std::uint64_t PendingCompletionLabels();
    // write-back completions (OnComplete) of unfinished batches: a reaper retires them when every
    // worker sleeps, as for the labels above;
    static std::uint64_t PendingWriteBackCompletions();
    // dispatches and draws recorded since the last submit (the driver counts them: CountRecordedWork),
    // so a batch can be submitted after a bounded amount of work (APS5_BATCH_CAP; 0, the default: uncapped).
    static std::uint64_t RecordedWorkSinceSubmit();
    static void CountRecordedWork();

    // The recorder of the current device, for code that only has guest addresses (the flush hook)
    // and for helpers that must not recycle resources the recorded work still uses.
    static Recorder* Active();
    // Whether the calling thread runs inside a batch's completion action (finish()): a store made
    // there must not have a publish recorded for it (UnitShadow), see SyncThrough.
    static bool InCompletion();
    void Activate();
    // APS5_PROFILE_DRAW: syncs by source (0 device idle, 1 CPU access to pending writes, 2 CPU read
    // after a recorded store, 3 address-based dispatch, 4 other). Contract: the call announces the
    // source for the Sync/SyncThrough this thread makes NEXT, which consumes it (a sync without a
    // preceding CountSync counts as "other"); call it right before the sync and never without one,
    // or the announcement attributes an unrelated later wait. `site` names the call site for the
    // [recorder] "top sync sites" table (a return address, printed as a module offset like the
    // [guestmem] callers): a caller that syncs on behalf of others (VulkanDevice::WaitIdle) passes
    // its own return address, so the table names the driver site, not the wrapper; without it the
    // return address of the Sync/SyncThrough call itself is taken, so an unannounced sync still
    // names its real caller.
    static void CountSync(int source, const void* site = nullptr);
    // Names only the site for the sync or drain this thread makes next (its source stays as
    // announced, or as the drain counts it): for a wrapper (VulkanDevice::ReapRecorded) whose
    // FinishUpTo counts the drain itself, so a CountSync there would count it twice.
    static void AnnounceSyncSite(const void* site);
    // APS5_PROFILE_DRAW: the calling thread's fence and timeline waits so far, in milliseconds (0
    // when not profiling): a caller reads it around a span of its own work to learn how much of
    // that span waited for the GPU (a resource build's nested flush-hook waits, a draw's).
    static double ThreadWaitedMs();
    // The calling thread's flush-hook syncs (SyncThrough) that waited for a target batch whose
    // fence had not signaled when the wait began: a read whose bytes are compared around such a
    // sync observed the GPU's work; one that found the fence signaled already read the GPU's bytes
    // both times and observed nothing (Driver.cpp's dword evidence reads it before and after).
    static std::uint64_t ThreadHookWaits();
    // Cumulative counts for the [labels] line (APS5_PROFILE_DRAW): stores recorded (RecordStore),
    // the runs they formed (one barrier pair each), stores joined to a pending contiguous one,
    // stores that replaced pending bytes, WAW barriers inside runs, joins refused because the
    // joined store would overlap one the run already recorded, queued labels noted (of which
    // over a recorded entry of the same dword), lookups a queued label satisfied on its own
    // queue, flush-hook accesses that had queued labels recorded first, and hook accesses made
    // by a completion action whose queued-label record was skipped.
    struct StoreStatistics {
        std::uint64_t stores, runs, joined, replaced, wawBarriers, joinsRefused, queuedNoted, queuedOverRecorded, queuedHits, queuedHookRecords, queuedHookInCompletion;
        // DCC key stores queued, the runs that recorded them (at Submit, or before a writer of a
        // queued range), and the queued stores a duplicate range joined.
        std::uint64_t keyStores, keyStoreRuns, keyStoreRunsForWriter, keyStoresJoined;
        // Store runs recorded at Submit, and in place before a later writer or reader of a queued
        // store's bytes (FlushStores).
        std::uint64_t runsAtSubmit, runsForced;
        // Page queries a queued label alone overlapped (QueuedLabelOverlapsThisThread).
        std::uint64_t queuedPageQueries;
    };
    static StoreStatistics StoreCounts();
    // APS5_PROFILE_GPU=1: GPU time of recorded work by key (a guest program address), from timestamp
    // queries around each timed range; the totals per key are reported every 10 s. Begin returns the
    // range index to pass to End, or NoTiming when timing is off or the batch's queries are used up.
    static constexpr std::uint32_t NoTiming = 0xffffffffu;
    static bool GpuTimingEnabled();
    static bool BatchStampsEnabled();
    // A range starts where the caller's next command goes and records nothing but its stamp: the
    // queued copy-backs stay queued (coalescing) and the barrier-merge state (Commands'
    // coveredAccess) stays, so profiling leaves the recorded stream as it is without it. Every
    // caller takes its command buffer first. Debug aid: APS5_PROFILE_GPU_FLUSHES=1 begins every
    // range with Commands(), as before (it records every queued copy-back at every range).
    std::uint32_t BeginGpuTiming(std::uint64_t key);
    static bool GpuTimingFlushes();
    // Timed ranges per batch (APS5_PROFILE_GPU_RANGES, default 8192, 64..65536; read once): a
    // batch's ranges past it are dropped and counted ([gputime] "ranges dropped").
    static std::uint32_t GpuTimingRangeCap();
    // Ranges a Begin*Timing refused because a render pass was open (every caller takes its
    // command buffer first, which ends the pass; a range begun inside it would nest in the pass's
    // draw range). Never reset; [gputime] reports the window's part.
    static std::uint64_t GpuTimingRefusedInPass();
    // A guest copy's transfer (VulkanDevice::CopyBuffer): keyed by its program address like a
    // dispatch, but nested in its copy class range, so the [gputime] split rows leave it there.
    std::uint32_t BeginTransferTiming(std::uint64_t programAddress);
    // `bytes`: what the range moved (a fill's, a copy's), summed per key on the [gputime] line.
    void EndGpuTiming(std::uint32_t index, std::uint64_t bytes = 0);
    // What a timed program or draw binds in place in the host imports, read over PCIe when it
    // runs: `read` the bytes of the ranges bound in place, `written` the part of them it may write,
    // `inputs` a draw's vertex and index ranges read in place; `leased` an address-based build,
    // whose leased heaps are not counted (its bytes are unknown). Bound bytes, not bytes fetched.
    struct InPlaceUse {
        std::uint64_t read = 0;
        std::uint64_t written = 0;
        std::uint64_t inputs = 0;
        bool leased = false;
    };
    using GuestRanges = std::span<const std::pair<std::uint64_t, std::uint64_t>>;
    // The union of `reads` (bytes), the bytes of `writes` inside it, the union of `inputs`;
    // `leased` drops the reads and writes (an address-based build's include whole heaps).
    static InPlaceUse InPlaceUseOf(GuestRanges reads, GuestRanges writes, GuestRanges inputs = {}, bool leased = false);
    // Adds `use` to the open batch's range `index` (a program's: VulkanDevice recordDispatch,
    // FastDispatch); nothing for NoTiming.
    void NoteInPlace(std::uint32_t index, const InPlaceUse& use);
    // A draw recorded into the pass whose draw range is `index` (the draw's own, or the open
    // pass's when it continued it: OpenRenderPassTiming): counts the draw and its `use`, and names
    // the pass by `target` (its first color target's guest address, else its depth target's) when
    // the pass has no name yet. The [gputime] draw-pass line sums passes by target.
    void NoteDrawInPass(std::uint32_t index, std::uint64_t target, const InPlaceUse& use);
    std::uint32_t OpenRenderPassTiming() const;
    // The key of the whole-batch range (first to last command; reported as "batch" on the [gputime]
    // line, apart from the per-program totals).
    static constexpr std::uint64_t BatchTimingKey = 0x3;
    // The driver's own command classes: their [gputime] ranges take the reserved keys 0x10 + class
    // (printed by name, with counts and bytes, per 10 s and per present) and their barriers are
    // counted on the [barriers] line (CountBarriers, printed every 10 s under APS5_PROFILE_DRAW
    // or APS5_PROFILE_GPU). A class range covers the command with its own barriers (a dispatch's
    // barriers are classes of their own, its program range stays keyed by the program), so the
    // union of every range of a batch and the batch span differ by what no class times ('untimed').
    enum class CommandClass : std::uint8_t { DispatchLeading = 0, DispatchTrailing, IndirectArguments, LabelRun, Fill, FillClear, Copy, StagingIn, StagingOut, Draw, StorageUpload, StorageWriteBack, DccClear, DccKeyStore, PresentBlit, ShadowPublish, TemplateDataRefresh, DeferredFlat, Count };
    static constexpr std::uint64_t ClassKey(CommandClass which) { return 0x10 + static_cast<std::uint64_t>(which); }
    std::uint32_t BeginGpuTiming(CommandClass which) { return startGpuTiming(ClassKey(which), TimingKind::Class); }
    // Records buffer copies into the open batch outside any render pass (an open pass ends),
    // ordered behind every earlier recorded write of their sources and visible to the work after;
    // `which` names the [gputime]/[barriers] class (deferred flat slots: Draw.cpp
    // recordDeferredFlat). Only source, destination, offsets and bytes of a copy are used.
    void RecordCopies(std::span<const DeferredCopy> copies, CommandClass which);
    static void CountBarriers(CommandClass which, std::uint32_t count = 1);
    // A leading barrier a command left out because the previous trailing barrier covered its
    // accesses (see Commands), on the [barriers] line as 'merged'. Debug aid: APS5_FULL_BARRIERS=1
    // (or APS5_NO_BARRIER_ELISION=1) records every leading barrier, as before.
    static bool MergeBarriers();
    static void CountMerged(CommandClass which);
    // Hazard tracker, counting mode (APS5_BARRIER_VALIDATE=1): every command tells the tracker
    // what it reads and writes (guest ranges through host imports, images) with its stage before
    // it is recorded; the tracker simulates barriers emitted only on a hazard against the accesses
    // since its last simulated barrier (RAW, WAW, WAR, an image written; a command whose ranges are
    // unknown, an address-based build or a BDA draw, is a hazard by itself) and counts, per class,
    // the leading barriers a hazard-driven recorder would have emitted and skipped, on the
    // [barriers] line; the real barriers are recorded as before. APS5_TRACE_BARRIERS=<n> prints
    // the first n skipped decisions with their ranges. Costs nothing when off.
    struct Access {
        std::span<const std::pair<std::uint64_t, std::uint64_t>> reads;
        std::span<const std::pair<std::uint64_t, std::uint64_t>> writes;
        // (image, written)
        std::span<const std::pair<VkImage, bool>> images;
        VkPipelineStageFlags stages;
        bool conservative = false;
    };
    static bool BarrierValidate();
    void NoteAccess(CommandClass which, const Access& access);
    // A range timed outside the recorder's batches (the presenter's blit, stamped into its own
    // pool and read after its fence): enters the class totals like a recorded one.
    static void AddGpuTiming(CommandClass which, double nanoseconds, std::uint64_t bytes);
    // A timed range as the [gputime] digest takes it (GpuTimingDigest::AddBatch): its key and
    // kind, the GpuMutex queue tag of the thread that began it (GuestMemory::GpuLockThreadTag),
    // its stamps once read (ticks), what it moved (EndGpuTiming) and what it bound in place
    // (NoteInPlace, NoteDrawInPass: `draws` and the pass `target` for a draw range).
    enum class TimingKind : std::uint8_t { Program = 0, Transfer, Class, Batch };
    struct TimedRange {
        std::uint64_t key = 0;
        TimingKind kind = TimingKind::Program;
        std::uint32_t queue = 0xffffffffu;
        std::uint64_t begin = 0;
        std::uint64_t end = 0;
        std::uint64_t bytes = 0;
        std::uint64_t inPlaceRead = 0;
        std::uint64_t inPlaceWritten = 0;
        std::uint64_t inPlaceInputs = 0;
        std::uint32_t leased = 0;
        std::uint32_t draws = 0;
        std::uint64_t target = 0;
        // The end stamp was written (EndGpuTiming, or Submit for a range left open).
        bool ended = false;
        // Ended by Submit, not by its End (a throw between Begin and End): it spans the rest of
        // the batch, so the digest counts it apart and adds it to no total.
        bool leftOpen = false;
    };
    // The [gputime] totals since the last report (tests; under the timing mutex).
    static GpuTimingDigest GpuTimingTotals();
    // Presentations, so the [gputime] class totals can be given per present.
    static void CountPresent();
    // Presentations so far (never reset): a frame clock for policies that count per frame, such as
    // StorageTexture's write-back hysteresis.
    static std::uint64_t Presents();

    // What the presenter learns about finished batches (Driver::Present's [present] line and the
    // frame record): finish() writes an entry per submitted batch into a ring of the last 128,
    // under GuestMemory::GpuMutex and the ring's own mutex; the readers below take only the ring
    // mutex, so the presenter reads them WITHOUT the GpuMutex. gpuStartNs/gpuEndNs are the
    // whole-batch stamps (0 unless BatchStampsEnabled); `reads` and `readGeneration` (the
    // write-watch generation collected over the noted in-place reads at submit) are filled with
    // APS5_FLIP_READ_CHECK=1 only.
    struct Completed {
        std::uint64_t serial = 0;
        double gpuStartNs = 0;
        double gpuEndNs = 0;
        std::chrono::steady_clock::time_point submittedAt{};
        std::chrono::steady_clock::time_point fenceSeenAt{};
        std::uint64_t readGeneration = 0;
        std::vector<std::pair<std::uint64_t, std::uint64_t>> reads;
    };
    // The entries with serial in (afterSerial, throughSerial], oldest first; `missing` counts the
    // serials of that range not in the ring (not finished yet, or overwritten by newer batches).
    // An entry is overwritten once the batch CompletedRingSize serials later finishes.
    static constexpr std::size_t CompletedRingSize = 512;
    std::vector<Completed> CompletedBatches(std::uint64_t afterSerial, std::uint64_t throughSerial, std::size_t& missing) const;
    // The newest submitted serial and its vkQueueSubmit time (ring mutex only).
    std::uint64_t NewestSubmitted(std::chrono::steady_clock::time_point* submittedAt = nullptr) const;
    // In-flight batches whose fence has not signaled (one status query each); under the mutex.
    std::size_t UnsignaledBatches() const;
    static bool FlipReadCheck();

private:
    struct Batch {
        VkCommandBuffer commands = VK_NULL_HANDLE;
        VkFence fence = VK_NULL_HANDLE;
        std::vector<std::shared_ptr<void>> kept;
        std::vector<std::function<void()>> completions;
        std::vector<std::pair<std::uint64_t, std::uint64_t>> writes;
        std::vector<std::uint64_t> writeNotes;
        // The writer of each range, parallel to `writes` like writeNotes (see PendingWriteInfo).
        struct WriteTag {
            std::uint32_t queue;
            std::uint32_t opcode;
            WriteKind kind;
        };
        std::vector<WriteTag> writeTags;
        // In-place reads (see NotePendingRead), dying with the batch: a finished batch's reads are done.
        struct Read {
            std::uint64_t begin;
            std::uint64_t end;
            ReadKind kind;
        };
        std::vector<Read> reads;
        // Read lists noted by reference (NotePendingReadSet), each with its reader kind.
        std::vector<std::pair<std::shared_ptr<const std::vector<std::pair<std::uint64_t, std::uint64_t>>>, ReadKind>> readSets;
        // Submission number (1-based): identifies a batch after its allocation may have been reused.
        std::uint64_t serial = 0;
        bool submitted = false;
        // The GpuMutex queue tag of the thread that opened the batch, and when it was submitted
        // (the [syncwait] line reports what a wait's target and the batches ahead of it were).
        std::uint32_t queue = 0xffffffffu;
        std::chrono::steady_clock::time_point submittedAt{};
        VkQueryPool queries = VK_NULL_HANDLE;
        VkQueryPool samples = VK_NULL_HANDLE;
        // The batch's timed ranges (BeginGpuTiming), stamps filled in when read.
        std::vector<TimedRange> timed;
        // The whole-batch timed range (BatchTimingKey) and its stamps once read (see Completed).
        std::uint32_t batchTiming = NoTiming;
        double gpuStartNs = 0;
        double gpuEndNs = 0;
        std::uint64_t readGeneration = 0;
        // Dword addresses this batch noted in the label table (removed when it finishes).
        std::vector<std::uint64_t> labelDwords;
        // Labels stored by this batch's completion actions (see AfterCompletions); subtracted from
        // the lock-free pending count when the batch finishes, whether or not its completions ran.
        std::uint32_t completionLabelCount = 0;
        // Write-back completions (OnComplete), likewise.
        std::uint32_t writeBackCompletionCount = 0;
        // The ranges of the completion labels also stored on the GPU, and whether each entered the
        // pending count (NoteWrittenBack counts one the first time a write-back overlaps it).
        struct CompletionLabel {
            std::uint64_t begin;
            std::uint64_t end;
            bool counted;
        };
        std::vector<CompletionLabel> completionLabelRanges;
        // The store run in progress (RecordStore): whether its leading barrier was recorded and
        // the trailing one is still owed, the store not yet recorded (joined by contiguous ones),
        // and the [buffer, offset, end) of the stores already recorded (the WAW check).
        struct StoreRun {
            bool open = false;
            VkBuffer buffer = VK_NULL_HANDLE;
            VkDeviceSize offset = 0;
            std::vector<std::byte> bytes;
            std::vector<std::tuple<VkBuffer, VkDeviceSize, VkDeviceSize>> recorded;
            // The run's [gputime] range (leading barrier to trailing barrier) and its stored bytes.
            std::uint32_t timing = NoTiming;
            std::uint64_t storedBytes = 0;
            // The stores queued for the batch's run (per-batch runs; see RecordStore), in order.
            struct Queued {
                VkBuffer buffer;
                VkDeviceSize offset;
                std::vector<std::byte> bytes;
                std::uint64_t address;
            };
            std::vector<Queued> queued;
        } run;
        // See Commands(): the accesses the last recorded command's trailing barrier covers.
        VkAccessFlags coveredAccess = 0;
        // The hazard tracker's accesses since its last simulated barrier (see NoteAccess).
        struct Tracker {
            struct Range {
                std::uint64_t begin;
                std::uint64_t end;
                VkPipelineStageFlags stages;
            };
            std::vector<Range> reads;
            std::vector<Range> writes;
            struct Image {
                VkImage image;
                bool written;
                VkPipelineStageFlags stages;
            };
            std::vector<Image> images;
        } tracker;
        // The render pass a recorded draw left open (see ContinuesRenderPass).
        struct RenderPass {
            bool open = false;
            bool continuable = false;
            std::uint64_t key = 0;
            std::uint32_t timing = NoTiming;
            // Counts the passes this recorder left open (OpenRenderPassSerial).
            std::uint64_t serial = 0;
        } renderPass;
        // A pass ended in this batch: Submit records the host-read barrier its draws left out.
        bool hostReadOwed = false;
        // Queued DCC key stores (see QueueKeyStore).
        struct KeyStore {
            VkBuffer buffer;
            VkDeviceSize first;
            VkDeviceSize last;
            std::shared_ptr<void> seed;
            std::uint64_t begin;
            std::uint64_t end;
        };
        std::vector<KeyStore> keyStores;
    };
    struct LabelEntry {
        std::uint32_t value;
        std::uint32_t queue;
        std::uint64_t stamp;
        // Null for a queued label (NoteQueuedLabel): not recorded yet, so no batch removes it.
        const Batch* batch;
        // Write-tracker generation at the close of the entry's record group (CloseLabelGroup);
        // 0 while the group is open. A recorded non-label GPU write over the dword noted after the
        // entry sets `overwritten`: memory will not end with `value`. Both serve the late rule only.
        std::uint64_t generation = 0;
        bool overwritten = false;
        // The value lands in memory only when the batch's completion actions run (LabelRefusal::BehindCompletion).
        bool behindCompletion = false;
    };
    void readGpuTiming(Batch& batch);
    void beginSamples(Batch& batch);
    void readSamples(Batch& batch);
    bool countingSamples = false;
    // BeginGpuTiming on the open batch without Commands() (RecordStore times its own run, which
    // Commands() would close); `kind` as the digest takes it.
    std::uint32_t beginTiming(std::uint64_t key, TimingKind kind);
    // The public Begin*Timing: the switch, the batch, then beginTiming.
    std::uint32_t startGpuTiming(std::uint64_t key, TimingKind kind);
    // Vulkan entry points resolved once (constructor): the loader's vkGetDeviceProcAddr is a name
    // lookup per call, paid by every reap, store and submit otherwise. APS5_NO_PROC_TABLE=1 leaves
    // them null and resolves per call as before; vkWaitSemaphoresKHR stays a lazy lookup (timeline
    // semaphores are optional).
    PFN_vkGetFenceStatus getFenceStatus = nullptr;
    PFN_vkResetFences resetFences = nullptr;
    PFN_vkWaitForFences waitForFences = nullptr;
    PFN_vkCmdUpdateBuffer cmdUpdateBuffer = nullptr;
    PFN_vkBeginCommandBuffer beginCommandBuffer = nullptr;
    PFN_vkEndCommandBuffer endCommandBuffer = nullptr;
    PFN_vkQueueSubmit queueSubmit = nullptr;
    PFN_vkCmdPipelineBarrier cmdPipelineBarrier = nullptr;
    template<typename TFunction>
    TFunction function(TFunction resolved, const char* name) const {
        return resolved != nullptr ? resolved : context.Function<TFunction>(name);
    }
    VkResult fenceStatus(VkFence fence) const { return function(getFenceStatus, "vkGetFenceStatus")(context.device, fence); }
    void recordBarrier(VkCommandBuffer commands, VkPipelineStageFlags sourceStage, VkPipelineStageFlags destinationStage, VkAccessFlags sourceAccess, VkAccessFlags destinationAccess) const;
    // Opens a batch when none is open, without closing a store run (Commands() does both).
    void ensureOpen();
    // Records the run's pending store (vkCmdUpdateBuffer), if any.
    void flushPendingStore();
    // Records the pending store and the run's trailing barrier; the next store starts a new run.
    // Per-batch runs: records the queued stores with their barriers (`atSubmit`: the batch-end
    // form, toward the host only); returns whether the batch's host-read barrier was included.
    bool closeStoreRun(bool atSubmit = false);
    // Ends the render pass a draw left open (vkCmdEndRenderPass, the pass's trailing barrier).
    void endOpenRenderPass();
    // Records the queued key stores as one run (`forWriter`: before a command writing, reading or
    // labelling over one, not at Submit).
    void recordKeyStores(bool forWriter);
    // Records the deferred copy-backs (DeferCopies) with their barrier pair; `claimed` too.
    void flushDeferredCopies(bool claimed = false, FlushReason reason = FlushReason::Command);
    // Records `copies` as one pass (lead barrier, the copies, trail barrier).
    void recordDeferredCopies(std::vector<DeferredCopy> copies, FlushReason reason);
    // The pass itself into `commands`: barriers, timing, covered access; returns the bytes.
    std::uint64_t recordCopyPass(VkCommandBuffer commands, std::vector<DeferredCopy> copies);
    // The copies of a pass between its barriers: plain ones as vkCmdCopyBuffer, narrow ones as
    // compare dispatches (recordNarrowCopies); returns the bytes.
    std::uint64_t recordCopyCommands(VkCommandBuffer commands, std::vector<DeferredCopy> copies);
    std::uint64_t recordNarrowCopies(VkCommandBuffer commands, const std::vector<DeferredCopy>& copies);
    // The compare pass's pipeline, made on first use; false (and narrow copies copy whole from then
    // on) when the device cannot make it.
    bool ensureNarrowPipeline();
    // Adds the dwords the compare passes stored, and the stale dwords APS5_NARROW_VERIFY found,
    // since the last look to the statistics; `complete` (every pass recorded so far completed)
    // also describes the first stale dword once.
    void noteNarrowStored(bool complete = false);
    // Waits for the fence of the in-flight batch `serial` (left in flight).
    void waitBatch(std::uint64_t serial);
    bool narrowCopyBacks = false;
    // APS5_NARROW_VERIFY=1 (with narrow copy-backs): the compare pass also reads the import where
    // the shadow equals its baseline, counts each dword the import holds otherwise (a stale
    // baseline: narrow would have lost that store) and stores the shadow's value there, as a whole
    // copy would have (the [barriers] digest's "verify: N stale dwords", the first described once).
    bool narrowVerify = false;
    std::atomic<bool> narrowFailed{false};
    std::uint64_t claimBreaks = 0;
    VkPipelineLayout narrowLayout = VK_NULL_HANDLE;
    VkPipeline narrowPipeline = VK_NULL_HANDLE;
    // The compare passes' counters (profiling or APS5_NARROW_VERIFY): word 0 the dwords stored,
    // word 1 the stale dwords verify found, words 2-6 the first of those (dword index in its span,
    // the span's guest address low and high, the import's and the shadow's value). Device-local
    // and host-visible where the device has such memory (the atomics stay off PCIe), else host
    // memory. With the last values seen.
    std::shared_ptr<Buffer> narrowStored;
    std::uint32_t narrowStoredSeen = 0;
    std::uint32_t narrowStaleSeen = 0;
    bool narrowStaleDescribed = false;
    // Commands() with the queued copy-backs recorded first unless `keepCopyBacks`.
    VkCommandBuffer commandsFor(VkAccessFlags* coveredAccess, bool keepCopyBacks);
    bool coalesceCopyBacks = false;
    // Resident buffers (KeepsResidentBuffers): at a Submit or label (`reason`), records the queued
    // copies but keeps the whole pages of the unclaimed ones `stored` does not overlap resident.
    void flushKeepingResident(FlushReason reason, const std::function<bool(std::uint64_t, std::uint64_t)>& stored);
    // A resident copy taken over by the build reusing its shadow keeps its pages guarded until the
    // open batch completed (the build's copy-back lands its bytes by then, or holds them resident).
    void keepGuards(std::vector<std::shared_ptr<ResidentGuard>> guards);
    bool resolveResident(std::uintptr_t address, bool owner);
    // Copies `copies` in order (later ones win) after everything submitted, and waits.
    void landResident(const std::vector<DeferredCopy>& copies);
    bool residentBuffers = false;
    ResidentReadSettings residentReads;
    std::uint64_t residentGeneration = 0;
    std::uint64_t passSerials = 0;
    std::vector<DeferredCopy> deferredCopies;
    std::vector<DeferredCopy> claimedCopies;

    // The unlocked wait of SyncThrough(waitUnlocked): the target batch (submitting the open one when
    // it is the target), the timeline wait with the mutex released, then the completions up to it.
    // Returns false when the locked path must run instead (no timeline, nested acquisition, off).
    bool syncThroughUnlocked(std::uint64_t address, std::uint64_t end, int source, const void* site);
    // `source` is the CountSync source the wait is attributed to.
    void finish(std::unique_ptr<Batch> batch, bool wait, int source);
    void release(Batch& batch) noexcept;
    static bool overlaps(const Batch& batch, std::uint64_t address, std::uint64_t end);
    // The first noted read of `batch` overlapping [address, end), or null.
    static const Batch::Read* readOverlap(const Batch& batch, std::uint64_t address, std::uint64_t end);
    // The reader kind of the first noted read list of `batch` overlapping [address, end), if any.
    static std::optional<ReadKind> readSetOverlap(const Batch& batch, std::uint64_t address, std::uint64_t end);
    bool signaled(const Batch& batch) const;
    // Rebuilds the lock-free snapshot of pending writes from open, inFlight and finishing.
    void publishPendingWrites() const;
    // Pending blocks (see BlockPending): marks the blocks of [address, end) with the batch serial
    // (the active recorder only), and advances CompletedSerial once a batch's completions ran.
    void markPendingBlocks(std::uint64_t address, std::uint64_t end, std::uint64_t serial) const;
    void noteBlocksFinished(std::uint64_t serial);
    // This recorder's offset of the pending-block serials, and the newest serial whose batch and
    // every earlier one finished.
    std::uint64_t blockSerialBase = 0;
    std::uint64_t blocksFinished = 0;
    // Publishes the snapshot with [address, end) added: a copy of the current snapshot with the
    // range merged in (O(N) over the merged ranges: no gathering of every batch's writes and no
    // sort), which keeps the snapshot a superset of the pending union as the full rebuild does; a
    // missing snapshot rebuilds in full. APS5_NO_INCREMENTAL_SNAPSHOT=1 rebuilds in full per note.
    void publishPendingWriteAdded(std::uint64_t address, std::uint64_t end) const;
    // Appends one range to the open batch; returns whether the snapshot must be rebuilt for it.
    // `ownLabel`: the range is a label's own store (NoteLabel, AfterCompletions), which does not
    // overwrite the table entries it covers; any other range flags them (table mutex, briefly).
    bool noteWrite(std::uint64_t address, std::size_t bytes, bool ownLabel = false, WriteKind kind = WriteKind::Unknown);
    // Appends one range to `batch` (open or in flight) and publishes the snapshot if needed.
    void noteWriteOn(Batch& batch, std::uint64_t address, std::size_t bytes, bool ownLabel = false, WriteKind kind = WriteKind::Unknown);
    void noteLabelOn(Batch& batch, std::uint64_t address, std::span<const std::byte> bytes, std::uint64_t stamp, std::uint32_t queue, bool behindCompletion = false);
    // Flags the recorded entries of `batch` over [begin, end) as stored by a completion action.
    void markBehindCompletion(const Batch& batch, std::uint64_t begin, std::uint64_t end);
    // Flags the recorded entries of the dwords overlapping [address, end) as overwritten.
    void markOverwritten(std::uint64_t address, std::uint64_t end);
    // Whether a write-back noted after `sequence` overlapped [begin, end); true when the ring no
    // longer reaches back to `sequence` (conservative: the store runs as before).
    bool writtenBackSince(std::uint64_t sequence, std::uint64_t begin, std::uint64_t end);
    HostMutex writtenBackMutex;
    std::deque<std::array<std::uint64_t, 3>> writtenBack;
    std::uint64_t writtenBackSequence = 0;
    // PendingLabel without the table mutex (the caller holds it, or the GPU mutex). `countHits`:
    // a hit on a queued label counts as a same-queue wait hit ([labels] line); LabelValueIn's
    // range scan, which is no wait, passes false.
    std::optional<LabelHit> lookupLabel(std::uint64_t address, std::size_t bytes, std::uint64_t afterStamp, LabelRefusal* refusal, bool countHits = true) const;
    // Whether the in-flight batch with that serial has not signaled its fence (false when it is
    // not in flight any more): what makes a wait for it a real GPU wait (ThreadHookWaits).
    bool unsignaled(std::uint64_t serial) const;

    Context context;
    VkSemaphore timeline = VK_NULL_HANDLE;
    // Identity for a thread that released the mutex around a wait (syncThroughUnlocked): the
    // recorder may have been torn down meanwhile, so it is looked up by this id, not by pointer.
    std::uint64_t id = 0;
    // Every access holds the table mutex (Recorder.cpp): the recorded entries are mutated under
    // GuestMemory::GpuMutex as well, the queued ones (NoteQueuedLabel) under the table mutex alone.
    // Ordered, so a range (a noted write, PendingLabelIn) finds its dwords by lower_bound.
    std::map<std::uint64_t, LabelEntry> labels;
    // The queued entries (batch always null), by dword; a lookup on the noting queue's thread
    // prefers them (its newest store in program order), every other lookup sees `labels` alone.
    std::map<std::uint64_t, LabelEntry> queuedLabels;
    // labels.size(), readable without the table mutex: a noted write skips the mutex while the
    // table is empty (most of the time between label groups).
    std::atomic<std::size_t> recordedLabels{0};
    std::unique_ptr<Batch> open;
    std::deque<std::unique_ptr<Batch>> inFlight;
    // Batches popped from inFlight whose completions have not run yet: their writes stay in the
    // snapshot (a rebuild from inside a completion must not drop them) until finish returns.
    std::vector<const Batch*> finishing;
    std::uint64_t submissions = 0;
    std::uint64_t writeNoteCount = 0;
    // Command buffers and fences of completed batches, reused by later ones (hundreds of batches per
    // frame would otherwise allocate and free their objects each time).
    std::vector<std::pair<VkCommandBuffer, VkFence>> spare;
    std::vector<VkQueryPool> sparePools;
    mutable HostMutex completedMutex;
    std::array<Completed, CompletedRingSize> completed;
    std::uint64_t newestSubmitted = 0;
    std::chrono::steady_clock::time_point newestSubmittedAt{};
    using DrawSnapshotKey = std::tuple<std::uint64_t, SnapshotUse, std::size_t>;
    struct DrawSnapshot {
        std::uint64_t generation;
        std::uint64_t registryGeneration;
        std::list<DrawSnapshotKey>::iterator recent;
        std::shared_ptr<Buffer> buffer;
        std::uint32_t derived;
    };
    struct DrawSnapshotPool {
        std::list<DrawSnapshotKey> recency;
        std::size_t bytes = 0;
    };
    std::map<DrawSnapshotKey, DrawSnapshot> drawSnapshots;
    std::array<DrawSnapshotPool, 2> drawSnapshotPools;
    void eraseDrawSnapshot(std::map<DrawSnapshotKey, DrawSnapshot>::iterator entry);
};

// APS5_PROFILE_GPU: the [gputime] totals of the timed ranges between two reports and the report's
// lines. The recorder adds every finished batch's ranges (AddBatch) and the presenter's blit
// (AddClass) under its timing mutex, and prints Report every 10 s:
// - the program and class line (programs keyed by guest address, with the MiB they bind in place);
// - one "row" line per bucket of GPU time per present, the guest's own work (programs, draws, the
//   guest's copies, fills and label stores) apart from the emulator's (copy-backs, staging copies,
//   detile uploads, retile write-backs, the dispatch barrier ranges, the rest), plus "untimed"
//   (batch spans outside every range): the rows add up to the batch spans ("busy");
// - the split by guest queue (the GpuMutex tag of the thread that recorded each range);
// - the draw passes by target address, with their draws and the MiB they bind in place.
class GpuTimingDigest {
public:
    using CommandClass = Recorder::CommandClass;
    struct Totals {
        std::uint64_t count = 0;
        double ms = 0;
        std::uint64_t bytes = 0;
        std::uint64_t read = 0;
        std::uint64_t written = 0;
        std::uint64_t inputs = 0;
        std::uint64_t leased = 0;
        std::uint64_t draws = 0;
    };
    struct QueueTotals {
        // The union of the queue's ranges, the program and class ranges summed, its range count;
        // the spans of the batches its threads opened.
        double timedMs = 0;
        double programMs = 0;
        double classMs = 0;
        std::uint64_t ranges = 0;
        double batchMs = 0;
        std::uint64_t batches = 0;
    };
    static constexpr std::size_t Classes = static_cast<std::size_t>(CommandClass::Count);
    // A finished batch's ranges, their stamps in ticks of `period` nanoseconds.
    void AddBatch(std::span<const Recorder::TimedRange> ranges, double period);
    // A range timed outside the batches (the presenter's blit, `queue` its thread's tag).
    void AddClass(CommandClass which, double nanoseconds, std::uint64_t bytes, std::uint32_t queue);
    // Programs (dispatches and the guest copies' transfers) by key, classes, draw passes by target.
    const std::map<std::uint64_t, Totals>& Programs() const { return programs; }
    const Totals& Class(CommandClass which) const { return classes[static_cast<std::size_t>(which)]; }
    const std::map<std::uint64_t, Totals>& Passes() const { return passes; }
    const std::map<std::uint32_t, QueueTotals>& Queues() const { return queues; }
    // The dispatch program ranges alone, and the guest copies' transfers alone.
    const Totals& Dispatches() const { return dispatches; }
    const Totals& Transfers() const { return transfers; }
    // Sums: program ranges (dispatches and transfers, the [gputime] line's first field), dispatches
    // alone, classes, the union of every range, the batch spans, and the batches.
    double ProgramMs() const { return programMs; }
    double DispatchMs() const { return dispatchMs; }
    double ClassMs() const { return classMs; }
    double UnionMs() const { return unionMs; }
    double BatchMs() const { return batchMs; }
    std::uint64_t Batches() const { return batches; }
    // Ranges Submit ended (TimedRange::leftOpen): counted, in no total and not in the union.
    std::uint64_t LeftOpen() const { return leftOpen; }
    // The report's lines (no newline), given the presents, the ranges dropped at the cap and the
    // ranges refused inside a render pass over the window.
    std::vector<std::string> Report(std::uint64_t presents, std::uint64_t dropped, std::uint32_t cap, std::uint64_t refusedInPass = 0) const;
    void Clear() { *this = GpuTimingDigest{}; }

private:
    std::map<std::uint64_t, Totals> programs;
    std::array<Totals, Classes> classes{};
    std::map<std::uint64_t, Totals> passes;
    std::map<std::uint32_t, QueueTotals> queues;
    Totals dispatches;
    Totals transfers;
    double programMs = 0;
    double dispatchMs = 0;
    double classMs = 0;
    double unionMs = 0;
    double batchMs = 0;
    std::uint64_t batches = 0;
    std::uint64_t leftOpen = 0;
};

}

#endif
