cmake_minimum_required(VERSION 3.10)
project(UartBridge C)
add_library(uart_bridge STATIC uart_bridge.c)
target_include_directories(uart_bridge PUBLIC .)
