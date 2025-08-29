set(SOURCES
    resolver.hpp
    resolver.cpp
)

add_library(coroio_dns ${SOURCES})
target_link_libraries(coroio_dns PUBLIC coroio)
