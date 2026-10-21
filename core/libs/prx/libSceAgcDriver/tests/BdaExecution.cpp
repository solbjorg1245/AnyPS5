#include "BdaShader.hpp"
#include "BdaAbi.hpp"
#include "SpirvBackend/SpirvBda.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Resources.hpp"
#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <string>
#include <utility>

namespace {

using namespace AgcDriver::Graphics;

class Pipeline {
public:
    Pipeline(const Context& context, std::span<const std::uint32_t> code, const std::array<Buffer*, 3>& buffers) : context(context) {
        try {
            std::array<VkDescriptorSetLayoutBinding, 3> bindings{};
            for (std::uint32_t i = 0; i < bindings.size(); ++i) bindings[i] = {i, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
            VkDescriptorSetLayoutCreateInfo setInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
            setInfo.bindingCount = bindings.size();
            setInfo.pBindings = bindings.data();
            Check(context.Function<PFN_vkCreateDescriptorSetLayout>("vkCreateDescriptorSetLayout")(context.device, &setInfo, nullptr, &setLayout), "vkCreateDescriptorSetLayout");
            const VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 3};
            VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
            poolInfo.maxSets = 1;
            poolInfo.poolSizeCount = 1;
            poolInfo.pPoolSizes = &size;
            Check(context.Function<PFN_vkCreateDescriptorPool>("vkCreateDescriptorPool")(context.device, &poolInfo, nullptr, &pool), "vkCreateDescriptorPool");
            VkDescriptorSetAllocateInfo allocation{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, nullptr, pool, 1, &setLayout};
            Check(context.Function<PFN_vkAllocateDescriptorSets>("vkAllocateDescriptorSets")(context.device, &allocation, &set), "vkAllocateDescriptorSets");
            for (std::uint32_t i = 0; i < buffers.size(); ++i) {
                const VkDescriptorBufferInfo buffer{buffers[i]->Handle(), 0, buffers[i]->Bytes().size()};
                VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
                write.dstSet = set;
                write.dstBinding = i;
                write.descriptorCount = 1;
                write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                write.pBufferInfo = &buffer;
                context.Function<PFN_vkUpdateDescriptorSets>("vkUpdateDescriptorSets")(context.device, 1, &write, 0, nullptr);
            }
            VkPipelineLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
            layoutInfo.setLayoutCount = 1;
            layoutInfo.pSetLayouts = &setLayout;
            Check(context.Function<PFN_vkCreatePipelineLayout>("vkCreatePipelineLayout")(context.device, &layoutInfo, nullptr, &layout), "vkCreatePipelineLayout");
            VkShaderModuleCreateInfo moduleInfo{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
            moduleInfo.codeSize = code.size_bytes();
            moduleInfo.pCode = code.data();
            Check(context.Function<PFN_vkCreateShaderModule>("vkCreateShaderModule")(context.device, &moduleInfo, nullptr, &module), "vkCreateShaderModule");
            VkComputePipelineCreateInfo pipelineInfo{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
            pipelineInfo.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, module, "main", nullptr};
            pipelineInfo.layout = layout;
            Check(context.Function<PFN_vkCreateComputePipelines>("vkCreateComputePipelines")(context.device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &pipeline), "vkCreateComputePipelines");
        } catch (...) { release(); throw; }
    }

    ~Pipeline() { release(); }

    void Run(std::uint32_t groups) {
        CommandBatch batch(context);
        const auto commands = batch.Handle();
        VkMemoryBarrier upload{VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr, VK_ACCESS_HOST_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT};
        context.Function<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier")(commands, VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &upload, 0, nullptr, 0, nullptr);
        context.Function<PFN_vkCmdBindPipeline>("vkCmdBindPipeline")(commands, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
        context.Function<PFN_vkCmdBindDescriptorSets>("vkCmdBindDescriptorSets")(commands, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &set, 0, nullptr);
        context.Function<PFN_vkCmdDispatch>("vkCmdDispatch")(commands, groups, 1, 1);
        VkMemoryBarrier download{VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT};
        context.Function<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier")(commands, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &download, 0, nullptr, 0, nullptr);
        batch.SubmitAndWait();
    }

private:
    void release() noexcept {
        if (pipeline) context.Function<PFN_vkDestroyPipeline>("vkDestroyPipeline")(context.device, pipeline, nullptr);
        if (module) context.Function<PFN_vkDestroyShaderModule>("vkDestroyShaderModule")(context.device, module, nullptr);
        if (layout) context.Function<PFN_vkDestroyPipelineLayout>("vkDestroyPipelineLayout")(context.device, layout, nullptr);
        if (pool) context.Function<PFN_vkDestroyDescriptorPool>("vkDestroyDescriptorPool")(context.device, pool, nullptr);
        if (setLayout) context.Function<PFN_vkDestroyDescriptorSetLayout>("vkDestroyDescriptorSetLayout")(context.device, setLayout, nullptr);
    }

    const Context& context;
    VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    VkDescriptorSet set = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkShaderModule module = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
};

// APS5_SPIRV_BDA_UNIFORM: 32 invocations read through a table of three 64-byte ranges (A, B, then C past a
// gap), several reads each, so the per-invocation state sees hits, misses, spans across ranges and faults
// after hits, and the uniform lookups see lanes inside and outside the first lane's range. Expected values
// and faults are worked out byte by byte, as the full lookup resolves them.
void RunBdaLookupTests(const Context& context, std::uint32_t stage, bool uniform) {
    namespace Abi = ShaderRecompiler::BdaAbi;
    constexpr std::uint32_t lanes = 32;
    constexpr std::uint32_t sentinel = 0xdeadbeef;
    constexpr std::uint64_t guest = 0x7fff12300000ULL;
    constexpr VkBufferUsageFlags usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    Buffer a(context, 64, usage);
    Buffer b(context, 64, usage);
    Buffer c(context, 64, usage);
    const std::array<Buffer*, 3> data{&a, &b, &c};
    for (std::uint32_t range = 0; range < data.size(); ++range) {
        for (std::uint32_t byte = 0; byte < 64; ++byte) data[range]->Bytes()[byte] = static_cast<std::byte>(range * 64 + byte + 1);
    }
    const std::array<std::uint64_t, 3> begins{guest, guest + 64, guest + 256};
    Buffer table(context, sizeof(Abi::Header) + 3 * sizeof(Abi::Range), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    Buffer fault(context, sizeof(Abi::Fault), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    Buffer output(context, lanes * 8 * sizeof(std::uint32_t), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    const auto run = [&](std::initializer_list<BdaTestAccess> list, std::uint32_t middlePermissions) {
        const std::vector<BdaTestAccess> accesses(list);
        std::array<Abi::Range, 3> ranges{};
        for (std::uint32_t range = 0; range < ranges.size(); ++range) ranges[range] = {begins[range], begins[range] + 64, data[range]->DeviceAddress(), range == 1 ? middlePermissions : Abi::Read, 0};
        const Abi::Header header{Abi::Version, 3, sizeof(Abi::Range), 0};
        std::memcpy(table.Bytes().data(), &header, sizeof(header));
        std::memcpy(table.Bytes().data() + sizeof(header), ranges.data(), sizeof(ranges));
        std::memset(fault.Bytes().data(), 0, fault.Bytes().size());
        for (std::size_t word = 0; word < output.Bytes().size() / sizeof(sentinel); ++word) std::memcpy(output.Bytes().data() + word * sizeof(sentinel), &sentinel, sizeof(sentinel));
        Pipeline pipeline(context, MakeBdaLookupTestShader(accesses, lanes, uniform), {&table, &fault, &output});
        pipeline.Run(1);
        struct Expected {
            std::uint64_t address;
            Abi::FaultReason reason;
            std::uint32_t instruction;
        };
        std::vector<Expected> faults;
        const auto where = "stage " + std::to_string(stage) + (uniform ? " uniform" : "") + ", first access at guest + " + std::to_string(static_cast<std::int64_t>(accesses.front().address - guest));
        for (std::uint32_t lane = 0; lane < lanes; ++lane) {
            bool stopped = false;
            for (std::uint32_t index = 0; index < accesses.size(); ++index) {
                std::uint32_t stored = 0;
                std::memcpy(&stored, output.Bytes().data() + (lane * accesses.size() + index) * sizeof(stored), sizeof(stored));
                const auto address = accesses[index].address + static_cast<std::uint64_t>(lane) * accesses[index].laneStride;
                std::uint32_t value = 0;
                for (std::uint32_t byte = 0; byte < 4 && !stopped; ++byte) {
                    const auto at = address + byte;
                    const auto range = std::find_if(ranges.begin(), ranges.end(), [&](const Abi::Range& entry) { return entry.begin <= at && at < entry.end; });
                    if (range == ranges.end() || (range->permissions & Abi::Read) == 0) {
                        faults.push_back({at, range == ranges.end() ? Abi::FaultReason::Unmapped : Abi::FaultReason::Permission, 0x1234 + index});
                        stopped = true;
                        break;
                    }
                    value |= static_cast<std::uint32_t>(data[range - ranges.begin()]->Bytes()[at - range->begin]) << (byte * 8);
                }
                Require(stored == (stopped ? sentinel : value), "BDA lookup read wrong data (" + where + ", lane " + std::to_string(lane) + ", access " + std::to_string(index) + ")");
            }
        }
        Abi::Fault report{};
        std::memcpy(&report, fault.Bytes().data(), sizeof(report));
        if (faults.empty()) {
            Require(report.state == Abi::FaultState::Empty, "BDA lookup recorded a fault no lane makes (" + where + ")");
            return;
        }
        Require(report.state == Abi::FaultState::Ready && std::any_of(faults.begin(), faults.end(), [&](const Expected& expected) { return report.address == expected.address && report.reason == expected.reason && report.instruction == expected.instruction; }), "BDA lookup fault differs from the full lookup's (" + where + ")");
    };
    // Uniform reads: hits in A, a miss into B, back to A, then C.
    run({{guest, 0}, {guest + 4, 0}, {guest + 64, 0}, {guest + 8, 0}, {guest + 260, 0}}, Abi::Read);
    // Lanes split over A and B, unaligned spans across A/B (byte path; lane 31 runs into the gap after B),
    // then lanes inside C.
    run({{guest, 4}, {guest + 2, 4}, {guest + 256, 2}, {guest, 4}}, Abi::Read);
    // A fault after hits: lanes 16-31 of the second read land in the gap after B.
    run({{guest, 4}, {guest + 64, 4}, {guest + 4, 0}}, Abi::Read);
    // Below the first range, at the start of a read.
    run({{guest - 4, 0}, {guest, 0}}, Abi::Read);
    // B readable by no lookup: Permission after hits in A, for every lane and for the split lanes only.
    run({{guest, 0}, {guest + 64, 0}}, Abi::Write);
    run({{guest + 8, 0}, {guest, 4}}, Abi::Write);
}

void RunBdaReadTests(const Context& context, bool uniform) {
    namespace Abi = ShaderRecompiler::BdaAbi;
    Buffer first(context, 3, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT);
    Buffer second(context, 2, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT);
    first.Bytes()[0] = std::byte{0x11};
    first.Bytes()[1] = std::byte{0x22};
    first.Bytes()[2] = std::byte{0x33};
    second.Bytes()[0] = std::byte{0x44};
    second.Bytes()[1] = std::byte{0x55};
    constexpr std::uint64_t guest = 0x7fff12340001ULL;
    std::array<Abi::Range, 2> ranges{{{guest, guest + 3, first.DeviceAddress(), Abi::Read, 0}, {guest + 3, guest + 5, second.DeviceAddress(), Abi::Read, 0}}};
    Buffer table(context, sizeof(Abi::Header) + sizeof(ranges), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    Buffer fault(context, sizeof(Abi::Fault), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    Buffer output(context, 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    const auto run = [&](std::uint64_t address, std::uint32_t bits, std::uint32_t expected, Abi::FaultReason reason, std::uint32_t count = 2, std::uint32_t groups = 1, std::int64_t offset = 0) {
        const Abi::Header header{Abi::Version, count, sizeof(Abi::Range), 0};
        std::memcpy(table.Bytes().data(), &header, sizeof(header));
        std::memcpy(table.Bytes().data() + sizeof(header), ranges.data(), sizeof(ranges));
        std::memset(fault.Bytes().data(), 0, fault.Bytes().size());
        const std::uint32_t sentinel = 0xdeadbeef;
        std::memcpy(output.Bytes().data(), &sentinel, sizeof(sentinel));
        Pipeline pipeline(context, MakeBdaTestShader(address, bits, offset, uniform), {&table, &fault, &output});
        pipeline.Run(groups);
        Abi::Fault report{};
        std::uint32_t result = 0;
        std::memcpy(&report, fault.Bytes().data(), sizeof(report));
        std::memcpy(&result, output.Bytes().data(), sizeof(result));
        if (static_cast<std::uint32_t>(reason) == 0) {
            Require(report.state == Abi::FaultState::Empty && result == expected, "BDA GPU read produced incorrect data or a fault");
        } else {
            Require(report.state == Abi::FaultState::Ready && report.reason == reason && report.instruction == 0x1234, "BDA GPU fault was not published correctly");
            Require(result == sentinel, "faulting BDA shader continued to output a substitute value");
        }
    };
    run(guest, 8, 0x11, static_cast<Abi::FaultReason>(0));
    run(guest + 1, 16, 0x3322, static_cast<Abi::FaultReason>(0));
    run(guest + 1, 32, 0x55443322, static_cast<Abi::FaultReason>(0));
    run(guest + 5, 8, 0, Abi::FaultReason::Unmapped, 2, 64);
    run(guest - 1, 8, 0, Abi::FaultReason::Unmapped);
    run(std::numeric_limits<std::uint64_t>::max() - 1, 32, 0, Abi::FaultReason::Overflow);
    run(guest, 8, 0, Abi::FaultReason::InvalidTable, 3);
    run(std::numeric_limits<std::uint64_t>::max() - 2, 8, 0, Abi::FaultReason::Overflow, 2, 1, 4);
    run(1, 8, 0, Abi::FaultReason::Overflow, 2, 1, -4);
    ranges[0].permissions = 0;
    run(guest, 8, 0, Abi::FaultReason::Permission);
}

}

void RunBdaExecutionTests(const Context& context) {
    // Every case under each APS5_SPIRV_BDA_UNIFORM stage, stage 3 with plain and uniform-base reads.
    for (const auto& config : std::array<std::pair<int, bool>, 5>{{{0, false}, {1, false}, {2, false}, {3, false}, {3, true}}}) {
        ShaderRecompiler::SetBdaUniformStageForTests(config.first);
        RunBdaReadTests(context, config.second);
        RunBdaLookupTests(context, static_cast<std::uint32_t>(config.first), config.second);
    }
    ShaderRecompiler::SetBdaUniformStageForTests(-1);
}
