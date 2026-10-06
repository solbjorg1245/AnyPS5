#include <cstdint>
#include <cstddef>
#include "SceTypes.hpp"
#include "prx/libc/include/General.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <map>
#include <mutex>
#include <vector>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <stdexcept>
#include <string>
#include <xmmintrin.h>

#ifdef _WIN32
#include <windows.h>
#endif

static constexpr int32_t SCE_OK = 0;
static constexpr int32_t SCE_FIBER_ERROR_NULL = static_cast<int32_t>(0x80590001);
static constexpr int32_t SCE_FIBER_ERROR_ALIGNMENT = static_cast<int32_t>(0x80590002);
static constexpr int32_t SCE_FIBER_ERROR_RANGE = static_cast<int32_t>(0x80590003);
static constexpr int32_t SCE_FIBER_ERROR_INVALID = static_cast<int32_t>(0x80590004);
static constexpr int32_t SCE_FIBER_ERROR_PERMISSION = static_cast<int32_t>(0x80590005);
static constexpr int32_t SCE_FIBER_ERROR_STATE = static_cast<int32_t>(0x80590006);

static constexpr std::size_t FIBER_OBJECT_SIZE = 0x100;
static constexpr std::size_t FIBER_OPT_PARAM_SIZE = 0x80;
static constexpr std::size_t FIBER_MIN_CONTEXT_SIZE = 512;
static constexpr std::uint64_t FIBER_CONTEXT_FILL = 0xdeadbeefdeadbeefull;

using GuestFiberEntry = void (APS5_VABI*)(std::uint64_t argOnInitialize, std::uint64_t argOnRun);

// A fiber that is switching away stays Suspending until its context is saved; whoever runs next
// on that host thread publishes Suspended. Fibers migrate between threads, so a resumer must never
// see Suspended before the saved stack pointer is valid.
enum class FiberState : std::uint32_t {
    Idle = 1,
    Running = 2,
    Suspending = 3,
    Suspended = 4,
};

struct Fiber {
    std::uint64_t magic;
    std::atomic<FiberState> state;
    std::uint32_t reserved;
    GuestFiberEntry entry;
    std::uint64_t argOnInitialize;
    std::uint8_t* context;
    std::uint64_t contextSize;
    void* savedStack;
    char name[FIBER_MAX_NAME_LENGTH + 1];
    bool contextSizeCheck;
};
static_assert(sizeof(Fiber) <= FIBER_OBJECT_SIZE, "guest reserves 0x100 bytes for SceFiber");

static constexpr std::uint64_t FIBER_MAGIC = 0x5245424946355041ull;

static std::atomic<bool> g_contextSizeCheck{false};

struct StackBounds {
    void* base;
    void* limit;
    void* deallocation;
};

struct ThreadFiberState {
    Fiber* current = nullptr;
    void* threadStack = nullptr;
    std::uint64_t threadFramePointer = 0;
    StackBounds threadBounds{};
    std::uint64_t transfer = 0;
    Fiber* pendingSuspend = nullptr;
};

static thread_local ThreadFiberState g_thread;

// A fiber may resume on a different host thread than the one that suspended it, so the
// thread-local state must be looked up again after every stack switch rather than cached.
__attribute__((noinline)) static ThreadFiberState& ThreadState() {
    asm volatile("" ::: "memory");
    return g_thread;
}

static bool TraceFibers() {
    static const bool enabled = std::getenv("APS5_TRACE_FIBER") != nullptr;
    return enabled;
}

// Debug aid APS5_FIBER_CHECK=1: the live part of every suspended fiber's stack (saved stack pointer
// to the context's end) is copied when it parks, and a watcher thread compares it every 2 ms until
// the fiber resumes; any change is a store by someone else into a parked stack, reported with the
// differing words (old -> new) when it happens rather than at the resume that finds a wiped frame.
struct ParkedStack {
    const void* begin = nullptr;
    std::vector<std::uint64_t> words;
    bool reported = false;
};

static bool CheckFibers() {
    static const bool enabled = std::getenv("APS5_FIBER_CHECK") != nullptr;
    return enabled;
}

static std::mutex& ParkedMutex() {
    static std::mutex mutex;
    return mutex;
}

static std::map<const Fiber*, ParkedStack>& Parked() {
    static std::map<const Fiber*, ParkedStack> parked;
    return parked;
}

