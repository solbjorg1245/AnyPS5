#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DRIVER_DRIVER_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DRIVER_DRIVER_HPP

#include "prx/libSceAgcDriver/Execution/include/Driver/Queues/Submission.hpp"
#include "prx/libc/include/HostMutex.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/DeviceAccess.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Packets/PacketHistory.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Dispatch/DispatchCache.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Draw/DrawCache.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Dispatch/DispatchTiming.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Draw/DrawTiming.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Memory/BufferCopy.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Dispatch/IndirectDispatch.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Memory/WriteEvidence.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Packets/PacketTiming.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Synchronization/WaitMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/Pm4.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Texture.hpp"
#include "prx/libSceAgcDriver/Graphics/include/ShaderInputState.hpp"
#include "prx/libc/include/Shutdown.hpp"
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <exception>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <span>
#include <stop_token>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace AgcDriver::DriverDetail {

class Driver {
public:
    static Driver& Get();
    ~Driver();
    void Shutdown();
    void WaitIdle();
    void CheckFailure();
    void ReportFailure(std::exception_ptr error);
    void Submit(const Packet* packet, std::uint32_t queue);
    void SuspendPoint();
    void RegisterVideoOutput(std::uint32_t handle, const std::shared_ptr<IVideoOutput>& output);
    void UnregisterVideoOutput(std::uint32_t handle, const std::shared_ptr<IVideoOutput>& output);
    void Present(const PresentationWindow& window, const DisplayBuffer* buffer, bool opaque, void (*gpuReady)(void*), void* context);
    void ReleaseWindow(void* window);
    void RegisterShader(const Shader* shader);

private:
    friend class SampledReadScope;
    friend struct PendingView;
    friend struct PacketTimer;
    void stop();
    Driver();
    void rethrowFailure() const;
    void checkStopping() const;
    static bool& onWorkerThread();
    static void copyCommands(Submission& submission, const std::uint32_t* guest, std::size_t words);
    static bool copySegment(Submission& submission, const std::uint32_t* guest, std::size_t words, std::size_t& budget);
    void waitForFlipRoom(const Submission& submission);
    void reserveOutputs(Submission& submission);
    void executeRewindTail(const Submission& stalled);
    void enqueue(Submission submission);
    void noteHeldAtSubmit(Submission& submission, std::size_t cursor);
    static void forgetUnfinishedWrites(QueueWorker& worker, const Submission& submission);
    void reportPresents(double waitedMs, std::size_t inFlight);
    static bool stampValidate();
    static bool dataHits();
    static bool verifyDataHits();
    static bool tsharpMask();
    static bool vsharpBases();
    static bool relocatedHits();
    // A dispatch lookup tries the front variant's relocation before the stored variants
    // (DispatchLookup.cpp); APS5_NO_RELOCATION_FIRST=1 compares the stored variants first.
    static bool relocationFirst();
    static RelocationVerdict learnRelocation(const DispatchVariant& old, DispatchVariant& fresh, std::uint64_t externalDelta = 0);
    static void traceFailedRelocation(std::uint64_t program, const DispatchVariant& candidate);
    static std::shared_ptr<DispatchVariant> relocateVariant(const DispatchVariant& variant, std::uint64_t& counterUnordered);
    static std::shared_ptr<DispatchVariant> shiftVariant(const DispatchVariant& variant, std::uint64_t delta, std::uint64_t& counterUnordered);
    static bool drawRelocation();
    static bool staysOnMove(const DispatchVariant& variant);
    static std::size_t pointerWordIndex(const DrawProgram& program, std::uint32_t reg);
    void findRelocationCandidates(const DrawKey& key, std::vector<DrawRelocationCandidate>& candidates);
    static bool quickShiftedMatch(const DispatchVariant& variant, std::uint64_t delta);
    bool chooseRelocationCandidate(DrawRelocation& relocation);
    static std::shared_ptr<DrawDecode> relocatedDecode(const DrawDecode& decode, const DrawKey& key);
    void rekeyDrawEntryLocked(const DrawRelocation& relocation, const std::vector<std::shared_ptr<DispatchVariant>>& matched, const std::vector<std::size_t>& ranks);
    void indexDrawKeyLocked(const DrawKey& key);
    void unindexDrawKeyLocked(std::uint64_t base, std::uint64_t key);
    static std::size_t dispatchVariants();
    static bool insertCompare();
    static std::size_t dispatchCacheEntries();
    static std::uint64_t variantBytes(const DispatchVariant& variant);
    void accountVariant(const DispatchVariant& variant, bool added);
    void eraseDispatchEntry(std::unordered_map<std::uint64_t, std::shared_ptr<DispatchEntry>>::iterator it);
    void classifyDiffering(std::uint64_t program, std::uint64_t key, const DispatchVariant& old, const DispatchVariant& fresh, const ShaderRecompiler::ResourceCapture* capture, EntryCounters& counters);
    void reportDispatchCache(EntryCounters& counters);
    // outcome: 0 hit, 1 no entry, 2 an entry that missed (APS5_PROFILE_DRAW).
    void noteDispatchKey(std::uint32_t queue, std::uint64_t program, std::uint64_t key, int outcome, const std::vector<std::uint32_t>& userData, const std::array<std::uint32_t, 5>& registers, const void* shader);
    void lookupDispatch(std::uint64_t address, const Submission& submission, std::uint64_t key, bool noDispatchCache, bool traceCache, bool profile, std::span<const ShaderRecompiler::MemoryRegion> memory, DispatchPhaseTiming& phaseTiming, std::array<double, DriverPhaseCount>& phaseMs, std::shared_ptr<const ShaderRecompiler::RecompileResult>& compiledResult, std::shared_ptr<DispatchVariant>& keepVariant, std::vector<ShaderRecompiler::MemoryRegion>& captured, std::vector<std::uint32_t>& liveWords, bool& dataHit, bool& cached, bool& validated, std::shared_ptr<DispatchEntry>& missedEntry, bool& missedDiffering, std::shared_ptr<DispatchVariant>& relocated, std::uint64_t baseKey, std::span<const std::uint32_t> userData, std::vector<UserPointerCandidate>& baseCandidates);
    void insertDispatch(std::uint64_t address, std::uint64_t key, bool noDispatchCache, bool profile, const std::shared_ptr<const ShaderSnapshot>& registeredShader, std::uint64_t forgetAtCapture, std::span<const ShaderRecompiler::MemoryRegion> memory, const std::shared_ptr<ShaderMemory>& shaderMemory, const std::vector<ShaderRecompiler::MemoryRegion>& captured, const std::shared_ptr<const ShaderRecompiler::ResourceCapture>& capture, const std::shared_ptr<const ShaderRecompiler::RecompileResult>& compiledResult, const std::shared_ptr<DispatchEntry>& missedEntry, bool missedDiffering, std::shared_ptr<DispatchVariant>& attachVariant, DispatchPhaseTiming& phaseTiming, std::uint64_t baseKey, std::span<const std::uint32_t> userData, const std::vector<UserPointerCandidate>& baseCandidates);
    // User-pointer relocation (DispatchRelocation.cpp).
    static bool userPointerRelocation();
    static std::size_t userPointerCandidateLimit();
    void collectUserPointerCandidates(std::uint64_t baseKey, std::span<const std::uint32_t> userData, std::vector<UserPointerCandidate>& candidates);
    void indexDispatchKey(std::uint64_t baseKey, std::uint64_t key, std::span<const std::uint32_t> userData, const DispatchVariant* variant);
    static std::uint64_t blockSignature(const DispatchVariant& variant, std::span<const std::uint32_t> words);
    void relocateByUserPointer(std::uint64_t address, const Submission& submission, std::uint64_t key, std::uint64_t baseKey, std::span<const std::uint32_t> userData, std::span<const ShaderRecompiler::MemoryRegion> memory, const std::vector<UserPointerCandidate>& candidates, std::shared_ptr<const ShaderRecompiler::RecompileResult>& compiledResult, std::vector<ShaderRecompiler::MemoryRegion>& captured, bool& cached, std::shared_ptr<DispatchVariant>& relocated);
    static bool sameInstance(const DispatchVariant& old, const DispatchVariant& fresh, std::uint64_t delta);
    void dispatch(QueueState& queue, std::span<const std::uint32_t> packet, const Submission& submission, std::uint64_t indirectArguments = 0);
    void verifyDataHit(const ShaderSnapshot& snapshot, std::size_t codeOffset, std::uint64_t deviceSerial, ShaderRecompiler::RecompileRequest request, std::span<const ShaderRecompiler::MemoryRegion> memory, std::uint64_t address, const DispatchVariant& variant, std::span<const std::uint32_t> liveWords, const ShaderRecompiler::RecompileResult& patched);
    static bool drawEntries();
    static bool verifyDrawEntries();
    static bool registerKeyEnabled();
    static bool verifyDrawRecipe();
    static std::size_t drawCacheEntries();
    static std::size_t drawEvictedKeyBound();
    void noteDrawEvictionLocked(std::uint64_t key);
    void noteAbsentDrawKeyLocked(std::uint64_t key, std::uint64_t base, const DrawKey* drawKey = nullptr);
    static bool newDrawKeyTop();
    void accountDrawVariant(const DispatchVariant& variant, bool added);
    void insertDrawEntry(const DrawKey& key, std::vector<std::shared_ptr<DispatchVariant>>& fresh, std::shared_ptr<const DrawDecode> decode, const DrawRelocation* relocation, const std::vector<std::shared_ptr<DispatchVariant>>* matched);
    std::shared_ptr<const DrawRecipe> findDrawRecipe(std::uint64_t key, const std::vector<std::shared_ptr<DispatchVariant>>& stages);
    void attachDrawRecipe(std::uint64_t key, const std::vector<std::shared_ptr<DispatchVariant>>& stages, std::shared_ptr<const DrawRecipe> recipe);
    void reportDrawCache(DrawEntryCounters& counters);
    void reportNewDrawKeys();
    static DrawKey drawRegisterKey(const QueueState& queue, const ShaderRegistry& registry, std::uint64_t deviceSerial);
    static bool sameVertexInfo(const ShaderRecompiler::ShaderVertexStageInfo& a, const ShaderRecompiler::ShaderVertexStageInfo& b);
    static bool sameDecode(const DrawDecode& a, const DrawDecode& b);
    std::shared_ptr<DrawDecode> decodeDraw(const QueueState& queue, const Submission& submission);
    void resolveDrawDecode(const QueueState& queue, const Submission& submission, std::shared_ptr<const DrawDecode>& decode, bool registerKey, std::uint64_t drawKey, bool profile);
    void lookupDraw(const Submission& submission, const std::shared_ptr<VulkanDevice>& localDevice, const Graphics::State& graphics, const ShaderRecompiler::ShaderPixelStageInfo& pixel, const std::vector<DrawProgram>& programs, const std::vector<ShaderRecompiler::ProgramRole>& roles, const std::vector<std::optional<ShaderRecompiler::ShaderVertexStageInfo>>& vertexInfos, bool useDrawEntries, bool registerKey, bool profile, std::uint64_t& drawKey, std::shared_ptr<DrawEntry>& entry, std::vector<std::shared_ptr<DispatchVariant>>& matched, std::vector<std::vector<ShaderRecompiler::MemoryRegion>>& matchedRegions, DrawStageHits& hits, bool& drawHit, bool& verifyHit, DrawPhaseTiming& phaseTiming, std::array<double, DrawDriverPhaseCount>& phaseMs, DrawRelocation* relocation);
    std::shared_ptr<const ShaderRecompiler::RecompileResult> compileDrawStage(std::size_t i, std::uint32_t pushOffset, const QueueState& queue, const Submission& submission, const std::vector<DrawProgram>& programs, const Graphics::State& graphics, const ShaderRecompiler::ShaderPixelStageInfo& pixel, const std::vector<std::optional<ShaderRecompiler::ShaderVertexStageInfo>>& vertexInfos, std::vector<ShaderRecompiler::MemoryRegion>& memory, const std::vector<ShaderRecompiler::LinkedProgram>& linked, const Pm4::DrawParameters& drawParameters, const std::shared_ptr<VulkanDevice>& localDevice, ShaderMemory& shaderMemory, std::vector<StageCapture>& stageCaptures, std::vector<bool>& recompiled, bool drawHit, const std::vector<std::shared_ptr<DispatchVariant>>& matched, const std::vector<std::vector<ShaderRecompiler::MemoryRegion>>& matchedRegions, bool profile, std::uint64_t dumpTarget, std::uint64_t dumpSlot1, std::uint64_t& captures, DrawPhaseTiming& phaseTiming, std::array<double, DrawDriverPhaseCount>& phaseMs);
    void cacheDrawStages(bool useDrawEntries, bool drawHit, const Pm4::DrawParameters& drawParameters, const std::optional<Graphics::IndirectDrawPath>& indirectCpu, const std::vector<DrawProgram>& programs, const std::vector<StageCapture>& stageCaptures, const std::vector<std::optional<ShaderRecompiler::ShaderVertexStageInfo>>& vertexInfos, const std::vector<std::vector<Graphics::DecodeRead>>& decodeReads, bool verifyHit, const std::vector<std::shared_ptr<DispatchVariant>>& matched, const DrawStageHits& hits, std::vector<std::shared_ptr<DispatchVariant>>& fresh, const DrawKey& drawKey, bool registerKey, const std::shared_ptr<const DrawDecode>& decode, DrawPhaseTiming& phaseTiming, DrawRelocation* relocation);
    static bool drawPrecheck();
    std::optional<DrawVerdict> precheckDraw(const QueueState& queue, const Submission& submission, std::span<const std::uint32_t> packet, const Pm4::DrawParameters& drawParameters, std::string& rejected, bool& traceIndirect);
    std::optional<DrawVerdict> precheckRegisters(const QueueState& queue, bool indexed, std::uint32_t indexSize, std::string& rejected);
    // The draw state key and its memo (APS5_FAST_STATE, FastState.cpp): the precheck verdict, the
    // draw key from the state key and the live user words, and the decode of a key without an entry.
    static bool fastState();
    static bool fastStateVerify();
    FastStateEntry& fastStateEntry(const QueueState& queue, const Submission& submission);
    std::optional<DrawVerdict> fastPrecheckRegisters(const QueueState& queue, const Submission& submission, const Pm4::DrawParameters& drawParameters, std::string& rejected);
    DrawKey fastDrawKey(const QueueState& queue, const Submission& submission, std::uint64_t deviceSerial);
    std::shared_ptr<const DrawDecode> fastDrawDecode(const QueueState& queue, const Submission& submission);
    // The draw fast path (APS5_FAST_DRAW, FastDraw.cpp, draw-fastpath.md F3b): the draw's verdict
    // when the fast path took it (Drawn, or Rejected by a known validation failure), nullopt when it
    // declined and Driver::draw goes on with the packet (nothing recorded). `retrying` (a snapshot
    // retry or the locked prepare) and `debugMode` (a probe or dump) decline at once.
    std::optional<DrawVerdict> fastDraw(QueueState& queue, const Submission& submission, const Pm4::DrawParameters& drawParameters, std::string& rejected, bool traceIndirect, bool retrying, bool debugMode, DrawPhaseTiming& phaseTiming);
    static void reportFastState();
    static std::uint32_t drawUserWord(const DrawProgram& program, std::int32_t sgpr);
    std::optional<Graphics::IndirectDrawPath> classifyIndirectDraw(const ShaderRecompiler::RecompileResult& result, const Graphics::State& graphics, const DrawProgram& frontProgram, const std::shared_ptr<VulkanDevice>& localDevice, Pm4::DrawParameters& drawParameters, bool traceIndirect);
    DrawVerdict draw(QueueState& queue, std::span<const std::uint32_t> packet, const Submission& submission, std::string& rejected);
    void addDriverPhases(DispatchClass which, const PendingDispatchPhases& pending);
    static PendingDispatchPhases& pendingDispatchPhases();
    static std::chrono::steady_clock::time_point& packetStartedAt();
    static PendingDrawPhases& pendingDrawPhases();
    void addDrawPhases(const std::array<double, DrawDriverPhaseCount>& ms, bool drawn, std::uint64_t captures);
    void noteLabelStore(std::uint64_t address, std::span<const std::byte> bytes, std::uint64_t stamp);
    bool storedSince(std::span<const std::uint32_t> packet, std::uint64_t address, std::size_t bytes, std::uint64_t received);
    static void traceLabel(std::span<const std::uint32_t> packet, std::uint32_t queue);
    static std::array<WriteRecord, 16384>& writeHistory();
    static std::size_t& writeCursor();
    static void dumpPackets(std::span<const std::uint32_t> commands, const std::uint32_t* guest = nullptr);
    static void validate(std::span<const std::uint32_t> commands, std::uint32_t queue, const std::uint32_t* guest = nullptr);
    static void reportSkip(const char* kind, const std::string& what);
    static std::string dumpRequest(std::uint64_t address, const ShaderRecompiler::RecompileRequest& request);
    static bool matchesFillKernel(std::span<const std::uint32_t> code, const std::vector<std::uint32_t>& userData, const ShaderRecompiler::ShaderComputeStageInfo& compute);
    static bool fillClearEnabled();
    static bool fillClearExactOnly();
    static void fillClearCount(const Graphics::StorageTexture::FillCoverage& coverage, std::size_t bytes, std::span<const std::uint32_t, 4> pattern, std::size_t discarded, bool cleared, const char* refusal);
    bool fillBuffer(QueueState& queue, std::uint32_t queueId, std::span<const std::uint32_t> packet, std::span<const std::uint32_t> code, const std::vector<std::uint32_t>& userData, const ShaderRecompiler::ShaderComputeStageInfo& compute, const std::shared_ptr<VulkanDevice>& localDevice);
    static bool matchesCopyKernel(std::span<const std::uint32_t> code, const std::vector<std::uint32_t>& userData, const ShaderRecompiler::ShaderComputeStageInfo& compute);
    static void countCopy(int path, std::size_t bytes, std::chrono::steady_clock::time_point started, std::chrono::steady_clock::time_point locked, const VulkanDevice::CopyOutcome* outcome = nullptr);
    void noteCopyWriter(std::uint64_t program, std::uint64_t begin, std::uint64_t end, std::uint32_t queue, std::span<const std::byte> value = {}, std::uint64_t generation = 0);
    static bool copyKnownValues();
    static bool knownValueVerify();
    bool copyBuffer(QueueState& queue, std::uint32_t queueId, std::span<const std::uint32_t> packet, std::span<const std::uint32_t> code, const std::vector<std::uint32_t>& userData, const ShaderRecompiler::ShaderComputeStageInfo& compute, const std::shared_ptr<VulkanDevice>& localDevice, std::uint64_t programAddress);
    void traceCopyRefused(const VulkanDevice::CopyOutcome& outcome, std::uint64_t source, std::uint64_t destination, std::size_t bytes, const std::shared_ptr<VulkanDevice>& localDevice);
    static bool traceCopy();
    void traceCopyPending(const char* what, std::uint64_t address, std::size_t bytes, std::uint64_t source, std::uint64_t destination, std::size_t copyBytes, const std::shared_ptr<VulkanDevice>& localDevice);
    static void countIndirect(int path, double readMs);
    void dispatchIndirect(QueueState& queue, std::span<const std::uint32_t> packet, const Submission& submission);
    static bool& sampledRead();
    // Set while a draw packet runs again after a SnapshotStale (Draw.cpp): queryPendingWrite answers
    // Sync for every word a recorded write still covers, so the capture waits for the writer.
    static bool& forcedSyncReads();
    static bool writeEvidenceEnabled();
    static bool writerKeyedEvidence();
    static bool foreignWriters();
    static bool observationGuard();
    static bool siteSampling();
    static std::uint64_t hookWaits();
    static ShaderMemory::HookWaitCounter hookWaitCounter();
    static bool validateSkipEnabled();
    static bool validateLegacy();
    static bool gateRetry();
    static bool validateSkipVerify();
    static std::uint32_t writeEvidenceAfter();
    static std::uint32_t writeEvidenceSampleEvery();
    static std::uint64_t writeEvidenceMaxBytes();
    static bool traceCapSync();
    void observeDword(std::uint64_t address, bool unchanged, std::uint32_t value);
    void observeRange(std::uint64_t address, std::span<const std::byte> before);
    static void observePendingWrite(std::uint64_t address, bool unchanged, std::uint32_t value);
    static bool knownValueCurrent(const WrittenBuffer& writer);
    ShaderMemory::PendingWrite classifyPendingWrite(std::uint64_t address, std::size_t bytes, std::uint64_t ValidateCounters::*& reason, const PendingView& pending, std::span<std::byte> known = {});
    static ShaderMemory::PendingWrite queryPendingWrite(std::uint64_t address, std::size_t bytes, std::span<std::byte> known);
    template<typename TVisit>
    static void forEachWrittenBuffer(const ShaderRecompiler::RecompileResult& compiled, TVisit&& visit);
    void noteWrittenBuffers(std::uint64_t program, std::uint32_t queue, const ShaderRecompiler::RecompileResult& compiled);
    void noteForeignWriter(std::uint64_t begin, std::uint64_t end, std::uint32_t queue);
    void noteDrawWriters(std::span<const Graphics::CompiledShader> stages, std::uint32_t queue);
    std::optional<WrittenBuffer> newestWriterLocked(std::uint64_t begin, std::uint64_t end) const;
    std::optional<WrittenBuffer> newestWriter(std::uint64_t begin, std::uint64_t end);
    std::string describeWriters(std::uint64_t begin, std::uint64_t end);
    static std::string describeSelf(const ShaderRecompiler::RecompileResult& compiled, std::uint64_t begin, std::uint64_t end);
    static bool traceBudget();
    void traceCapture(const char* what, std::uint64_t program, std::uint32_t queue, std::span<const ShaderRecompiler::MemoryRegion> regions, double waitedMs);
    void reportValidation(ValidateCounters& counters);
    bool captureStable(std::span<const ShaderRecompiler::MemoryRegion> captured);
    bool validateCaptured(std::uint64_t program, std::uint32_t queue, std::span<const ShaderRecompiler::MemoryRegion> captured, const ShaderRecompiler::RecompileResult& compiled, bool inPlace, const PendingView& view, bool* unmapped = nullptr, std::optional<SampledReadScope>* sampling = nullptr, const DataMask* data = nullptr);
    static void appendEntryRegions(const DispatchVariant& variant, std::vector<ShaderRecompiler::MemoryRegion>& regions, const std::vector<std::uint32_t>* words = nullptr);
    bool syncPendingRuns(std::uint64_t program, std::uint32_t queue, const ShaderRecompiler::RecompileResult& compiled, std::span<const ShaderRecompiler::MemoryRegion> regions, std::uint64_t& synced, PendingView& pending, std::optional<SampledReadScope>& sampling);
    EntryOutcome validateVariant(std::uint64_t program, std::uint32_t queue, const DispatchVariant& variant, std::span<const ShaderRecompiler::MemoryRegion> regions, std::uint64_t& imagesFlushed, std::uint64_t& runsSynced, std::optional<SampledReadScope>& sampling, std::vector<std::pair<std::uint32_t, std::uint32_t>>* live = nullptr);
    static int waitTimeoutMs();
    static bool pollReapAll();
    static bool pollTryEach();
    static bool traceLateLabels();
    void waitMemory(std::span<const std::uint32_t> packet, std::uint32_t queue, const PacketHistory& context, std::uint64_t received, bool heldAtSubmit);
    static PollStats& pollStats();
    static WaitOutcomes& waitOutcomes();
    static Graphics::Recorder::LateStatistics& lateCountsSeen();
    static EpochBumps& epochBumps();
    static void bumpEpoch(std::uint64_t EpochBumps::*counter);
    static bool packetEpoch();
    static bool labelTryEachPacket();
    static std::chrono::microseconds labelFlushDeadline();
    static bool labelFlushDue();
    static bool completionsPending();
    static bool reapEachPacket();
    static std::uint64_t batchCap();
    void recordDeferredLabels(VulkanDevice* localDevice, std::uint32_t queue);
    bool recordLabelsForPacket(VulkanDevice* localDevice, std::uint32_t queue);
    void recordQueuedLabelsBeforeRead(std::uint32_t queue);
    bool recordQueuedLabelsAfterCapture(std::uint32_t queue, std::span<const ShaderRecompiler::MemoryRegion> regions);
    void noteQueuedLabels(std::uint32_t queue);
    void recordQueuedLabelsByTry(std::uint32_t queue, std::atomic<std::uint64_t>& counter);
    static void recordQueuedLabelsFromHook();
    void flushBetweenPackets(std::uint32_t queue, std::uint32_t header, bool labelPacket);
    bool preparePacketMemory(const Submission& submission, QueueState& queue, std::span<const std::uint32_t> packet, std::uint32_t header, std::uint32_t opcode, bool& wroteOnGpu, bool& endOfPipeInterrupt, bool& interruptDeferred, bool& drawPacket, bool& sampleDump);
    void dumpSampleCounters(std::uint64_t address);
    template <typename TWork>
    static void timed(double WorkerProfile::*bucket, TWork&& work);
    template <typename TWork>
    void tolerate(const char* kind, TWork&& work);
    void traceDispatch(const QueueState& queue, std::span<const std::uint32_t> packet, std::uint32_t queueId);
    void execute(const Submission& submission);
    void markCompleted(std::uint64_t serial);
    static const std::atomic<std::uint64_t>*& workerQueued();
    void reapCompletionLabels();
    void run(std::uint32_t id) noexcept;

