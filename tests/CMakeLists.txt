# Automated tests (ANWEISUNG.md section 14). Buildable without a headset.
add_executable(test_protocol test_protocol.cpp)
target_link_libraries(test_protocol PRIVATE fearvr-common fearvr-warnings)
add_test(NAME protocol COMMAND test_protocol)

add_executable(test_xr_session_state test_xr_session_state.cpp)
target_link_libraries(test_xr_session_state PRIVATE fearvr-common fearvr-warnings)
add_test(NAME xr_session_state COMMAND test_xr_session_state)

add_executable(test_stereo_math test_stereo_math.cpp)
target_link_libraries(test_stereo_math PRIVATE fearvr-common fearvr-warnings)
add_test(NAME stereo_math COMMAND test_stereo_math)

add_executable(test_head_tracking_math test_head_tracking_math.cpp)
target_link_libraries(
  test_head_tracking_math PRIVATE fearvr-common fearvr-warnings
)
add_test(NAME head_tracking_math COMMAND test_head_tracking_math)

add_executable(test_stereo_hud_math test_stereo_hud_math.cpp)
target_link_libraries(
  test_stereo_hud_math PRIVATE fearvr-common fearvr-warnings
)
add_test(NAME stereo_hud_math COMMAND test_stereo_hud_math)

add_executable(test_input_state test_input_state.cpp)
target_link_libraries(
  test_input_state PRIVATE fearvr-common fearvr-warnings
)
add_test(NAME input_state COMMAND test_input_state)

add_executable(test_controller_mapping test_controller_mapping.cpp)
target_link_libraries(
  test_controller_mapping PRIVATE fearvr-common fearvr-warnings
)
# This test checks only through assert; RelWithDebInfo otherwise defines
# NDEBUG and would make the test ineffective.
if(MSVC)
  target_compile_options(test_controller_mapping PRIVATE /UNDEBUG)
endif()
add_test(NAME controller_mapping COMMAND test_controller_mapping)

add_executable(test_two_handed_grip test_two_handed_grip.cpp)
target_link_libraries(
  test_two_handed_grip PRIVATE fearvr-common fearvr-warnings
)
if(MSVC)
  target_compile_options(test_two_handed_grip PRIVATE /UNDEBUG)
endif()
add_test(NAME two_handed_grip COMMAND test_two_handed_grip)

add_executable(test_weapon_weight test_weapon_weight.cpp)
target_link_libraries(
  test_weapon_weight PRIVATE fearvr-common fearvr-warnings
)
if(MSVC)
  target_compile_options(test_weapon_weight PRIVATE /UNDEBUG)
endif()
add_test(NAME weapon_weight COMMAND test_weapon_weight)

add_executable(test_melee_thrust test_melee_thrust.cpp)
target_link_libraries(
  test_melee_thrust PRIVATE fearvr-common fearvr-warnings
)
if(MSVC)
  target_compile_options(test_melee_thrust PRIVATE /UNDEBUG)
endif()
add_test(NAME melee_thrust COMMAND test_melee_thrust)

add_executable(test_melee_actions test_melee_actions.cpp)
target_link_libraries(
  test_melee_actions PRIVATE fearvr-common fearvr-warnings
)
if(MSVC)
  target_compile_options(test_melee_actions PRIVATE /UNDEBUG)
endif()
add_test(NAME melee_actions COMMAND test_melee_actions)

add_executable(test_climb_grip test_climb_grip.cpp)
target_link_libraries(
  test_climb_grip PRIVATE fearvr-common fearvr-warnings
)
if(MSVC)
  target_compile_options(test_climb_grip PRIVATE /UNDEBUG)
endif()
add_test(NAME climb_grip COMMAND test_climb_grip)

add_executable(test_lean_collision test_lean_collision.cpp)
target_link_libraries(
  test_lean_collision PRIVATE fearvr-common fearvr-warnings
)
if(MSVC)
  target_compile_options(test_lean_collision PRIVATE /UNDEBUG)
endif()
add_test(NAME lean_collision COMMAND test_lean_collision)

add_executable(test_lean_body test_lean_body.cpp)
target_link_libraries(
  test_lean_body PRIVATE fearvr-common fearvr-warnings
)
if(MSVC)
  target_compile_options(test_lean_body PRIVATE /UNDEBUG)
endif()
add_test(NAME lean_body COMMAND test_lean_body)

add_executable(test_vr_menu_model test_vr_menu_model.cpp)
target_link_libraries(
  test_vr_menu_model PRIVATE fearvr-common fearvr-warnings
)
if(MSVC)
  target_compile_options(test_vr_menu_model PRIVATE /UNDEBUG)
endif()
add_test(NAME vr_menu_model COMMAND test_vr_menu_model)

add_executable(test_render_scale test_render_scale.cpp)
target_link_libraries(
  test_render_scale PRIVATE fearvr-common fearvr-warnings
)
if(MSVC)
  target_compile_options(test_render_scale PRIVATE /UNDEBUG)
