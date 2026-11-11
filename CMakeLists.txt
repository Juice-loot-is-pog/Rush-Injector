
cmake_minimum_required(VERSION 3.20)
project(mcbeDllInjector VERSION 1.5.13)

if(NOT WIN32 OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "Build on Windows with an x64 toolchain")
endif()
add_compile_definitions(UNICODE _UNICODE NOMINMAX _WIN32_WINNT=0x0A00)

set(CMAKE_CXX_STANDARD 20)

set(wxBUILD_SHARED OFF CACHE BOOL "OFF for static library!" FORCE) # force wxWidgets to use static library
mark_as_advanced(wxBUILD_SHARED)

add_executable(${PROJECT_NAME} WIN32)
target_compile_definitions(${PROJECT_NAME} PRIVATE RUSH_INJECTOR_VERSION="${PROJECT_VERSION}")
set_target_properties(${PROJECT_NAME} PROPERTIES OUTPUT_NAME "Rush Injector")
set(RUSH_DOWNLOAD_URL "https://rush-download-service.juiceloot-github.workers.dev/client" CACHE STRING "Public HTTPS endpoint for Rush client downloads")
target_compile_definitions(${PROJECT_NAME} PRIVATE "RUSH_DOWNLOAD_URL=L\"${RUSH_DOWNLOAD_URL}\"")
add_subdirectory(src)

target_link_options(${PROJECT_NAME} PRIVATE "/MANIFESTUAC:level='asInvoker' uiAccess='false'")

add_subdirectory(wxWidgets-3.2.0)

target_link_libraries(${PROJECT_NAME} wx::net wx::core wx::base advapi32 dwmapi winhttp bcrypt)

add_executable(provision_download_key download-service/provision_key.cpp src/client_download.cpp)
target_include_directories(provision_download_key PRIVATE src)
target_link_libraries(provision_download_key PRIVATE winhttp bcrypt advapi32)

include(CTest)
if(BUILD_TESTING)
    add_subdirectory(tests)
endif()

set(CPACK_PROJECT_NAME ${PROJECT_NAME})
set(CPACK_PROJECT_VERSION ${PROJECT_VERSION})
