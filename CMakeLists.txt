cmake_minimum_required(VERSION 3.16)

# Firmware version — shown in startup log and used for OTA update checks.
# Bump this before tagging a GitHub release (must match the tag, e.g. "1.1.0").
set(PROJECT_VER "1.0.5")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(lights_project)