    std::mutex mutex;
    std::mutex shutdownMutex;
    std::condition_variable changed;

    std::map<std::uint32_t, QueueWorker> workers;
    std::uint64_t frameSerial = 0;

    std::atomic<std::uint64_t> flipsCounted{0};
    std::atomic<std::uint64_t> flipSerial{0};
    std::atomic<std::uint64_t> flipBatchesUnsignaled{0};

    alignas(64) std::atomic<int> packetsInFlight{0};
    std::atomic<std::uint64_t> packetsDone{0};

    std::atomic<std::uint64_t> eventSerial{0};
    alignas(64) std::shared_ptr<ShaderRegistry> shaders = std::make_shared<ShaderRegistry>();

    std::unordered_map<std::uint64_t, std::shared_ptr<DispatchEntry>> dispatchCache;

    std::list<std::uint64_t> dispatchOrder;
    HostMutex dispatchCacheMutex;
    std::uint64_t dispatchCacheHits = 0;
    std::uint64_t dispatchCacheEvictions = 0;

    std::uint64_t dispatchCacheVariants = 0;
    std::uint64_t dispatchCacheVariantBytes = 0;

    EntryCounters entryCounters;

    std::atomic<std::uint64_t> dataPendingMisses{0};
    // Pending regions a variant compare accepted through the data mask with refreshed words
    // (CaptureValidation.cpp pendingDataMask): against known bytes, against synced memory.
    std::atomic<std::uint64_t> dataPendingKnownRefreshed{0};
    std::atomic<std::uint64_t> dataPendingSyncedRefreshed{0};
    // Words a draw variant compare accepted through its don't-care bits (DispatchVariant::ignoredBits).
    std::atomic<std::uint64_t> ignoredBitWords{0};

