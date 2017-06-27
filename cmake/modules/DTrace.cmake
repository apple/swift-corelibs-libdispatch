MACRO (DTRACE_HEADER provider header)
  if (DTRACE)
    ADD_CUSTOM_COMMAND(
      OUTPUT  ${header}
      COMMAND ${DTRACE} -h -s ${provider} -o ${header}
      DEPENDS ${provider}
    )
  ENDIF()
ENDMACRO()

