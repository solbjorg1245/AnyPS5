// The guest unwinder on a small stack (docs/research/loading-deaths-2.md): a backtrace through a
// function whose CFI remembers and restores state must report every frame, and the unwinder must
// stay well inside the 16 KiB of a guest job fiber's context. Run once as is and once with
// APS5_UNWIND_STACK_RULES=1 (the old on-stack remember-state array, which must show up in the measurement).
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include "prx/libc/include/general/VabiMacros.hpp"
#include "prx/libc/include/specifics/itanium/UnwindAbi.hpp"

extern "C" _Unwind_Reason_Code APS5_VABI _Unwind_Backtrace_nid_postfix(_Unwind_Trace_Fn, void*);
extern "C" std::uintptr_t APS5_VABI _Unwind_GetIP_nid_postfix(_Unwind_Context*);

// Calls callee with a CFI that remembers the frame-pointer state before an early-return epilogue and
// restores it after: the call's return address is only unwound right if restore_state brings back
// CFA = rbp + 16.
extern "C" void CallWithRememberedState(void (*callee)());
#ifdef _WIN32
#define UNWIND_STACK_ARG "%rcx"
#else
#define UNWIND_STACK_ARG "%rdi"
#endif
asm(".text\n"
    ".globl CallWithRememberedState\n"
    "CallWithRememberedState:\n"
    ".cfi_startproc\n"
    "pushq %rbp\n"
    ".cfi_def_cfa_offset 16\n"
    ".cfi_offset %rbp, -16\n"
    "movq %rsp, %rbp\n"
    ".cfi_def_cfa_register %rbp\n"
    "testq " UNWIND_STACK_ARG ", " UNWIND_STACK_ARG "\n"
    "jnz 1f\n"
    ".cfi_remember_state\n"
    "popq %rbp\n"
    ".cfi_def_cfa %rsp, 8\n"
    "ret\n"
    "1:\n"
    ".cfi_restore_state\n"
    "subq $32, %rsp\n"
    "call *" UNWIND_STACK_ARG "\n"
    "leave\n"
    ".cfi_def_cfa %rsp, 8\n"
    "ret\n"
    ".cfi_endproc\n");

struct Trace {
    std::uintptr_t ips[64] {};
    int count = 0;
    _Unwind_Reason_Code result = _URC_NO_REASON;
};
static Trace trace;

static _Unwind_Reason_Code Record(_Unwind_Context* context, void*) {
    if (trace.count < 64) trace.ips[trace.count] = _Unwind_GetIP_nid_postfix(context);
    ++trace.count;
    return _URC_NO_REASON;
}

[[gnu::noinline]] static void Walk() {
    trace = {};
    trace.result = _Unwind_Backtrace_nid_postfix(Record, nullptr);
    asm volatile("" ::: "memory");
}

[[gnu::noinline]] static void WalkThroughRememberedState() {
    CallWithRememberedState(Walk);
    asm volatile("" ::: "memory");
}

constexpr std::size_t PaintBytes = 64 * 1024;
constexpr unsigned char Paint = 0xa5;
static std::uintptr_t paintLow;

[[gnu::noinline]] static void PaintStack() {
    volatile unsigned char area[PaintBytes];
    for (std::size_t i = 0; i < PaintBytes; ++i) area[i] = Paint;
    paintLow = reinterpret_cast<std::uintptr_t>(&area[0]);
}

// Bytes of stack below this frame that walk() and the unwinder under it wrote.
[[gnu::noinline]] static std::size_t Measure(void (*walk)()) {
    PaintStack();
    walk();
    const auto* bytes = reinterpret_cast<const volatile unsigned char*>(paintLow);
    std::size_t untouched = 0;
    while (untouched < PaintBytes && bytes[untouched] == Paint) ++untouched;
    asm volatile("" ::: "memory");
    return PaintBytes - untouched;
}

static int Fail(const char* what) {
    std::fprintf(stderr, "unwind stack test failed: %s\n", what);
    return 1;
}

int main() {
    const char* rules = std::getenv("APS5_UNWIND_STACK_RULES");
    const bool onStack = rules != nullptr && *rules == '1';
    // Warm up: the first remember_state allocates the host thread's vector.
    Measure(WalkThroughRememberedState);

    const std::size_t directBytes = Measure(Walk);
    const Trace direct = trace;
    const std::size_t rememberedBytes = Measure(WalkThroughRememberedState);
    const Trace remembered = trace;
    std::printf("unwind stack: %s, direct %d frames %zu B, through remember_state %d frames %zu B\n",
        onStack ? "rules on stack" : "rules on host thread", direct.count, directBytes, remembered.count, rememberedBytes);

    if (direct.result != _URC_END_OF_STACK || remembered.result != _URC_END_OF_STACK) return Fail("backtrace did not reach the end of the stack");
    if (direct.count < 3 || direct.count > 60) return Fail("unexpected direct frame count");
    if (remembered.count != direct.count + 2) return Fail("remember_state frame lost or broke the chain");
    if (remembered.ips[0] != direct.ips[0]) return Fail("first frame differs");
    const auto trampoline = reinterpret_cast<std::uintptr_t>(CallWithRememberedState);
    if (remembered.ips[1] <= trampoline || remembered.ips[1] - trampoline > 64) return Fail("return address in the remember_state frame");
    const auto through = reinterpret_cast<std::uintptr_t>(WalkThroughRememberedState);
    if (remembered.ips[2] <= through || remembered.ips[2] - through > 256) return Fail("frame above the remember_state frame");
    // Frame 1 returns into Measure's one call site; frame 2 is main, which calls Measure from a
    // different site per walk, so its address differs by design. Frames from 3 on must match again.
    if (remembered.ips[3] != direct.ips[1]) return Fail("Measure frame differs");
    for (int i = 3; i < direct.count; ++i)
        if (remembered.ips[i + 2] != direct.ips[i]) return Fail("outer frames differ");

    // A 16 KiB job fiber must keep room for the guest's own frames and the personality routine.
    if (!onStack && rememberedBytes > 8 * 1024) return Fail("unwinder uses more than 8 KiB of stack");
    // The old path value-initializes Rules saved[16] in Instructions' frame (Rule is 16 B; Rules holds
    // 33 rules on Windows, 17 on Linux, plus 24 B of CFA state).
#ifdef _WIN32
    constexpr std::size_t onStackRules = 16 * (33 * 16 + 24);
#else
    constexpr std::size_t onStackRules = 16 * (17 * 16 + 24);
#endif
    if (onStack && rememberedBytes < onStackRules) return Fail("on-stack rules path did not show its array (measurement broken?)");
    std::puts("unwind stack tests passed");
    return 0;
}
