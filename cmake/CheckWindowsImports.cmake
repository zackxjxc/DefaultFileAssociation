# Catch known Windows 8+ imports, including the v145 filesystem regression.
# This is a regression guard; testing on Windows 7 is still required.
execute_process(
    COMMAND "${DUMPBIN}" /imports "${EXECUTABLE}"
    RESULT_VARIABLE result
    OUTPUT_VARIABLE imports
    ERROR_VARIABLE error
)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "Cannot inspect executable imports: ${error}")
endif()

foreach(api IN ITEMS CreateFile2 CopyFile2 GetTempPath2W GetTempPath2A
        GetSystemTimePreciseAsFileTime WaitOnAddress WakeByAddressSingle
        WakeByAddressAll SetThreadDescription GetThreadDescription)
    if(imports MATCHES "[ \t]${api}[\r\n]")
        message(FATAL_ERROR "${api} is incompatible with Windows 7: ${EXECUTABLE}")
    endif()
endforeach()
message(STATUS "Windows API import regression check passed")
