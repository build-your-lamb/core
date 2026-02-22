cmake_minimum_required(VERSION 3.1)

project(lamb)

option(USE_GST_MEDIA "Use GStreamer for audio/video pipelines" ON)

if(USE_GST_MEDIA)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(GST REQUIRED gstreamer-1.0 gstreamer-app-1.0)
endif()

if(CMAKE_SYSTEM_PROCESSOR MATCHES "arm")
    set(EXTERNAL_INCLUDE_DIRS
      /home/user/Workspace/lamb/core/sysroot/include
    )
    set(EXTERNAL_LINK_DIRS
	   /home/user/Workspace/lamb/core/sysroot/usr/lib/aarch64-linux-gnu 
	    #/home/user/Workspace/luckfox-pico/media/out/lib
    )
else()
    set(EXTERNAL_LIBS X11)
endif()


include_directories(
    ${PROJECT_SOURCE_DIR}/protobuf
    ${PROJECT_SOURCE_DIR}/third_party/protobuf-c/protobuf-c
    ${CMAKE_BINARY_DIR}/dist/include
    ${CMAKE_BINARY_DIR}/libpeer-prefix/src/libpeer-build/dist/include
    ${EXTERNAL_INCLUDE_DIRS}
)

link_directories(
    ${CMAKE_BINARY_DIR}/dist/lib
    ${EXTERNAL_LINK_DIRS}
)

set(LAMB_SOURCES
    src/main.c
    src/meet.c
    src/agent.c
    src/utils.c
    src/telegram.c
    src/display.c
    third_party11/inih/ini.c
    protobuf/livekit_models.pb-c.c
    protobuf/livekit_rtc.pb-c.c
    protobuf/livekit_metrics.pb-c.c
    protobuf/google/protobuf/timestamp.pb-c.c
)

if(USE_GST_MEDIA)
    list(APPEND LAMB_SOURCES
        src/gst_audio.c
        src/gst_video.c
    )
else()
    list(APPEND LAMB_SOURCES
        src/audio.c
        src/test_video.c
    )
endif()

add_executable(lamb ${LAMB_SOURCES})

if(USE_GST_MEDIA)
    target_include_directories(lamb PRIVATE ${GST_INCLUDE_DIRS})
    target_link_directories(lamb PRIVATE ${GST_LIBRARY_DIRS})
    target_link_libraries(lamb websockets protobuf-c peer curl nng opus asound cjson pthread ${MEDIA_LIBS} lvgl_linux lvgl lvgl_linux lvgl_thorvg stdc++ freetype ${GST_LIBRARIES} m ${EXTERNAL_LIBS})
else()
    target_link_libraries(lamb websockets protobuf-c peer curl nng opus asound cjson pthread ${MEDIA_LIBS} lvgl_linux lvgl lvgl_linux lvgl_thorvg stdc++ freetype m ${EXTERNAL_LIBS})
endif()

include(ExternalProject)
EXternalProject_Add(libpeer
  SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/libpeer
  CMAKE_ARGS
    -DCMAKE_INSTALL_PREFIX=${CMAKE_BINARY_DIR}/dist
    -DCMAKE_TOOLCHAIN_FILE=${CMAKE_TOOLCHAIN_FILE}
)

EXternalProject_Add(nng
  SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/nng
  CMAKE_ARGS
    -DCMAKE_INSTALL_PREFIX=${CMAKE_BINARY_DIR}/dist
    -DCMAKE_TOOLCHAIN_FILE=${CMAKE_TOOLCHAIN_FILE}
)

EXternalProject_Add(opus
  SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/opus
  CMAKE_ARGS
    -DCMAKE_INSTALL_PREFIX=${CMAKE_BINARY_DIR}/dist
    -DCMAKE_TOOLCHAIN_FILE=${CMAKE_TOOLCHAIN_FILE}
)

EXternalProject_Add(lv_port_linux
  SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/lv_port_linux
  CMAKE_ARGS
    -DCMAKE_INSTALL_PREFIX=${CMAKE_BINARY_DIR}/dist
    -DCMAKE_TOOLCHAIN_FILE=${CMAKE_TOOLCHAIN_FILE}
)

add_dependencies(lamb libpeer nng opus lv_port_linux)
add_compile_definitions(INI_MAX_LINE=10000)
