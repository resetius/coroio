set(SOURCES
    actor.hpp
    actorsystem.hpp
    actorsystem.cpp
    node.hpp
    node.cpp
    messages.hpp
    messages.cpp
)

add_library(coroio_actors ${SOURCES})
target_link_libraries(coroio_actors PUBLIC coroio)
