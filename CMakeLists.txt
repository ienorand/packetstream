PROJECT(packetstream)

SET(PACKETSTREAM_SOVER 0)
SET(PACKETSTREAM_VER 0.1.4)

IF (NOT CMAKE_BUILD_TYPE)
  SET(CMAKE_BUILD_TYPE "Release")
ENDIF (NOT CMAKE_BUILD_TYPE)

SUBDIRS(src)

OPTION(EXAMPLES
       "Build examples"
       OFF)

OPTION(MLIBDIR
       "Standard library directory for current architecture"
       "lib")

IF (EXAMPLES)
  SUBDIRS(examples)
ENDIF (EXAMPLES)
