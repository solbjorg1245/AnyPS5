function(ensure_agc_glslang)
    if(TARGET glslang AND TARGET SPIRV AND TARGET glslang-standalone)
        return()
    endif()

    if(TARGET glslang)
        if(NOT TARGET SPIRV)
            message(FATAL_ERROR "glslang exists but SPIRV target is missing")
        endif()
        if(NOT TARGET glslang-standalone)
            message(FATAL_ERROR "glslang exists but glslang-standalone target is missing")
        endif()
        return()
    endif()

    set(BUILD_SHARED_LIBS OFF)
    set(BUILD_EXTERNAL OFF)
    set(GLSLANG_TESTS OFF)
    set(GLSLANG_ENABLE_INSTALL OFF)
    set(ENABLE_GLSLANG_BINARIES ON)
    set(ENABLE_SPIRV ON)
    set(ENABLE_SPVREMAPPER OFF)
    set(ENABLE_OPT OFF)

    add_subdirectory(
            "${CMAKE_SOURCE_DIR}/3rdparty/glslang"
            "${CMAKE_BINARY_DIR}/3rdparty/glslang"
            EXCLUDE_FROM_ALL
    )
endfunction()

ensure_agc_glslang()

set(agcTextureDetileShaderSource ${CMAKE_CURRENT_SOURCE_DIR}/Graphics/shaders/TextureDetile.comp)
set(agcTextureDetileGeneratedRoot ${CMAKE_CURRENT_BINARY_DIR}/generated)
set(agcTextureDetileGeneratedDir ${agcTextureDetileGeneratedRoot}/prx/libSceAgcDriver/Graphics/shaders)
set(agcTextureDetileShaderSpv ${agcTextureDetileGeneratedDir}/TextureDetile.spv)
set(agcTextureDetileShaderHeader ${agcTextureDetileGeneratedDir}/TextureDetile_spv.h)
add_custom_command(
        OUTPUT "${agcTextureDetileShaderHeader}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${agcTextureDetileGeneratedDir}"
        COMMAND "$<TARGET_FILE:glslang-standalone>" -V --target-env vulkan1.1 -o "${agcTextureDetileShaderSpv}" "${agcTextureDetileShaderSource}"
        COMMAND ${CMAKE_COMMAND} -DINPUT=${agcTextureDetileShaderSpv} -DOUTPUT=${agcTextureDetileShaderHeader}
        -DSYMBOL=TEXTURE_DETILE_SPV -P "${CMAKE_CURRENT_SOURCE_DIR}/embed_spirv.cmake"
        DEPENDS "${agcTextureDetileShaderSource}" glslang-standalone
        VERBATIM
)

# Narrow copy-backs' compare pass (Recorder::NarrowsCopyBacks): buffer device addresses only.
set(agcCopyBackDiffShaderSource ${CMAKE_CURRENT_SOURCE_DIR}/Graphics/shaders/CopyBackDiff.comp)
set(agcCopyBackDiffShaderSpv ${agcTextureDetileGeneratedDir}/CopyBackDiff.spv)
set(agcCopyBackDiffShaderHeader ${agcTextureDetileGeneratedDir}/CopyBackDiff_spv.h)
add_custom_command(
        OUTPUT "${agcCopyBackDiffShaderHeader}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${agcTextureDetileGeneratedDir}"
        COMMAND "$<TARGET_FILE:glslang-standalone>" -V --target-env vulkan1.1 -o "${agcCopyBackDiffShaderSpv}" "${agcCopyBackDiffShaderSource}"
        COMMAND ${CMAKE_COMMAND} -DINPUT=${agcCopyBackDiffShaderSpv} -DOUTPUT=${agcCopyBackDiffShaderHeader}
        -DSYMBOL=COPY_BACK_DIFF_SPV -P "${CMAKE_CURRENT_SOURCE_DIR}/embed_spirv.cmake"
        DEPENDS "${agcCopyBackDiffShaderSource}" glslang-standalone
        VERBATIM
)