// The last events of each fiber (APS5_FIBER_CHECK), printed with a parked-stack or wiped-context
// report: what ran on the stack, on which host thread, from which stack pointer, and where it
// parked. Kinds: N initialize, R run from a thread, I switched in, O switched out, T returned to
// its thread, P parked (saved stack published), U resumed (unparked), S started from its entry
// (resumed while Idle: a fresh initial frame at the context's top), F finalized, X a run or switch
// to it refused (ReportRefused).
struct FiberEvent {
    std::uint64_t ms = 0;
    std::uint32_t thread = 0;
    char kind = 0;
    const void* saved = nullptr;
    const void* sp = nullptr;
};

struct FiberEvents {
    std::array<FiberEvent, 48> ring{};
    std::size_t next = 0;
};

static std::map<const Fiber*, FiberEvents>& Events() {
    static std::map<const Fiber*, FiberEvents> events;
    return events;
}

static std::uint32_t HostThreadId() {
#ifdef _WIN32
    return GetCurrentThreadId();
#else
    return 0;
#endif
}

static std::uint64_t NowMs() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

static void NoteEvent(const Fiber* fiber, char kind) {
    if (!CheckFibers() || fiber == nullptr) return;
    const FiberEvent event{NowMs(), HostThreadId(), kind, fiber->savedStack, __builtin_frame_address(0)};
    std::lock_guard lock(ParkedMutex());
    auto& events = Events()[fiber];
    events.ring[events.next++ % events.ring.size()] = event;
}

// Caller holds ParkedMutex.
static void DumpEvents(const Fiber* fiber) {
    const auto found = Events().find(fiber);
    if (found == Events().end()) return;
    const auto& events = found->second;
    const auto count = std::min(events.next, events.ring.size());
    std::fprintf(stderr, "[fiber-check]   last %zu events of '%s' (ms, host thread, kind, saved stack, sp):\n", count, fiber->name);
    for (std::size_t i = events.next - count; i < events.next; ++i) {
        const auto& event = events.ring[i % events.ring.size()];
        std::fprintf(stderr, "[fiber-check]     %llu t%u %c saved=%p sp=%p\n", static_cast<unsigned long long>(event.ms), event.thread, event.kind, event.saved, event.sp);
    }
}

// Every other fiber's events of the last `window` ms in time order: who ran where around a change.
// Caller holds ParkedMutex.
static void DumpRecentEvents(const Fiber* except, std::uint64_t window) {
    const auto now = NowMs();
    std::vector<std::pair<const FiberEvent*, const Fiber*>> recent;
    for (const auto& [fiber, events] : Events()) {
        if (fiber == except) continue;
        const auto count = std::min(events.next, events.ring.size());
        for (std::size_t i = events.next - count; i < events.next; ++i) {
            const auto& event = events.ring[i % events.ring.size()];
            if (event.ms + window >= now) recent.emplace_back(&event, fiber);
        }
    }
    std::stable_sort(recent.begin(), recent.end(), [](const auto& a, const auto& b) { return a.first->ms < b.first->ms; });
    std::fprintf(stderr, "[fiber-check]   other fibers' events of the last %llu ms (%zu):\n", static_cast<unsigned long long>(window), recent.size());
    for (const auto& [event, fiber] : recent) {
        std::fprintf(stderr, "[fiber-check]     %llu t%u %c %p saved=%p sp=%p context=%p+0x%llx\n", static_cast<unsigned long long>(event->ms), event->thread, event->kind, static_cast<const void*>(fiber), event->saved, event->sp, static_cast<const void*>(fiber->context), static_cast<unsigned long long>(fiber->contextSize));
    }
}

// Compares a parked stack with its copy; reports the first change of each parking. Caller holds
// ParkedMutex.
static void CompareParked(const Fiber* fiber, ParkedStack& parked, const char* when) {
    if (parked.reported) return;
    const auto* live = static_cast<const std::uint64_t*>(parked.begin);
    if (std::memcmp(live, parked.words.data(), parked.words.size() * sizeof(std::uint64_t)) == 0) return;
    std::size_t differing = 0;
    for (std::size_t i = 0; i < parked.words.size(); ++i) differing += live[i] != parked.words[i];
    // A few words are the waits the fiber parked on (counters and flags on its stack that other
    // threads update); those are taken into the copy. A wipe changes many.
    if (differing < 8) {
        std::memcpy(parked.words.data(), live, parked.words.size() * sizeof(std::uint64_t));
        return;
    }
    parked.reported = true;
    std::fprintf(stderr, "[fiber-check] parked stack of '%s' changed (%s, %llu ms): %zu of %zu words differ in %p+0x%zx\n", fiber->name, when, static_cast<unsigned long long>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count()), differing, parked.words.size(), parked.begin, parked.words.size() * sizeof(std::uint64_t));
    std::size_t shown = 0;
    for (std::size_t i = 0; i < parked.words.size() && shown < 24; ++i) {
        if (live[i] == parked.words[i]) continue;
        ++shown;
        std::fprintf(stderr, "[fiber-check]   %p: %016llx -> %016llx\n", static_cast<const void*>(live + i), static_cast<unsigned long long>(parked.words[i]), static_cast<unsigned long long>(live[i]));
    }
