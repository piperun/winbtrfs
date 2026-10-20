execute_process(
    COMMAND "${NM}" -u "${OBJECT}"
    RESULT_VARIABLE result
    OUTPUT_VARIABLE undefined
    ERROR_VARIABLE error
    OUTPUT_STRIP_TRAILING_WHITESPACE)

if(NOT result EQUAL 0 OR NOT undefined STREQUAL "")
    file(REMOVE "${OBJECT}")
    message(FATAL_ERROR "Rust range object has unresolved symbols: ${undefined} ${error}")
endif()
