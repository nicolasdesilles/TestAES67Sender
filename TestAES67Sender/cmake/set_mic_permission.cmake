if (NOT DEFINED PLIST_PATH OR PLIST_PATH STREQUAL "")
    message(FATAL_ERROR "Missing PLIST_PATH (expected -DPLIST_PATH=/path/to/Info.plist)")
endif()

set(plist_path "${PLIST_PATH}")
set(desc "TestAES67Sender needs microphone access for audio input")
set(local_network_desc "TestAES67Sender needs local network access for NMOS/Bonjour discovery and PTP operation")

if (NOT EXISTS "${plist_path}")
    message(STATUS "Info.plist not found (skipping mic permission): ${plist_path}")
    return()
endif()

execute_process(
    COMMAND /usr/libexec/PlistBuddy -c "Set :NSMicrophoneUsageDescription ${desc}" "${plist_path}"
    RESULT_VARIABLE set_result
    OUTPUT_QUIET
    ERROR_QUIET
)

if (NOT set_result EQUAL 0)
    execute_process(
        COMMAND /usr/libexec/PlistBuddy -c "Add :NSMicrophoneUsageDescription string ${desc}" "${plist_path}"
        RESULT_VARIABLE add_result
        OUTPUT_QUIET
        ERROR_VARIABLE add_err
    )

    if (NOT add_result EQUAL 0)
        message(FATAL_ERROR "Failed to set NSMicrophoneUsageDescription in ${plist_path}: ${add_err}")
    endif()
endif()

message(STATUS "NSMicrophoneUsageDescription set in ${plist_path}")

# --- Local Network privacy prompt (macOS 11+) ---
execute_process(
    COMMAND /usr/libexec/PlistBuddy -c "Set :NSLocalNetworkUsageDescription ${local_network_desc}" "${plist_path}"
    RESULT_VARIABLE ln_set_result
    OUTPUT_QUIET
    ERROR_QUIET
)

if (NOT ln_set_result EQUAL 0)
    execute_process(
        COMMAND /usr/libexec/PlistBuddy -c "Add :NSLocalNetworkUsageDescription string ${local_network_desc}" "${plist_path}"
        RESULT_VARIABLE ln_add_result
        OUTPUT_QUIET
        ERROR_VARIABLE ln_add_err
    )
    if (NOT ln_add_result EQUAL 0)
        message(FATAL_ERROR "Failed to set NSLocalNetworkUsageDescription in ${plist_path}: ${ln_add_err}")
    endif()
endif()

# --- Bonjour service types used by ravennakit (for Local Network privacy) ---
# We recreate the array each time for determinism.
execute_process(COMMAND /usr/libexec/PlistBuddy -c "Delete :NSBonjourServices" "${plist_path}"
                RESULT_VARIABLE bs_del_result OUTPUT_QUIET ERROR_QUIET)
execute_process(COMMAND /usr/libexec/PlistBuddy -c "Add :NSBonjourServices array" "${plist_path}"
                RESULT_VARIABLE bs_add_result OUTPUT_QUIET ERROR_VARIABLE bs_add_err)
if (NOT bs_add_result EQUAL 0)
    message(FATAL_ERROR "Failed to create NSBonjourServices array in ${plist_path}: ${bs_add_err}")
endif()

set(bonjour_services
    "_nmos-register._tcp"
    "_nmos-registration._tcp"
    "_rtsp._tcp,_ravenna"
    "_rtsp._tcp,_ravenna_session"
)

set(idx 0)
foreach(svc IN LISTS bonjour_services)
    execute_process(
        COMMAND /usr/libexec/PlistBuddy -c "Add :NSBonjourServices:${idx} string ${svc}" "${plist_path}"
        RESULT_VARIABLE svc_add_result
        OUTPUT_QUIET
        ERROR_VARIABLE svc_add_err
    )
    if (NOT svc_add_result EQUAL 0)
        message(FATAL_ERROR "Failed adding NSBonjourServices item ${idx} (${svc}) in ${plist_path}: ${svc_add_err}")
    endif()
    math(EXPR idx "${idx}+1")
endforeach()

message(STATUS "NSLocalNetworkUsageDescription and NSBonjourServices set in ${plist_path}")

