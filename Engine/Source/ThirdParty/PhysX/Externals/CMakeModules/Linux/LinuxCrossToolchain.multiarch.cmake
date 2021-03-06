IF(NOT $ENV{LINUX_MULTIARCH_ROOT} EQUAL "")
	SET(CMAKE_SYSTEM_NAME Linux)

	# expects multiarch root to end with a /
	SET(LINUX_ROOT "$ENV{LINUX_MULTIARCH_ROOT}${ARCHITECTURE_TRIPLE}")
	STRING(REGEX REPLACE "\\\\" "/" LINUX_ROOT ${LINUX_ROOT})

	message (STATUS "LINUX_ROOT is '${LINUX_ROOT}'")

	SET(CMAKE_CROSSCOMPILING TRUE)
	SET(CMAKE_SYSTEM_NAME Linux)
	SET(CMAKE_SYSTEM_VERSION 1)

	# sysroot (needed so the tests don't fail)
	IF(${ARCHITECTURE_TRIPLE} MATCHES "x86_64-unknown-linux-gnu")
		SET(CMAKE_SYSROOT "$ENV{LINUX_MULTIARCH_ROOT}x86_64-unknown-linux-gnu")
		#SET(CMAKE_SYSROOT "${LINUX_ROOT}x86_64-unknown-linux-gnu")
	ELSEIF(${ARCHITECTURE_TRIPLE} MATCHES "arm-unknown-linux-gnueabihf")
		SET(CMAKE_SYSROOT "$ENV{LINUX_MULTIARCH_ROOT}arm-unknown-linux-gnueabihf")
	ELSEIF(${ARCHITECTURE_TRIPLE} MATCHES "aarch64-unknown-linux-gnueabi")
		SET(CMAKE_SYSROOT "$ENV{LINUX_MULTIARCH_ROOT}aarch64-unknown-linux-gnueabi")
	ENDIF(${ARCHITECTURE_TRIPLE} MATCHES "x86_64-unknown-linux-gnu")
	
	#SET(CMAKE_SYSROOT "${LINUX_ROOT} + ${ARCHITECTURE_TRIPLE} + arm-unknown-linux-gnueabihf + x86_64-unknown-linux-gnu + ${ARCHITECTURE_TRIPLE} | ${LINUX_ROOT} | /")
	message (STATUS "CMAKE_SYSROOT is '${CMAKE_SYSROOT}'")

	SET(CMAKE_LIBRARY_ARCHITECTURE ${ARCHITECTURE_TRIPLE})

	# specify the cross compiler
	#SET(CMAKE_C_COMPILER   ${CMAKE_SYSROOT}/bin/clang.exe)
	SET(CMAKE_C_COMPILER   ${LINUX_ROOT}/bin/clang.exe)
	SET(CMAKE_C_COMPILER_TARGET ${ARCHITECTURE_TRIPLE})
	SET(CMAKE_C_FLAGS   "-target ${ARCHITECTURE_TRIPLE}  --sysroot=${LINUX_ROOT}")

	#SET(CMAKE_CXX_COMPILER   ${CMAKE_SYSROOT}/bin/clang++.exe)
	SET(CMAKE_CXX_COMPILER   ${LINUX_ROOT}/bin/clang++.exe)
	SET(CMAKE_CXX_COMPILER_TARGET ${ARCHITECTURE_TRIPLE})
	SET(CMAKE_CXX_FLAGS   "-target ${ARCHITECTURE_TRIPLE}  --sysroot=${LINUX_ROOT}")

	SET(CMAKE_FIND_ROOT_PATH  ${LINUX_ROOT})
	#set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM ONLY)	# hoping to force it to use ar
	#set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
	#set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
ELSE()
	MESSAGE("LINUX_ROOT environment variable not defined!")
ENDIF()


