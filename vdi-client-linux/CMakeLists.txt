cmake_minimum_required(VERSION 3.16)

project(VDIClient VERSION 1.5.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

find_package(Qt6 REQUIRED COMPONENTS Core Widgets Network)

set(PROJECT_SOURCES
    src/main.cpp
    src/loginwindow.cpp
    src/loginwindow.h
    resources/resources.qrc
)

add_executable(${PROJECT_NAME} ${PROJECT_SOURCES})

target_compile_definitions(${PROJECT_NAME} PRIVATE QT_NO_DEBUG_OUTPUT)

target_link_libraries(${PROJECT_NAME} PRIVATE
    Qt6::Core
    Qt6::Widgets
    Qt6::Network
)

# Copy qf-client binary to build directory
set(QF_CLIENT_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/../qfreerdp-master/build/qf-client")
if(EXISTS "${QF_CLIENT_SOURCE}")
    file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
    add_custom_target(copy_qf_client ALL
        COMMAND ${CMAKE_COMMAND} -E copy "${QF_CLIENT_SOURCE}" "${CMAKE_BINARY_DIR}/bin/qf-client"
        COMMAND chmod +x "${CMAKE_BINARY_DIR}/bin/qf-client"
        COMMENT "Copying qf-client binary to build directory"
    )
    add_dependencies(${PROJECT_NAME} copy_qf_client)
endif()
