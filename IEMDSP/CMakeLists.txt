cmake_minimum_required(VERSION 3.16)

add_library(IEMDSP STATIC
    upstream/efficientSHvanilla.cpp
    src/AlignedPlanarBuffer.cpp
    src/Grain.cpp
    src/GranularEncoder.cpp
    src/IemEngine.cpp
    src/IemParams.cpp
    src/IemTelemetry.cpp
    src/MultiEncoder.cpp
    src/PlanarBlockScheduler.cpp
    src/SphericalHarmonics.cpp
    src/StereoEncoder.cpp
    src/StreamingResampler.cpp
)

target_include_directories(IEMDSP
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_features(IEMDSP PUBLIC cxx_std_17)
target_compile_options(IEMDSP PRIVATE
    -Wall -Wextra -Werror
    -fno-exceptions -fno-rtti
)
