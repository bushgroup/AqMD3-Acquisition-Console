# x64-windows, with one change: libzmq is built without its AF_UNIX signaler.
#
# libzmq gives every socket a signaler backed by a socket pair. When ZMQ_HAVE_IPC is on, and it
# is on by default wherever afunix.h exists, make_fdpair first tries an AF_UNIX pair over a
# socket file in %TMP%, and it stops considering the TCP fallback the moment that file binds
# (src/ip.cpp). On at least one Windows 11 machine, connect() to such a file fails with
# WSAEINVAL under %LOCALAPPDATA%\Temp while succeeding elsewhere, so make_fdpair returns -1;
# signaler_t does not check the return value (src/signaler.cpp) and keeps invalid descriptors,
# and the first epoll_ctl on one of them aborts the process with "Bad file descriptor".
#
# Turning ZMQ_HAVE_IPC off gives the TCP loopback signaler libzmq used on Windows for years,
# and which pyzmq still ships. The console speaks only tcp://, so losing the ipc:// transport
# costs it nothing.
#
# ZMQ_HAVE_IPC is a check_include_files result variable, and check_include_files leaves an
# already-defined variable alone, so pre-setting it in the cache is enough. No patch needed.

set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE dynamic)

if(PORT STREQUAL "zeromq")
    list(APPEND VCPKG_CMAKE_CONFIGURE_OPTIONS "-DZMQ_HAVE_IPC=0")
endif()
