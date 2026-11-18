cmake_minimum_required(VERSION 3.24)
execute_process(COMMAND "${DOCUMENT}" --headless "${BINARY}/document-check.png"
 COMMAND_ERROR_IS_FATAL ANY)
execute_process(COMMAND "${TESTS}" "${BINARY}/document-dark.png" "${BINARY}/document-light.png"
 COMMAND_ERROR_IS_FATAL ANY)
