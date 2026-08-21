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

add_library(viper_daemon_protocol STATIC
    protocol/AppCommand.cpp
    protocol/DriverEvent.cpp
    protocol/ParameterStream.cpp
    protocol/ViperDaemonProtocol.cpp
    protocol/DeviceKey.cpp
    protocol/SnapshotSchema.cpp
    protocol/SnapshotCommand.cpp
    protocol/OwnerProtocol.cpp
    tools/Sha256.cpp
)
target_compile_features(viper_daemon_protocol PUBLIC cxx_std_20)
target_include_directories(viper_daemon_protocol PUBLIC
    protocol
    tools
    ViPERDSP/include
)

find_package(Threads REQUIRED)

add_library(viper_daemon_store STATIC
    daemon/SnapshotStore.cpp
    daemon/GenerationArbiter.cpp
    daemon/SessionRegistry.cpp
    daemon/DriverEventServer.cpp
    daemon/OwnerServer.cpp
    daemon/OwnerSupervisor.cpp
    daemon/OwnerProcess.cpp
    daemon/AppEventServer.cpp
    daemon/RouteWatcher.cpp
    daemon/RouteCache.cpp
    daemon/DaemonRuntime.cpp
)
target_compile_features(viper_daemon_store PUBLIC cxx_std_20)
target_include_directories(viper_daemon_store PUBLIC
    daemon
    protocol
)
target_link_libraries(viper_daemon_store PUBLIC viper_daemon_protocol Threads::Threads)

# Separate executable target: the daemon is a standalone root process and is
# never linked into libv4a_re.so.
add_executable(viper-daemon daemon/main.cpp)
target_link_libraries(viper-daemon PRIVATE viper_daemon_store)
target_compile_features(viper-daemon PRIVATE cxx_std_20)

# ANDROID_ABI is only defined by the NDK toolchain file. Termux's host clang also
# reports ANDROID, and there -static-libstdc++ resolves to a missing
# libc++_shared, so ANDROID alone is the wrong gate.
if(DEFINED ANDROID_ABI)
    # init execs this binary before any app process, so it must not depend on an
    # APK-provided libc++_shared.so.
    target_link_options(viper-daemon PRIVATE
        -static-libstdc++
        -Wl,--gc-sections)
