# Switch cross-compile toolchain wrapper for ps2-native-recomp.
if(DEFINED ENV{DEVKITPRO})
    file(TO_CMAKE_PATH "$ENV{DEVKITPRO}" DEVKITPRO)
elseif(WIN32 OR CMAKE_HOST_WIN32)
    set(DEVKITPRO "C:/devkitPro")
else()
    set(DEVKITPRO "/opt/devkitpro")
endif()
set(ENV{DEVKITPRO} "${DEVKITPRO}")

include("${DEVKITPRO}/cmake/Switch.cmake")
