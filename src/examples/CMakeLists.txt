set(STATICLIBS protobuf util z)
set(DYNAMICLIBS lzo2 pthread rt dl ${Boost_LIBRARIES})

protobuf_generate_cpp(EXAMPLE_PB_SRC EXAMPLE_PB_HDR examples.proto )
piccolo_pp(k-means.pp 
		   matmul.pp
		   raytrace.pp
		   shortest-path.pp
		   wordcount.pp)
		    

add_custom_target(example_proto DEPENDS ${EXAMLE_PB_HDR})

add_library(example 
			matmul.pp.cc             
            k-means.pp.cc
            shortest-path.pp.cc
	        pagerank.cc
            test-tables.cc
#            wordcount.pp.cc
#            raytrace.pp.cc
#            facedet/cpp/pgmimage.c
#            facedet/cpp/imagenet.cpp
#            facedet/cpp/backprop.cpp
#            faceclass.cc
            nas/isort.cc 
            nas/n-body.cc            
            ${EXAMPLE_PB_HDR} 
            ${EXAMPLE_PB_SRC})
            
add_dependencies(example worker_proto common_proto)
add_executable(example-dsm main.cc)
#include_directories(${CMAKE_CURRENT_SOURCE_DIR}/facedet/cpp)

# we need to resolve static initializers, so glob all the symbols in
target_link_libraries(example-dsm -Wl,-whole-archive common worker kernel master example -Wl,-no-whole-archive)
target_link_libraries(example-dsm gflags glog gtest unwind webgraph)

# link statically if possible
target_link_libraries(example-dsm -Wl,-Bstatic ${STATICLIBS} blas -Wl,-Bdynamic )
target_link_libraries(example-dsm ${DYNAMICLIBS} ${MPI_LINK_FLAGS})    

add_executable(matmul-mpi matmul-mpi.cc)
target_link_libraries(matmul-mpi glog gflags blas rt ${MPI_LINK_FLAGS})
