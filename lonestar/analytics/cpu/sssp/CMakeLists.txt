add_executable(sssp-cpu SSSP.cpp)
add_dependencies(apps sssp-cpu)
target_link_libraries(sssp-cpu PRIVATE Galois::shmem lonestar)
install(TARGETS sssp-cpu DESTINATION "${CMAKE_INSTALL_BINDIR}" COMPONENT apps EXCLUDE_FROM_ALL)

add_executable(sssp-crelax SSSP.cpp)
add_dependencies(apps sssp-crelax)
target_compile_definitions(sssp-crelax PRIVATE COUNT_RELAX)
target_link_libraries(sssp-crelax PRIVATE Galois::shmem lonestar)
install(TARGETS sssp-crelax DESTINATION "${CMAKE_INSTALL_BINDIR}" COMPONENT apps EXCLUDE_FROM_ALL)

add_test_scale(small1 sssp-cpu "${BASEINPUT}/reference/structured/rome99.gr" -delta 8)
add_test_scale(small2 sssp-cpu "${BASEINPUT}/scalefree/rmat10.gr" -delta 8)
