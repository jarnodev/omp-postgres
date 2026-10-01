cmake_minimum_required(VERSION 3.19)
project(omp-postgres LANGUAGES C CXX VERSION 0.1.0)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# omp-server is a 32-bit binary, so the component must be built as i386 too.
# Configure with: cmake -B build -DCMAKE_TOOLCHAIN_FILE=cmake/linux-i386.cmake
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
	message(WARNING "Building a 64-bit omp-postgres: it will NOT load in the 32-bit omp-server. Use cmake/linux-i386.cmake.")
endif()

include(FetchContent)

# The SDK's bundled glm declares an ancient cmake_minimum_required that CMake 4 rejects.
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)

# Pinned so builds are reproducible; bump deliberately when updating open.mp.
FetchContent_Declare(omp-sdk
	GIT_REPOSITORY https://github.com/openmultiplayer/open.mp-sdk.git
	GIT_TAG 473b732b2de159773b2e2908fd11aa99f07db284
)
# Only the AMX headers are needed from the Pawn compiler repo; the bogus
# SOURCE_SUBDIR stops CMake from adding its build.
FetchContent_Declare(pawn-compiler
	GIT_REPOSITORY https://github.com/openmultiplayer/compiler.git
	GIT_TAG 29f1a8c7fd2e01929807cd8c50353fbc93bbd651
	GIT_SUBMODULES ""
	SOURCE_SUBDIR do-not-build
)
FetchContent_MakeAvailable(omp-sdk pawn-compiler)

find_path(PQ_INCLUDE_DIR libpq-fe.h PATH_SUFFIXES postgresql pgsql REQUIRED)
find_library(PQ_LIBRARY NAMES pq REQUIRED)
message(STATUS "libpq: ${PQ_LIBRARY} (headers: ${PQ_INCLUDE_DIR})")

add_library(${PROJECT_NAME} SHARED
	src/main.cpp
	src/connection.cpp
	src/natives.cpp
)

set_target_properties(${PROJECT_NAME} PROPERTIES PREFIX "" OUTPUT_NAME "Postgres")

target_include_directories(${PROJECT_NAME} PRIVATE
	${pawn-compiler_SOURCE_DIR}/source
	${pawn-compiler_SOURCE_DIR}/source/linux
	${PQ_INCLUDE_DIR}
)

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
	-static-libstdc++
	-static-libgcc
)
