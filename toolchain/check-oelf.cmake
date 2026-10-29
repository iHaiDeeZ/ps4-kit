# cmake -DOELF=<file.oelf> -P check-oelf.cmake
#
# Fails if the OELF made by create-fself doesn't have exactly one read-write PT_LOAD. PacBrew's
# create-fself sometimes duplicates the data segment (see ps4-kit.cmake
# ps4_kit_add_eboot), and the console then refuses to start the app with CE-41839-5.
# Also where a build keeps its own LLVM (SMW's SMW_LLVM, here PS4_LLVM) and OpenOrbis' tools.
set(_hints)
foreach (_var PS4_LLVM OPENORBIS)
    if (DEFINED ENV{${_var}})
        list(APPEND _hints "$ENV{${_var}}/bin")
    endif ()
endforeach ()
set(_names llvm-readelf)
if (DEFINED ENV{CLANG_VER})
    list(APPEND _names llvm-readelf-$ENV{CLANG_VER})
endif ()
find_program(READELF NAMES ${_names} llvm-readelf-21 llvm-readelf-12 readelf
    HINTS ${_hints} /opt/pacbrew/ps4/openorbis/bin)
if (NOT READELF)
    message(WARNING "check-oelf: no readelf found, skipping the segment check")
    return()
endif ()
execute_process(COMMAND ${READELF} -l -W ${OELF} OUTPUT_VARIABLE out RESULT_VARIABLE rc)
if (NOT rc EQUAL 0)
    message(FATAL_ERROR "check-oelf: ${READELF} failed on ${OELF}")
endif ()
string(REGEX MATCHALL "\n *LOAD +[^\n]* RW  [^\n]*" rw_loads "${out}")
list(LENGTH rw_loads count)
if (NOT count EQUAL 1)
    message(FATAL_ERROR "check-oelf: ${OELF} has ${count} read-write PT_LOAD segments (expected 1); "
        "the console will refuse to start it. create-fself bug, see ps4-kit.cmake ps4_kit_add_eboot.\n${out}")
endif ()
message(STATUS "check-oelf: ${OELF}: segments OK")
