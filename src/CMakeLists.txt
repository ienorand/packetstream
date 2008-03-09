SET(PACKETSTREAM_SRC packetstream.h packetstream.c)

IF (UNIX)
  SET(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fvisibility=hidden")
ENDIF (UNIX)

ADD_LIBRARY(packetstream_shared SHARED ${PACKETSTREAM_SRC})
TARGET_LINK_LIBRARIES(packetstream_shared pthread)
SET_TARGET_PROPERTIES(packetstream_shared PROPERTIES
		      OUTPUT_NAME packetstream
		      VERSION ${PACKETSTREAM_VER}
		      SOVERSION ${PACKETSTREAM_SOVER})

IF (UNIX)
  IF (NOT MLIBDIR)
    SET(MLIBDIR "lib")
  ENDIF (NOT MLIBDIR)
  INSTALL(TARGETS packetstream_shared
  	  LIBRARY DESTINATION ${MLIBDIR})
  INSTALL(FILES packetstream.h DESTINATION include)
ENDIF (UNIX)
