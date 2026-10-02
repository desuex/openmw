set(MWUE_EXPORTER
    main.cpp
    content.cpp
    cachewriter.cpp
)

set(MWUE_EXPORTER_HEADER
    content.hpp
    cachewriter.hpp
)

source_group(apps\\mwue-exporter FILES ${MWUE_EXPORTER} ${MWUE_EXPORTER_HEADER})

openmw_add_executable(openmw-mwue-exporter ${MWUE_EXPORTER} ${MWUE_EXPORTER_HEADER})

target_link_libraries(openmw-mwue-exporter
    Boost::program_options
    components
)

if (WIN32)
    target_sources(openmw-mwue-exporter PRIVATE ${CMAKE_SOURCE_DIR}/files/windows/other-apps.manifest)
    install(TARGETS openmw-mwue-exporter RUNTIME DESTINATION ".")
endif()

if (MSVC AND PRECOMPILE_HEADERS_WITH_MSVC)
    target_precompile_headers(openmw-mwue-exporter REUSE_FROM components)
endif()
