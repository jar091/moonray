# Copyright 2023-2024 DreamWorks Animation LLC
# SPDX-License-Identifier: Apache-2.0

set(component application)

set(installIncludeDir ${PACKAGE_NAME}/application)
set(exportGroup ${PROJECT_NAME}Targets)

add_library(${component} SHARED "")
add_library(${PROJECT_NAME}::${component} ALIAS ${component})

# ChangeWatcher::CreateChangeWatcher() has one implementation per platform:
# FSEvents on macOS, inotify on Linux, ReadDirectoryChangesW on Windows. The
# historical `else(IsLinuxPlatform)` was decorative - it is a plain else - so
# Windows was being handed the inotify implementation.
if(IsDarwinPlatform)
    set(PlatformSpecificSources macos/ChangeWatcherImpl.mm)
elseif(IsWindowsPlatform)
    set(PlatformSpecificSources windows/ChangeWatcherImpl.cc)
else()
    set(PlatformSpecificSources linux/ChangeWatcherImpl.cc)
endif()

target_sources(${component}
    PRIVATE
        RaasApplication.cc
        RaasApplication.h
        ${PlatformSpecificSources}
)

set_property(TARGET ${component}
    PROPERTY PUBLIC_HEADER
        ChangeWatcher.h
        RaasApplication.h
)

target_include_directories(${component}
    PUBLIC
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_link_libraries(${component}
    PUBLIC
        SceneRdl2::common_except
        SceneRdl2::render_logging
        ${PROJECT_NAME}::rendering_rndr
)

if(IsDarwinPlatform)
    find_library(CoreServices CoreServices REQUIRED)
    target_link_libraries(${component} PUBLIC ${CoreServices})
endif()

# If at Dreamworks add a SConscript stub file so others can use this library.
SConscript_Stub(${component})

# Set standard compile/link options
Moonray_cxx_compile_definitions(${component})
Moonray_cxx_compile_features(${component})
Moonray_cxx_compile_options(${component})
Moonray_link_options(${component})

# -------------------------------------
# Install the target and the export set
# -------------------------------------
include(GNUInstallDirs)

# install the target
install(TARGETS ${component}
    COMPONENT ${component}
    EXPORT ${exportGroup}
    LIBRARY
        DESTINATION ${CMAKE_INSTALL_LIBDIR}
        NAMELINK_SKIP
    RUNTIME
        DESTINATION ${CMAKE_INSTALL_BINDIR}
    ARCHIVE
        DESTINATION ${CMAKE_INSTALL_LIBDIR}
    PUBLIC_HEADER
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/${installIncludeDir}
)

# # install the export set
# install(
#     EXPORT ${exportGroup}
#     NAMESPACE ${PROJECT_NAME}::
#     DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/${PROJECT_NAME}-${PROJECT_VERSION}
# )
