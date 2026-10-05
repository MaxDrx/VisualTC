# ---------------------------------------------------------------------------
# visualtc_core: pure C++20 (no Qt). DICOM parsing/decoding (GDCM), geometry,
# imaging math, measurements and MPR reslicing. Fully unit tested.
# ---------------------------------------------------------------------------
add_library(visualtc_core STATIC
    core/Logger.cpp
    core/SystemInfo.cpp
    dicom/TextUtil.cpp
    dicom/DicomPreflight.cpp
    dicom/DicomParser.cpp
    dicom/DicomScanner.cpp
    dicom/DicomGeometry.cpp
    dicom/DicomSorter.cpp
    dicom/DicomStudy.cpp
    dicom/DicomDecoder.cpp
    dicom/WorkerProtocol.cpp
    imaging/PixelData.cpp
    imaging/WindowLevel.cpp
    imaging/Orientation.cpp
    imaging/FrameCache.cpp
    measurements/MeasurementMath.cpp
    measurements/RoiStatistics.cpp
    mpr/ImageVolume.cpp
    mpr/Reslicer.cpp
    mpr/MprGeometry.cpp
    synchronization/SpatialSync.cpp
)
add_library(VisualTC::core ALIAS visualtc_core)
target_include_directories(visualtc_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_include_directories(visualtc_core SYSTEM PRIVATE ${GDCM_INCLUDE_DIRS})
target_link_libraries(visualtc_core
    PUBLIC Threads::Threads VisualTC::options
    PRIVATE gdcmMSFF)
set_target_properties(visualtc_core PROPERTIES POSITION_INDEPENDENT_CODE ON)

# ---------------------------------------------------------------------------
# visualtc-worker: isolated parser/decoder process (no Qt). The viewer sends
# every untrusted file through it, so codec crashes cannot close the viewer.
# ---------------------------------------------------------------------------
add_executable(visualtc-worker worker/WorkerMain.cpp)
target_link_libraries(visualtc-worker PRIVATE VisualTC::core)
set_target_properties(visualtc-worker PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

if(VISUALTC_BUILD_APP)
    include(${CMAKE_CURRENT_SOURCE_DIR}/AppTargets.cmake)
endif()
