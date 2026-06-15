set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR arm)

set(CMAKE_C_COMPILER   arm-linux-musleabihf-gcc)
set(CMAKE_CXX_COMPILER arm-linux-musleabihf-g++)
set(CMAKE_AR           arm-linux-musleabihf-ar)
set(CMAKE_RANLIB       arm-linux-musleabihf-ranlib)
set(CMAKE_STRIP        arm-linux-musleabihf-strip)

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
