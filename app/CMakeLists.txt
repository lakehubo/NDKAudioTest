cmake_minimum_required(VERSION 3.4.1)
#ffmpeg so文件路径
set(lib_src_DIR ${CMAKE_SOURCE_DIR}/src/main/jniLibs/${ANDROID_ABI})

#配置加载native依赖
include_directories(${CMAKE_SOURCE_DIR}/src/main/cpp/include)

#动态方式加载ffmepg的so文件 第三方库加载方式
add_library(ffmpeg SHARED IMPORTED)

#引入libffmpeg.so文件
set_target_properties(ffmpeg PROPERTIES IMPORTED_LOCATION
${lib_src_DIR}/libffmpeg.so)

add_library(native-lib SHARED ${CMAKE_SOURCE_DIR}/src/main/cpp/native-lib.cpp )
find_library(log-lib
             log )

target_link_libraries( native-lib
                       android
                       ffmpeg
                       OpenSLES
                       ${log-lib} )