#ifdef _WIN32
    MEMORY_BASIC_INFORMATION info{};
    if (VirtualQuery(parked.begin, &info, sizeof(info)) != 0) std::fprintf(stderr, "[fiber-check]   page: base=%p allocation=%p size=0x%llx state=0x%lx protect=0x%lx type=0x%lx\n", info.BaseAddress, info.AllocationBase, static_cast<unsigned long long>(info.RegionSize), info.State, info.Protect, info.Type);
#endif
    DumpEvents(fiber);
    DumpRecentEvents(fiber, 100);
    std::fflush(stderr);
}

static void ParkFiber(const Fiber* fiber) {
    if (!CheckFibers()) return;
    NoteEvent(fiber, 'P');
    static std::once_flag watcher;
    std::call_once(watcher, [] {
        std::thread([] {
            for (;;) {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
                std::lock_guard lock(ParkedMutex());
                for (auto& [fiber, parked] : Parked()) CompareParked(fiber, parked, "while parked");
            }
        }).detach();
    });
    const auto begin = reinterpret_cast<std::uintptr_t>(fiber->savedStack);
    const auto end = reinterpret_cast<std::uintptr_t>(fiber->context + fiber->contextSize) & ~static_cast<std::uintptr_t>(7);
    if (end <= begin) return;
    ParkedStack parked;
    parked.begin = fiber->savedStack;
    parked.words.assign(reinterpret_cast<const std::uint64_t*>(begin), reinterpret_cast<const std::uint64_t*>(end));
    std::lock_guard lock(ParkedMutex());
    Parked()[fiber] = std::move(parked);
}

static void UnparkFiber(const Fiber* fiber) {
    if (!CheckFibers()) return;
    NoteEvent(fiber, 'U');
    std::lock_guard lock(ParkedMutex());
    const auto found = Parked().find(fiber);
    if (found == Parked().end()) return;
    CompareParked(fiber, found->second, "at resume");
    Parked().erase(found);
}

// A fiber initialized over a context that a live fiber (suspended or running) still uses, or a
// suspended fiber initialized anew, runs two stacks in one: the parked fiber's frames are
// overwritten by the new one's (the 'wiped context' abort). Reported once per pair; the contexts
// of every initialized fiber are kept by address for the check.
static void ReportContextReuse(Fiber* fiber, const char* name, std::uint8_t* context, std::uint64_t size) {
    static std::mutex mutex;
    static std::map<std::uintptr_t, Fiber*> contexts;
    static int reports = 0;
    std::lock_guard lock(mutex);
    const auto describe = [](const Fiber* other) {
        return other->magic == FIBER_MAGIC ? static_cast<unsigned>(other->state.load(std::memory_order_acquire)) : 0u;
    };
    if (fiber->magic == FIBER_MAGIC && describe(fiber) != static_cast<unsigned>(FiberState::Idle) && reports < 16) {
        ++reports;
        std::fprintf(stderr, "[fiber] '%s' (%p) initialized as '%s' while in state %u\n", fiber->name, static_cast<void*>(fiber), name, describe(fiber));
    }
    const auto begin = reinterpret_cast<std::uintptr_t>(context);
    const auto end = begin + size;
    for (auto it = contexts.begin(); it != contexts.end();) {
        auto* other = it->second;
        const auto otherBegin = it->first;
        const bool current = other->magic == FIBER_MAGIC && reinterpret_cast<std::uintptr_t>(other->context) == otherBegin;
        if (!current) {
            it = contexts.erase(it);
            continue;
        }
        const auto otherEnd = otherBegin + other->contextSize;
        const auto state = describe(other);
        if (other != fiber && otherBegin < end && begin < otherEnd && state != static_cast<unsigned>(FiberState::Idle) && reports < 16) {
            ++reports;
            std::fprintf(stderr, "[fiber] '%s' (%p) initialized over %p+0x%llx, which live fiber '%s' (%p, state %u) uses at %p+0x%llx\n", name, static_cast<void*>(fiber), context, static_cast<unsigned long long>(size), other->name, static_cast<void*>(other), state, other->context, static_cast<unsigned long long>(other->contextSize));
            std::fflush(stderr);
        }
        ++it;
    }
    contexts[begin] = fiber;
}

