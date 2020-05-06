
# NOTE(compnerd) Today regardless of whether or not ObjC interop is enabled,
# swift will use an autoreleased return value convention for certain CF
# functions (including some that are used/related to dispatch). This means that
# the swift compiler in callers to such functions will call the function, and
# then pass the result of the function to objc_retainAutoreleasedReturnValue. In
# a context where we have ObjC interop disabled, we do not have access to the
# objc runtime so an implementation of objc_retainAutoreleasedReturnValue is not
# available. To work around this, we provide a shim for
# objc_retainAutoreleasedReturnValue in DispatchStubs.cc that just calls retain
# on the object. Once we fix the swift compiler to switch to a different model
# for handling these arguments with objc-interop disabled these shims can be
# eliminated.
add_library(DispatchStubs STATIC
  DispatchStubs.cc)
target_include_directories(DispatchStubs PRIVATE
  ${PROJECT_SOURCE_DIR})
set_target_properties(DispatchStubs PROPERTIES
  POSITION_INDEPENDENT_CODE YES)

add_library(swiftDispatch
  Block.swift
  Data.swift
  Dispatch.swift
  IO.swift
  Private.swift
  Queue.swift
  Source.swift
  Time.swift
  Wrapper.swift)
target_compile_options(swiftDispatch PRIVATE
  "SHELL:-Xcc -fblocks"
  "SHELL:-Xcc -fmodule-map-file=${PROJECT_SOURCE_DIR}/dispatch/module.modulemap"
  "SHELL:-Xcc -I${PROJECT_SOURCE_DIR}")
set_target_properties(swiftDispatch PROPERTIES
  Swift_MODULE_NAME Dispatch
  Swift_MODULE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/swift
  INTERFACE_INCLUDE_DIRECTORIES ${CMAKE_CURRENT_BINARY_DIR}/swift)
target_link_libraries(swiftDispatch PRIVATE
  DispatchStubs
  BlocksRuntime::BlocksRuntime)
target_link_libraries(swiftDispatch PUBLIC
  dispatch)
add_dependencies(swiftDispatch module-maps)

get_swift_host_arch(swift_arch)
install(FILES
  ${CMAKE_CURRENT_BINARY_DIR}/swift/Dispatch.swiftmodule
  ${CMAKE_CURRENT_BINARY_DIR}/swift/Dispatch.swiftdoc
  DESTINATION ${INSTALL_TARGET_DIR}/${swift_arch})
set_property(GLOBAL APPEND PROPERTY DISPATCH_EXPORTS swiftDispatch)
install(TARGETS swiftDispatch
  EXPORT dispatchExports
  ARCHIVE DESTINATION ${INSTALL_TARGET_DIR}
  LIBRARY DESTINATION ${INSTALL_TARGET_DIR}
  RUNTIME DESTINATION bin)
if(NOT BUILD_SHARED_LIBS)
  set_property(GLOBAL APPEND PROPERTY DISPATCH_EXPORTS DispatchStubs)
  install(TARGETS DispatchStubs
    EXPORT dispatchExports
    DESTINATION ${INSTALL_TARGET_DIR})
elseif(NOT CMAKE_SYSTEM_NAME MATCHES "Darwin|Windows")
  target_link_options(swiftDispatch PRIVATE "SHELL:-no-toolchain-stdlib-rpath")
  set_target_properties(swiftDispatch PROPERTIES INSTALL_RPATH "$ORIGIN")
endif()
