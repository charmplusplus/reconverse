target_include_directories(reconverse PUBLIC .)

# Install PUP headers
install(FILES 
  ../src/pup/pup.h
  ../src/pup/pup_c.h  
  ../src/pup/pup_stl.h
  ../src/pup/pup_toNetwork.h
  ../src/pup/pup_toNetwork4.h
  ../src/pup/pup_paged.h
  ../src/pup/pup_mpi.h
  ../src/pup/pup_cmialloc.h
  ../src/pup/pupf.h
  ../src/pup/pup_c_functions.h
  DESTINATION include/pup)
