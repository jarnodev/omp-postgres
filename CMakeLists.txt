cmake_minimum_required(VERSION 3.25)
project(omp-postgres LANGUAGES C CXX VERSION 0.3.0)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# omp-server is a 32-bit binary, so the component must be built as 32-bit too.
# Linux: -DCMAKE_TOOLCHAIN_FILE=cmake/linux-i386.cmake; Windows: -A Win32.
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
	message(WARNING "Building a 64-bit omp-postgres: it will NOT load in the 32-bit omp-server.")
endif()

# Windows links libpq statically (vcpkg x86-windows-static), which needs the static CRT.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

include(FetchContent)

# The SDK's bundled glm declares an ancient cmake_minimum_required that CMake 4 rejects.
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)

option(OMP_POSTGRES_BUILD_TESTS "Build the connection tests (need a PostgreSQL server, see tests/)" OFF)

# Pinned so builds are reproducible; bump deliberately when updating open.mp.
# SYSTEM keeps warnings from their headers out of our build.
FetchContent_Declare(omp-sdk
	GIT_REPOSITORY https://github.com/openmultiplayer/open.mp-sdk.git
	GIT_TAG 473b732b2de159773b2e2908fd11aa99f07db284
	SYSTEM
)
# Only the AMX headers are needed from the Pawn compiler repo; the bogus
# SOURCE_SUBDIR stops CMake from adding its build.
FetchContent_Declare(pawn-compiler
	GIT_REPOSITORY https://github.com/openmultiplayer/compiler.git
	GIT_TAG 29f1a8c7fd2e01929807cd8c50353fbc93bbd651
	GIT_SUBMODULES ""
	SOURCE_SUBDIR do-not-build
	SYSTEM
)
FetchContent_MakeAvailable(omp-sdk pawn-compiler)

if(WIN32)
	# vcpkg's FindPostgreSQL wrapper adds OpenSSL and the Windows system libraries.
	find_package(PostgreSQL REQUIRED)
	set(PQ_LIBRARY PostgreSQL::PostgreSQL)
	set(PQ_INCLUDE_DIR ${PostgreSQL_INCLUDE_DIRS})
else()
	find_path(PQ_INCLUDE_DIR libpq-fe.h PATH_SUFFIXES postgresql pgsql REQUIRED)
	find_library(PQ_LIBRARY NAMES pq REQUIRED)
endif()
message(STATUS "libpq: ${PQ_LIBRARY} (headers: ${PQ_INCLUDE_DIR})")

add_library(${PROJECT_NAME} SHARED
	src/main.cpp
	src/connection.cpp
	src/natives.cpp
)

set_target_properties(${PROJECT_NAME} PROPERTIES PREFIX "" OUTPUT_NAME "Postgres")

target_include_directories(${PROJECT_NAME} SYSTEM PRIVATE
	${pawn-compiler_SOURCE_DIR}/source
	${PQ_INCLUDE_DIR}
)
if(UNIX)
	target_include_directories(${PROJECT_NAME} SYSTEM PRIVATE ${pawn-compiler_SOURCE_DIR}/source/linux)
endif()

target_compile_definitions(${PROJECT_NAME} PRIVATE
	PAWN_CELL_SIZE=32
	HAVE_STDINT_H=1
	# The version lives only in project() above; the component reports it to open.mp.
	OMP_POSTGRES_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
	OMP_POSTGRES_VERSION_MINOR=${PROJECT_VERSION_MINOR}
	OMP_POSTGRES_VERSION_PATCH=${PROJECT_VERSION_PATCH}
)

find_package(Threads REQUIRED)

target_link_libraries(${PROJECT_NAME} PRIVATE
	OMP-SDK
	${PQ_LIBRARY}
	Threads::Threads
)
if(MSVC)
	target_compile_options(${PROJECT_NAME} PRIVATE /W4)
	# std::getenv is fine here: it is only read, never stored.
	target_compile_definitions(${PROJECT_NAME} PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
	# Export only ComponentEntryPoint. Without this the statically linked libstdc++
	# is exported too and can clash with other components loaded into the server.
	target_compile_options(${PROJECT_NAME} PRIVATE
		-Wall
		-Wextra
		-fvisibility=hidden
		-fvisibility-inlines-hidden
		-fstack-protector-strong
		# -U first: some toolchains (Ubuntu 24.04) already define it, which would warn.
		"$<$<NOT:$<CONFIG:Debug>>:-U_FORTIFY_SOURCE;-D_FORTIFY_SOURCE=2>"
	)
	set(EXPORTS_MAP ${CMAKE_CURRENT_SOURCE_DIR}/cmake/exports.map)
	target_link_libraries(${PROJECT_NAME} PRIVATE
		-static-libstdc++
		-static-libgcc
		-Wl,--exclude-libs,ALL
		-Wl,--version-script=${EXPORTS_MAP}
		-Wl,-z,relro,-z,now
		-Wl,--no-undefined
		$<$<CONFIG:Release>:-s>
	)
	set_target_properties(${PROJECT_NAME} PROPERTIES LINK_DEPENDS ${EXPORTS_MAP})
endif()

if(OMP_POSTGRES_BUILD_TESTS)
	enable_testing()
	# The connection layer doesn't depend on open.mp, so it is tested on its own.
	add_executable(connection_test tests/connection_test.cpp src/connection.cpp)
	target_include_directories(connection_test PRIVATE src)
	target_include_directories(connection_test SYSTEM PRIVATE ${PQ_INCLUDE_DIR})
	target_link_libraries(connection_test PRIVATE ${PQ_LIBRARY} Threads::Threads)
	if(NOT MSVC)
		target_compile_options(connection_test PRIVATE -Wall -Wextra)
	endif()
	add_test(NAME connection_test COMMAND connection_test)
endif()