// A refused run or switch: the title's wrappers ignore the result and carry on as if the switch had
// happened, so the calling fiber keeps running while the title's scheduler takes it for parked (a
// later resume then runs a second execution over its stack: the 'wiped context' aborts). Reported
// with the states, rate-limited.
static void ReportRefused(const char* call, const Fiber* self, const Fiber* target, int32_t result) {
    static std::atomic<int> reports{0};
    if (reports.fetch_add(1, std::memory_order_relaxed) >= 64) return;
    const auto state = [](const Fiber* fiber) { return fiber != nullptr && fiber->magic == FIBER_MAGIC ? static_cast<unsigned>(fiber->state.load(std::memory_order_acquire)) : 0u; };
    std::fprintf(stderr, "[fiber] %s refused (0x%08x) on t%u: self '%s' (state %u), target '%s' (state %u)\n", call, static_cast<unsigned>(result), HostThreadId(), self != nullptr ? self->name : "-", state(self), target != nullptr ? target->name : "-", state(target));
    NoteEvent(target, 'X');
}

static void CompletePendingSuspend() {
    auto& thread = ThreadState();
    if (thread.pendingSuspend) {
        ParkFiber(thread.pendingSuspend);
        thread.pendingSuspend->state.store(FiberState::Suspended, std::memory_order_release);
        thread.pendingSuspend = nullptr;
    }
}

#ifdef _WIN32

extern "C" void Aps5FiberSwitchStack_nid_no_patch(void** save, void* load);
extern "C" void Aps5FiberTrampoline_nid_no_patch();

asm(R"(
    .text
    .globl Aps5FiberSwitchStack_nid_no_patch
    .def Aps5FiberSwitchStack_nid_no_patch; .scl 2; .type 32; .endef
Aps5FiberSwitchStack_nid_no_patch:
    push %rbp
    push %rbx
    push %rdi
    push %rsi
    push %r12
    push %r13
    push %r14
    push %r15
    sub $0xa8, %rsp
    movaps %xmm6, 0x00(%rsp)
    movaps %xmm7, 0x10(%rsp)
    movaps %xmm8, 0x20(%rsp)
    movaps %xmm9, 0x30(%rsp)
    movaps %xmm10, 0x40(%rsp)
    movaps %xmm11, 0x50(%rsp)
    movaps %xmm12, 0x60(%rsp)
    movaps %xmm13, 0x70(%rsp)
    movaps %xmm14, 0x80(%rsp)
    movaps %xmm15, 0x90(%rsp)
    stmxcsr 0xa0(%rsp)
    fnstcw 0xa4(%rsp)
    mov %rsp, (%rcx)
    mov %rdx, %rsp
    movaps 0x00(%rsp), %xmm6
    movaps 0x10(%rsp), %xmm7
    movaps 0x20(%rsp), %xmm8
    movaps 0x30(%rsp), %xmm9
    movaps 0x40(%rsp), %xmm10
    movaps 0x50(%rsp), %xmm11
    movaps 0x60(%rsp), %xmm12
    movaps 0x70(%rsp), %xmm13
    movaps 0x80(%rsp), %xmm14
    movaps 0x90(%rsp), %xmm15
    ldmxcsr 0xa0(%rsp)
    fldcw 0xa4(%rsp)
    add $0xa8, %rsp
    pop %r15
    pop %r14
    pop %r13
    pop %r12
    pop %rsi
    pop %rdi
    pop %rbx
    pop %rbp
    ret

    .globl Aps5FiberTrampoline_nid_no_patch
    .def Aps5FiberTrampoline_nid_no_patch; .scl 2; .type 32; .endef
Aps5FiberTrampoline_nid_no_patch:
    mov %r12, %rcx
    and $-16, %rsp
    sub $32, %rsp
    call Aps5FiberMain_nid_no_patch
    ud2
)");

