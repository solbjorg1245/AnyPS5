#ifndef CORE_SHADER_RECOMPILIER_OPTIMIZATION_INCLUDE_OPTIMIZATION_RESOURCEMATERIALIZER_HPP
#define CORE_SHADER_RECOMPILIER_OPTIMIZATION_INCLUDE_OPTIMIZATION_RESOURCEMATERIALIZER_HPP

#include "IntermediateRepresentation/IrProgram.hpp"
#include "Optimization/SrtWalker.hpp"
#include <cstdint>
#include <vector>

namespace ShaderRecompiler {

struct ResourceSpecialization {
    struct Buffer {
        std::uint32_t packedStride = 0;
        IrBufferFormat descriptorFormat = IrBufferFormat::Invalid;
        std::uint32_t descriptorSwizzle = ShaderImageIdentitySwizzle;
        bool empty = false;

        bool operator==(const Buffer& other) const;
    };

    struct Image {
        IrTextureNumericClass numericClass = IrTextureNumericClass::Unsupported;
        RdnaImageDimension dimension = RdnaImageDimension::Unknown;
        std::uint32_t mipCount = 1;
        IrBufferFormat conversionFormat = IrBufferFormat::Invalid;
        std::uint32_t shaderSwizzle = ShaderImageIdentitySwizzle;
        std::uint32_t indirectRoot = ImageResource::NoIndirectImage;
        std::uint32_t indirectMappingOffset = 0;
        std::uint32_t indirectSearchIterations = 0;
        bool cube = false;
        bool fmask = false;
        bool depthBits = false;
        bool depthUnorm16 = false;
        bool float16Store = false;

        bool operator==(const Image& other) const;
    };

    std::vector<Buffer> buffers;
    std::vector<Image> images;

    bool operator==(const ResourceSpecialization& other) const;

    std::vector<std::uint32_t> boundDescriptors;
};

// Why a bindless image table (a T# loaded from a table buffer at a runtime key) was not bound;
// counted on the [bindless] line (APS5_PROFILE_DRAW). LoopEntry: a loop-counter table entry the
// driver cannot decode (ResourceMaterializer::StrictLoopTables). TableEntry: a keyed table entry
// the table maps (a texture in words 0-3) whose words 5-6 the driver cannot decode
// (ResourceMaterializer::StrictTableEntries).
enum class BindlessRejection { Capacity, MaterialScan, NoEntry, Storage, NonUniform, ImageSlots, LoopEntry, TableEntry, Count };

// Image elements bound as null instead of failing the whole draw (opt-in, APS5_NULL_UNDECODABLE=1;
// off by default, the draw fails as before): a T# whose words 5-6 ask for what the driver does not implement (array pitch,
// corner sampling, a partially resident default color, MSAA depth; the capture walk read past a T#
// array into the next struct), as a directly loaded image or a bindless table entry, and a table
// entry in unreadable memory (a loop-counter table bounded past its end).
enum class NullBoundImage { Undecodable, TableUndecodable, TableUnreadable, Count };

class ResourceMaterializer {
public:
    void Apply(IrProgram& program, const ResourceSpecialization& specialization) const;
    [[nodiscard]] IrResourcePlan ExtractPlan(const IrProgram& program) const;
    void Materialize(const IrResourcePlan& program, const SrtRuntime& runtime, ResourceSnapshot& snapshot, ResourceSpecialization& specialization) const;
    // APS5_PROFILE_DRAW: the time Materialize spent building specializations, over every call.
    static std::uint64_t SpecializationNanoseconds();
    // The slots every bindless image table binds (APS5_BINDLESS_SLOTS, default 16, 1..48).
    static std::uint32_t BindlessSlots();
    static void CountBindlessRejection(BindlessRejection reason);
    // Whether undecodable image elements bind null (APS5_NULL_UNDECODABLE set; off by default).
    static bool NullUndecodable();
    // Whether an undecodable entry of a loop-counter table fails the draw instead of binding null
    // (APS5_NO_STRICT_LOOP_TABLES unset): the loop samples every entry up to its count.
    static bool StrictLoopTables();
    // Whether a keyed table entry with undecodable words 5-6 binds null only where the table would
    // not map it anyway (an invalid T#, another shape) and fails the draw where it would, as the
    // driver's decode did (APS5_NO_STRICT_TABLE_ENTRIES unset; set: every such entry binds null).
    static bool StrictTableEntries();
    // The elements bound as null for `reason` since the last call (the APS5_PROFILE_DRAW digest).
    static std::uint64_t TakeNullBound(NullBoundImage reason);
};

}

#endif
