add_executable(sssp-cpu-int32 SSSP.cpp)
add_dependencies(apps sssp-cpu-int32)
target_link_libraries(sssp-cpu-int32 PRIVATE Galois::shmem lonestar)
install(TARGETS sssp-cpu-int32 DESTINATION "${CMAKE_INSTALL_BINDIR}" COMPONENT apps EXCLUDE_FROM_ALL)

add_executable(sssp-cpu-float SSSP.cpp)
add_dependencies(apps sssp-cpu-float)
target_link_libraries(sssp-cpu-float PRIVATE Galois::shmem lonestar)
target_compile_definitions(sssp-cpu-float PRIVATE USE_FLOAT)
install(TARGETS sssp-cpu-float DESTINATION "${CMAKE_INSTALL_BINDIR}" COMPONENT apps EXCLUDE_FROM_ALL)

add_custom_target(sssp-cpu DEPENDS sssp-cpu-int32 sssp-cpu-float)

add_executable(sssp-cpu-crelax SSSP.cpp)
add_dependencies(apps sssp-cpu-crelax)
target_compile_definitions(sssp-cpu-crelax PRIVATE COUNT_RELAX)
target_link_libraries(sssp-cpu-crelax PRIVATE Galois::shmem lonestar)
install(TARGETS sssp-cpu-crelax DESTINATION "${CMAKE_INSTALL_BINDIR}" COMPONENT apps EXCLUDE_FROM_ALL)

add_test_scale(small1 sssp-cpu "${BASEINPUT}/reference/structured/rome99.gr" -delta 8)
add_test_scale(small2 sssp-cpu "${BASEINPUT}/scalefree/rmat10.gr" -delta 8)
