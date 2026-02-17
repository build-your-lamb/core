cmake_minimum_required(VERSION 3.1)

project(lamb)

# if arm, use rk_media else use dummy
if(CMAKE_SYSTEM_PROCESSOR MATCHES "arm")
    set(MEDIA_SRC src/rk_media.c)
    set(MEDIA_LIBS rockit rockchip_mpp rga ssl crypto mount blkid intl iconv z)
    set(EXTERNAL_INCLUDE_DIRS
        /home/user/Workspace/luckfox-pico/sysdrv/source/buildroot/buildroot-2023.02.6/output/host/arm-buildroot-linux-uclibcgnueabihf/sysroot/usr/include
        /home/user/Workspace/luckfox-pico/media/out/include
    )
    set(EXTERNAL_LINK_DIRS
        /home/user/Workspace/luckfox-pico/sysdrv/source/buildroot/buildroot-2023.02.6/output/host/arm-buildroot-linux-uclibcgnueabihf/sysroot/usr/lib
        /home/user/Workspace/luckfox-pico/media/out/lib
    )
else()
    set(MEDIA_SRC src/test_video.c)
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

add_executable(lamb
    src/main.c
    src/meet.c
    src/agent.c
    src/utils.c
    src/telegram.c
    third_party11/inih/ini.c
    ${MEDIA_SRC}
    protobuf/livekit_models.pb-c.c
    protobuf/livekit_rtc.pb-c.c
    protobuf/livekit_metrics.pb-c.c
    protobuf/google/protobuf/timestamp.pb-c.c
)

target_link_libraries(lamb websockets protobuf-c peer curl nng cjson pthread ${MEDIA_LIBS})

target_link_libraries(lamb websockets protobuf-c peer curl nng cjson pthread ${MEDIA_LIBS})

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

add_dependencies(lamb libpeer nng)
add_compile_definitions(INI_MAX_LINE=10000)