struct InitialFrame {
    std::uint8_t xmm[0xa0];
    std::uint32_t mxcsr;
    std::uint16_t fpuControl;
    std::uint16_t padding;
    std::uint64_t r15, r14, r13, r12, rsi, rdi, rbx, rbp;
    std::uint64_t returnAddress;
};
static_assert(sizeof(InitialFrame) == 0xa8 + 8 * 8 + 8, "initial fiber frame must match Aps5FiberSwitchStack_nid_no_patch");

static StackBounds CurrentBounds() {
    auto* tib = reinterpret_cast<NT_TIB*>(NtCurrentTeb());
    auto* teb = reinterpret_cast<std::uint8_t*>(tib);
    return {tib->StackBase, tib->StackLimit, *reinterpret_cast<void**>(teb + 0x1478)};
}

static void SetBounds(const StackBounds& bounds) {
    auto* tib = reinterpret_cast<NT_TIB*>(NtCurrentTeb());
    auto* teb = reinterpret_cast<std::uint8_t*>(tib);
    tib->StackBase = bounds.base;
    tib->StackLimit = bounds.limit;
    *reinterpret_cast<void**>(teb + 0x1478) = bounds.deallocation;
}

#else

extern "C" void Aps5FiberSwitchStack_nid_no_patch(void** save, void* load);
extern "C" void Aps5FiberTrampoline_nid_no_patch();

asm(R"(
    .text
    .globl Aps5FiberSwitchStack_nid_no_patch
    .type Aps5FiberSwitchStack_nid_no_patch, @function
Aps5FiberSwitchStack_nid_no_patch:
    push %rbp
    push %rbx
    push %r12
    push %r13
    push %r14
    push %r15
    sub $8, %rsp
    stmxcsr 0(%rsp)
    fnstcw 4(%rsp)
    mov %rsp, (%rdi)
    mov %rsi, %rsp
    ldmxcsr 0(%rsp)
    fldcw 4(%rsp)
    add $8, %rsp
    pop %r15
    pop %r14
    pop %r13
    pop %r12
    pop %rbx
    pop %rbp
    ret
    .size Aps5FiberSwitchStack_nid_no_patch, .-Aps5FiberSwitchStack_nid_no_patch

    .globl Aps5FiberTrampoline_nid_no_patch
    .type Aps5FiberTrampoline_nid_no_patch, @function
Aps5FiberTrampoline_nid_no_patch:
    mov %r12, %rdi
    and $-16, %rsp
    call Aps5FiberMain_nid_no_patch
    ud2
    .size Aps5FiberTrampoline_nid_no_patch, .-Aps5FiberTrampoline_nid_no_patch
)");

struct InitialFrame {
    std::uint32_t mxcsr;
    std::uint16_t fpuControl;
    std::uint16_t padding;
    std::uint64_t r15, r14, r13, r12, rbx, rbp;
    std::uint64_t returnAddress;
};
static_assert(sizeof(InitialFrame) == 8 + 6 * 8 + 8, "initial fiber frame must match Aps5FiberSwitchStack_nid_no_patch");

static StackBounds CurrentBounds() {
    return {};
}

static void SetBounds(const StackBounds&) {}

#endif

static StackBounds FiberBounds(const Fiber* fiber) {
    return {fiber->context + fiber->contextSize, fiber->context, fiber->context};
}

static Fiber* AsFiber(FiberObject* object) {
    auto* fiber = reinterpret_cast<Fiber*>(object);
    return fiber && fiber->magic == FIBER_MAGIC ? fiber : nullptr;
}

extern "C" [[noreturn]] void Aps5FiberMain_nid_no_patch(Fiber* fiber) {
    CompletePendingSuspend();
    fiber->entry(fiber->argOnInitialize, ThreadState().transfer);
    throw std::runtime_error(std::string("sceFiber: entry function of fiber '") + fiber->name + "' returned");
}

static void PrepareInitialStack(Fiber* fiber) {
    const auto top = reinterpret_cast<std::uintptr_t>(fiber->context + fiber->contextSize) & ~static_cast<std::uintptr_t>(15);
    auto* frame = reinterpret_cast<InitialFrame*>(top - 256);
    std::memset(frame, 0, sizeof(*frame));
    frame->mxcsr = _mm_getcsr();
    std::uint16_t control = 0;
    asm volatile("fnstcw %0" : "=m"(control));
    frame->fpuControl = control;
    frame->r12 = reinterpret_cast<std::uint64_t>(fiber);
    frame->returnAddress = reinterpret_cast<std::uint64_t>(&Aps5FiberTrampoline_nid_no_patch);
    fiber->savedStack = frame;
}

