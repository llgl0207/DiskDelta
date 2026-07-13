cmake_minimum_required(VERSION 3.16)
project(DiskDelta VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Compiler-specific settings
if(MSVC)
    # MSVC (Visual Studio)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /EHsc /utf-8 /W4 /wd4100 /wd4127 /wd4324")
elseif(MINGW OR CMAKE_COMPILER_IS_GNUCXX)
    # MinGW-w64 / GCC
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O2 -s -static")
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -static")
endif()

add_definitions(-D_WIN32_WINNT=0x0A00)

# Find required packages
# (All Windows APIs are available natively - no external dependencies required)

# Source files
set(SOURCES
    src/main.cpp
    src/mft_reader.cpp
    src/diff_engine.cpp
    src/http_server.cpp
)

# Header files (for IDE)
set(HEADERS
    src/mft_reader.h
    src/diff_engine.h
    src/http_server.h
)

# Include directories
include_directories(${CMAKE_SOURCE_DIR}/src)

# Build executable
add_executable(${PROJECT_NAME} WIN32 ${SOURCES} ${HEADERS})

# Required libraries
target_link_libraries(${PROJECT_NAME}
    ws2_32
    shell32
)

# Copy web directory and data directory to build output
add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_SOURCE_DIR}/web"
        "$<TARGET_FILE_DIR:${PROJECT_NAME}>/web"
    COMMENT "Copying web UI files..."
)

add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory
        "$<TARGET_FILE_DIR:${PROJECT_NAME}>/data"
    COMMENT "Creating data directory..."
)

# Install rules
install(TARGETS ${PROJECT_NAME} RUNTIME DESTINATION bin)
install(DIRECTORY web/ DESTINATION bin/web)
install(DIRECTORY data/ DESTINATION bin/data)

# Properties
set_target_properties(${PROJECT_NAME} PROPERTIES
    OUTPUT_NAME "DiskDelta"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)
