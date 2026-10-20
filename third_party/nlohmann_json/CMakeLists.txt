# Application-owned dependency, independent of the SDK's private inja headers.
add_library(narutorise_json INTERFACE)
add_library(narutorise::json ALIAS narutorise_json)
target_include_directories(narutorise_json INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/include")
