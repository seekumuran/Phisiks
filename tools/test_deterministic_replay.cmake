# ctest DeterministicReplay: runs PhisiksSamples headless in deterministic mode twice and requires the
# exported simulation data to be byte-identical; then records a short hidden-window run and
# requires the PNG frames to exist.
#   cmake -DLAB=<PhisiksSamples.exe> -DWORK=<dir> -P test_deterministic_replay.cmake   (cwd = repo root)

file(REMOVE_RECURSE "${WORK}")
foreach(run a b)
    execute_process(
        COMMAND "${LAB}" --scene PhysicsTower --headless --deterministic --max-frames 600
                --run-dir "${WORK}/${run}" --export-dir "${WORK}/${run}/export" --export-data
        RESULT_VARIABLE result)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "headless run ${run} failed (exit ${result})")
    endif()
endforeach()

file(READ "${WORK}/a/export/sim_data.csv" a)
file(READ "${WORK}/b/export/sim_data.csv" b)
string(LENGTH "${a}" bytes)
if(bytes LESS 100000)
    message(FATAL_ERROR "sim_data.csv is unexpectedly small (${bytes} bytes)")
endif()
if(NOT a STREQUAL b)
    message(FATAL_ERROR "deterministic runs differ: ${WORK}/a/export/sim_data.csv vs ${WORK}/b/export/sim_data.csv")
endif()
string(REGEX MATCHALL "\n" lines "${a}")
list(LENGTH lines rows)
message(STATUS "deterministic replay: 2 runs x 600 frames, ${rows} rows, ${bytes} bytes, identical")

# Every frame was recorded: the top tower box has rows for the first and the last game frame.
string(REGEX MATCH "\n1,[^\n]*Tower_15[^\n]*" first "${a}")
string(REGEX MATCH "\n599,[^\n]*Tower_15[^\n]*" last "${a}")
if(first STREQUAL "" OR last STREQUAL "")
    message(FATAL_ERROR "expected Tower_15 rows for frames 1 and 599")
endif()
# The bodies were simulated (positions change over the run), so equality is not trivially true.
string(REGEX REPLACE "^\n[0-9]+,[0-9]+,[0-9.]+," "" firstState "${first}")
string(REGEX REPLACE "^\n[0-9]+,[0-9]+,[0-9.]+," "" lastState "${last}")
if(firstState STREQUAL lastState)
    message(FATAL_ERROR "Tower_15 did not move between frame 1 and frame 599")
endif()

if(SKIP_RENDER)
    message(STATUS "hidden-window recording skipped (SKIP_RENDER)")
    return()
endif()
execute_process(
    COMMAND "${LAB}" --scene PhysicsTower --hidden --deterministic --max-frames 31
            --run-dir "${WORK}/frames" --export-dir "${WORK}/frames/export" --export-frames --export-every 10
    RESULT_VARIABLE result)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "hidden-window recording failed (exit ${result})")
endif()
foreach(frame 000010 000020 000030)
    if(NOT EXISTS "${WORK}/frames/export/frame_${frame}.png")
        message(FATAL_ERROR "missing exported frame frame_${frame}.png")
    endif()
endforeach()
message(STATUS "hidden-window recording: frames 10/20/30 exported")
