# BiliMouse — 躺床看 B 站视频的无线遥控鼠标
# 目标板：立创·实战派 ESP32-S3 (LCKFB-SZPI-ESP32-S3-VA)

cmake_minimum_required(VERSION 3.16)

# 固件版本号：OTA 升级用它和服务器 manifest 里的 version 比较。
# 发布新版本时改这里，保持和 docs/OTA.md 的发布流程一致。
set(PROJECT_VER "2.7.9")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(bilimouse)

# Workaround for ESP-IDF v6.0.2 fatfs bug: an unreachable 'fail:' label in f_open
# when CONFIG_FATFS_USE_DYN_BUFFERS is enabled. The label is wrapped only in
# FF_USE_DYN_BUFFER while the matching goto is gated by !FF_FS_READONLY && !FF_FS_TINY.
# Suppress the resulting -Werror=unused-label.
idf_build_set_property(COMPILE_OPTIONS "-Wno-unused-label" APPEND)
