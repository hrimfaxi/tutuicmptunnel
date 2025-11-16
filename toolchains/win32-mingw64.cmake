# 设置系统名称和架构
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

# 指定交叉编译工具链的路径
set(TOOLCHAIN_PATH /usr/bin/)
# 设置交叉编译器
set(CMAKE_C_COMPILER ${TOOLCHAIN_PATH}/x86_64-w64-mingw32-cc)
set(CMAKE_STRIP ${TOOLCHAIN_PATH}/x86_64-w64-mingw32-strip)
#set(CMAKE_SYSROOT /usr/i686-w64-mingw32/)
#include_directories(${CMAKE_SYSROOT}/include)
