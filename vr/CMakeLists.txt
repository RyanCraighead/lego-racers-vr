add_library(racers_vr STATIC)

target_include_directories(racers_vr PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(racers_vr PRIVATE SDL3::SDL3)

if(RACERS_OPENXR)
  if(NOT WIN32)
    message(FATAL_ERROR "RACERS_OPENXR currently requires desktop Windows and OpenGL 3.3")
  endif()

  # Keep the loader reproducible and independent of any machine-wide OpenXR SDK.
  set(BUILD_API_LAYERS OFF CACHE BOOL "Build OpenXR API layers" FORCE)
  set(BUILD_CONFORMANCE_TESTS OFF CACHE BOOL "Build OpenXR conformance tests" FORCE)
  set(BUILD_TESTS OFF CACHE BOOL "Build OpenXR tests" FORCE)
  set(BUILD_LOADER ON CACHE BOOL "Build the OpenXR loader" FORCE)
  # A static loader avoids an extra redistributable DLL and the SDK's Windows
  # post-build copy hooks, which conflict with this project's shared output dir.
  set(DYNAMIC_LOADER OFF CACHE BOOL "Build the OpenXR loader as a shared library" FORCE)
  FetchContent_Declare(
    openxr_sdk
    GIT_REPOSITORY https://github.com/KhronosGroup/OpenXR-SDK.git
    # Exact commit for the official release-1.1.60 tag.
    GIT_TAG 64f2b37c8c6da3d83c9b4d11865ba1fb752cb8ec
  )
  FetchContent_MakeAvailable(openxr_sdk)

  if(TARGET OpenXR::openxr_loader)
    set(RACERS_OPENXR_LOADER_TARGET OpenXR::openxr_loader)
  elseif(TARGET openxr_loader)
    set(RACERS_OPENXR_LOADER_TARGET openxr_loader)
  else()
    message(FATAL_ERROR "The pinned OpenXR-SDK did not provide an OpenXR loader target")
  endif()

  target_sources(racers_vr PRIVATE src/racers_vr_openxr.cpp)
  target_compile_definitions(racers_vr PRIVATE XR_USE_GRAPHICS_API_OPENGL XR_USE_PLATFORM_WIN32)
  target_link_libraries(racers_vr PRIVATE ${RACERS_OPENXR_LOADER_TARGET} opengl32)
else()
  target_sources(racers_vr PRIVATE src/racers_vr_stub.cpp)
endif()
