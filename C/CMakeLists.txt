cmake_minimum_required(VERSION 3.16.0)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(SleepMonitoring LANGUAGE C)

#Create firmware executable
add_executable(firmware.elf 
    C/src/main.c
    C/src/i2c_manager.c
    )

#Header file directory
target_include_directories(firmware.elf PRIVATE 
    C/include
    
    )