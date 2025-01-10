option(USE_DUCKDB "Enable support for DuckDB" OFF)
option(USE_MYSQL "Enable support for MySQL" ON)
SET(BINARY_NAME "pstress")
ADD_SUBDIRECTORY(third_party)
INCLUDE_DIRECTORIES(third_party/inih++/include third_party/rapidjson/include)

IF (USE_MYSQL AND MYSQL_FOUND)
  MESSAGE(STATUS "Configuring for MySQL...")
  IF (MARIADB)
    INCLUDE_DIRECTORIES( ${MYSQL_INCLUDE_DIR} ${MYSQL_INCLUDE_DIR}/.. )
  ELSE()
    INCLUDE_DIRECTORIES( ${MYSQL_INCLUDE_DIR} )
  ENDIF(MARIADB)
  ADD_EXECUTABLE(${BINARY_NAME}-${PSTRESS_EXT} pstress.cpp help.cpp node.cpp thread.cpp random_test.cpp)
  TARGET_LINK_LIBRARIES( ${BINARY_NAME}-${PSTRESS_EXT} ${MYSQL_LIBRARY} ${OTHER_LIBS} inih++)
  INSTALL(TARGETS ${BINARY_NAME}-${PSTRESS_EXT} DESTINATION bin)
ENDIF(USE_MYSQL AND MYSQL_FOUND)

IF (USE_DUCKDB)
  MESSAGE(STATUS "Configuring for DuckDB...")

  # Manually set the paths for DuckDB if not found
  set(DUCKDB_INCLUDE_DIR "/usr/local/lib")
  set(DUCKDB_LIBRARY "/usr/local/lib/libduckdb.so")

  # Check if DuckDB includes and libraries are provided
  if (DUCKDB_INCLUDE_DIR AND DUCKDB_LIBRARY)
    INCLUDE_DIRECTORIES(${DUCKDB_INCLUDE_DIR})
    ADD_EXECUTABLE(${BINARY_NAME}-duckdb pstress.cpp help.cpp node.cpp thread.cpp random_test.cpp)
    TARGET_LINK_LIBRARIES(${BINARY_NAME}-duckdb ${DUCKDB_LIBRARY} ${OTHER_LIBS} inih++)
    INSTALL(TARGETS ${BINARY_NAME}-duckdb DESTINATION bin)
  else()
    message(FATAL_ERROR "DuckDB include or library not found")
  endif()

ENDIF(USE_DUCKDB)

FILE(COPY
         grammar.sql
         DESTINATION .)
  INSTALL(FILES grammar.sql DESTINATION bin)
SET( CMAKE_EXPORT_COMPILE_COMMANDS ON )
