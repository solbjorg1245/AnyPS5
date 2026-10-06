// FindInstanceBaseSgpr: the start-instance SGPR an NGG vertex shader adds to its instance ID.
// usage: agc_instance_base_sgpr_tests [code.bin...] (extra files: print the decision per file)
#include "RdnaDecoder/RdnaInstructionDecoder.hpp"
#include "Translation/EmbeddedVertexFetch.hpp"
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstdlib>
#include <span>
#include <vector>

using namespace ShaderRecompiler;

namespace {

std::int32_t find(std::span<const std::uint32_t> words, std::uint32_t userDataCount = 16u) {
    const auto program = RdnaInstructionDecoder{}.Decode(words);
    return FindInstanceBaseSgpr(program, 8u, userDataCount);
}

}

int main(int argc, char** argv) {
    int failures = 0;
    const auto expect = [&](const char* name, std::int32_t got, std::int32_t want) {
        if (got == want) return;
        std::fprintf(stderr, "%s: got s%d, want s%d\n", name, got, want);
        ++failures;
    };
    constexpr std::uint32_t endProgram = 0xbf810000u;
    // v_add_nc_u32_sdwa v4, s10, v8 src0_sel:WORD_0
    constexpr std::array<std::uint32_t, 2> sdwaAdd{0x4a0810f9u, 0x0684060au};
    // v_add_nc_u32_e32 v4, s10, v8
    constexpr std::uint32_t plainAdd = 0x4a08100au;
    // s_mov_b32 s0, s10
    constexpr std::uint32_t readElsewhere = 0xbe80030au;
    expect("sdwa add", find(std::array<std::uint32_t, 3>{sdwaAdd[0], sdwaAdd[1], endProgram}), 10);
    expect("plain add", find(std::array<std::uint32_t, 2>{plainAdd, endProgram}), 10);
    expect("read elsewhere", find(std::array<std::uint32_t, 3>{plainAdd, readElsewhere, endProgram}), -1);
    // s_load_dwordx2 vcc, s[8:9], 0x10 reads the pair s8:s9 only
    expect("pair base", find(std::array<std::uint32_t, 4>{plainAdd, 0xf4041a84u, 0xfa000010u, endProgram}), 10);
    expect("outside user data", find(std::array<std::uint32_t, 2>{plainAdd, endProgram}, 2u), -1);
    for (int i = 1; i < argc; ++i) {
        std::vector<std::uint32_t> words;
        if (auto* file = std::fopen(argv[i], "rb")) {
            std::uint32_t word = 0;
            while (std::fread(&word, sizeof(word), 1, file) == 1) words.push_back(word);
            std::fclose(file);
        }
        try {
            const auto program = RdnaInstructionDecoder{}.Decode(words);
            if (std::getenv("TRACE_SGPR")) {
                for (const auto& inst : program.instructions) {
                    std::printf("  pc %x op %d dst %d/%u s0 %d/%u sel %u s1 %d/%u sel %u s2 %d\n", inst.programCounter, static_cast<int>(inst.op), static_cast<int>(inst.destination.kind), inst.destination.reg, static_cast<int>(inst.source0.kind), inst.source0.reg, inst.source0.sdwaSel, static_cast<int>(inst.source1.kind), inst.source1.reg, inst.source1.sdwaSel, static_cast<int>(inst.source2.kind));
                    if (inst.op == RdnaOpcode::SEndpgm) break;
                }
            }
            std::printf("%s: %zu instructions, s%d\n", argv[i], program.instructions.size(), find(words));
        } catch (const std::exception& error) {
            std::printf("%s: %s\n", argv[i], error.what());
        }
    }
    return failures == 0 ? 0 : 1;
}
