cmake_minimum_required(VERSION 3.22)
find_package(Git REQUIRED)

function(apply_patch directory patch)
    execute_process(COMMAND "${GIT_EXECUTABLE}" apply --reverse --check "${patch}"
        WORKING_DIRECTORY "${directory}" RESULT_VARIABLE already_applied
        OUTPUT_QUIET ERROR_QUIET)
    if(already_applied EQUAL 0)
        return()
    endif()
    execute_process(COMMAND "${GIT_EXECUTABLE}" apply --check "${patch}"
        WORKING_DIRECTORY "${directory}" COMMAND_ERROR_IS_FATAL ANY)
    execute_process(COMMAND "${GIT_EXECUTABLE}" apply "${patch}"
        WORKING_DIRECTORY "${directory}" COMMAND_ERROR_IS_FATAL ANY)
    execute_process(COMMAND "${GIT_EXECUTABLE}" apply --reverse --check "${patch}"
        WORKING_DIRECTORY "${directory}" COMMAND_ERROR_IS_FATAL ANY)
endfunction()

apply_patch("${DYNARMIC_SOURCE_DIR}" "${CMAKE_CURRENT_LIST_DIR}/dynarmic_bridge.patch")
apply_patch("${DYNARMIC_SOURCE_DIR}/externals/oaknut"
    "${CMAKE_CURRENT_LIST_DIR}/oaknut_94c726ce_bridge.patch")
