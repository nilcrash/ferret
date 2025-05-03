cmake_minimum_required(VERSION 3.12)
project(ferret VERSION 0.1.0 LANGUAGES CXX)

# Set C++ standard
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Set compiler flags
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra -Wpedantic -Wno-unused-parameter")
    set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -g -O0 -DDEBUG")
endif()

# Default to Debug build
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug)
endif()

# Find required packages
find_package(X11 REQUIRED)
find_package(OpenGL REQUIRED)
find_package(GLEW REQUIRED)
find_package(fmt REQUIRED)  # Add fmt library
find_package(spdlog REQUIRED)  # Add spdlog library explicitly

# Include GLX in the required X11 components
set(X11_REQUIRED_COMPONENTS Xcomposite Xdamage Xrender Xfixes Xinerama)
find_package(X11 REQUIRED COMPONENTS ${X11_REQUIRED_COMPONENTS})

# Create a custom target for formatting with clang-format
file(GLOB_RECURSE ALL_SOURCE_FILES 
    ${CMAKE_SOURCE_DIR}/src/*.cpp
    ${CMAKE_SOURCE_DIR}/src/*.hpp
    ${CMAKE_SOURCE_DIR}/src/*.h
)

find_program(CLANG_FORMAT "clang-format")
if(CLANG_FORMAT)
    add_custom_target(
        format
        COMMAND ${CLANG_FORMAT} -i -style=file ${ALL_SOURCE_FILES}
        COMMENT "Formatting source code with clang-format"
        VERBATIM
    )
endif()

# Create executable
add_executable(ferret 
    src/main.cpp
    src/WindowManager.cpp
    src/Compositor.cpp
)

# Include directories
target_include_directories(ferret PRIVATE 
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${X11_INCLUDE_DIR}
    ${OPENGL_INCLUDE_DIR}
)

# Link libraries
target_link_libraries(ferret PRIVATE
    ${X11_LIBRARIES}
    ${OPENGL_LIBRARIES}
    ${OPENGL_gl_LIBRARY}
    ${OPENGL_glu_LIBRARY}
    ${GLEW_LIBRARIES}
    X11::X11
    X11::Xcomposite
    X11::Xdamage
    X11::Xrender
    X11::Xfixes
    X11::Xinerama
    fmt::fmt
    spdlog::spdlog
)

# Install target
install(TARGETS ferret DESTINATION bin)
