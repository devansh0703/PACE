#----------------------------------------------------------------
# Generated CMake target import file for configuration "Release".
#----------------------------------------------------------------

# Commands may need to know the format version.
set(CMAKE_IMPORT_FILE_VERSION 1)

# Import target "pace::pace" for configuration "Release"
set_property(TARGET pace::pace APPEND PROPERTY IMPORTED_CONFIGURATIONS RELEASE)
set_target_properties(pace::pace PROPERTIES
  IMPORTED_LINK_INTERFACE_LANGUAGES_RELEASE "C"
  IMPORTED_LOCATION_RELEASE "${_IMPORT_PREFIX}/lib/libpace.a"
  )

list(APPEND _cmake_import_check_targets pace::pace )
list(APPEND _cmake_import_check_files_for_pace::pace "${_IMPORT_PREFIX}/lib/libpace.a" )

# Commands beyond this point should not need to know the version.
set(CMAKE_IMPORT_FILE_VERSION)