endif()

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

    add_executable(viper_daemon_protocol_test tests/ViperDaemonProtocolTest.cpp)
    target_link_libraries(viper_daemon_protocol_test PRIVATE viper_daemon_protocol)
    target_compile_features(viper_daemon_protocol_test PRIVATE cxx_std_20)
    target_compile_options(viper_daemon_protocol_test PRIVATE -UNDEBUG)
    add_test(NAME viper_daemon_protocol_test COMMAND viper_daemon_protocol_test)

    add_executable(owner_protocol_test tests/OwnerProtocolTest.cpp)
    target_link_libraries(owner_protocol_test PRIVATE viper_daemon_protocol)
    target_compile_features(owner_protocol_test PRIVATE cxx_std_20)
    target_compile_options(owner_protocol_test PRIVATE -UNDEBUG)
    add_test(NAME owner_protocol_test COMMAND owner_protocol_test)

    # Fixture-only emitter: tests/owner/owner_wire_smoke.sh diffs its output
    # against the ART owner's Java codec. Not a CTest case; it asserts nothing
    # by itself.
    add_executable(owner_wire_vectors tests/OwnerWireVectors.cpp)
    target_link_libraries(owner_wire_vectors PRIVATE viper_daemon_protocol)
    target_compile_features(owner_wire_vectors PRIVATE cxx_std_20)
    target_compile_options(owner_wire_vectors PRIVATE -UNDEBUG)


    add_executable(owner_server_test tests/OwnerServerTest.cpp)
    target_link_libraries(owner_server_test PRIVATE viper_daemon_store)
    target_compile_features(owner_server_test PRIVATE cxx_std_20)
    target_compile_options(owner_server_test PRIVATE -UNDEBUG)
    add_test(NAME owner_server_test COMMAND owner_server_test)

    add_executable(owner_supervisor_test tests/OwnerSupervisorTest.cpp)
    target_link_libraries(owner_supervisor_test PRIVATE viper_daemon_store)
    target_compile_features(owner_supervisor_test PRIVATE cxx_std_20)
    target_compile_options(owner_supervisor_test PRIVATE -UNDEBUG)
    add_test(NAME owner_supervisor_test COMMAND owner_supervisor_test)

    add_executable(device_key_test tests/DeviceKeyTest.cpp)
    target_link_libraries(device_key_test PRIVATE viper_daemon_protocol)
    target_compile_features(device_key_test PRIVATE cxx_std_20)
    target_compile_options(device_key_test PRIVATE -UNDEBUG)
    add_test(NAME device_key_test COMMAND device_key_test)

    add_executable(snapshot_schema_test tests/SnapshotSchemaTest.cpp)
    target_link_libraries(snapshot_schema_test PRIVATE viper_daemon_protocol)
    target_compile_features(snapshot_schema_test PRIVATE cxx_std_20)
    target_compile_options(snapshot_schema_test PRIVATE -UNDEBUG)
    add_test(NAME snapshot_schema_test COMMAND snapshot_schema_test)

    add_executable(snapshot_store_test tests/SnapshotStoreTest.cpp)
    target_link_libraries(snapshot_store_test PRIVATE viper_daemon_store)
    target_compile_features(snapshot_store_test PRIVATE cxx_std_20)
    target_compile_options(snapshot_store_test PRIVATE -UNDEBUG)
    add_test(NAME snapshot_store_test COMMAND snapshot_store_test)

    add_executable(generation_arbiter_test tests/GenerationArbiterTest.cpp)
    target_link_libraries(generation_arbiter_test PRIVATE viper_daemon_store)
    target_compile_features(generation_arbiter_test PRIVATE cxx_std_20)
    target_compile_options(generation_arbiter_test PRIVATE -UNDEBUG)
    add_test(NAME generation_arbiter_test COMMAND generation_arbiter_test)

    add_executable(driver_daemon_bridge_test
        tests/DriverDaemonBridgeTest.cpp
        src/DriverDaemonBridge.cpp
    )
    target_include_directories(driver_daemon_bridge_test PRIVATE src)
    target_link_libraries(driver_daemon_bridge_test
        PRIVATE viper_daemon_protocol Threads::Threads)
    target_compile_features(driver_daemon_bridge_test PRIVATE cxx_std_20)
    target_compile_options(driver_daemon_bridge_test PRIVATE -UNDEBUG)
    add_test(NAME driver_daemon_bridge_test COMMAND driver_daemon_bridge_test)

    add_executable(driver_event_publisher_test
        tests/DriverEventPublisherTest.cpp
        src/DriverDaemonBridge.cpp
        src/DriverEventPublisher.cpp
    )
    target_include_directories(driver_event_publisher_test PRIVATE src tests)
    target_link_libraries(driver_event_publisher_test
        PRIVATE viper_daemon_protocol Threads::Threads)
    target_compile_features(driver_event_publisher_test PRIVATE cxx_std_20)
    target_compile_options(driver_event_publisher_test PRIVATE -UNDEBUG)
    add_test(NAME driver_event_publisher_test COMMAND driver_event_publisher_test)

    add_executable(session_registry_test tests/SessionRegistryTest.cpp)
    target_link_libraries(session_registry_test PRIVATE viper_daemon_store)
    target_compile_features(session_registry_test PRIVATE cxx_std_20)
    target_compile_options(session_registry_test PRIVATE -UNDEBUG)
    add_test(NAME session_registry_test COMMAND session_registry_test)

    add_executable(daemon_runtime_test tests/DaemonRuntimeTest.cpp)
    target_link_libraries(daemon_runtime_test PRIVATE viper_daemon_store)
    target_compile_features(daemon_runtime_test PRIVATE cxx_std_20)
    target_compile_options(daemon_runtime_test PRIVATE -UNDEBUG)
    add_test(NAME daemon_runtime_test COMMAND daemon_runtime_test)

    add_executable(daemon_driver_e2e_test
        tests/DaemonDriverEndToEndTest.cpp
        src/DriverDaemonBridge.cpp
        src/DriverEventPublisher.cpp
    )
    target_include_directories(daemon_driver_e2e_test PRIVATE src)
    target_link_libraries(daemon_driver_e2e_test PRIVATE viper_daemon_store)
    target_compile_features(daemon_driver_e2e_test PRIVATE cxx_std_20)
    target_compile_options(daemon_driver_e2e_test PRIVATE -UNDEBUG)
    add_test(NAME daemon_driver_e2e_test COMMAND daemon_driver_e2e_test)

    add_executable(parameter_stream_test tests/ParameterStreamTest.cpp)
    target_link_libraries(parameter_stream_test PRIVATE viper_daemon_protocol)
    target_compile_features(parameter_stream_test PRIVATE cxx_std_20)
    target_compile_options(parameter_stream_test PRIVATE -UNDEBUG)
    add_test(NAME parameter_stream_test COMMAND parameter_stream_test)

    add_executable(snapshot_apply_controller_test
        tests/SnapshotApplyControllerTest.cpp
        src/SnapshotApplyController.cpp
    )
    target_include_directories(snapshot_apply_controller_test PRIVATE src)
    target_link_libraries(snapshot_apply_controller_test PRIVATE viper_daemon_protocol)
    target_compile_features(snapshot_apply_controller_test PRIVATE cxx_std_20)
    target_compile_options(snapshot_apply_controller_test PRIVATE -UNDEBUG)
    add_test(NAME snapshot_apply_controller_test COMMAND snapshot_apply_controller_test)

    add_executable(snapshot_command_test tests/SnapshotCommandTest.cpp)
    target_link_libraries(snapshot_command_test PRIVATE viper_daemon_protocol)
    target_compile_features(snapshot_command_test PRIVATE cxx_std_20)
    target_compile_options(snapshot_command_test PRIVATE -UNDEBUG)
    add_test(NAME snapshot_command_test COMMAND snapshot_command_test)

    add_executable(viper_context_snapshot_test
        tests/ViperContextSnapshotTest.cpp
        src/AudioFormat.cpp
        src/DriverDaemonBridge.cpp
        src/DriverEventPublisher.cpp
        src/DspGraph.cpp
        src/DspGraphSlots.cpp
        src/DspResources.cpp
        src/GraphCrossfade.cpp
        src/IemContext.cpp
        src/IemGraph.cpp
        src/IemGraphSlots.cpp
        src/IemParameterMailbox.cpp
        src/IemResources.cpp
        src/ParameterMailbox.cpp
        src/SnapshotApplyController.cpp
        src/ViperContext.cpp
    )
    target_include_directories(viper_context_snapshot_test PRIVATE
        src src/include ViPERDSP ViPERDSP/include)
    target_link_libraries(viper_context_snapshot_test
        PRIVATE viper_daemon_protocol IEMDSP ViPERDSP Threads::Threads m)
    target_compile_features(viper_context_snapshot_test PRIVATE cxx_std_20)
    target_compile_options(viper_context_snapshot_test PRIVATE -UNDEBUG)
    add_test(NAME viper_context_snapshot_test COMMAND viper_context_snapshot_test)

    add_executable(viper_context_configure_test
        tests/ViperContextConfigureTest.cpp
        src/AudioFormat.cpp
        src/DriverDaemonBridge.cpp
        src/DriverEventPublisher.cpp
        src/DspGraph.cpp
        src/DspGraphSlots.cpp
        src/DspResources.cpp
        src/GraphCrossfade.cpp
        src/IemContext.cpp
        src/IemGraph.cpp
        src/IemGraphSlots.cpp
        src/IemParameterMailbox.cpp
        src/IemResources.cpp
        src/ParameterMailbox.cpp
        src/SnapshotApplyController.cpp
        src/ViperContext.cpp
    )
    target_include_directories(viper_context_configure_test PRIVATE
        src src/include ViPERDSP ViPERDSP/include)
    target_link_libraries(viper_context_configure_test
        PRIVATE viper_daemon_protocol IEMDSP ViPERDSP Threads::Threads m)
    target_compile_features(viper_context_configure_test PRIVATE cxx_std_20)
    target_compile_options(viper_context_configure_test PRIVATE -UNDEBUG)
    add_test(NAME viper_context_configure_test COMMAND viper_context_configure_test)

    add_executable(snapshot_ack_e2e_test
        tests/SnapshotAckEndToEndTest.cpp
        src/AudioFormat.cpp
        src/DriverDaemonBridge.cpp
        src/DriverEventPublisher.cpp
        src/DspGraph.cpp
        src/DspGraphSlots.cpp
        src/DspResources.cpp
        src/GraphCrossfade.cpp
        src/IemContext.cpp
        src/IemGraph.cpp
        src/IemGraphSlots.cpp
        src/IemParameterMailbox.cpp
        src/IemResources.cpp
        src/ParameterMailbox.cpp
        src/SnapshotApplyController.cpp
        src/ViperContext.cpp
    )
    target_include_directories(snapshot_ack_e2e_test PRIVATE
        src src/include tests ViPERDSP ViPERDSP/include)
    target_link_libraries(snapshot_ack_e2e_test
        PRIVATE viper_daemon_protocol IEMDSP ViPERDSP Threads::Threads m)
    target_compile_features(snapshot_ack_e2e_test PRIVATE cxx_std_20)
    target_compile_options(snapshot_ack_e2e_test PRIVATE -UNDEBUG)
    add_test(NAME snapshot_ack_e2e_test COMMAND snapshot_ack_e2e_test)

    add_executable(daemon_golden_vector tools/DaemonGoldenVector.cpp)
    target_link_libraries(daemon_golden_vector PRIVATE viper_daemon_protocol)
    target_compile_features(daemon_golden_vector PRIVATE cxx_std_20)

    add_executable(route_watcher_test tests/RouteWatcherTest.cpp)
    target_link_libraries(route_watcher_test PRIVATE viper_daemon_store)
    target_compile_features(route_watcher_test PRIVATE cxx_std_20)
    target_compile_options(route_watcher_test PRIVATE -UNDEBUG)
    add_test(NAME route_watcher_test COMMAND route_watcher_test)

    add_executable(route_cache_test tests/RouteCacheTest.cpp)
    target_link_libraries(route_cache_test PRIVATE viper_daemon_store)
    target_compile_features(route_cache_test PRIVATE cxx_std_20)
    target_compile_options(route_cache_test PRIVATE -UNDEBUG)
    add_test(NAME route_cache_test COMMAND route_cache_test)

    add_executable(app_event_server_test tests/AppEventServerTest.cpp)
    target_link_libraries(app_event_server_test PRIVATE viper_daemon_store)
    target_compile_features(app_event_server_test PRIVATE cxx_std_20)
    target_compile_options(app_event_server_test PRIVATE -UNDEBUG)
    add_test(NAME app_event_server_test COMMAND app_event_server_test)

    add_executable(route_restore_integration_test
        tests/RouteRestoreIntegrationTest.cpp
        src/AudioFormat.cpp
        src/DriverDaemonBridge.cpp
        src/DriverEventPublisher.cpp
        src/DspGraph.cpp
        src/DspGraphSlots.cpp
        src/DspResources.cpp
        src/GraphCrossfade.cpp
        src/IemContext.cpp
        src/IemGraph.cpp
        src/IemGraphSlots.cpp
        src/IemParameterMailbox.cpp
        src/IemResources.cpp
        src/ParameterMailbox.cpp
        src/SnapshotApplyController.cpp
        src/ViperContext.cpp
    )
    target_include_directories(route_restore_integration_test PRIVATE
        src src/include tests ViPERDSP ViPERDSP/include)
    target_link_libraries(route_restore_integration_test
        PRIVATE viper_daemon_store IEMDSP ViPERDSP Threads::Threads m)
    target_compile_features(route_restore_integration_test PRIVATE cxx_std_20)
    target_compile_options(route_restore_integration_test PRIVATE -UNDEBUG)
    add_test(NAME route_restore_integration_test
        COMMAND route_restore_integration_test)

    add_executable(app_daemon_driver_e2e_test
        tests/AppDaemonDriverEndToEndTest.cpp
        src/AudioFormat.cpp
        src/DriverDaemonBridge.cpp
        src/DriverEventPublisher.cpp
        src/DspGraph.cpp
        src/DspGraphSlots.cpp
        src/DspResources.cpp
        src/GraphCrossfade.cpp
        src/IemContext.cpp
        src/IemGraph.cpp
        src/IemGraphSlots.cpp
        src/IemParameterMailbox.cpp
        src/IemResources.cpp
        src/ParameterMailbox.cpp
        src/SnapshotApplyController.cpp
        src/ViperContext.cpp
    )
    target_include_directories(app_daemon_driver_e2e_test PRIVATE
        src src/include tests ViPERDSP ViPERDSP/include)
    target_link_libraries(app_daemon_driver_e2e_test
        PRIVATE viper_daemon_store IEMDSP ViPERDSP Threads::Threads m)
    target_compile_features(app_daemon_driver_e2e_test PRIVATE cxx_std_20)
    target_compile_options(app_daemon_driver_e2e_test PRIVATE -UNDEBUG)
    add_test(NAME app_daemon_driver_e2e_test COMMAND app_daemon_driver_e2e_test)
endif()

add_library(v4a_re SHARED
    src/AudioFormat.cpp
    src/DriverDaemonBridge.cpp
    src/DriverEventPublisher.cpp
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
    src/SnapshotApplyController.cpp
    src/ViPER4Android.cpp
    src/ViperContext.cpp
)

target_include_directories(v4a_re PRIVATE
    src
    src/include
    protocol
)

target_link_libraries(v4a_re IEMDSP ViPERDSP viper_daemon_protocol log)

target_compile_options(v4a_re PRIVATE
    -Wall -Wno-unused-parameter -Wno-unused-variable -Wno-unused-function)

target_link_options(v4a_re PRIVATE
    -Wl,--gc-sections
    -Wl,--strip-all
    -Wl,--exclude-libs,ALL
    -fvisibility=hidden)
