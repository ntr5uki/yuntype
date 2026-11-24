cmake_minimum_required(VERSION 3.16)
project(fcitx5-yuntype VERSION 0.1.12 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Threads REQUIRED)

add_library(yuntype_core STATIC
    core/llm_client.cpp
)
target_include_directories(yuntype_core PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/core"
)
target_link_libraries(yuntype_core PUBLIC
    Threads::Threads
)
if(WIN32)
    target_link_libraries(yuntype_core PUBLIC winhttp)
    target_compile_definitions(yuntype_core PUBLIC UNICODE _UNICODE NOMINMAX _CRT_SECURE_NO_WARNINGS)
    if(MSVC)
        target_compile_options(yuntype_core PUBLIC /utf-8)
    endif()
endif()

option(BUILD_TESTING "Build unit tests" OFF)
if(BUILD_TESTING)
    enable_testing()
    add_executable(test_core
        test/test_core.cpp
    )
    target_link_libraries(test_core PRIVATE yuntype_core)
    add_test(NAME test_core COMMAND test_core)

    add_executable(test_win_config
        test/test_win_config.cpp
        windows/src/win_config.cpp
    )
    target_link_libraries(test_win_config PRIVATE yuntype_core)
    if(WIN32)
        target_link_libraries(test_win_config PRIVATE shell32 advapi32 ole32)
    endif()
    add_test(NAME test_win_config COMMAND test_win_config)
endif()

if(WIN32)
    set(YUNTYPE_WIN_MANIFEST_SRC windows/yuntype.rc)
    if(MSVC)
        set(YUNTYPE_WIN_MANIFEST_SRC windows/yuntype.manifest)
    endif()
    add_executable(yuntype WIN32
        windows/src/main.cpp
        windows/src/win_app.cpp
        windows/src/win_caret.cpp
        windows/src/win_config.cpp
        windows/src/win_overlay.cpp
        windows/src/win_sender.cpp
        windows/src/win_settings_server.cpp
        ${YUNTYPE_WIN_MANIFEST_SRC}
    )
    target_include_directories(yuntype PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/windows/src"
    )
    target_compile_definitions(yuntype PRIVATE
        YUNTYPE_VERSION="${PROJECT_VERSION}"
        UNICODE _UNICODE NOMINMAX
    )
    if(MSVC)
        target_compile_options(yuntype PRIVATE /utf-8 /W3)
        target_compile_options(yuntype_core PRIVATE /utf-8 /W3)
    endif()
    target_link_libraries(yuntype PRIVATE
        yuntype_core
        user32
        gdi32
        shell32
        advapi32
        ole32
        oleaut32
        oleacc
        dwmapi
        ws2_32
        bcrypt
        uuid
    )
else()
    include(GNUInstallDirs)

    find_package(Fcitx5Core 5.0 REQUIRED)
    find_package(Fcitx5Config 5.0 REQUIRED)
    find_package(Fcitx5Utils 5.0 REQUIRED)

    if(NOT DEFINED FCITX_INSTALL_PKGDATADIR)
        set(FCITX_INSTALL_PKGDATADIR "${CMAKE_INSTALL_DATADIR}/fcitx5")
    endif()

    set_target_properties(yuntype_core PROPERTIES POSITION_INDEPENDENT_CODE ON)

    add_library(yuntype MODULE
        src/yuntype.cpp
    )
    target_include_directories(yuntype PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    target_compile_definitions(yuntype PRIVATE
        FCITX5_YUNTYPE_VERSION="${PROJECT_VERSION}"
    )
    target_link_libraries(yuntype PRIVATE
        yuntype_core
        Fcitx5::Core
        Fcitx5::Config
        Fcitx5::Utils
    )

    install(TARGETS yuntype
        LIBRARY DESTINATION "${FCITX_INSTALL_ADDONDIR}"
    )

    string(REGEX MATCH "^[0-9]+\\.[0-9]+" _fcitx_minor "${Fcitx5Core_VERSION}")
    set(FCITX_CORE_DEP "${_fcitx_minor}.0")
    configure_file(conf/yuntype.conf.in
        "${CMAKE_CURRENT_BINARY_DIR}/yuntype.conf" @ONLY)

    install(FILES "${CMAKE_CURRENT_BINARY_DIR}/yuntype.conf"
        DESTINATION "${FCITX_INSTALL_PKGDATADIR}/addon"
        RENAME yuntype.conf
    )

    set(VERSION "${PROJECT_VERSION}")
    configure_file(settings/fcitx5-yuntype-config
        "${CMAKE_CURRENT_BINARY_DIR}/fcitx5-yuntype-config" @ONLY)

    install(PROGRAMS "${CMAKE_CURRENT_BINARY_DIR}/fcitx5-yuntype-config"
        DESTINATION "${CMAKE_INSTALL_BINDIR}"
    )

    install(FILES data/sentence-prompt.txt
        DESTINATION "${CMAKE_INSTALL_DATADIR}/fcitx5-yuntype"
    )

    install(FILES settings/org.fcitx.Fcitx5.YunType.Config.desktop
        DESTINATION "${CMAKE_INSTALL_DATADIR}/applications"
    )

    if(BUILD_TESTING)
        add_executable(test_yuntype
            test/test_yuntype.cpp
            src/yuntype.cpp
        )
        target_include_directories(test_yuntype PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/src"
        )
        target_compile_definitions(test_yuntype PRIVATE
            FCITX5_YUNTYPE_VERSION="${PROJECT_VERSION}"
        )
        target_link_libraries(test_yuntype PRIVATE
            yuntype_core
            Fcitx5::Core
            Fcitx5::Config
            Fcitx5::Utils
        )
        add_test(NAME test_yuntype COMMAND test_yuntype)
    endif()
endif()
