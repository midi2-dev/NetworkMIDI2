set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

set(CMAKE_C_COMPILER   aarch64-linux-musl-gcc)
set(CMAKE_CXX_COMPILER aarch64-linux-musl-g++)
set(CMAKE_AR           aarch64-linux-musl-ar)
set(CMAKE_RANLIB       aarch64-linux-musl-ranlib)
set(CMAKE_STRIP        aarch64-linux-musl-strip)

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