    using ValueSet = std::pair<std::vector<std::pair<std::uint64_t, std::uint64_t>>, std::vector<std::uint32_t>>;
    std::unordered_map<std::uint64_t, std::deque<ValueSet>> priorValueSets;
    // (queue << 48) ^ program -> the key inputs last used (noteDispatchKey). Under dispatchCacheMutex.
    std::unordered_map<std::uint64_t, KeyInputs> lastKeyInputs;
    // Base key (the dispatch key without the user data) -> the keys inserted under it with their
    // user data, oldest first (user-pointer relocation). Under dispatchCacheMutex.
    std::unordered_map<std::uint64_t, DispatchBaseIndex> dispatchBaseIndex;
    // Base key -> the user-pointer delta of its last same-instance match (the ring's frame stride):
    // the candidate at that delta is validated first. Under dispatchCacheMutex.
    std::unordered_map<std::uint64_t, std::uint64_t> dispatchBaseStride;

    std::unordered_map<std::uint64_t, std::shared_ptr<DrawEntry>> drawCache;
    std::list<std::uint64_t> drawOrder;
    // Base key -> the keys of its entries, oldest first; a relocated hit rotates its key to the back.
    std::unordered_map<std::uint64_t, std::vector<std::uint64_t>> drawBaseIndex;
    HostMutex drawCacheMutex;
    std::uint64_t drawCacheHits = 0, drawCacheEvictions = 0, drawCacheVariants = 0, drawCacheVariantBytes = 0;
    // Keys evicted from the draw cache with the eviction count at the time, those of the last
    // drawEvictedKeyBound() evictions (APS5_DRAW_EVICTED_KEYS; on under APS5_PROFILE_DRAW): an
    // absent key found here was evicted, and the evictions since say how many more entries would
    // have kept it (DrawEntryCounters::absentEvicted). The order holds (eviction count, key) per
    // eviction, so an expiry pops its front. Under drawCacheMutex.
    std::unordered_map<std::uint64_t, std::uint64_t> drawEvictedKeys;
    std::deque<std::pair<std::uint64_t, std::uint64_t>> drawEvictedOrder;
    // The never-seen keys of the report window by base key (APS5_PROFILE_DRAW with the evicted-key
    // window; APS5_NO_NEW_DRAW_KEY_TOP=1 = off). Under drawCacheMutex.
    NewDrawKeyTally drawNewKeys;

