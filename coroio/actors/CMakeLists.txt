set(SOURCES
    actor.hpp
    actorsystem.hpp
    actorsystem.cpp
    node.hpp
    node.cpp
    messages.hpp
    messages.cpp
    messages_factory.hpp
    messages_factory.cpp
    queue.hpp
    queue.cpp
)

add_library(coroio_actors ${SOURCES})
target_link_libraries(coroio_actors PUBLIC coroio)
