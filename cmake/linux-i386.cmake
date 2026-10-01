# Toolchain for building 32-bit (i386) components on a 64-bit Linux host.
set(CMAKE_SYSTEM_NAME Linux)
# "x86" (not i686) so the SDK enables SSE2 like the official open.mp builds.
set(CMAKE_SYSTEM_PROCESSOR x86)

set(CMAKE_C_FLAGS_INIT "-m32")
set(CMAKE_CXX_FLAGS_INIT "-m32")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "-m32")
set(CMAKE_EXE_LINKER_FLAGS_INIT "-m32")

# Fedora puts i686 libraries in /usr/lib, Debian/Ubuntu in /usr/lib/i386-linux-gnu.
set(CMAKE_LIBRARY_ARCHITECTURE i386-linux-gnu)
set_property(GLOBAL PROPERTY FIND_LIBRARY_USE_LIB64_PATHS OFF)
set(CMAKE_LIBRARY_PATH /usr/lib/i386-linux-gnu /usr/lib)

# Fedora: when only the i686 libstdc++-devel is installed, g++ -m32 can't find
# bits/c++config.h in its usual multilib location; point it at the i686 copy.
file(GLOB _i686_cxx_config_dirs /usr/include/c++/*/i686-redhat-linux)
foreach(_dir IN LISTS _i686_cxx_config_dirs)
	string(REGEX REPLACE "/i686-redhat-linux$" "/x86_64-redhat-linux/32" _multilib "${_dir}")
	if(NOT EXISTS "${_multilib}")
		string(APPEND CMAKE_CXX_FLAGS_INIT " -isystem ${_dir}")
	endif()
endforeach()
