#ifndef CORE_LIBS_PRX_LIBC_INCLUDE_CRASHRESOURCES_HPP
#define CORE_LIBS_PRX_LIBC_INCLUDE_CRASHRESOURCES_HPP

#include <cstddef>
#include <cstdint>

// The crash report (Windows, CrashReport.cpp) hands a FATAL's guest addresses to a describer the GPU
// driver registers (APS5_CRASH_RESOURCES=1), after the register dump, on the faulting thread while
// every other thread still runs: `addresses` holds the fault address, the general registers and the
// words the dump printed; `print` writes one line straight to the stderr handle. The describer must
// not block (try-locks only) nor allocate much; a fault inside it ends the report there.
namespace CrashResources {

extern "C" {

using Print = void (*)(const char* text);
using Describer = void (*)(const std::uint64_t* addresses, std::size_t count, Print print);
void CrashResourcesSetDescriber_nid_postfix(Describer describer);

}

}

#endif
