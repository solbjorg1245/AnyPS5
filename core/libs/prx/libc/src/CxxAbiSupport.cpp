#include "prx/libc/include/exceptions/Runtime.hpp"
#include <cstddef>
#ifndef _UNWIND_H
#define _UNWIND_H
#endif

#include <cxxabi.h>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <stdexcept>
#include <typeinfo>
#include <new>
#include <ios>
#include <locale>
#include <regex>
#include <functional>

#include "prx/libc/include/General.hpp"
#include "prx/libc/include/general/ExportMacros.hpp"

#include <vector>

namespace {

using GuestThreadDestructor = void (APS5_VABI *)(void*);

// Guest thread_local destructors. They use the guest calling convention, so they cannot be handed
// to the host C++ runtime directly; the host only owns this per-thread list and runs it in reverse
// registration order when the thread exits, or earlier when the guest libc forces it.
class GuestThreadDestructors {
public:
    ~GuestThreadDestructors() { RunAll(); }

    void Add(GuestThreadDestructor destructor, void* object) { entries.push_back({destructor, object}); }

    void RunAll() {
        while (!entries.empty()) {
            const Entry entry = entries.back();
            entries.pop_back();
            entry.destructor(entry.object);
        }
    }

private:
    struct Entry {
        GuestThreadDestructor destructor;
        void* object;
    };
    std::vector<Entry> entries;
};

thread_local GuestThreadDestructors t_guestThreadDestructors;

}  // namespace

extern "C" {

void* APS5_VABI __cxa_demangle_nid_postfix(const char* mangled, char* buf, std::size_t* len, int* status) {
    return abi::__cxa_demangle(mangled, buf, len, status);
}

int APS5_VABI __cxa_thread_atexit_impl_nid_postfix(GuestThreadDestructor func, void* arg, void* dso) {
    (void)dso;
    t_guestThreadDestructors.Add(func, arg);
    return 0;
}

APS5_EXPORT("qBS714-Jr3g", LibcInternalExtCxaThreadAtexit_nid_postfix);
int APS5_VABI LibcInternalExtCxaThreadAtexit_nid_postfix(GuestThreadDestructor destructor, void* object, void* module_id) {
    return __cxa_thread_atexit_impl_nid_postfix(destructor, object, module_id);
}

// Guest libc calls this on its exit paths to run the calling thread's thread_local destructors now.
void APS5_VABI _sceLibcInternalForceTlsDestructor_nid_postfix(int reason) {
    (void)reason;
    t_guestThreadDestructors.RunAll();
}

int APS5_VABI _sceLibcInternalThreadDtors_nid_postfix(void) {
    t_guestThreadDestructors.RunAll();
    return 0;
}

const std::error_category* _ZSt17iostream_categoryv_nid_postfix() { return &std::iostream_category(); }

}
