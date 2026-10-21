#include "prx/libc/include/CpuTopology.hpp"
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>

static void Require(bool value, const char* what) {
    if (value) return;
    std::fprintf(stderr, "cpu topology test failed: %s\n", what);
    std::exit(1);
}

#ifdef __linux__
static std::uint64_t Parse(const char* text) {
    const auto path = std::filesystem::temp_directory_path() / "anyps5_cpu_topology_test_cpus";
    {
        std::ofstream file(path, std::ios::trunc);
        file << text;
    }
    const auto mask = CpuTopology::MaskFromCpuList(path.c_str());
    std::filesystem::remove(path);
    return mask;
}
#endif

static void SplitCases() {
    CpuTopology::Core smt[8];
    for (unsigned i = 0; i < 8; ++i) smt[i] = {std::uint64_t{3} << (2 * i), 0};
    auto split = CpuTopology::SplitCores(smt, 8, 0xffff, 3);
    Require(split.driver == 0xfc00 && split.jobs == 0x3ff, "three of eight smt cores: the highest go to the driver");
    split = CpuTopology::SplitCores(smt, 8, 0xffff, 2);
    Require(split.driver == 0xf000 && split.jobs == 0xfff, "two of eight smt cores");
    split = CpuTopology::SplitCores(smt, 8, 0xffff, 6);
    Require(split.driver == 0xfff0 && split.jobs == 0xf, "six cores leave two for the job workers");
    split = CpuTopology::SplitCores(smt, 8, 0xffff, 7);
    Require(split.driver == 0 && split.jobs == 0, "fewer than two job cores left: no split");
    split = CpuTopology::SplitCores(smt, 8, 0xffff, 0);
    Require(split.driver == 0 && split.jobs == 0, "no cores reserved: no split");
    split = CpuTopology::SplitCores(smt, 8, 0x0fff, 2);
    Require(split.driver == 0xf00 && split.jobs == 0xff, "cores outside the process are left out");
    split = CpuTopology::SplitCores(smt, 8, 0, 2);
    Require(split.driver == 0xf000 && split.jobs == 0xfff, "an unknown process mask means every core");

    CpuTopology::Core hybrid[12];
    for (unsigned i = 0; i < 4; ++i) hybrid[i] = {std::uint64_t{3} << (2 * i), 1};
    for (unsigned i = 0; i < 8; ++i) hybrid[4 + i] = {std::uint64_t{1} << (8 + i), 0};
    split = CpuTopology::SplitCores(hybrid, 12, 0xffff, 2);
    Require(split.driver == 0xf0 && split.jobs == 0xff0f, "hybrid: the driver gets the highest performance cores");
}

int main() {
    SplitCases();

#ifdef __linux__
    Require(Parse("0-15\n") == 0xffffull, "range");
    Require(Parse("16-31\n") == 0xffff0000ull, "high range");
    Require(Parse("0,2,4-5\n") == 0x35ull, "list of cpus and ranges");
    Require(Parse("3") == 0x8ull, "single cpu without a newline");
    Require(Parse("62-70\n") == 0xc000000000000000ull, "cpus past 63 are left out");
    Require(Parse("") == 0, "empty list");
    Require(Parse("4-2\n") == 0, "reversed range");
    Require(Parse("0-x\n") == 0, "malformed range");
    Require(Parse("0;1\n") == 0, "malformed separator");
    Require(CpuTopology::MaskFromCpuList("/nonexistent/anyps5/cpus") == 0, "missing file");
#endif

    const auto& layout = CpuTopology::Get();
    Require(layout.process != 0, "process affinity read");
    Require(!layout.hybrid || (layout.performant & layout.efficient) == 0, "hybrid classes overlap");
    std::uint64_t cores = 0;
    for (unsigned i = 0; i < layout.coreCount; ++i) {
        Require(layout.cores[i].mask != 0 && (cores & layout.cores[i].mask) == 0, "physical cores are disjoint");
        cores |= layout.cores[i].mask;
    }
    Require(layout.coreCount != 0 && (cores & layout.process) == layout.process, "physical cores cover the process");
#ifdef __linux__
    const std::uint64_t first = layout.process & (~layout.process + 1);
    Require(CpuTopology::Pin(nullptr, first) == first, "pin to one cpu of the process");
    cpu_set_t set;
    CPU_ZERO(&set);
    Require(pthread_getaffinity_np(pthread_self(), sizeof(set), &set) == 0, "read thread affinity");
    Require(CPU_COUNT(&set) == 1 && CPU_ISSET(static_cast<unsigned>(__builtin_ctzll(first)), &set), "thread runs on the pinned cpu only");
    Require(CpuTopology::Pin(nullptr, ~layout.process) == 0, "mask outside the process is not applied");
    int dummy = 0;
    Require(CpuTopology::Pin(&dummy, first) == 0, "other threads are not pinned by handle");
#endif
    return 0;
}
