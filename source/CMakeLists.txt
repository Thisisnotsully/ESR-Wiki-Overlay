cmake_minimum_required(VERSION 3.29)
project(ESRWikiOverlay VERSION 1.1.2 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

# PluginSDK-master lives beside Downloads/Work in the original setup; override if not.
set(ESR_PLUGIN_SDK_INCLUDE "${CMAKE_CURRENT_SOURCE_DIR}/../../../PluginSDK-master/include"
    CACHE PATH "PluginSDK include folder (contains D2RLPlugin/api.h)")
option(ESR_WIKI_BUILD_TESTS "Build the portable model tests and register them with CTest" ON)

# Portable model: parser, search, paging, color codes. Builds on any host.
add_library(esr_wiki_model STATIC src/wiki_model.cpp)
target_include_directories(esr_wiki_model PUBLIC src)
if(NOT MSVC)
  target_compile_options(esr_wiki_model PRIVATE -Wall -Wextra -Werror)
else()
  # Every MSVC target must use the same C runtime, or the link fails (LNK2038).
  # The item database plugin uses the static runtime, so match it here.
  set_property(TARGET esr_wiki_model PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

if(ESR_WIKI_BUILD_TESTS)
  enable_testing()
  add_executable(esr_wiki_model_test tests/wiki_model_test.cpp)
  target_link_libraries(esr_wiki_model_test PRIVATE esr_wiki_model)
  if(NOT MSVC)
    target_compile_options(esr_wiki_model_test PRIVATE -Wall -Wextra -Werror)
  else()
    set_property(TARGET esr_wiki_model_test PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
  endif()
  add_test(NAME wiki_model
           COMMAND esr_wiki_model_test "${CMAKE_CURRENT_SOURCE_DIR}/data/wiki-data.txt")
endif()

# Win32 overlay and D2RLoader plugin (Windows only).
if(WIN32)
  enable_language(RC)
  add_library(esr_wiki_overlay SHARED src/plugin.cpp src/overlay.cpp src/plugin.rc)
  target_include_directories(esr_wiki_overlay PRIVATE "${ESR_PLUGIN_SDK_INCLUDE}")
  target_link_libraries(esr_wiki_overlay PRIVATE esr_wiki_model user32 gdi32 gdiplus msimg32)
  set_target_properties(esr_wiki_overlay PROPERTIES OUTPUT_NAME "d2rl-esr-wiki-overlay" PREFIX "")

  # After each successful build, copy the DLL and data into the live mod (see deploy.cmake).
  # Turn off with -DESR_WIKI_DEPLOY_LIVE=OFF, or point elsewhere with -DESR_WIKI_LIVE_PLUGINS_DIR=...
  option(ESR_WIKI_DEPLOY_LIVE "Copy the built overlay into the live mod after each build" ON)
  set(ESR_WIKI_LIVE_PLUGINS_DIR
      "C:/Program Files (x86)/Diablo II Resurrected/mods/Eastern_Sun_Resurrected/d2rloader/plugins"
      CACHE PATH "Live D2RLoader plugins folder for the ESR mod")
  if(ESR_WIKI_DEPLOY_LIVE)
    add_custom_command(TARGET esr_wiki_overlay POST_BUILD
      COMMAND "${CMAKE_COMMAND}"
              "-DSRC_DLL=$<TARGET_FILE:esr_wiki_overlay>"
              "-DSRC_DATA=${CMAKE_CURRENT_SOURCE_DIR}/data"
              "-DDEST=${ESR_WIKI_LIVE_PLUGINS_DIR}"
              -P "${CMAKE_CURRENT_SOURCE_DIR}/deploy.cmake"
      VERBATIM)
  endif()
  if(MSVC)
    target_compile_options(esr_wiki_overlay PRIVATE /W4 /permissive- /utf-8)
    set_property(TARGET esr_wiki_overlay PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
  endif()

  # Install layout matches the D2RLoader plugins folder:
  #   plugins/d2rl-esr-wiki-overlay.dll
  #   plugins/esr-wiki-overlay/wiki-data.txt
  #   plugins/esr-wiki-overlay/images/*.png
  install(TARGETS esr_wiki_overlay RUNTIME DESTINATION d2rloader/plugins)
  install(FILES data/wiki-data.txt DESTINATION d2rloader/plugins/esr-wiki-overlay)
  install(FILES data/cube-wiki-data.txt DESTINATION d2rloader/plugins/esr-wiki-overlay)
  install(DIRECTORY data/images/ DESTINATION d2rloader/plugins/esr-wiki-overlay/images)
endif()
