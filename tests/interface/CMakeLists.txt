file(COPY test_instances DESTINATION ${CMAKE_CURRENT_BINARY_DIR})
file(COPY test_preset.ini DESTINATION ${CMAKE_CURRENT_BINARY_DIR})

add_gmock_test(mtkahypar_interface_test interface_test.cc)
target_link_libraries(mtkahypar_interface_test MtKaHyPar-LibraryBuildSources MtKaHyPar-Test mtkahypar)