    DrawEntryCounters drawEntryCounters;

    HostMutex driverPhasesMutex;
    std::array<DriverPhaseTotals, DispatchClassCount> driverPhaseTotals{};
    std::chrono::steady_clock::time_point driverPhasesReport = std::chrono::steady_clock::now();

    HostMutex drawPhasesMutex;
    DrawPhaseTotals drawPhaseTotals;

    std::map<std::uint32_t, QueueState> queues;
    std::map<std::uint32_t, std::shared_ptr<IVideoOutput>> outputs;
    DevicePointer device;

    HostMutex labelStoresMutex;
    std::unordered_map<std::uint64_t, std::array<LabelStore, LabelStoreHistory>> labelStores;

    std::vector<std::shared_ptr<VulkanDevice>> replacedDevices;
    std::stop_token shutdownToken = LibcShutdownToken_nid_postfix();
    DeviceUseGate deviceReplacement;
    std::uint64_t accepted = 0;
    std::uint64_t completed = 0;
    std::set<std::uint64_t> completedOutOfOrder;
    std::exception_ptr failure;

    std::atomic<bool> failed{false};
    std::atomic<bool> stopping{false};
    bool stopped = false;
    bool resetGraphics = false;

    std::uint32_t idleWaiters = 0;

    std::atomic<std::uint64_t> evidenceReads{0};
    std::atomic<std::uint64_t> evidenceValidations{0};

    HostMutex writtenBuffersMutex;
    std::deque<WrittenBuffer> writtenBuffers;
    std::uint64_t writtenBufferSerial = 0;
    std::unordered_map<std::uint64_t, DwordEvidence> dwordEvidence;
    std::atomic<std::uint64_t> observedUnchanged{0};
    std::atomic<std::uint64_t> observedChanged{0};

    std::atomic<std::uint64_t> observationsNoWait{0};
    std::atomic<std::uint64_t> observationsNotReached{0};

    HostMutex validateMutex;
    ValidateCounters validateCounters;

};

}

#endif