set(agcDriverSources
        ${CMAKE_CURRENT_SOURCE_DIR}/Unimplemented.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Execution/src/DisplayBuffer.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Execution/src/DisplayFormat.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Execution/src/Driver/DeviceAccess.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Execution/src/Driver/Diagnostics.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Execution/src/Driver/FrameTrace.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Execution/src/Driver/Dispatch/DataHitVerification.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Execution/src/Driver/Dispatch/Dispatch.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Execution/src/Driver/Dispatch/FastDispatch.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Execution/src/Driver/Dispatch/DispatchCache.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Execution/src/Driver/Dispatch/DispatchCacheStatistics.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Execution/src/Driver/Dispatch/DispatchInsert.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Execution/src/Driver/Dispatch/DispatchLookup.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Execution/src/Driver/Dispatch/DispatchRelocation.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Execution/src/Driver/Dispatch/DispatchTiming.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Execution/src/Driver/Dispatch/IndirectDispatch.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Execution/src/Driver/Dispatch/VariantValidation.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Execution/src/Driver/Draw/Draw.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Execution/src/Driver/Draw/DrawCache.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Execution/src/Driver/Draw/DrawCacheStatistics.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Execution/src/Driver/Draw/DrawCapture.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Execution/src/Driver/Draw/DrawDecode.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Execution/src/Driver/Draw/DrawLookup.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Execution/src/Driver/Draw/DrawRelocation.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Execution/src/Driver/Draw/DrawPrecheck.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Execution/src/Driver/Draw/DrawRegisterKey.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Execution/src/Driver/Draw/FastState.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Execution/src/Driver/Draw/DrawTiming.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Execution/src/Driver/Draw/FastCensus.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Execution/src/Driver/Draw/FastWalk.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Execution/src/Driver/Draw/FastDraw.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Execution/src/Driver/Draw/FastRead.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Execution/src/Driver/Draw/IndirectDraw.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Execution/src/Driver/EntryPoints.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Execution/src/Driver/Lifecycle.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Execution/src/Driver/Memory/BufferCopy.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Execution/src/Driver/Memory/BufferFill.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Execution/src/Driver/Memory/CaptureDiagnostics.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Execution/src/Driver/Memory/CaptureValidation.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Execution/src/Driver/Memory/WriteEvidence.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Execution/src/Driver/Memory/WrittenBuffers.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Execution/src/Driver/Packets/CommandValidation.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Execution/src/Driver/Packets/PacketExecution.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Execution/src/Driver/Packets/PacketFlush.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Execution/src/Driver/Packets/PacketHistory.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Execution/src/Driver/Packets/PacketMemory.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Execution/src/Driver/Packets/PacketTiming.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Execution/src/Driver/Presentation.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Execution/src/Driver/Queues/QueueWorker.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Execution/src/Driver/Queues/Submission.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Execution/src/Driver/Queues/WorkerAffinity.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Execution/src/Driver/Shaders/ShaderDiagnostics.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Execution/src/Driver/Shaders/ShaderRegistry.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Execution/src/Driver/Synchronization/DeferredLabels.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Execution/src/Driver/Synchronization/LabelHistory.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Execution/src/Driver/Synchronization/SynchronizationStatistics.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Execution/src/Driver/Synchronization/WaitMemory.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Execution/src/Driver/Synchronization/WaitStatistics.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Execution/src/WorkerSampler.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Execution/src/GuestMemory.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Execution/src/ShaderMemory.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Execution/src/BdaFeatures.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Execution/src/Pm4.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Execution/src/VulkanDevice.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Execution/src/AspectFit.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Execution/src/PresentationScaler.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Graphics/src/Draw.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Graphics/src/DepthSurface.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Graphics/src/ColorTargetLayout.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Graphics/src/ColorTargetTransfer.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Graphics/src/Pipeline.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Graphics/src/FastLayouts.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Graphics/src/FastRing.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Graphics/src/FastDraw.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Graphics/src/FastDispatch.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Graphics/src/PipelineCache.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Graphics/src/Resources.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Graphics/src/Recorder.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Graphics/src/ShaderInputState.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Graphics/src/ShaderResources.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Graphics/src/ShaderAddressResources.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Graphics/src/GuestBufferMemory.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Graphics/src/BdaResources.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Graphics/src/BufferAddress.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Graphics/src/ShaderValidation.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Graphics/src/State.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Graphics/src/GuestTextureResource.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Graphics/src/GuestSamplerResource.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Graphics/src/TextureFormat.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Graphics/src/DccMetadata.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Graphics/src/TextureTiling.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Graphics/src/TextureDetiler.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Graphics/src/Texture.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Graphics/src/UnitShadow.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Graphics/src/Sampler.cpp
        ${agcTextureDetileShaderHeader}
        ${agcCopyBackDiffShaderHeader}
        ${CMAKE_CURRENT_SOURCE_DIR}/Eq/src/Event.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Eq/src/Query.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Resource/src/Lifecycle.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Resource/src/Registration.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/State/src/Configuration.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/State/src/Status.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Submit/src/Acb.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Submit/src/CommandBuffer.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Submit/src/Dcb.cpp
)

# The host replacements below work around MinGW's runtime (winpthreads' clock_gettime, emulated TLS,
# the msvcrt-style heap) through Win32 and PE import tricks; on Linux glibc does all three natively,
# and HostStubs.cpp supplies the counters the profile lines read from them.
if(MINGW)
    list(APPEND agcDriverSources
            ${CMAKE_CURRENT_SOURCE_DIR}/Execution/src/FastClock.cpp
            ${CMAKE_CURRENT_SOURCE_DIR}/Execution/src/FastEmutls.cpp
            ${CMAKE_CURRENT_SOURCE_DIR}/Execution/src/OperatorNew.cpp
    )
else()
    list(APPEND agcDriverSources
            ${CMAKE_CURRENT_SOURCE_DIR}/Execution/src/HostStubs.cpp
    )
endif()

add_library(libSceAgcDriver SHARED EXCLUDE_FROM_ALL
        ${agcDriverSources}
)

target_include_directories(libSceAgcDriver PRIVATE
        ${LIBS_INCLUDE_DIR}
        ${agcTextureDetileGeneratedRoot}
        ${CMAKE_SOURCE_DIR}/3rdparty/Vulkan-Headers/include
        ${CMAKE_SOURCE_DIR}/3rdparty/SPIRV-Headers/include
)

add_shader_recompiler(shader_recompiler_agc_driver libc)

target_link_libraries(libSceAgcDriver PRIVATE
        shader_recompiler_agc_driver
        SDL2-static
        libc
        libkernel
)

set_target_properties(libSceAgcDriver PROPERTIES CXX_EXTENSIONS OFF)
configure_windows_unwind(libSceAgcDriver)
if(MINGW)
    # The driver's own operator new/delete (OperatorNew.cpp), __emutls_get_address (FastEmutls.cpp)
    # and std::chrono::steady_clock::now (FastClock.cpp)
    # stay private: auto-exported, the NID
    # patcher renames them and libSceAgc.prx, which links against the driver, fails to load.
    target_link_options(libSceAgcDriver PRIVATE
            -Wl,--exclude-symbols=_Znwy
            -Wl,--exclude-symbols=_Znay
            -Wl,--exclude-symbols=_ZnwyRKSt9nothrow_t
            -Wl,--exclude-symbols=_ZnayRKSt9nothrow_t
            -Wl,--exclude-symbols=_ZdlPv
            -Wl,--exclude-symbols=_ZdaPv
            -Wl,--exclude-symbols=_ZdlPvy
            -Wl,--exclude-symbols=_ZdaPvy
            -Wl,--exclude-symbols=__emutls_get_address
            -Wl,--exclude-symbols=_ZNSt6chrono3_V212steady_clock3nowEv)
endif()
if(WIN32)
    set_source_files_properties(${CMAKE_CURRENT_SOURCE_DIR}/Execution/src/GuestMemory.cpp PROPERTIES COMPILE_DEFINITIONS _WIN32_WINNT=0x0602)
endif()

foreach(runtimeLinkFlag IN ITEMS -static-libgcc -static-libstdc++ -static)
    string(REPLACE "${runtimeLinkFlag}" "" CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS}")
