#ifndef CORE_SHADER_RECOMPILIER_OPTIMIZATION_INCLUDE_OPTIMIZATION_RESOURCETRACKER_HPP
#define CORE_SHADER_RECOMPILIER_OPTIMIZATION_INCLUDE_OPTIMIZATION_RESOURCETRACKER_HPP

#include "IntermediateRepresentation/IrProgram.hpp"

namespace ShaderRecompiler {

class ResourceTracker {
public:
    void Track(IrProgram& program) const;
};

namespace Detail {

// How many values a bindless table key that is a loop counter takes (0: the key is no counter):
// a phi over immediate starts and small immediate steps of itself (Demon's Souls walks a T# table
// behind an SRT pointer in a counted loop). Counting up, the bound is the immediate the loop's exit
// branch compares the counter against, else `limit`; counting down, the largest start plus one.
std::uint32_t LoopCounterRange(IrValue* key, std::uint32_t limit);

}

}

#endif
