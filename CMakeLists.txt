cmake_minimum_required(VERSION 2.8)

if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  if(DEFINED ENV{VITASDK})
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake" CACHE PATH "toolchain file")
  else()
    message(FATAL_ERROR "Please define VITASDK to point to your SDK path!")
  endif()
endif()

project(HRM C CXX)
include("${VITASDK}/share/vita.cmake" REQUIRED)
set(VITA_APP_NAME "Human Resource Machine")
set(VITA_TITLEID  "HMNRSCMCN")
set(VITA_MKSFOEX_FLAGS "-d ATTRIBUTE2=12")

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -g -Wl,-q,--wrap,sceTouchPeek,--wrap,memcpy,--wrap,memmove,--wrap,memset,--allow-multiple-definition -Wall -O3 -fdiagnostics-color=always -fno-optimize-sibling-calls -mfloat-abi=softfp")
set(CMAKE_CXX_FLAGS "${CMAKE_C_FLAGS} -std=c++11 -Wno-write-strings")

add_executable(HRM
  loader/main.c
  loader/dialog.c
  loader/so_util.c
  loader/sha1.c
  loader/ctype_patch.c
  loader/trophies.c
)

target_link_libraries(HRM
  -Wl,--whole-archive pthread -Wl,--no-whole-archive
  SDL2_mixer
  SDL2_image
  SDL2
  png
  webp
  jpeg
  mikmod
  z
  m
  stdc++
  opensles
  sndfile
  vorbis
  vorbisenc
  vorbisfile
  ogg
  FLAC
  openal
  imgui
  vitaGL
  vitashark
  SceShaccCgExt
  mathneon
  mpg123
  taihen_stub
  kubridge_stub
  SceAppMgr_stub
  SceAppUtil_stub
  SceAudio_stub
  SceAudioIn_stub
  SceCtrl_stub
  SceCommonDialog_stub
  SceDisplay_stub
  SceFios2_stub
  SceGxm_stub
  SceLibcBridge_stub
  SceAvPlayer_stub
  SceShaccCg_stub
  SceSysmodule_stub
  ScePower_stub
  SceTouch_stub
  SceIme_stub
  SceMotion_stub
  SceHid_stub
  SceVshBridge_stub
  SceKernelDmacmgr_stub
  SceNpTrophy_stub
)

vita_create_self(eboot.bin HRM UNSAFE)
vita_create_vpk(HRM.vpk ${VITA_TITLEID} eboot.bin
  VERSION ${VITA_VERSION}
  NAME ${VITA_APP_NAME}
  FILE ${CMAKE_SOURCE_DIR}/livearea/bg0.png sce_sys/livearea/contents/bg0.png
       ${CMAKE_SOURCE_DIR}/livearea/cpu.png sce_sys/livearea/contents/cpu.png
       ${CMAKE_SOURCE_DIR}/livearea/icon0.png sce_sys/icon0.png
       ${CMAKE_SOURCE_DIR}/livearea/pic0.png sce_sys/pic0.png
       ${CMAKE_SOURCE_DIR}/livearea/template.xml sce_sys/livearea/contents/template.xml
       ${CMAKE_SOURCE_DIR}/livearea/TROPHY.TRP sce_sys/trophy/HURM00001_00/TROPHY.TRP
)
