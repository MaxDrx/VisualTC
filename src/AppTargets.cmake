# ---------------------------------------------------------------------------
# VisualTC desktop application (Qt 6 Widgets)
# ---------------------------------------------------------------------------
find_package(Qt6 6.5 REQUIRED COMPONENTS Core Gui Widgets Concurrent Svg)
qt_standard_project_setup()

set(VISUALTC_UI_SOURCES
    app/AppSettings.cpp
    app/Theme.cpp
    io/DecoderClient.cpp
    io/ExtractionArea.cpp
    io/FrameProvider.cpp
    io/ImportTask.cpp
    io/ThumbnailProvider.cpp
    ui/Icons.cpp
    ui/MainWindow.cpp
    ui/SeriesBrowser.cpp
    ui/SeriesDock.cpp
    ui/ViewerGrid.cpp
    ui/PreferencesDialog.cpp
    ui/HistogramDialog.cpp
    ui/DicomInfoDialog.cpp
    viewer2d/ImageSource.cpp
    viewer2d/StackSource.cpp
    viewer2d/Viewport.cpp
    viewer2d/Annotations.cpp
    viewer2d/AnnotationStore.cpp
    mpr/MprSession.cpp
    mpr/MprSource.cpp
    export/ImageExporter.cpp
)

qt_add_library(visualtc_ui STATIC ${VISUALTC_UI_SOURCES})
target_include_directories(visualtc_ui PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(visualtc_ui PUBLIC VisualTC::core Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Concurrent Qt6::Svg)
target_compile_definitions(visualtc_ui PUBLIC
    VISUALTC_VERSION="${PROJECT_VERSION}")

set(VISUALTC_ICNS ${CMAKE_SOURCE_DIR}/packaging/macos/VisualTC.icns)
set_source_files_properties(${VISUALTC_ICNS} PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
qt_add_executable(VisualTC WIN32 MACOSX_BUNDLE app/main.cpp ${VISUALTC_ICNS})
qt_add_resources(VisualTC "visualtc_resources"
    PREFIX "/"
    BASE ${CMAKE_SOURCE_DIR}/resources
    FILES
        ${CMAKE_SOURCE_DIR}/resources/icons/app.svg
        ${CMAKE_SOURCE_DIR}/resources/icons/open-file.svg
        ${CMAKE_SOURCE_DIR}/resources/icons/open-folder.svg
        ${CMAKE_SOURCE_DIR}/resources/icons/layout.svg
        ${CMAKE_SOURCE_DIR}/resources/icons/window-level.svg
        ${CMAKE_SOURCE_DIR}/resources/icons/pan.svg
        ${CMAKE_SOURCE_DIR}/resources/icons/zoom.svg
        ${CMAKE_SOURCE_DIR}/resources/icons/scroll.svg
        ${CMAKE_SOURCE_DIR}/resources/icons/ruler.svg
        ${CMAKE_SOURCE_DIR}/resources/icons/angle.svg
        ${CMAKE_SOURCE_DIR}/resources/icons/cobb.svg
        ${CMAKE_SOURCE_DIR}/resources/icons/roi-rect.svg
        ${CMAKE_SOURCE_DIR}/resources/icons/roi-ellipse.svg
        ${CMAKE_SOURCE_DIR}/resources/icons/roi-freehand.svg
        ${CMAKE_SOURCE_DIR}/resources/icons/probe.svg
        ${CMAKE_SOURCE_DIR}/resources/icons/mpr.svg
        ${CMAKE_SOURCE_DIR}/resources/icons/reset.svg
        ${CMAKE_SOURCE_DIR}/resources/icons/settings.svg
        ${CMAKE_SOURCE_DIR}/resources/icons/rotate-cw.svg
        ${CMAKE_SOURCE_DIR}/resources/icons/rotate-ccw.svg
        ${CMAKE_SOURCE_DIR}/resources/icons/flip-h.svg
        ${CMAKE_SOURCE_DIR}/resources/icons/flip-v.svg
        ${CMAKE_SOURCE_DIR}/resources/icons/invert.svg
        ${CMAKE_SOURCE_DIR}/resources/icons/sync.svg
        ${CMAKE_SOURCE_DIR}/resources/icons/reference-lines.svg
        ${CMAKE_SOURCE_DIR}/resources/icons/annotations.svg
        ${CMAKE_SOURCE_DIR}/resources/icons/capture.svg
        ${CMAKE_SOURCE_DIR}/resources/icons/export.svg
        ${CMAKE_SOURCE_DIR}/resources/icons/cine.svg
        ${CMAKE_SOURCE_DIR}/resources/icons/crosshair.svg
        ${CMAKE_SOURCE_DIR}/resources/icons/undo.svg
        ${CMAKE_SOURCE_DIR}/resources/icons/delete.svg
        ${CMAKE_SOURCE_DIR}/resources/icons/fit.svg
        ${CMAKE_SOURCE_DIR}/resources/icons/info.svg
        ${CMAKE_SOURCE_DIR}/resources/icons/sidebar.svg
        ${CMAKE_SOURCE_DIR}/resources/icons/collapse-left.svg
        ${CMAKE_SOURCE_DIR}/resources/icons/collapse-right.svg
        ${CMAKE_SOURCE_DIR}/resources/icons/menu-arrow.svg
        ${CMAKE_SOURCE_DIR}/resources/icons/histogram.svg
        ${CMAKE_SOURCE_DIR}/resources/icons/lut.svg
)
target_link_libraries(VisualTC PRIVATE visualtc_ui)
add_dependencies(VisualTC visualtc-worker)
set_target_properties(VisualTC PROPERTIES
    OUTPUT_NAME VisualTC
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
    MACOSX_BUNDLE_GUI_IDENTIFIER org.visualtc.viewer
    MACOSX_BUNDLE_BUNDLE_NAME VisualTC
    MACOSX_BUNDLE_BUNDLE_VERSION ${PROJECT_VERSION}
    MACOSX_BUNDLE_SHORT_VERSION_STRING ${PROJECT_VERSION}
    MACOSX_BUNDLE_INFO_PLIST ${CMAKE_SOURCE_DIR}/packaging/macos/Info.plist.in)
if(WIN32)
    # Icon and the version shown in Explorer (Properties > Details).
    configure_file(${CMAKE_SOURCE_DIR}/packaging/windows/visualtc.rc.in ${CMAKE_BINARY_DIR}/visualtc.rc @ONLY)
    target_sources(VisualTC PRIVATE ${CMAKE_BINARY_DIR}/visualtc.rc)
endif()
if(APPLE)
    # The isolated decoder lives next to the executable inside the bundle.
    add_custom_command(TARGET VisualTC POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:visualtc-worker>
                $<TARGET_BUNDLE_CONTENT_DIR:VisualTC>/MacOS/visualtc-worker)
endif()