endforeach()

add_test_executable(agc_driver_tests EXCLUDE_FROM_ALL
        tests/Submit.cpp
)

target_include_directories(agc_driver_tests PRIVATE
        ${LIBS_INCLUDE_DIR}
        ${CMAKE_SOURCE_DIR}/core/shader/recompiler
        ${CMAKE_SOURCE_DIR}/3rdparty/Vulkan-Headers/include
)

target_link_libraries(agc_driver_tests PRIVATE
        libSceAgcDriver
        libkernel
        libc
)

set_target_properties(agc_driver_tests PROPERTIES CXX_EXTENSIONS OFF)
configure_windows_unwind(agc_driver_tests)

add_test_executable(agc_driver_recompiler_tests EXCLUDE_FROM_ALL
        tests/Recompiler.cpp
)

target_include_directories(agc_driver_recompiler_tests PRIVATE
        ${LIBS_INCLUDE_DIR}
)

target_link_libraries(agc_driver_recompiler_tests PRIVATE
        libSceAgcDriver
        libc
)

set_target_properties(agc_driver_recompiler_tests PROPERTIES CXX_EXTENSIONS OFF)
configure_windows_unwind(agc_driver_recompiler_tests)

add_test_executable(agc_shader_memory_tests EXCLUDE_FROM_ALL
        tests/ShaderMemory.cpp
        Execution/src/ShaderMemory.cpp
        Execution/src/GuestMemory.cpp
)
target_include_directories(agc_shader_memory_tests PRIVATE ${LIBS_INCLUDE_DIR})
target_link_libraries(agc_shader_memory_tests PRIVATE shader_recompiler_agc_driver libc)
set_target_properties(agc_shader_memory_tests PROPERTIES CXX_EXTENSIONS OFF)
configure_windows_unwind(agc_shader_memory_tests)
# The fast walk's decline must not throw: tests/ShaderMemory.cpp counts every C++ throw of the
# test's objects and static libraries through a __cxa_throw wrapper (GNU ld and lld --wrap).
if(MINGW OR CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_link_options(agc_shader_memory_tests PRIVATE -Wl,--wrap=__cxa_throw)
    target_compile_definitions(agc_shader_memory_tests PRIVATE APS5_COUNT_CXX_THROWS=1)
endif()

add_test_executable(agc_write_tracking_tests EXCLUDE_FROM_ALL
        tests/WriteTracking.cpp
        Execution/src/GuestMemory.cpp
)
target_include_directories(agc_write_tracking_tests PRIVATE ${LIBS_INCLUDE_DIR})
target_link_libraries(agc_write_tracking_tests PRIVATE shader_recompiler_agc_driver libc)
set_target_properties(agc_write_tracking_tests PROPERTIES CXX_EXTENSIONS OFF)
configure_windows_unwind(agc_write_tracking_tests)
if(BUILD_TESTING)
    set_target_properties(agc_write_tracking_tests PROPERTIES EXCLUDE_FROM_ALL OFF)
    add_test(NAME agc_write_tracking COMMAND agc_write_tracking_tests)
    set_tests_properties(agc_write_tracking PROPERTIES SKIP_RETURN_CODE 77)
    if(WIN32)
        set_tests_properties(agc_write_tracking PROPERTIES ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
    endif()
endif()

add_test_executable(agc_structurizer_tests EXCLUDE_FROM_ALL tests/Structurizer.cpp)
target_link_libraries(agc_structurizer_tests PRIVATE shader_recompiler_agc_driver libc)
set_target_properties(agc_structurizer_tests PROPERTIES CXX_EXTENSIONS OFF)
configure_windows_unwind(agc_structurizer_tests)

add_test_executable(agc_dpp_write_tests EXCLUDE_FROM_ALL tests/DppWrite.cpp)
target_link_libraries(agc_dpp_write_tests PRIVATE shader_recompiler_agc_driver libc)
set_target_properties(agc_dpp_write_tests PROPERTIES CXX_EXTENSIONS OFF)
configure_windows_unwind(agc_dpp_write_tests)

add_test_executable(agc_instance_base_sgpr_tests EXCLUDE_FROM_ALL tests/InstanceBaseSgpr.cpp)
target_link_libraries(agc_instance_base_sgpr_tests PRIVATE shader_recompiler_agc_driver libc)
set_target_properties(agc_instance_base_sgpr_tests PROPERTIES CXX_EXTENSIONS OFF)
configure_windows_unwind(agc_instance_base_sgpr_tests)

add_test_executable(agc_vop1_control_decode_tests EXCLUDE_FROM_ALL tests/Vop1ControlDecode.cpp)
target_link_libraries(agc_vop1_control_decode_tests PRIVATE shader_recompiler_agc_driver libc)
set_target_properties(agc_vop1_control_decode_tests PROPERTIES CXX_EXTENSIONS OFF)
configure_windows_unwind(agc_vop1_control_decode_tests)

add_test_executable(agc_dcc_key_sharing_tests EXCLUDE_FROM_ALL tests/DccKeySharing.cpp)
target_include_directories(agc_dcc_key_sharing_tests PRIVATE ${LIBS_INCLUDE_DIR} ${CMAKE_SOURCE_DIR}/core/shader/recompiler ${CMAKE_SOURCE_DIR}/3rdparty/Vulkan-Headers/include)
target_link_libraries(agc_dcc_key_sharing_tests PRIVATE libc)
set_target_properties(agc_dcc_key_sharing_tests PROPERTIES CXX_EXTENSIONS OFF)
configure_windows_unwind(agc_dcc_key_sharing_tests)
if(BUILD_TESTING)
    set_target_properties(agc_dcc_key_sharing_tests PROPERTIES EXCLUDE_FROM_ALL OFF)
    add_test(NAME agc_dcc_key_sharing COMMAND agc_dcc_key_sharing_tests)
    if(WIN32)
        set_tests_properties(agc_dcc_key_sharing PROPERTIES ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
    endif()
endif()

add_test_executable(agc_shader_disk_cache_tests EXCLUDE_FROM_ALL tests/ShaderDiskCache.cpp)
target_include_directories(agc_shader_disk_cache_tests PRIVATE ${LIBS_INCLUDE_DIR})
target_link_libraries(agc_shader_disk_cache_tests PRIVATE shader_recompiler_agc_driver libc)
set_target_properties(agc_shader_disk_cache_tests PROPERTIES CXX_EXTENSIONS OFF)
configure_windows_unwind(agc_shader_disk_cache_tests)

add_test_executable(agc_shader_control_flow_tests EXCLUDE_FROM_ALL
        tests/ControlFlow.cpp
)
target_include_directories(agc_shader_control_flow_tests PRIVATE ${LIBS_INCLUDE_DIR})
target_link_libraries(agc_shader_control_flow_tests PRIVATE shader_recompiler_agc_driver libc)
set_target_properties(agc_shader_control_flow_tests PROPERTIES CXX_EXTENSIONS OFF)
configure_windows_unwind(agc_shader_control_flow_tests)

add_test_executable(agc_driver_flip_tests EXCLUDE_FROM_ALL
        tests/Flip.cpp
)

target_include_directories(agc_driver_flip_tests PRIVATE
        ${LIBS_INCLUDE_DIR}
)

target_link_libraries(agc_driver_flip_tests PRIVATE
        libSceAgcDriver
        libc
)

set_target_properties(agc_driver_flip_tests PROPERTIES CXX_EXTENSIONS OFF)
configure_windows_unwind(agc_driver_flip_tests)

set(agcSampleLodShaderSource ${CMAKE_CURRENT_SOURCE_DIR}/tests/shaders/SampleLod.comp)
set(agcTestShaderGeneratedRoot ${CMAKE_CURRENT_BINARY_DIR}/generated-tests)
set(agcSampleLodShaderSpv ${agcTestShaderGeneratedRoot}/SampleLod.spv)
set(agcSampleLodShaderHeader ${agcTestShaderGeneratedRoot}/SampleLod_spv.h)
add_custom_command(
        OUTPUT "${agcSampleLodShaderHeader}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${agcTestShaderGeneratedRoot}"
        COMMAND "$<TARGET_FILE:glslang-standalone>" -V --target-env vulkan1.1 -o "${agcSampleLodShaderSpv}" "${agcSampleLodShaderSource}"
        COMMAND ${CMAKE_COMMAND} -DINPUT=${agcSampleLodShaderSpv} -DOUTPUT=${agcSampleLodShaderHeader}
        -DSYMBOL=SAMPLE_LOD_SPV -P "${CMAKE_CURRENT_SOURCE_DIR}/embed_spirv.cmake"
        DEPENDS "${agcSampleLodShaderSource}" glslang-standalone
        VERBATIM
)

add_test_executable(agc_driver_recorder_tests EXCLUDE_FROM_ALL
        tests/Recorder.cpp
        tests/ResidentPresent.cpp
        ${agcSampleLodShaderHeader}
)

target_include_directories(agc_driver_recorder_tests PRIVATE
        ${LIBS_INCLUDE_DIR}
        ${agcTestShaderGeneratedRoot}
        ${CMAKE_SOURCE_DIR}/core/shader/recompiler
        ${CMAKE_SOURCE_DIR}/3rdparty/Vulkan-Headers/include
        # The fast reader's known-value test walks a shader (Optimization/ResourceProgram.hpp).
        $<TARGET_PROPERTY:shader_recompiler_agc_driver,INTERFACE_INCLUDE_DIRECTORIES>
)

target_link_libraries(agc_driver_recorder_tests PRIVATE
        libSceAgcDriver
        SDL2-static
        libc
)

set_target_properties(agc_driver_recorder_tests PROPERTIES CXX_EXTENSIONS OFF)
configure_windows_unwind(agc_driver_recorder_tests)

add_test_executable(agc_driver_pm4_tests EXCLUDE_FROM_ALL
        tests/Pm4.cpp
)

target_include_directories(agc_driver_pm4_tests PRIVATE
        ${LIBS_INCLUDE_DIR}
)

target_link_libraries(agc_driver_pm4_tests PRIVATE
        libSceAgcDriver
        libc
)

set_target_properties(agc_driver_pm4_tests PROPERTIES CXX_EXTENSIONS OFF)
configure_windows_unwind(agc_driver_pm4_tests)

add_test_executable(agc_driver_graphics_tests EXCLUDE_FROM_ALL
        tests/Graphics.cpp
        tests/ColorTargetLayout.cpp
        tests/BdaResources.cpp
        tests/GuestAllocations.cpp
        tests/LiveStack.cpp
        tests/TextureFormat.cpp
        tests/TextureTiling.cpp
        tests/ImageMemoryPool.cpp
        tests/GuestTextureResource.cpp
        tests/GuestSamplerResource.cpp
        tests/TextureDetiler.cpp
        Graphics/src/Draw.cpp
        Graphics/src/DepthSurface.cpp
        Graphics/src/Pipeline.cpp
        Graphics/src/FastLayouts.cpp
        Graphics/src/FastRing.cpp
        Graphics/src/FastDraw.cpp
        Graphics/src/State.cpp
        Graphics/src/ColorTargetLayout.cpp
        Graphics/src/ColorTargetTransfer.cpp
        Graphics/src/Resources.cpp
        Graphics/src/ShaderResources.cpp
        Graphics/src/ShaderAddressResources.cpp
        Graphics/src/GuestBufferMemory.cpp
        Graphics/src/BdaResources.cpp
        Graphics/src/BufferAddress.cpp
        Graphics/src/ShaderValidation.cpp
        Graphics/src/GuestTextureResource.cpp
        Graphics/src/GuestSamplerResource.cpp
        Graphics/src/TextureFormat.cpp
        Graphics/src/DccMetadata.cpp
        Graphics/src/TextureTiling.cpp
        Graphics/src/TextureDetiler.cpp
        Graphics/src/Texture.cpp
        Graphics/src/UnitShadow.cpp
        Graphics/src/Sampler.cpp
        Graphics/src/Recorder.cpp
        Graphics/src/ShaderInputState.cpp
        ${agcTextureDetileShaderHeader}
        ${agcCopyBackDiffShaderHeader}
        Execution/src/GuestMemory.cpp
        Execution/src/ShaderMemory.cpp
        Execution/src/Pm4.cpp
        Execution/src/Driver/FrameTrace.cpp
)

target_include_directories(agc_driver_graphics_tests PRIVATE
        ${LIBS_INCLUDE_DIR}
        ${agcTextureDetileGeneratedRoot}
        ${CMAKE_SOURCE_DIR}/core/shader/recompiler
        ${CMAKE_SOURCE_DIR}/core/shader/recompiler/IntermediateRepresentation/include
        ${CMAKE_SOURCE_DIR}/core/shader/recompiler/ControlFlow/include
        ${CMAKE_SOURCE_DIR}/core/shader/recompiler/RdnaDecoder/include
        ${CMAKE_SOURCE_DIR}/3rdparty/Vulkan-Headers/include
        ${CMAKE_SOURCE_DIR}/3rdparty/SPIRV-Headers/include
)

target_link_libraries(agc_driver_graphics_tests PRIVATE
        shader_recompiler_agc_driver
        libc
)

set_target_properties(agc_driver_graphics_tests PROPERTIES CXX_EXTENSIONS OFF)
configure_windows_unwind(agc_driver_graphics_tests)

add_test_executable(agc_driver_bda_device_tests EXCLUDE_FROM_ALL
        tests/BdaDevice.cpp
        tests/BdaShader.cpp
        tests/BdaExecution.cpp
        tests/BdaContracts.cpp
        Execution/src/BdaFeatures.cpp
        Graphics/src/Resources.cpp
        Graphics/src/BufferAddress.cpp
)

target_include_directories(agc_driver_bda_device_tests PRIVATE
        ${LIBS_INCLUDE_DIR}
        ${CMAKE_SOURCE_DIR}/core/shader/recompiler
        ${CMAKE_SOURCE_DIR}/3rdparty/Vulkan-Headers/include
)
target_link_libraries(agc_driver_bda_device_tests PRIVATE SDL2-static libc shader_recompiler_agc_driver)
set_target_properties(agc_driver_bda_device_tests PROPERTIES CXX_EXTENSIONS OFF)
configure_windows_unwind(agc_driver_bda_device_tests)

add_test_executable(agc_driver_mesh_tests EXCLUDE_FROM_ALL
        tests/Mesh.cpp
        ${agcDriverSources}
)
target_include_directories(agc_driver_mesh_tests PRIVATE
        ${LIBS_INCLUDE_DIR}
        ${agcTextureDetileGeneratedRoot}
        ${CMAKE_SOURCE_DIR}/3rdparty/Vulkan-Headers/include
        ${CMAKE_SOURCE_DIR}/3rdparty/SPIRV-Headers/include
)
target_link_libraries(agc_driver_mesh_tests PRIVATE shader_recompiler_agc_driver SDL2-static libc libkernel)
set_target_properties(agc_driver_mesh_tests PROPERTIES CXX_EXTENSIONS OFF)
configure_windows_unwind(agc_driver_mesh_tests)

file(GLOB agcExecutionTestSources CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/tests/execution/*.cpp)
set(agcExecutionTests)
foreach(agcExecutionTestSource IN LISTS agcExecutionTestSources)
    get_filename_component(agcExecutionTestName ${agcExecutionTestSource} NAME_WE)
    string(REGEX REPLACE "([a-z0-9])([A-Z])" "\\1_\\2" agcExecutionTestName "${agcExecutionTestName}")
    string(TOLOWER "agc_driver_${agcExecutionTestName}_tests" agcExecutionTest)
    add_test_executable(${agcExecutionTest} EXCLUDE_FROM_ALL ${agcExecutionTestSource})
    target_include_directories(${agcExecutionTest} PRIVATE
            ${LIBS_INCLUDE_DIR}
            ${CMAKE_SOURCE_DIR}/core/shader/recompiler
            ${CMAKE_SOURCE_DIR}/3rdparty/Vulkan-Headers/include
    )
    target_link_libraries(${agcExecutionTest} PRIVATE libSceAgcDriver SDL2-static libc)
    set_target_properties(${agcExecutionTest} PROPERTIES CXX_EXTENSIONS OFF)
    configure_windows_unwind(${agcExecutionTest})
    list(APPEND agcExecutionTests ${agcExecutionTest})
endforeach()

if(BUILD_TESTING)
    foreach(agcTest IN ITEMS agc_driver_tests agc_shader_memory_tests agc_structurizer_tests agc_dpp_write_tests agc_vop1_control_decode_tests agc_shader_disk_cache_tests agc_shader_control_flow_tests agc_driver_flip_tests agc_driver_pm4_tests agc_driver_graphics_tests ${agcExecutionTests})
        string(REGEX REPLACE "_tests$" "" agcTestName "${agcTest}")
        set_target_properties(${agcTest} PROPERTIES EXCLUDE_FROM_ALL OFF)
        add_test(NAME ${agcTestName} COMMAND ${agcTest})
        set_tests_properties(${agcTestName} PROPERTIES ENVIRONMENT "ANYPS5_NO_SHADER_CACHE=1" SKIP_RETURN_CODE 77)
        # Tests that open a Vulkan device carry the gpu label (ctest -LE gpu runs the rest).
        if(agcTest STREQUAL "agc_driver_graphics_tests" OR agcTest IN_LIST agcExecutionTests)
            set_tests_properties(${agcTestName} PROPERTIES LABELS gpu)
        endif()
        if(WIN32)
            set_tests_properties(${agcTestName} PROPERTIES ENVIRONMENT_MODIFICATION
                "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
        endif()
    endforeach()
    # Narrow copy-backs again under APS5_PROFILE_GPU (timing ranges record queued copies, the
    # compare pass among them, at every range), with APS5_NARROW_VERIFY=1 and with
    # APS5_RESIDENT_BUFFERS=1.
    foreach(agcNarrowVariant IN ITEMS profile-gpu verify resident)
        string(REPLACE "-" "_" agcNarrowVariantName "${agcNarrowVariant}")
        add_test(NAME agc_driver_narrow_copy_back_${agcNarrowVariantName} COMMAND agc_driver_narrow_copy_back_tests --${agcNarrowVariant})
        set_tests_properties(agc_driver_narrow_copy_back_${agcNarrowVariantName} PROPERTIES ENVIRONMENT "ANYPS5_NO_SHADER_CACHE=1" SKIP_RETURN_CODE 77 LABELS gpu)
        if(WIN32)
            set_tests_properties(agc_driver_narrow_copy_back_${agcNarrowVariantName} PROPERTIES ENVIRONMENT_MODIFICATION
                "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
        endif()
    endforeach()
    # Test binaries again with a fast-path switch set (latched once per process), so each fallback
    # the switch restores stays covered:
    # - agc_shader_memory_no_walk_reuse: the fast walk's in-place build off (APS5_NO_WALK_REUSE=1):
    #   TryMaterialize's fresh-object form;
    # - agc_shader_memory_no_variant_memo: PopulateVariant's memo off (APS5_NO_VARIANT_MEMO=1): its
    #   scan and fresh result;
    # - agc_driver_no_memos, agc_driver_fast_dispatch_no_memos: every s53-fast-cost-b memo off (the
    #   import, pipeline, sampler and variant memos, the in-place decode).
    # (agc_driver_recorder_tests is not built by BUILD_TESTING, so its APS5_NO_OOM_RECLAIM=1 variant is
    #  run by hand: APS5_NO_OOM_RECLAIM=1 agc_driver_recorder_tests.)
    # Fields: name|target|environment (comma-separated, no list separators inside one entry).
    set(agcFastCostMemosOff "APS5_NO_IMPORT_MEMO=1,APS5_NO_PIPELINE_MEMO=1,APS5_NO_SAMPLER_MEMO=1,APS5_NO_VARIANT_MEMO=1,APS5_NO_DECODE_REUSE=1")
    set(agcSwitchedTests
        "agc_shader_memory_no_walk_reuse|agc_shader_memory_tests|APS5_NO_WALK_REUSE=1"
        "agc_shader_memory_no_variant_memo|agc_shader_memory_tests|APS5_NO_VARIANT_MEMO=1"
        "agc_driver_no_memos|agc_driver_tests|${agcFastCostMemosOff}")
    if(TARGET agc_driver_fast_dispatch_tests)
        list(APPEND agcSwitchedTests "agc_driver_fast_dispatch_no_memos|agc_driver_fast_dispatch_tests|${agcFastCostMemosOff}")
    endif()
    foreach(agcSwitchedTest IN LISTS agcSwitchedTests)
        string(REPLACE "|" ";" agcSwitchedFields "${agcSwitchedTest}")
        list(GET agcSwitchedFields 0 agcSwitchedName)
        list(GET agcSwitchedFields 1 agcSwitchedTarget)
        list(GET agcSwitchedFields 2 agcSwitchedEnvironment)
        string(REPLACE "," ";" agcSwitchedEnvironment "${agcSwitchedEnvironment}")
        add_test(NAME ${agcSwitchedName} COMMAND ${agcSwitchedTarget})
        set_tests_properties(${agcSwitchedName} PROPERTIES ENVIRONMENT "ANYPS5_NO_SHADER_CACHE=1;${agcSwitchedEnvironment}" SKIP_RETURN_CODE 77)
        if(agcSwitchedTarget IN_LIST agcExecutionTests)
            set_tests_properties(${agcSwitchedName} PROPERTIES LABELS gpu)
        endif()
        if(WIN32)
            set_tests_properties(${agcSwitchedName} PROPERTIES ENVIRONMENT_MODIFICATION
                "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
        endif()
    endforeach()
endif()

option(AGC_BUILD_VISUAL_TEST "Enable the standalone AGC SPIR-V visual test" OFF)

if(AGC_BUILD_VISUAL_TEST)
    ensure_agc_glslang()

    set(visualShaders)

    foreach(stage IN ITEMS vert frag)
        set(shaderOutput "${ANYPS5_TEST_OUTPUT_DIRECTORY}/Triangle.${stage}.spv")

        add_custom_command(
                OUTPUT "${shaderOutput}"
                COMMAND ${CMAKE_COMMAND} -E make_directory "${ANYPS5_TEST_OUTPUT_DIRECTORY}"
                COMMAND "$<TARGET_FILE:glslang-standalone>"
                -V
                --target-env vulkan1.1
                -o "${shaderOutput}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/shaders/Triangle.${stage}"
                DEPENDS
                glslang-standalone
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/shaders/Triangle.${stage}"
                VERBATIM
        )

        list(APPEND visualShaders "${shaderOutput}")
    endforeach()

    add_test_executable(agc_driver_visual_test EXCLUDE_FROM_ALL
            tests/Visual.cpp
            Execution/src/VulkanDevice.cpp
            Execution/src/DisplayFormat.cpp
            Execution/src/AspectFit.cpp
            Execution/src/PresentationScaler.cpp
            Execution/src/GuestMemory.cpp
            Execution/src/BdaFeatures.cpp
            Graphics/src/Draw.cpp
            Graphics/src/DepthSurface.cpp
            Graphics/src/ColorTargetLayout.cpp
            Graphics/src/ColorTargetTransfer.cpp
            Graphics/src/Pipeline.cpp
            Graphics/src/FastLayouts.cpp
            Graphics/src/FastRing.cpp
            Graphics/src/FastDraw.cpp
            Graphics/src/PipelineCache.cpp
            Graphics/src/Resources.cpp
            Graphics/src/ShaderResources.cpp
            Graphics/src/ShaderAddressResources.cpp
            Graphics/src/GuestBufferMemory.cpp
            Graphics/src/BdaResources.cpp
            Graphics/src/BufferAddress.cpp
            Graphics/src/ShaderValidation.cpp
            Graphics/src/GuestTextureResource.cpp
            Graphics/src/GuestSamplerResource.cpp
            Graphics/src/TextureFormat.cpp
            Graphics/src/DccMetadata.cpp
            Graphics/src/TextureTiling.cpp
            Graphics/src/TextureDetiler.cpp
            Graphics/src/Texture.cpp
            Graphics/src/UnitShadow.cpp
            Graphics/src/Sampler.cpp
            ${agcTextureDetileShaderHeader}
            ${visualShaders}
    )

    target_include_directories(agc_driver_visual_test PRIVATE
            ${LIBS_INCLUDE_DIR}
            ${agcTextureDetileGeneratedRoot}
            ${CMAKE_SOURCE_DIR}/core/shader/recompiler
            ${CMAKE_SOURCE_DIR}/core/shader/recompiler/IntermediateRepresentation/include
            ${CMAKE_SOURCE_DIR}/core/shader/recompiler/ControlFlow/include
            ${CMAKE_SOURCE_DIR}/core/shader/recompiler/RdnaDecoder/include
            ${CMAKE_SOURCE_DIR}/3rdparty/Vulkan-Headers/include
            ${CMAKE_SOURCE_DIR}/3rdparty/SPIRV-Headers/include
    )

    target_link_libraries(agc_driver_visual_test PRIVATE
            SDL2-static
            libc
    )

    set_target_properties(agc_driver_visual_test PROPERTIES CXX_EXTENSIONS OFF)
    configure_windows_unwind(agc_driver_visual_test)

    if(WIN32)
        set(visualRuntimeFiles "$<TARGET_FILE:libc>")

        if(MINGW)
            get_filename_component(visualCompilerDirectory "${CMAKE_CXX_COMPILER}" DIRECTORY)

            foreach(runtimeName IN ITEMS
                    libgcc_s_seh-1.dll
                    libstdc++-6.dll
                    libwinpthread-1.dll
            )
                set(runtimePath "${visualCompilerDirectory}/${runtimeName}")

                if(NOT EXISTS "${runtimePath}")
                    message(FATAL_ERROR "Cannot locate MinGW runtime: ${runtimeName}")
                endif()

                list(APPEND visualRuntimeFiles "${runtimePath}")
            endforeach()
        endif()

        add_custom_target(agc_driver_visual_runtime
                COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                ${visualRuntimeFiles}
                "$<TARGET_FILE_DIR:agc_driver_visual_test>"
                DEPENDS libc
                COMMAND_EXPAND_LISTS
                VERBATIM
        )

        add_dependencies(agc_driver_visual_test agc_driver_visual_runtime)
    endif()
endif()

include(PostCompile.cmake)
# Replays shader_*.req files the driver writes when a compute dispatch cannot be recompiled.
add_executable(agc_shader_replay EXCLUDE_FROM_ALL
        tools/ShaderReplay.cpp
)
target_include_directories(agc_shader_replay PRIVATE ${LIBS_INCLUDE_DIR})
target_link_libraries(agc_shader_replay PRIVATE shader_recompiler_agc_driver libc)
if(ANYPS5_ENABLE_SPIRV_TOOLS)
    target_link_libraries(agc_shader_replay PRIVATE SPIRV-Tools-static)
endif()
set_target_properties(agc_shader_replay PROPERTIES CXX_EXTENSIONS OFF)
configure_windows_unwind(agc_shader_replay)

if(BUILD_TESTING)
    add_test_executable(texture_format_lookup_tests ${LIBS_INCLUDE_DIR}/tests/TextureFormatLookup.cpp)
    target_include_directories(texture_format_lookup_tests PRIVATE ${LIBS_INCLUDE_DIR} ${CMAKE_SOURCE_DIR}/3rdparty/Vulkan-Headers/include)
    target_link_libraries(texture_format_lookup_tests PRIVATE libSceAgcDriver libc)
    configure_windows_unwind(texture_format_lookup_tests)
    add_test(NAME texture_format_lookup COMMAND texture_format_lookup_tests)
    if(WIN32)
        set_tests_properties(texture_format_lookup PROPERTIES ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
    endif()
    add_test_executable(linear_display_buffer_tests ${LIBS_INCLUDE_DIR}/tests/LinearDisplayBuffer.cpp)
    target_include_directories(linear_display_buffer_tests PRIVATE ${LIBS_INCLUDE_DIR})
    target_link_libraries(linear_display_buffer_tests PRIVATE libSceAgcDriver libc)
    configure_windows_unwind(linear_display_buffer_tests)
    add_test(NAME linear_display_buffer COMMAND linear_display_buffer_tests)
    if(WIN32)
        set_tests_properties(linear_display_buffer PROPERTIES ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
    endif()
    add_test_executable(guest_rendering_wait_tests ${LIBS_INCLUDE_DIR}/tests/GuestRenderingWait.cpp)
    target_include_directories(guest_rendering_wait_tests PRIVATE ${LIBS_INCLUDE_DIR})
    target_link_libraries(guest_rendering_wait_tests PRIVATE libSceAgcDriver libc)
    configure_windows_unwind(guest_rendering_wait_tests)
    add_test(NAME guest_rendering_wait COMMAND guest_rendering_wait_tests)
    if(WIN32)
        set_tests_properties(guest_rendering_wait PROPERTIES ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
    endif()
    add_test_executable(shader_buffer_address_tests ${LIBS_INCLUDE_DIR}/tests/ShaderBufferAddress.cpp)
    target_link_libraries(shader_buffer_address_tests PRIVATE shader_recompiler_agc_driver libc)
    configure_windows_unwind(shader_buffer_address_tests)
    add_test(NAME shader_buffer_address COMMAND shader_buffer_address_tests)
    if(WIN32)
        set_tests_properties(shader_buffer_address PROPERTIES ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
    endif()
    add_test_executable(ray_tracing_miss_tests ${LIBS_INCLUDE_DIR}/tests/RayTracingMiss.cpp)
    target_link_libraries(ray_tracing_miss_tests PRIVATE shader_recompiler_agc_driver libc)
    configure_windows_unwind(ray_tracing_miss_tests)
    add_test(NAME ray_tracing_miss COMMAND ray_tracing_miss_tests)
    if(WIN32)
        set_tests_properties(ray_tracing_miss PROPERTIES ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
    endif()
    add_test_executable(vector_compare_i16_u16_tests ${LIBS_INCLUDE_DIR}/tests/VectorCompareI16U16.cpp)
    target_link_libraries(vector_compare_i16_u16_tests PRIVATE shader_recompiler_agc_driver libc)
    configure_windows_unwind(vector_compare_i16_u16_tests)
    add_test(NAME vector_compare_i16_u16 COMMAND vector_compare_i16_u16_tests)
    if(WIN32)
        set_tests_properties(vector_compare_i16_u16 PROPERTIES ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
    endif()
    add_test_executable(vector_compare_const_tests ${LIBS_INCLUDE_DIR}/tests/VectorCompareConst.cpp)
    target_link_libraries(vector_compare_const_tests PRIVATE shader_recompiler_agc_driver libc)
    configure_windows_unwind(vector_compare_const_tests)
    add_test(NAME vector_compare_const COMMAND vector_compare_const_tests)
    if(WIN32)
        set_tests_properties(vector_compare_const PROPERTIES ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
    endif()
    add_test_executable(vector_min_max_med16_tests ${LIBS_INCLUDE_DIR}/tests/VectorMinMaxMed16.cpp)
    target_link_libraries(vector_min_max_med16_tests PRIVATE shader_recompiler_agc_driver libc)
    configure_windows_unwind(vector_min_max_med16_tests)
    add_test(NAME vector_min_max_med16 COMMAND vector_min_max_med16_tests)
    if(WIN32)
        set_tests_properties(vector_min_max_med16 PROPERTIES ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
    endif()
    add_test_executable(vector_mad_legacy_tests ${LIBS_INCLUDE_DIR}/tests/VectorMadLegacy.cpp)
    target_link_libraries(vector_mad_legacy_tests PRIVATE shader_recompiler_agc_driver libc)
    configure_windows_unwind(vector_mad_legacy_tests)
    add_test(NAME vector_mad_legacy COMMAND vector_mad_legacy_tests)
    if(WIN32)
        set_tests_properties(vector_mad_legacy PROPERTIES ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
    endif()
    add_test_executable(scalar_saveexec_tests ${LIBS_INCLUDE_DIR}/tests/ScalarSaveexec.cpp)
    target_link_libraries(scalar_saveexec_tests PRIVATE shader_recompiler_agc_driver libc)
    configure_windows_unwind(scalar_saveexec_tests)
    add_test(NAME scalar_saveexec COMMAND scalar_saveexec_tests)
    if(WIN32)
        set_tests_properties(scalar_saveexec PROPERTIES ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
    endif()
    add_test_executable(scalar_bitcount64_tests ${LIBS_INCLUDE_DIR}/tests/ScalarBitCount64.cpp)
    target_link_libraries(scalar_bitcount64_tests PRIVATE shader_recompiler_agc_driver libc)
    configure_windows_unwind(scalar_bitcount64_tests)
    add_test(NAME scalar_bitcount64 COMMAND scalar_bitcount64_tests)
    if(WIN32)
        set_tests_properties(scalar_bitcount64 PROPERTIES ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
    endif()
    add_test_executable(image_sample_gather_tests ${LIBS_INCLUDE_DIR}/tests/ImageSampleGather.cpp)
    target_link_libraries(image_sample_gather_tests PRIVATE shader_recompiler_agc_driver libc)
    configure_windows_unwind(image_sample_gather_tests)
    add_test(NAME image_sample_gather COMMAND image_sample_gather_tests)
    if(WIN32)
        set_tests_properties(image_sample_gather PROPERTIES ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
    endif()
    add_test_executable(guest_indexed_draw_tests ${LIBS_INCLUDE_DIR}/tests/GuestIndexedDraw.cpp)
    target_include_directories(guest_indexed_draw_tests PRIVATE ${LIBS_INCLUDE_DIR})
    target_link_libraries(guest_indexed_draw_tests PRIVATE libSceAgc libSceAgcDriver libc)
    configure_windows_unwind(guest_indexed_draw_tests)
    add_test(NAME guest_indexed_draw COMMAND guest_indexed_draw_tests)
    if(WIN32)
        set_tests_properties(guest_indexed_draw PROPERTIES ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
    endif()
endif()

add_test_executable(agc_rdna_truncated_modifier_tests EXCLUDE_FROM_ALL tests/RdnaTruncatedModifier.cpp)
target_link_libraries(agc_rdna_truncated_modifier_tests PRIVATE shader_recompiler_agc_driver libc)
set_target_properties(agc_rdna_truncated_modifier_tests PROPERTIES CXX_EXTENSIONS OFF)
configure_windows_unwind(agc_rdna_truncated_modifier_tests)
if(BUILD_TESTING)
    set_target_properties(agc_rdna_truncated_modifier_tests PROPERTIES EXCLUDE_FROM_ALL OFF)
    add_test(NAME agc_rdna_truncated_modifier COMMAND agc_rdna_truncated_modifier_tests)
    if(WIN32)
        set_tests_properties(agc_rdna_truncated_modifier PROPERTIES ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
    endif()
endif()

add_test_executable(agc_pixel_position_export_tests EXCLUDE_FROM_ALL tests/PixelPositionExport.cpp)
target_link_libraries(agc_pixel_position_export_tests PRIVATE shader_recompiler_agc_driver libc)
set_target_properties(agc_pixel_position_export_tests PROPERTIES CXX_EXTENSIONS OFF)
configure_windows_unwind(agc_pixel_position_export_tests)
if(BUILD_TESTING)
    set_target_properties(agc_pixel_position_export_tests PROPERTIES EXCLUDE_FROM_ALL OFF)
    add_test(NAME agc_pixel_position_export COMMAND agc_pixel_position_export_tests)
    set_tests_properties(agc_pixel_position_export PROPERTIES ENVIRONMENT "ANYPS5_NO_SHADER_CACHE=1")
    if(WIN32)
        set_tests_properties(agc_pixel_position_export PROPERTIES ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
    endif()
endif()
