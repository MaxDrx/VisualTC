# Installation layout and CPack configuration.
#
#   Windows : bin/VisualTC.exe + visualtc-worker.exe (+ Qt DLLs via windeployqt);
#             installer built with Inno Setup (packaging/windows/visualtc.iss).
#   macOS   : VisualTC.app (worker inside Contents/MacOS), DMG via macdeployqt.
#   Linux   : bin/VisualTC + bin/visualtc-worker, .desktop entry and icon;
#             DEB via CPack, AppImage via linuxdeploy (see docs/BUILD.md).
include(GNUInstallDirs)

if(APPLE)
    install(TARGETS VisualTC BUNDLE DESTINATION .)
else()
    install(TARGETS VisualTC visualtc-worker RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

if(UNIX AND NOT APPLE)
    install(FILES ${CMAKE_SOURCE_DIR}/packaging/linux/visualtc.desktop
            DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
    install(FILES ${CMAKE_SOURCE_DIR}/resources/icons/app.svg
            DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps
            RENAME visualtc.svg)
endif()

install(FILES ${CMAKE_SOURCE_DIR}/README.md ${CMAKE_SOURCE_DIR}/THIRD_PARTY_LICENSES.md ${CMAKE_SOURCE_DIR}/LICENSE
        DESTINATION ${CMAKE_INSTALL_DOCDIR} OPTIONAL)

set(CPACK_PACKAGE_NAME "visualtc")
set(CPACK_PACKAGE_VENDOR "VisualTC")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "VisualTC — DICOM Medical Image Viewer")
set(CPACK_PACKAGE_VERSION ${PROJECT_VERSION})
set(VISUALTC_PACKAGE_CONTACT "VisualTC Maintainers <maintainers@example.invalid>"
    CACHE STRING "Maintainer field of the Linux packages (set to the real distributor)")
set(CPACK_PACKAGE_CONTACT "${VISUALTC_PACKAGE_CONTACT}")
set(CPACK_PACKAGE_INSTALL_DIRECTORY "VisualTC")
set(CPACK_DEBIAN_PACKAGE_SECTION "science")
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
set(CPACK_DEBIAN_FILE_NAME "visualtc_${PROJECT_VERSION}_amd64.deb")
set(CPACK_GENERATOR "TGZ")
if(UNIX AND NOT APPLE)
    list(APPEND CPACK_GENERATOR "DEB")
endif()
include(CPack)
