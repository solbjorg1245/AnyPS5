#ifndef CORE_SHADER_RECOMPILER_OPTIMIZATION_RESOURCEPROGRAM_HPP
#define CORE_SHADER_RECOMPILER_OPTIMIZATION_RESOURCEPROGRAM_HPP

#include "Recompiler.hpp"
#include "IntermediateRepresentation/IrProgram.hpp"
#include "Optimization/ResourceMaterializer.hpp"
#include <cstdint>
#include <memory>
#include <span>

namespace ShaderRecompiler {

[[nodiscard]] IrProgram PrepareResourceProgram(const RecompileRequest& request);
[[nodiscard]] std::shared_ptr<const IrResourcePlan> GetResourcePlan(const RecompileRequest& request);

// What a driver's capture of a request produces: the plan and the materialization of the words the
// runtime read through it. Recompile(request, capture) compiles from these without a second walk.
struct SourceEntry;
struct ResourceCapture {
    std::shared_ptr<const IrResourcePlan> plan;
    ResourceSnapshot snapshot;
    ResourceSpecialization specialization;
    // The cache entry the plan belongs to; null when the request bypasses the cache.
    std::shared_ptr<SourceEntry> source;
    // The walk's read addresses when the plan has pure flat slots (the leaf of each pure slot,
    // and every other read sorted and deduplicated); empty otherwise.
    SrtReadTrace readTrace;
    // APS5_PROFILE_DRAW: what CaptureResources spent resolving the source (the stage inputs, the
    // key over the code, the plan) before the walk.
    std::uint64_t sourceNanoseconds = 0;
};
[[nodiscard]] std::shared_ptr<const ResourceCapture> CaptureResources(const RecompileRequest& request, const SrtRuntime& runtime);

// The resolved source of a request (its cache entry with the plan built), for a driver that
// memoizes it per registered shader: ResolveSource is what CaptureResources does before the walk
// (the stage input validation, the key over the code, the lookup, the first-sight plan build), and
// the overload below captures over a handle without repeating it. A handle stays valid for every
// request with the same code and the same cache key fields (RecompileCacheKey::ContextHash plus
// the target); the vertex stages' input validation is repeated per capture because it reads V#
// fields the key does not cover. Null for a request that bypasses the cache (useCache false).
struct SourceHandle {
    std::shared_ptr<SourceEntry> source;
};
[[nodiscard]] std::shared_ptr<const SourceHandle> ResolveSource(const RecompileRequest& request);
[[nodiscard]] std::shared_ptr<const ResourceCapture> CaptureResources(const RecompileRequest& request, const SrtRuntime& runtime, const SourceHandle& handle);

// The fast path's walk (docs/design/draw-fastpath.md section 2.3, F2): the materialization a
// capture over `handle` makes, by the plan's express walk alone and through the runtime's readers
// (the driver's direct reader), with no read trace and no deferred slots. What the express walk
// does not cover declines instead of falling back to the interpreter: no source, an incomplete
// plan, no walk program, a bindless image table, an unsupported root or a failing op, a read the
// reader declined (ReadDeclined); Failed is any other materialization error (the reader may have
// declined a read outside the express walk, the uniform fill's). `runtime`'s userData,
// shaderBase, walk, trace and deferral fields are replaced. A walk or uniform-fill evaluation that
// does not complete declines without an exception (ResourceMaterializer::TryMaterialize); only a
// malformed plan or descriptor throws inside, caught here as before.
enum class WalkStatus : std::uint8_t { Walked, NoSource, IncompletePlan, NoProgram, Bindless, UnsupportedRoot, OpFailed, ReadDeclined, Failed, Count };
[[nodiscard]] const char* WalkStatusName(WalkStatus status);
[[nodiscard]] WalkStatus WalkResources(const SourceHandle& handle, std::span<const std::uint32_t> userData, std::uint64_t shaderBase, const SrtRuntime& runtime, ResourceSnapshot& snapshot, ResourceSpecialization& specialization);
// The result Recompile(request, capture) gives for a capture with `snapshot` and `specialization`
// when `handle`'s source already holds that variant (the request's layout, the specialization):
// its bindings, push constants and vertex attributes populated over the snapshot, without
// compiling (false: no such variant). The variant comes from a per-thread memo of the last few the
// thread selected (no source mutex, no scan of the source's variants) and `result` is written in
// place, its vectors keeping their storage (the fast paths pass a per-thread result);
// APS5_NO_VARIANT_MEMO=1 scans under the mutex and assigns a fresh result, as before. A throw
// leaves `result` unspecified.
[[nodiscard]] bool PopulateVariant(const SourceHandle& handle, const RecompileRequest& request, const ResourceSnapshot& snapshot, const ResourceSpecialization& specialization, RecompileResult& result);
// The calling thread's PopulateVariant memo lookups so far (tests).
struct VariantMemoCounts {
    std::uint64_t hits = 0;
    std::uint64_t misses = 0;
};
[[nodiscard]] VariantMemoCounts ThreadVariantMemoCounts();

}

#endif