static bool AcquireForResume(Fiber* target) {
    for (;;) {
        auto state = target->state.load(std::memory_order_acquire);
        if (state == FiberState::Suspending) {
            std::this_thread::yield();
            continue;
        }
        if (state != FiberState::Idle && state != FiberState::Suspended) return false;
        if (target->state.compare_exchange_weak(state, FiberState::Running, std::memory_order_acq_rel)) {
            if (state == FiberState::Suspended) UnparkFiber(target);
            if (state == FiberState::Idle) {
                PrepareInitialStack(target);
                NoteEvent(target, 'S');
            }
            return true;
        }
    }
}

static void Resume(Fiber* target, void** save, std::uint64_t argOnRun) {
    const auto* frame = static_cast<const InitialFrame*>(target->savedStack);
    if (frame->returnAddress == 0) {
        std::fprintf(stderr, "[fiber] resuming '%s' with a wiped context: saved=%p context=%p size=0x%llx\n", target->name, target->savedStack,
                     static_cast<void*>(target->context), static_cast<unsigned long long>(target->contextSize));
#ifdef _WIN32
        MEMORY_BASIC_INFORMATION info{};
        if (VirtualQuery(target->savedStack, &info, sizeof(info)) != 0) std::fprintf(stderr, "[fiber]   page: base=%p allocation=%p size=0x%llx state=0x%lx protect=0x%lx type=0x%lx\n", info.BaseAddress, info.AllocationBase, static_cast<unsigned long long>(info.RegionSize), info.State, info.Protect, info.Type);
#endif
        const auto* words = static_cast<const std::uint64_t*>(target->savedStack);
        for (int i = 0; i < 40; i += 4) std::fprintf(stderr, "[fiber]   +0x%03x %016llx %016llx %016llx %016llx\n", i * 8, static_cast<unsigned long long>(words[i]), static_cast<unsigned long long>(words[i + 1]), static_cast<unsigned long long>(words[i + 2]), static_cast<unsigned long long>(words[i + 3]));
        if (CheckFibers()) {
            std::lock_guard lock(ParkedMutex());
            DumpEvents(target);
            DumpRecentEvents(target, 100);
        }
        std::fflush(stderr);
        std::abort();
    }
    ThreadState().current = target;
    ThreadState().transfer = argOnRun;
    SetBounds(FiberBounds(target));
    Aps5FiberSwitchStack_nid_no_patch(save, target->savedStack);
}

