include(RegexUtils)
test_escape_string_as_regex()

file(GLOB Eigen_directory_files "*")

escape_string_as_regex(ESCAPED_CMAKE_CURRENT_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")

foreach(f ${Eigen_directory_files})
  if(NOT f MATCHES "\\.txt" AND NOT f MATCHES "${ESCAPED_CMAKE_CURRENT_SOURCE_DIR}/[.].+" AND NOT f MATCHES "${ESCAPED_CMAKE_CURRENT_SOURCE_DIR}/src")
    list(APPEND Eigen_directory_files_to_install ${f})
  endif()
endforeach(f ${Eigen_directory_files})

install(FILES
  ${Eigen_directory_files_to_install}
  DESTINATION ${INCLUDE_INSTALL_DIR}/Eigen COMPONENT Devel
  )

foreach(LibName Eigen Eigen3)
    add_library(${LibName} INTERFACE)
    target_include_directories(${LibName} INTERFACE
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/..>
        $<INSTALL_INTERFACE:${INCLUDE_INSTALL_DIR}>)
    string(TOLOWER "${LibName}" libname)
    string(TOUPPER "${LibName}" LIBNAME)
    install(TARGETS ${LibName} EXPORT ${libname}-targets DESTINATION lib)
    install(EXPORT ${libname}-targets
        NAMESPACE :: DESTINATION lib/cmake/${LibName})
    set(hidedir "${CMAKE_CURRENT_BINARY_DIR}/hide")
    file(WRITE ${hidedir}/${libname}-config.cmake "
include(\"\${CMAKE_CURRENT_LIST_DIR}/${libname}-targets.cmake\")
if(TARGET ::${LibName})
    set(${LIBNAME}_FOUND 1)
    get_target_property(${LIBNAME}_INCLUDE_DIRS ::${LibName} INTERFACE_INCLUDE_DIRECTORIES)
    set(${LIBNAME}_VERSION \"${EIGEN_VERSION_NUMBER}\")
else()
    set(${LIBNAME}_FOUND 0)
endif()
        ")
    install(FILES "${hidedir}/${libname}-config.cmake"
        DESTINATION lib/cmake/${LibName})
endforeach()
  
install(DIRECTORY src DESTINATION ${INCLUDE_INSTALL_DIR}/Eigen COMPONENT Devel FILES_MATCHING PATTERN "*.h")
