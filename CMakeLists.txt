cmake_minimum_required(VERSION 3.10)
project(RTSP_server)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 查找FFmpeg
find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBAVCODEC REQUIRED libavcodec)
pkg_check_modules(LIBAVFORMAT REQUIRED libavformat)
pkg_check_modules(LIBAVUTIL REQUIRED libavutil)
pkg_check_modules(LIBSWSCALE REQUIRED libswscale)
pkg_check_modules(LIBSWRESAMPLE REQUIRED libswresample)

# 查找 OpenSSL（新增）
find_package(OpenSSL REQUIRED)

# 设置Live555路径
set(LIVE555_DIR $ENV{HOME}/project/live555)
set(LIVE555_INCLUDE_DIRS
    ${LIVE555_DIR}/liveMedia/include
    ${LIVE555_DIR}/groupsock/include
    ${LIVE555_DIR}/BasicUsageEnvironment/include
    ${LIVE555_DIR}/UsageEnvironment/include
)
set(LIVE555_LIB_DIRS
    ${LIVE555_DIR}/liveMedia
    ${LIVE555_DIR}/groupsock
    ${LIVE555_DIR}/BasicUsageEnvironment
    ${LIVE555_DIR}/UsageEnvironment
)
set(LIVE555_LIBS liveMedia groupsock BasicUsageEnvironment UsageEnvironment)

# 添加可执行文件
add_executable(${PROJECT_NAME} rtsp_server.cpp)

# 包含目录
target_include_directories(${PROJECT_NAME} PRIVATE
    ${LIVE555_INCLUDE_DIRS}
    ${LIBAVCODEC_INCLUDE_DIRS}
    ${LIBAVFORMAT_INCLUDE_DIRS}
    ${LIBAVUTIL_INCLUDE_DIRS}
    ${LIBSWSCALE_INCLUDE_DIRS}
    ${LIBSWRESAMPLE_INCLUDE_DIRS}
    ${OPENSSL_INCLUDE_DIR}  # 新增
)

# 链接库目录
target_link_directories(${PROJECT_NAME} PRIVATE ${LIVE555_LIB_DIRS})

# 链接库（新增 OpenSSL）
target_link_libraries(${PROJECT_NAME}
    ${LIVE555_LIBS}
    ${LIBAVCODEC_LIBRARIES}
    ${LIBAVFORMAT_LIBRARIES}
    ${LIBAVUTIL_LIBRARIES}
    ${LIBSWSCALE_LIBRARIES}
    ${LIBSWRESAMPLE_LIBRARIES}
    ${OPENSSL_LIBRARIES}  # 新增
    pthread
    dl
)

# 屏蔽废弃警告
target_compile_options(${PROJECT_NAME} PRIVATE -Wno-deprecated-declarations)