extern "C" {

int32_t APS5_VABI _sceFiberInitializeImpl_nid_postfix(FiberObject* object, const char* name, GuestFiberEntry entry, uint64_t arg_on_initialize, void* addr_context, uint64_t size_context, const void* opt_param, uint32_t build_version) {
    (void)opt_param;
    (void)build_version;
    if (!object || !name || !entry) return SCE_FIBER_ERROR_NULL;
    if ((reinterpret_cast<std::uintptr_t>(object) & 7) != 0) return SCE_FIBER_ERROR_ALIGNMENT;
    if (addr_context == nullptr && size_context != 0) return SCE_FIBER_ERROR_INVALID;
    if ((reinterpret_cast<std::uintptr_t>(addr_context) & 15) != 0 || (size_context & 15) != 0) return SCE_FIBER_ERROR_ALIGNMENT;
    if (addr_context == nullptr) return SCE_FIBER_ERROR_INVALID;
    if (size_context < FIBER_MIN_CONTEXT_SIZE) return SCE_FIBER_ERROR_RANGE;
    auto* fiber = reinterpret_cast<Fiber*>(object);
    ReportContextReuse(fiber, name, static_cast<std::uint8_t*>(addr_context), size_context);
    std::memset(object, 0, FIBER_OBJECT_SIZE);
    fiber->magic = FIBER_MAGIC;
    fiber->state.store(FiberState::Idle, std::memory_order_relaxed);
    fiber->entry = entry;
    fiber->argOnInitialize = arg_on_initialize;
    fiber->context = static_cast<std::uint8_t*>(addr_context);
    fiber->contextSize = size_context;
    std::strncpy(fiber->name, name, FIBER_MAX_NAME_LENGTH);
    fiber->contextSizeCheck = g_contextSizeCheck.load(std::memory_order_relaxed);
    if (fiber->contextSizeCheck) {
        auto* words = static_cast<std::uint64_t*>(addr_context);
        std::fill(words, words + size_context / sizeof(std::uint64_t), FIBER_CONTEXT_FILL);
    }
    NoteEvent(fiber, 'N');
    if (TraceFibers()) std::fprintf(stderr, "[fiber] init %s object=%p context=%p+0x%llx entry=%p\n", fiber->name, static_cast<void*>(object), addr_context, static_cast<unsigned long long>(size_context), reinterpret_cast<void*>(entry));
    return SCE_OK;
}

int32_t APS5_VABI sceFiberFinalize(FiberObject* object) {
    auto* fiber = AsFiber(object);
    if (!fiber) return object ? SCE_FIBER_ERROR_INVALID : SCE_FIBER_ERROR_NULL;
    const auto state = fiber->state.load(std::memory_order_acquire);
    if (state == FiberState::Running || state == FiberState::Suspending) return SCE_FIBER_ERROR_STATE;
    if (state == FiberState::Suspended) UnparkFiber(fiber);
    NoteEvent(fiber, 'F');
    fiber->magic = 0;
    return SCE_OK;
}

int32_t APS5_VABI sceFiberRun_nid_postfix(FiberObject* object, uint64_t arg_on_run, uint64_t* arg_on_return) {
    auto* fiber = AsFiber(object);
    if (!fiber) return object ? SCE_FIBER_ERROR_INVALID : SCE_FIBER_ERROR_NULL;
    if (ThreadState().current) {
        ReportRefused("sceFiberRun", ThreadState().current, fiber, SCE_FIBER_ERROR_PERMISSION);
        return SCE_FIBER_ERROR_PERMISSION;
    }
    if (!AcquireForResume(fiber)) {
        ReportRefused("sceFiberRun", nullptr, fiber, SCE_FIBER_ERROR_STATE);
        return SCE_FIBER_ERROR_STATE;
    }
    ThreadState().threadFramePointer = reinterpret_cast<std::uint64_t>(static_cast<void**>(__builtin_frame_address(0))[0]);
    ThreadState().threadBounds = CurrentBounds();
    NoteEvent(fiber, 'R');
    Resume(fiber, &ThreadState().threadStack, arg_on_run);
    CompletePendingSuspend();
    SetBounds(ThreadState().threadBounds);
    if (arg_on_return) *arg_on_return = ThreadState().transfer;
    return SCE_OK;
}

int32_t APS5_VABI sceFiberSwitch(FiberObject* object, uint64_t arg_on_run, uint64_t* arg_on_run_out) {
    auto* target = AsFiber(object);
    if (!target) return object ? SCE_FIBER_ERROR_INVALID : SCE_FIBER_ERROR_NULL;
    auto* self = ThreadState().current;
    if (!self) {
        ReportRefused("sceFiberSwitch", nullptr, target, SCE_FIBER_ERROR_PERMISSION);
        return SCE_FIBER_ERROR_PERMISSION;
    }
    if (target == self || !AcquireForResume(target)) {
        ReportRefused("sceFiberSwitch", self, target, SCE_FIBER_ERROR_STATE);
        return SCE_FIBER_ERROR_STATE;
    }
    if (TraceFibers()) {
        auto** frame = static_cast<void**>(__builtin_frame_address(0));
        void* chain[6] = {};
        auto** guest = static_cast<void**>(frame[0]);
        for (int depth = 0; depth < 6 && guest; ++depth) {
            chain[depth] = guest[1];
            guest = static_cast<void**>(guest[0]);
        }
        std::fprintf(stderr, "[fiber] switch %s -> %s from %p %p %p %p %p %p\n", self->name, target->name, chain[0], chain[1], chain[2], chain[3], chain[4], chain[5]);
    }
    NoteEvent(self, 'O');
    NoteEvent(target, 'I');
    self->state.store(FiberState::Suspending, std::memory_order_relaxed);
    ThreadState().pendingSuspend = self;
    Resume(target, &self->savedStack, arg_on_run);
    CompletePendingSuspend();
    if (arg_on_run_out) *arg_on_run_out = ThreadState().transfer;
    return SCE_OK;
}

int32_t APS5_VABI sceFiberReturnToThread(uint64_t arg_on_return, uint64_t* arg_on_run) {
    auto* self = ThreadState().current;
    if (!self) {
        ReportRefused("sceFiberReturnToThread", nullptr, nullptr, SCE_FIBER_ERROR_PERMISSION);
        return SCE_FIBER_ERROR_PERMISSION;
    }
    if (TraceFibers()) std::fprintf(stderr, "[fiber] return %s from %p\n", self->name, __builtin_return_address(0));
    NoteEvent(self, 'T');
    self->state.store(FiberState::Suspending, std::memory_order_relaxed);
    ThreadState().pendingSuspend = self;
    ThreadState().current = nullptr;
    ThreadState().transfer = arg_on_return;
    SetBounds(ThreadState().threadBounds);
    Aps5FiberSwitchStack_nid_no_patch(&self->savedStack, ThreadState().threadStack);
    CompletePendingSuspend();
    if (arg_on_run) *arg_on_run = ThreadState().transfer;
    return SCE_OK;
}

int32_t APS5_VABI sceFiberGetSelf(FiberObject** fiber) {
    if (!fiber) return SCE_FIBER_ERROR_NULL;
    if (!ThreadState().current) return SCE_FIBER_ERROR_PERMISSION;
    *fiber = reinterpret_cast<FiberObject*>(ThreadState().current);
    return SCE_OK;
}

int32_t APS5_VABI sceFiberGetInfo(FiberObject* object, FiberInfo* fiber_info) {
    auto* fiber = AsFiber(object);
    if (!fiber || !fiber_info) return object && fiber_info ? SCE_FIBER_ERROR_INVALID : SCE_FIBER_ERROR_NULL;
    if (fiber_info->size != sizeof(FiberInfo)) return SCE_FIBER_ERROR_INVALID;
    fiber_info->entry = reinterpret_cast<FiberEntry>(fiber->entry);
    fiber_info->arg_on_initialize = fiber->argOnInitialize;
    fiber_info->addr_context = fiber->context;
    fiber_info->size_context = fiber->contextSize;
    std::memcpy(fiber_info->name, fiber->name, sizeof(fiber_info->name));
    fiber_info->size_context_margin = static_cast<uint64_t>(-1);
    if (fiber->contextSizeCheck) {
        const auto* words = reinterpret_cast<const std::uint64_t*>(fiber->context);
        const auto* end = words + fiber->contextSize / sizeof(std::uint64_t);
        fiber_info->size_context_margin = static_cast<uint64_t>(std::find_if(words, end, [](std::uint64_t word) { return word != FIBER_CONTEXT_FILL; }) - words) * sizeof(std::uint64_t);
    }
    return SCE_OK;
}

int32_t APS5_VABI sceFiberRename(FiberObject* object, const char* name) {
    auto* fiber = AsFiber(object);
    if (!fiber || !name) return object && name ? SCE_FIBER_ERROR_INVALID : SCE_FIBER_ERROR_NULL;
    std::memset(fiber->name, 0, sizeof(fiber->name));
    std::strncpy(fiber->name, name, FIBER_MAX_NAME_LENGTH);
    return SCE_OK;
}

int32_t APS5_VABI sceFiberOptParamInitialize(FiberOptParam* opt_param) {
    if (!opt_param) return SCE_FIBER_ERROR_NULL;
    std::memset(opt_param, 0, FIBER_OPT_PARAM_SIZE);
    return SCE_OK;
}

int32_t APS5_VABI sceFiberGetThreadFramePointerAddress(uint64_t* addr_frame_pointer) {
    if (!addr_frame_pointer) return SCE_FIBER_ERROR_NULL;
    if (!ThreadState().current) return SCE_FIBER_ERROR_PERMISSION;
    *addr_frame_pointer = ThreadState().threadFramePointer;
    return SCE_OK;
}

int32_t APS5_VABI sceFiberStartContextSizeCheck(uint32_t flags) {
    if (flags != 0) return SCE_FIBER_ERROR_INVALID;
    bool expected = false;
    return g_contextSizeCheck.compare_exchange_strong(expected, true) ? SCE_OK : SCE_FIBER_ERROR_STATE;
}

int32_t APS5_VABI sceFiberStopContextSizeCheck(void) {
    bool expected = true;
    return g_contextSizeCheck.compare_exchange_strong(expected, false) ? SCE_OK : SCE_FIBER_ERROR_STATE;
}

}

