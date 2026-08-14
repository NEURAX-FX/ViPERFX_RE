cmake_minimum_required(VERSION 3.16.3)
project("ViPER4Android")

option(BUILD_ANALYZER_TESTS "Build host-side audio analyzer tests" OFF)

set(VERSION_CODE "00000000" CACHE STRING "Build version code")
set(VERSION_NAME "0.0.0" CACHE STRING "Build version name")

add_compile_definitions(VERSION_CODE=${VERSION_CODE})
add_compile_definitions(VERSION_NAME="${VERSION_NAME}")

set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ON)

add_compile_options(
    -O3 -DNDEBUG
    -ffunction-sections -fdata-sections
    -fno-exceptions -fno-rtti
    -fno-unwind-tables -fno-asynchronous-unwind-tables
    -fvisibility=hidden -fvisibility-inlines-hidden)

add_library(pffft_core STATIC ViPERDSP/viper/utils/pffft.c)
target_include_directories(pffft_core PUBLIC ViPERDSP/viper/utils)
target_link_libraries(pffft_core PUBLIC m)

add_subdirectory(IEMDSP)
add_subdirectory(ViPERDSP)

if(BUILD_ANALYZER_TESTS)
    enable_testing()

    add_executable(iem_asset_compiler
        tools/IemAssetCompiler.cpp
        tools/Sha256.cpp
    )
    target_include_directories(iem_asset_compiler PRIVATE tools)
    target_link_libraries(iem_asset_compiler PRIVATE IEMDSP)

    add_executable(iem_asset_compiler_test
        tests/IemAssetCompilerTest.cpp
        tools/IemAssetCompiler.cpp
        tools/Sha256.cpp
    )
    target_compile_definitions(iem_asset_compiler_test PRIVATE
        IEM_ASSET_COMPILER_NO_MAIN=1
        IEM_ASSET_SOURCE_DIR="${CMAKE_SOURCE_DIR}/IEMDSP/resources/source"
        IEM_GENERATED_RESOURCE_DIR="${CMAKE_SOURCE_DIR}/IEMDSP/resources/generated"
    )
    target_include_directories(iem_asset_compiler_test PRIVATE
        tools
        IEMDSP/resources/generated
    )
    target_link_libraries(iem_asset_compiler_test PRIVATE IEMDSP)
    add_test(NAME iem_asset_compiler_test COMMAND iem_asset_compiler_test)

    add_executable(iem_ku100_decoder_test
        tests/IemKu100DecoderTest.cpp
    )
    target_link_libraries(iem_ku100_decoder_test PRIVATE IEMDSP)
    add_test(NAME iem_ku100_decoder_test COMMAND iem_ku100_decoder_test)

    add_executable(iem_limiter_test tests/IemLimiterTest.cpp)
    target_link_libraries(iem_limiter_test PRIVATE IEMDSP)
    add_test(NAME iem_limiter_test COMMAND iem_limiter_test)

    add_executable(iem_pipeline_test tests/IemPipelineTest.cpp)
    target_link_libraries(iem_pipeline_test PRIVATE IEMDSP)
    add_test(NAME iem_pipeline_test COMMAND iem_pipeline_test)

    add_executable(iem_realtime_audit_test tests/IemRealtimeAuditTest.cpp)
    target_link_libraries(iem_realtime_audit_test PRIVATE IEMDSP)
    add_test(NAME iem_realtime_audit_test COMMAND iem_realtime_audit_test)

    find_package(Threads REQUIRED)
    add_executable(audio_analyzer_test
        tests/AudioAnalyzerTest.cpp
        ViPERDSP/viper/effects/AudioAnalyzer.cpp
    )
    target_include_directories(audio_analyzer_test PRIVATE ViPERDSP src)
    target_link_libraries(audio_analyzer_test PRIVATE IEMDSP pffft_core m)
    add_test(NAME audio_analyzer_test COMMAND audio_analyzer_test)

    add_executable(audio_format_test
        tests/AudioFormatTest.cpp
        src/AudioFormat.cpp
    )
    target_include_directories(audio_format_test PRIVATE src)
    add_test(NAME audio_format_test COMMAND audio_format_test)

    add_executable(graph_crossfade_test
        tests/GraphCrossfadeTest.cpp
        src/AudioFormat.cpp
        src/GraphCrossfade.cpp
    )
    target_include_directories(graph_crossfade_test PRIVATE src)
    add_test(NAME graph_crossfade_test COMMAND graph_crossfade_test)

    add_executable(dsp_graph_test
        tests/DspGraphTest.cpp
        src/AudioFormat.cpp
        src/DspGraph.cpp
        src/GraphCrossfade.cpp
    )
    target_include_directories(dsp_graph_test PRIVATE src ViPERDSP ViPERDSP/include)
    target_link_libraries(dsp_graph_test PRIVATE ViPERDSP m)
    add_test(NAME dsp_graph_test COMMAND dsp_graph_test)

    add_executable(parameter_snapshot_test
        tests/ParameterSnapshotTest.cpp
        ViPERDSP/viper/ParameterSnapshot.cpp
    )
    target_include_directories(parameter_snapshot_test PRIVATE ViPERDSP ViPERDSP/include)
    add_test(NAME parameter_snapshot_test COMMAND parameter_snapshot_test)

    add_executable(dsp_resources_test
        tests/DspResourcesTest.cpp
        src/AudioFormat.cpp
        src/DspGraph.cpp
        src/DspResources.cpp
        src/GraphCrossfade.cpp
    )
    target_include_directories(dsp_resources_test PRIVATE src ViPERDSP ViPERDSP/include)
    target_link_libraries(dsp_resources_test PRIVATE ViPERDSP m)
    add_test(NAME dsp_resources_test COMMAND dsp_resources_test)

    add_executable(dsp_graph_slots_test
        tests/DspGraphSlotsTest.cpp
        src/AudioFormat.cpp
        src/DspGraph.cpp
        src/DspGraphSlots.cpp
        src/DspResources.cpp
        src/GraphCrossfade.cpp
    )
    target_include_directories(dsp_graph_slots_test PRIVATE src ViPERDSP ViPERDSP/include)
    target_link_libraries(dsp_graph_slots_test PRIVATE ViPERDSP m)
    add_test(NAME dsp_graph_slots_test COMMAND dsp_graph_slots_test)

    add_executable(parameter_mailbox_test
        tests/ParameterMailboxTest.cpp
        src/ParameterMailbox.cpp
    )
    target_include_directories(parameter_mailbox_test PRIVATE src ViPERDSP/include)
    add_test(NAME parameter_mailbox_test COMMAND parameter_mailbox_test)

    add_executable(matrix_convolver_test
        tests/MatrixConvolverTest.cpp
        ViPERDSP/viper/utils/MatrixConvolver.cpp
    )
    target_include_directories(matrix_convolver_test PRIVATE ViPERDSP)
    target_link_libraries(matrix_convolver_test PRIVATE pffft_core m)
    add_test(NAME matrix_convolver_test COMMAND matrix_convolver_test)

    add_executable(convolver_test tests/ConvolverTest.cpp)
    target_include_directories(convolver_test PRIVATE ViPERDSP)
    target_link_libraries(convolver_test PRIVATE ViPERDSP m)
    add_test(NAME convolver_test COMMAND convolver_test)

    add_executable(wav_reader_test tests/WavReaderTest.cpp)
    target_include_directories(wav_reader_test PRIVATE ViPERDSP)
    add_test(NAME wav_reader_test COMMAND wav_reader_test)

    add_executable(iem_aligned_planar_buffer_test
        tests/IemAlignedPlanarBufferTest.cpp
    )
    target_link_libraries(iem_aligned_planar_buffer_test PRIVATE IEMDSP)
    add_test(NAME iem_aligned_planar_buffer_test
        COMMAND iem_aligned_planar_buffer_test)

    add_executable(iem_params_test tests/IemParamsTest.cpp)
    target_link_libraries(iem_params_test PRIVATE IEMDSP)
    add_test(NAME iem_params_test COMMAND iem_params_test)

    add_executable(iem_fann_dialog_net_test tests/IemFannDialogNetTest.cpp)
    target_link_libraries(iem_fann_dialog_net_test PRIVATE IEMDSP)
    add_test(NAME iem_fann_dialog_net_test COMMAND iem_fann_dialog_net_test)

    add_executable(iem_halo_stft_test tests/IemHaloStftTest.cpp)
    target_link_libraries(iem_halo_stft_test PRIVATE IEMDSP)
    add_test(NAME iem_halo_stft_test COMMAND iem_halo_stft_test)

    add_executable(iem_halo_dialog_extractor_test tests/IemHaloDialogExtractorTest.cpp)
    target_link_libraries(iem_halo_dialog_extractor_test PRIVATE IEMDSP)
    add_test(NAME iem_halo_dialog_extractor_test COMMAND iem_halo_dialog_extractor_test)

    add_executable(iem_halo_surround_assigner_test tests/IemHaloSurroundAssignerTest.cpp)
    target_link_libraries(iem_halo_surround_assigner_test PRIVATE IEMDSP)
    add_test(NAME iem_halo_surround_assigner_test COMMAND iem_halo_surround_assigner_test)

    add_executable(iem_halo_diffusion_test tests/IemHaloDiffusionTest.cpp)
    target_link_libraries(iem_halo_diffusion_test PRIVATE IEMDSP)
    add_test(NAME iem_halo_diffusion_test COMMAND iem_halo_diffusion_test)

    add_executable(iem_halo_lfe_synth_test tests/IemHaloLfeSynthTest.cpp)
    target_link_libraries(iem_halo_lfe_synth_test PRIVATE IEMDSP)
    add_test(NAME iem_halo_lfe_synth_test COMMAND iem_halo_lfe_synth_test)

    add_executable(iem_halo_downmix_params_test tests/IemHaloDownmixParamsTest.cpp)
    target_link_libraries(iem_halo_downmix_params_test PRIVATE IEMDSP)
    add_test(NAME iem_halo_downmix_params_test COMMAND iem_halo_downmix_params_test)

    add_executable(iem_halo_downmix_processor_test tests/IemHaloDownmixProcessorTest.cpp)
    target_link_libraries(iem_halo_downmix_processor_test PRIVATE IEMDSP)
    add_test(NAME iem_halo_downmix_processor_test COMMAND iem_halo_downmix_processor_test)

    add_executable(iem_halo_encoder_test tests/IemHaloEncoderTest.cpp)
    target_link_libraries(iem_halo_encoder_test PRIVATE IEMDSP)
    add_test(NAME iem_halo_encoder_test COMMAND iem_halo_encoder_test)

    add_executable(iem_halo_downmix_decoder_test tests/IemHaloDownmixDecoderTest.cpp)
    target_link_libraries(iem_halo_downmix_decoder_test PRIVATE IEMDSP)
    add_test(NAME iem_halo_downmix_decoder_test COMMAND iem_halo_downmix_decoder_test)

    add_executable(iem_spherical_harmonics_test
        tests/IemSphericalHarmonicsTest.cpp
    )
    target_link_libraries(iem_spherical_harmonics_test PRIVATE IEMDSP)
    add_test(NAME iem_spherical_harmonics_test
        COMMAND iem_spherical_harmonics_test
    )

    add_executable(iem_encoder_test tests/IemEncoderTest.cpp)
    target_link_libraries(iem_encoder_test PRIVATE IEMDSP)
    add_test(NAME iem_encoder_test COMMAND iem_encoder_test)

    add_executable(iem_granular_encoder_test
        tests/IemGranularEncoderTest.cpp
    )
    target_link_libraries(iem_granular_encoder_test PRIVATE IEMDSP)
    add_test(NAME iem_granular_encoder_test
        COMMAND iem_granular_encoder_test
    )

    add_executable(iem_scene_rotator_test
        tests/IemSceneRotatorTest.cpp
    )
    target_link_libraries(iem_scene_rotator_test PRIVATE IEMDSP)
    add_test(NAME iem_scene_rotator_test COMMAND iem_scene_rotator_test)

    add_executable(iem_partitioned_matrix_convolver_test
        tests/IemPartitionedMatrixConvolverTest.cpp
    )
    target_link_libraries(iem_partitioned_matrix_convolver_test PRIVATE IEMDSP)
    add_test(NAME iem_partitioned_matrix_convolver_test
        COMMAND iem_partitioned_matrix_convolver_test
    )

    add_executable(iem_parameter_mailbox_test
        tests/IemParameterMailboxTest.cpp
        src/IemParameterMailbox.cpp
    )
    target_include_directories(iem_parameter_mailbox_test PRIVATE src)
    target_link_libraries(iem_parameter_mailbox_test PRIVATE
        IEMDSP Threads::Threads)
    add_test(NAME iem_parameter_mailbox_test
        COMMAND iem_parameter_mailbox_test)

    add_executable(iem_streaming_resampler_test
        tests/IemStreamingResamplerTest.cpp
    )
    target_link_libraries(iem_streaming_resampler_test PRIVATE IEMDSP m)
    add_test(NAME iem_streaming_resampler_test
        COMMAND iem_streaming_resampler_test)

    add_executable(iem_planar_block_scheduler_test
        tests/IemPlanarBlockSchedulerTest.cpp
    )
    target_link_libraries(iem_planar_block_scheduler_test PRIVATE IEMDSP)
    add_test(NAME iem_planar_block_scheduler_test
        COMMAND iem_planar_block_scheduler_test)

    add_executable(iem_engine_test tests/IemEngineTest.cpp)
    target_link_libraries(iem_engine_test PRIVATE IEMDSP m)
    add_test(NAME iem_engine_test COMMAND iem_engine_test)

    add_executable(iem_graph_slots_test
        tests/IemGraphSlotsTest.cpp
        src/AudioFormat.cpp
        src/GraphCrossfade.cpp
        src/IemGraph.cpp
        src/IemGraphSlots.cpp
        src/IemResources.cpp
    )
    target_include_directories(iem_graph_slots_test PRIVATE src)
    target_link_libraries(iem_graph_slots_test PRIVATE IEMDSP m)
    add_test(NAME iem_graph_slots_test COMMAND iem_graph_slots_test)

    add_executable(iem_context_test
        tests/IemContextTest.cpp
        src/AudioFormat.cpp
        src/DspGraph.cpp
        src/GraphCrossfade.cpp
        src/IemContext.cpp
        src/IemGraph.cpp
        src/IemGraphSlots.cpp
        src/IemParameterMailbox.cpp
        src/IemResources.cpp
    )
    target_include_directories(iem_context_test PRIVATE src ViPERDSP ViPERDSP/include)
    target_link_libraries(iem_context_test PRIVATE IEMDSP ViPERDSP m)
    add_test(NAME iem_context_test COMMAND iem_context_test)

    add_executable(iem_telemetry_test tests/IemTelemetryTest.cpp)
    target_include_directories(iem_telemetry_test PRIVATE src ViPERDSP)
    target_link_libraries(iem_telemetry_test PRIVATE IEMDSP Threads::Threads)
    add_test(NAME iem_telemetry_test COMMAND iem_telemetry_test)
endif()

add_library(v4a_re SHARED
    src/AudioFormat.cpp
    src/DspGraph.cpp
    src/DspGraphSlots.cpp
    src/GraphCrossfade.cpp
    src/IemGraph.cpp
    src/IemGraphSlots.cpp
    src/IemContext.cpp
    src/IemParameterMailbox.cpp
    src/IemResources.cpp
    src/ParameterMailbox.cpp
    src/DspResources.cpp
    src/ViPER4Android.cpp
    src/ViperContext.cpp
)

target_include_directories(v4a_re PRIVATE
    src
    src/include
)

target_link_libraries(v4a_re IEMDSP ViPERDSP log)

target_compile_options(v4a_re PRIVATE
    -Wall -Wno-unused-parameter -Wno-unused-variable -Wno-unused-function)

target_link_options(v4a_re PRIVATE
    -Wl,--gc-sections
    -Wl,--strip-all
    -Wl,--exclude-libs,ALL
    -fvisibility=hidden)