endif()
add_test(NAME render_scale COMMAND test_render_scale)

if(CMAKE_SIZEOF_VOID_P EQUAL 4 AND TARGET fearvr-proxy)
  # Isolated M2 producer: the system import remains d3d9.lib. A dependency
  # target refreshes the app-local proxy before every producer build.
  add_executable(d3d9_test_producer d3d9_test_producer.cpp)
  target_link_libraries(d3d9_test_producer PRIVATE
    fearvr-warnings
    d3d9
  )
  add_custom_target(stage_d3d9_test_proxy
    COMMAND ${CMAKE_COMMAND} -E make_directory
      "$<TARGET_FILE_DIR:d3d9_test_producer>"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      "$<TARGET_FILE:fearvr-proxy>"
      "$<TARGET_FILE_DIR:d3d9_test_producer>/d3d9.dll"
    DEPENDS fearvr-proxy
    COMMENT "Staging M2 d3d9.dll beside the isolated test producer"
  )
  add_dependencies(d3d9_test_producer stage_d3d9_test_proxy)
  add_test(
    NAME d3d9ex_compat_smoke
    COMMAND d3d9_test_producer
      -fearvr-d3d9ex-compat
      --compat-smoke
  )
  set_tests_properties(
    d3d9ex_compat_smoke PROPERTIES LABELS "d3d9;gpu;compatibility"
  )
  add_test(
    NAME d3d9ex_compat_present_ex_smoke
    COMMAND d3d9_test_producer
      -fearvr-d3d9ex-compat
      --compat-smoke
      --present-ex
  )
  set_tests_properties(
    d3d9ex_compat_present_ex_smoke
    PROPERTIES LABELS "d3d9;gpu;compatibility"
  )
  add_test(
    NAME d3d9ex_compat_swapchain_present_smoke
    COMMAND d3d9_test_producer
      -fearvr-d3d9ex-compat
      --compat-smoke
      --swapchain-present
  )
  set_tests_properties(
    d3d9ex_compat_swapchain_present_smoke
    PROPERTIES LABELS "d3d9;gpu;compatibility"
  )

  # A separate output directory without a local d3d9.dll proxy simulates the
  # archcfg loader loading the uniquely named bridge and patching the EXE IAT
  # after Windows has already imported the system d3d9 library.
  add_executable(iat_hook_test iat_hook_test.cpp)
  target_link_libraries(iat_hook_test PRIVATE fearvr-warnings d3d9)
  add_dependencies(iat_hook_test fearvr-proxy)
  set_target_properties(iat_hook_test PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/iat-test"
  )
  add_test(NAME d3d9_iat_hook
    COMMAND iat_hook_test "$<TARGET_FILE:fearvr-proxy>")
  set_tests_properties(d3d9_iat_hook PROPERTIES LABELS "d3d9;gpu;iat")

  # Simulates Retail's early app-local proxy load, then initializes the
  # archcfg GameClient loader. The loader must reuse d3d9.dll instead of
  # loading fearvr-d3d9.dll as a second bridge instance.
  add_executable(gameclient_bridge_reuse_test
    gameclient_bridge_reuse_test.cpp
  )
  target_link_libraries(gameclient_bridge_reuse_test PRIVATE
    fearvr-warnings
    d3d9
  )
  set_target_properties(gameclient_bridge_reuse_test PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bridge-reuse-test"
  )
  add_custom_target(stage_gameclient_bridge_reuse
    COMMAND ${CMAKE_COMMAND} -E make_directory
      "$<TARGET_FILE_DIR:gameclient_bridge_reuse_test>"
    COMMAND ${CMAKE_COMMAND} -E make_directory
      "$<TARGET_FILE_DIR:gameclient_bridge_reuse_test>/modules"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      "$<TARGET_FILE:fearvr-proxy>"
      "$<TARGET_FILE_DIR:gameclient_bridge_reuse_test>/d3d9.dll"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      "$<TARGET_FILE:fearvr-proxy>"
      "$<TARGET_FILE_DIR:gameclient_bridge_reuse_test>/modules/fearvr-d3d9.dll"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      "$<TARGET_FILE:fearvr-gameclient-loader>"
      "$<TARGET_FILE_DIR:gameclient_bridge_reuse_test>/modules/GameClient.dll"
    DEPENDS
      fearvr-proxy
      fearvr-gameclient-loader
  )
  add_dependencies(gameclient_bridge_reuse_test
    stage_gameclient_bridge_reuse
  )
  add_test(NAME gameclient_bridge_reuse
    COMMAND gameclient_bridge_reuse_test)
endif()

# Additional test targets follow by milestone:
#   - test_math       (quaternion, axis mapping, pose, FOV-to-projection)
#   - test_ringbuffer (generations and timeout paths)
