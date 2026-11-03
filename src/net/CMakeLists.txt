# Poller: kqueue no macOS, epoll no Linux (mesma interface em Poller.hpp)
if(APPLE)
    set(CITIUS_POLLER_SOURCE PollerKqueue.cpp)
else()
    set(CITIUS_POLLER_SOURCE PollerEpoll.cpp)
endif()

target_sources(citius_core PRIVATE
    Socket.cpp
    ${CITIUS_POLLER_SOURCE}
)
