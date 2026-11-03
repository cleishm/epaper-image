# Shared by the panel viewer firmwares (../firmware-e1004, ../firmware-ee05).
# Include from a project's top-level CMakeLists.txt before project.cmake.
#
# main/idf_component.yml takes the idfxx components from the component registry.
# If IDFXX_PATH is set, each one it lists comes from that checkout instead:
# EXTRA_COMPONENT_DIRS take priority over managed components, and each
# component's own manifest points at its idfxx siblings by relative
# override_path, so the whole chain comes from the same checkout.
if(DEFINED ENV{IDFXX_PATH} AND NOT "$ENV{IDFXX_PATH}" STREQUAL "")
    file(REAL_PATH "$ENV{IDFXX_PATH}" _idfxx_path EXPAND_TILDE)
    file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/main/idf_component.yml" _idfxx_deps
         REGEX "cleishm/idfxx_")
    if("${_idfxx_deps}" STREQUAL "")
        message(FATAL_ERROR "main/idf_component.yml lists no cleishm/idfxx_* dependencies.")
    endif()
    foreach(_idfxx_dep IN LISTS _idfxx_deps)
        if(NOT "${_idfxx_dep}" MATCHES "^  cleishm/(idfxx_[a-z0-9_]+):")
            message(FATAL_ERROR
                "Cannot parse the idfxx dependency line '${_idfxx_dep}' in main/idf_component.yml "
                "(expected a two-space indent and an unquoted key).")
        endif()
        set(_idfxx_name "${CMAKE_MATCH_1}")
        set(_idfxx_dir "${_idfxx_path}/components/${_idfxx_name}")
        if(NOT EXISTS "${_idfxx_dir}/idf_component.yml")
            message(FATAL_ERROR
                "IDFXX_PATH is '${_idfxx_path}', but it has no ${_idfxx_name} component. "
                "Set IDFXX_PATH to an idfxx checkout, or unset it to use the component registry.")
        endif()
        list(APPEND EXTRA_COMPONENT_DIRS "${_idfxx_dir}")
    endforeach()
    message(STATUS "idfxx: local checkout at ${_idfxx_path}")
else()
    message(STATUS "idfxx: component registry (set IDFXX_PATH to use a local checkout)")
endif()

# The shared wire-protocol component (components/epi_protocol).
list(APPEND EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../components")
