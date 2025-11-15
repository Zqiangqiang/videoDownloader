cmake_minimum_required(VERSION 3.29)
project(videoDownloader)

set(CMAKE_CXX_STANDARD 17)

# 设置openssl根目录
set(OPENSSL_ROOT_DIR "/opt/homebrew/opt/openssl@3")
find_package(OpenSSL REQUIRED)
find_package(CURL REQUIRED)

# 告诉 CMake Qt 在哪里
set(CMAKE_PREFIX_PATH "/opt/homebrew/opt/qt")
find_package(Qt6 COMPONENTS Widgets REQUIRED Concurrent)
qt_standard_project_setup()

# 输出openssl和curl信息
message(STATUS "OpenSSL include dir: ${OPENSSL_INCLUDE_DIR}")
message(STATUS "OpenSSL libraries: ${OPENSSL_LIBRARIES}")
message(STATUS "CURL include dir: ${CURL_INCLUDE_DIRS}")
message(STATUS "CURL libraries: ${CURL_LIBRARIES}")

# 可执行文件
add_executable(videoDownloader main.cpp
        downloader/http_client.h
        downloader/m3u8_downloader.h
        downloader/thread_pool.h
        downloader/http_client.cpp
        downloader/ffmpeg_downloader.h
        downloader/ffmpeg_downloader.cpp
        downloader/m3u8_downloader.cpp
)

# 包含目录
target_include_directories(videoDownloader PRIVATE ${CURL_INCLUDE_DIRS} ${OPENSSL_INCLUDE_DIR} downloader)
# 链接库
target_link_libraries(videoDownloader PRIVATE ${CURL_LIBRARIES} OpenSSL::Crypto Qt6::Widgets Qt6::Concurrent)
