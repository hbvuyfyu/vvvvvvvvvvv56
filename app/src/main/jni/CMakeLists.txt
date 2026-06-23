cmake_minimum_required(VERSION 3.22.1)
project("vcam_native")

find_library(log-lib     log)
find_library(jnigraphics jnigraphics)
find_library(android-lib android)
find_library(m-lib       m)

# Library 1: vcam_native — JNI for V4L2-based injection
add_library(vcam_native SHARED
    vcam_jni.cpp
    v4l2_injector.cpp
    image_converter.cpp
)
target_link_libraries(vcam_native
    ${log-lib} ${jnigraphics-lib} ${android-lib} ${m-lib})
target_compile_options(vcam_native PRIVATE -O2 -Wall -std=c++17)

# Library 2: vcam_inject — LD_PRELOAD Camera HAL hook
add_library(vcam_inject SHARED vcam_inject.cpp)
target_link_libraries(vcam_inject ${log-lib} dl ${m-lib})
target_compile_options(vcam_inject PRIVATE -O2 -Wall -std=c++17)