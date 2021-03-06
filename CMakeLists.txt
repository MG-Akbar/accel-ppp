cmake_minimum_required(VERSION 2.6)
cmake_policy(SET CMP0005 NEW)
cmake_policy(SET CMP0003 OLD)

project(accel-ppp C)

include(cmake/cpack.cmake)

IF (EXISTS ${CMAKE_HOME_DIRECTORY}/.git AND NOT DEFINED IGNORE_GIT)
	EXECUTE_PROCESS(
		COMMAND git log --no-color
		COMMAND head -n1
		COMMAND awk "{print $2}"
		WORKING_DIRECTORY ${CMAKE_HOME_DIRECTORY}
		OUTPUT_VARIABLE ACCEL_PPP_VERSION
	)
	STRING(STRIP ${ACCEL_PPP_VERSION} ACCEL_PPP_VERSION)
ELSE (EXISTS ${CMAKE_HOME_DIRECTORY}/.git AND NOT DEFINED IGNORE_GIT)
	SET (ACCEL_PPP_VERSION 1.7.4)
ENDIF (EXISTS ${CMAKE_HOME_DIRECTORY}/.git AND NOT DEFINED IGNORE_GIT)

ADD_DEFINITIONS(-DACCEL_PPP_VERSION="${ACCEL_PPP_VERSION}")

if (NOT BUILD_DRIVER_ONLY)
	if (MEMDEBUG)
		set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fstack-protector -fstack-protector-all")
	endif (MEMDEBUG)

	if (NOT DEFINED CRYPTO)
		set(CRYPTO OPENSSL)
	endif (NOT DEFINED CRYPTO)

	if (CRYPTO STREQUAL OPENSSL)
		add_definitions(-DCRYPTO_OPENSSL)
		set(crypto_lib crypto ssl)
	elseif (CRYPTO STREQUAL TOMCRYPT)
		add_definitions(-DCRYPTO_TOMCRYPT)
		set(crypto_lib tomcrypt)
	elseif (CRYPTO STREQUAL INTERNAL)
		INCLUDE_DIRECTORIES(${CMAKE_CURRENT_SOURCE_DIR}/crypto)
		set(crypto_lib internal-crypto)
	else (CRYPTO STREQUAL OPENSSL)
		message(FATAL_ERROR "unknown CRYPTO type")
	endif (CRYPTO STREQUAL OPENSSL)
	
	add_subdirectory(accel-pppd)
	add_subdirectory(crypto)
	add_subdirectory(accel-cmd)
endif (NOT BUILD_DRIVER_ONLY)

if (BUILD_DRIVER OR BUILD_DRIVER_ONLY)
	add_subdirectory(driver)
endif (BUILD_DRIVER OR BUILD_DRIVER_ONLY)

