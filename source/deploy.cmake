# Copies the built overlay into the live D2RLoader plugins folder.
# Run by CMake after each link (see CMakeLists.txt), or by hand:
#   cmake -DSRC_DLL=<dll> -DSRC_DATA=<overlay>/data -DDEST=<plugins folder> -P deploy.cmake
#
# Never fails the build: problems are reported as warnings, so a running game
# or a read-only Program Files folder does not stop compilation.

if(NOT EXISTS "${DEST}")
  message(WARNING "ESR Wiki overlay: live plugins folder not found, nothing deployed: ${DEST}")
  return()
endif()

set(_dest_data "${DEST}/esr-wiki-overlay")
set(_dest_images "${_dest_data}/images")
file(MAKE_DIRECTORY "${_dest_images}")

file(COPY_FILE "${SRC_DLL}" "${DEST}/d2rl-esr-wiki-overlay.dll"
     RESULT _result ONLY_IF_DIFFERENT)
if(NOT _result EQUAL 0)
  message(WARNING "ESR Wiki overlay: could not copy the DLL to ${DEST}. "
                  "Close Diablo II, or run the build from an Administrator shell. (${_result})")
  return()
endif()

file(COPY_FILE "${SRC_DATA}/wiki-data.txt" "${_dest_data}/wiki-data.txt"
     RESULT _result ONLY_IF_DIFFERENT)
if(NOT _result EQUAL 0)
  message(WARNING "ESR Wiki overlay: could not copy wiki-data.txt to ${_dest_data}.")
endif()

file(COPY_FILE "${SRC_DATA}/cube-wiki-data.txt" "${_dest_data}/cube-wiki-data.txt"
     RESULT _result ONLY_IF_DIFFERENT)
if(NOT _result EQUAL 0)
  message(WARNING "ESR Wiki overlay: could not copy cube-wiki-data.txt to ${_dest_data}.")
endif()

file(GLOB _pngs "${SRC_DATA}/images/*.png")
set(_copied 0)
foreach(_png IN LISTS _pngs)
  get_filename_component(_name "${_png}" NAME)
  file(COPY_FILE "${_png}" "${_dest_images}/${_name}" RESULT _result ONLY_IF_DIFFERENT)
  if(_result EQUAL 0)
    math(EXPR _copied "${_copied} + 1")
  endif()
endforeach()

message(STATUS "ESR Wiki overlay deployed to ${DEST} (DLL, wiki-data.txt, ${_copied} images)")
