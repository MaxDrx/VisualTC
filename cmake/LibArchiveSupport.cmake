# libarchive: reading of compressed exams (ZIP, 7z, RAR, TAR.*, GZ, ISO 9660).
#
# With vcpkg, the port's CMake wrapper adds the static dependencies to
# LibArchive::LibArchive. A static libarchive built from source
# (scripts/build_deps_linux.sh) does not describe its dependencies, so they
# are added here.
find_package(LibArchive 3.6 REQUIRED)

if(NOT DEFINED VCPKG_TOOLCHAIN AND LibArchive_LIBRARIES MATCHES "\\.(a|lib)$")
    set(_vtc_archive_deps "")
    find_package(ZLIB REQUIRED)
    list(APPEND _vtc_archive_deps ZLIB::ZLIB)
    find_package(BZip2 QUIET)
    if(BZip2_FOUND)
        list(APPEND _vtc_archive_deps BZip2::BZip2)
    endif()
    find_package(LibLZMA QUIET)
    if(LibLZMA_FOUND)
        list(APPEND _vtc_archive_deps LibLZMA::LibLZMA)
    endif()
    find_package(zstd CONFIG QUIET)
    if(TARGET zstd::libzstd_static)
        list(APPEND _vtc_archive_deps zstd::libzstd_static)
    elseif(TARGET zstd::libzstd_shared)
        list(APPEND _vtc_archive_deps zstd::libzstd_shared)
    endif()
    if(NOT APPLE AND NOT WIN32)
        find_package(OpenSSL QUIET COMPONENTS Crypto)
        if(OpenSSL_FOUND)
            list(APPEND _vtc_archive_deps OpenSSL::Crypto)
        endif()
    endif()
    if(WIN32)
        list(APPEND _vtc_archive_deps bcrypt)
    endif()
    set_property(TARGET LibArchive::LibArchive APPEND PROPERTY INTERFACE_LINK_LIBRARIES ${_vtc_archive_deps})
    set_property(TARGET LibArchive::LibArchive APPEND PROPERTY INTERFACE_COMPILE_DEFINITIONS LIBARCHIVE_STATIC)
endif()

message(STATUS "libarchive ${LibArchive_VERSION}: ${LibArchive_LIBRARIES}")
