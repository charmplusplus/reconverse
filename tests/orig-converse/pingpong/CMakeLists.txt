add_reconverse_executable(pingpong pingpong.cpp)
add_test(NAME pingpong COMMAND pingpong +pe 2)

add_reconverse_executable(pingpong_multipairs pingpong_multipairs.cpp)
add_test(NAME pingpong_multipairs COMMAND pingpong_multipairs +pe 4)

if(TARGET LCI::lci)
	find_program(LCI_PINGPONG_LAUNCHER NAMES lcrun HINTS "${CMAKE_BINARY_DIR}/_deps/lci-src")
	if(NOT LCI_PINGPONG_LAUNCHER)
		set(LCI_PINGPONG_LAUNCHER ${RECONVERSE_TEST_LAUNCHER})
	endif()

	add_executable(pingpong_lci pingpong_lci.cpp)
	target_link_libraries(pingpong_lci PRIVATE LCI::lci)
	set_target_properties(pingpong_lci PROPERTIES CXX_STANDARD 17)
	add_test(NAME pingpong_lci COMMAND ${LCI_PINGPONG_LAUNCHER} -n 2 $<TARGET_FILE:pingpong_lci>)
endif()
