cmake_minimum_required(VERSION 3.16)
project(fcitx5-llm-sentence VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(GNUInstallDirs)

find_package(Fcitx5Core 5.0 REQUIRED)
find_package(Fcitx5Config 5.0 REQUIRED)
find_package(Fcitx5Utils 5.0 REQUIRED)
find_package(Threads REQUIRED)

if(NOT DEFINED FCITX_INSTALL_PKGDATADIR)
    set(FCITX_INSTALL_PKGDATADIR "${CMAKE_INSTALL_DATADIR}/fcitx5")
endif()

add_library(llm_sentence MODULE
    src/llm_sentence.cpp
    src/llm_client.cpp
)

target_include_directories(llm_sentence PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
)

target_compile_definitions(llm_sentence PRIVATE
    FCITX5_LLM_SENTENCE_VERSION="${PROJECT_VERSION}"
)

target_link_libraries(llm_sentence PRIVATE
    Fcitx5::Core
    Fcitx5::Config
    Fcitx5::Utils
    Threads::Threads
)

install(TARGETS llm_sentence
    LIBRARY DESTINATION "${FCITX_INSTALL_ADDONDIR}"
)

string(REGEX MATCH "^[0-9]+\\.[0-9]+" _fcitx_minor "${Fcitx5Core_VERSION}")
set(FCITX_CORE_DEP "${_fcitx_minor}.0")
configure_file(conf/llm_sentence.conf.in
    "${CMAKE_CURRENT_BINARY_DIR}/llm_sentence.conf" @ONLY)

install(FILES "${CMAKE_CURRENT_BINARY_DIR}/llm_sentence.conf"
    DESTINATION "${FCITX_INSTALL_PKGDATADIR}/addon"
    RENAME llm_sentence.conf
)

set(VERSION "${PROJECT_VERSION}")
configure_file(settings/fcitx5-llm-sentence-config
    "${CMAKE_CURRENT_BINARY_DIR}/fcitx5-llm-sentence-config" @ONLY)

install(PROGRAMS "${CMAKE_CURRENT_BINARY_DIR}/fcitx5-llm-sentence-config"
    DESTINATION "${CMAKE_INSTALL_BINDIR}"
)

install(FILES data/sentence-prompt.txt
    DESTINATION "${CMAKE_INSTALL_DATADIR}/fcitx5-llm-sentence"
)

install(FILES settings/org.fcitx.Fcitx5.LlmSentence.Config.desktop
    DESTINATION "${CMAKE_INSTALL_DATADIR}/applications"
)

option(BUILD_TESTING "Build unit tests" OFF)
if(BUILD_TESTING)
    enable_testing()
    add_executable(test_llm_sentence
        test/test_llm_sentence.cpp
        src/llm_sentence.cpp
        src/llm_client.cpp
    )
    target_include_directories(test_llm_sentence PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    target_compile_definitions(test_llm_sentence PRIVATE
        FCITX5_LLM_SENTENCE_VERSION="${PROJECT_VERSION}"
    )
    target_link_libraries(test_llm_sentence PRIVATE
        Fcitx5::Core
        Fcitx5::Config
        Fcitx5::Utils
        Threads::Threads
    )
    add_test(NAME test_llm_sentence COMMAND test_llm_sentence)
endif()
