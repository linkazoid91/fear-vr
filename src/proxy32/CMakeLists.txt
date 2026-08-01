# x86 D3D9 proxy/bridge — ANWEISUNG.md §5.2
#
# Build one bridge binary under the unique staged name fearvr-d3d9.dll.
# Release installation also deploys the same binary as a hash-tracked
# app-local d3d9.dll beside FEAR.exe for early interception. Isolated tests
# stage their own private d3d9.dll copies.
set(FEARVR_MINHOOK_ROOT
  "${CMAKE_SOURCE_DIR}/vendor-local/minhook"
  CACHE PATH "Local pinned checkout of MinHook")
if(NOT EXISTS "${FEARVR_MINHOOK_ROOT}/CMakeLists.txt")
  message(FATAL_ERROR
    "MinHook missing at ${FEARVR_MINHOOK_ROOT}. "
    "Run tools/prepare-dependencies.ps1 first.")
endif()
add_subdirectory(
  "${FEARVR_MINHOOK_ROOT}"
  "${CMAKE_BINARY_DIR}/third_party/minhook"
  EXCLUDE_FROM_ALL
)

add_library(fearvr-proxy SHARED
  bridge.cpp
  bridge.h
  d3d9ex_compat.cpp
  d3d9ex_compat.h
  d3d9.def
  dllmain.cpp
  iat_hook.cpp
  iat_hook.h
  proxy_exports.cpp
  system_d3d9.cpp
  system_d3d9.h
)
target_link_libraries(fearvr-proxy PRIVATE
  fearvr-common
  fearvr-version
  fearvr-warnings
  minhook
  dxgi
  shell32
)

# Keep the unique name in the archcfg stage. Release installation and isolated
# tests deliberately copy the same binary as d3d9.dll at their own targets.
set_target_properties(fearvr-proxy PROPERTIES
  OUTPUT_NAME "fearvr-d3d9"
  PREFIX ""
)
