set(SOURCES
    actor.hpp
    actor.cpp
    actorsystem.hpp
    actorsystem.cpp
    envelope_reader.hpp
    envelope_reader.cpp
    node.hpp
    node.cpp
    threaded_inbound.hpp
    threaded_node.hpp
    messages.hpp
    messages.cpp
    messages_factory.hpp
    messages_factory.cpp
    queue.hpp
    queue.cpp
)

add_library(coroio_actors ${SOURCES})
target_link_libraries(coroio_actors PUBLIC coroio coroio_dns)
