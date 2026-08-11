cmake_minimum_required(VERSION 3.16)

add_library(IEMDSP STATIC
    upstream/efficientSHvanilla.cpp
    resources/generated/IemResources.cpp
    src/AlignedPlanarBuffer.cpp
    src/Grain.cpp
    src/GranularEncoder.cpp
    src/IemEngine.cpp
    src/IemParams.cpp
    src/IemTelemetry.cpp
    src/MultiEncoder.cpp
    src/PartitionedMatrixConvolver.cpp
    src/PlanarBlockScheduler.cpp
    src/SceneRotator.cpp
    src/SphericalHarmonics.cpp
    src/StereoEncoder.cpp
    src/StreamingResampler.cpp
)

target_include_directories(IEMDSP
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/resources/generated
)

target_compile_features(IEMDSP PUBLIC cxx_std_17)
target_link_libraries(IEMDSP PUBLIC pffft_core)
target_compile_options(IEMDSP PRIVATE
    -Wall -Wextra -Werror
    -fno-exceptions -fno-rtti
)
