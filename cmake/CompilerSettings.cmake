# Shared compiler settings for every VisualTC target.
add_library(visualtc_options INTERFACE)
add_library(VisualTC::options ALIAS visualtc_options)

if(MSVC)
    # /utf-8 is mandatory: sources contain pt-BR strings in UTF-8.
    target_compile_options(visualtc_options INTERFACE /W4 /permissive- /utf-8 /Zc:__cplusplus /EHsc /MP)
    target_compile_definitions(visualtc_options INTERFACE NOMINMAX _USE_MATH_DEFINES WIN32_LEAN_AND_MEAN)
    if(VISUALTC_WARNINGS_AS_ERRORS)
        target_compile_options(visualtc_options INTERFACE /WX)
    endif()
else()
    target_compile_options(visualtc_options INTERFACE
        -Wall -Wextra -Wpedantic -Wshadow -Wnon-virtual-dtor -Wcast-align -Woverloaded-virtual
        -Wdouble-promotion -Wformat=2 -Wimplicit-fallthrough)
    if(VISUALTC_WARNINGS_AS_ERRORS)
        target_compile_options(visualtc_options INTERFACE -Werror)
    endif()
    if(VISUALTC_ENABLE_SANITIZERS)
        target_compile_options(visualtc_options INTERFACE -fsanitize=address,undefined -fno-omit-frame-pointer
                                                          -fno-sanitize-recover=undefined)
        target_link_options(visualtc_options INTERFACE -fsanitize=address,undefined)
    endif()
endif()
