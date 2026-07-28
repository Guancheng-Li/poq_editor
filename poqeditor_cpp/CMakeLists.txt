cmake_minimum_required(VERSION 3.16)
project(PoqEditorCpp LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Unicode entry point (wWinMain -> main via Qt)
add_compile_definitions(UNICODE _UNICODE QT_DEPRECATED_WARNINGS)

# UTF-8 everywhere: source files are saved UTF-8 (no BOM), and all narrow
# string literals (including Chinese text and exception messages) must be UTF-8
# bytes at runtime so QString::fromUtf8 / ex.what() decode correctly.
if(MSVC)
    add_compile_options(/utf-8)
else()
    add_compile_options(-finput-charset=UTF-8 -fexec-charset=UTF-8)
endif()

# --- Qt6 ---
find_package(Qt6 COMPONENTS Widgets Core Gui REQUIRED)

set(SOURCES
    src/main.cpp
    src/MainWindow.cpp
    src/Core/OffsetsConfig.cpp
    src/Core/IniTemplate.cpp
    src/Core/GameModder.cpp
    src/Native/ProcessMemory.cpp
)

set(HEADERS
    src/MainWindow.h
    src/Core/OffsetsConfig.h
    src/Core/ItemProperty.h
    src/Core/IniTemplate.h
    src/Core/GameModder.h
    src/Native/ProcessMemory.h
)

qt_add_executable(PoqEditorCpp WIN32 ${SOURCES} ${HEADERS})

target_link_libraries(PoqEditorCpp PRIVATE Qt6::Widgets Qt6::Core Qt6::Gui)

target_include_directories(PoqEditorCpp PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

# Windows: embed admin manifest via .rc resource.
if(WIN32)
    target_sources(PoqEditorCpp PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/app.rc)
endif()

# --- Copy config files next to the executable so they can be loaded at runtime ---
set(CONFIG_FILES offsets.json EquMod.ini)
foreach(f IN LISTS CONFIG_FILES)
    add_custom_command(TARGET PoqEditorCpp POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CMAKE_CURRENT_SOURCE_DIR}/${f}"
                "$<TARGET_FILE_DIR:PoqEditorCpp>/${f}")
endforeach()

set_target_properties(PoqEditorCpp PROPERTIES
    WIN32_EXECUTABLE ON
    OUTPUT_NAME "PoqEditorCpp"
)
