# Build kotekan with tests and HDF5 in the docker container

cd build
cmake \
        -DBLAZE_PATH=/usr/local/include/blaze \
        -DBLAS_LIBRARIES=/usr/lib/x86_64-linux-gnu/libopenblas.so.0 \
        -DLAPACK_LIBRARIES=/usr/lib/x86_64-linux-gnu/lapack/liblapack.so.3.7.1 \
        -DCMAKE_BUILD_TYPE=Debug \
        -DUSE_HDF5=ON \
        -DHIGHFIVE_PATH=/code/build/HighFive \
        -DWITH_TESTS=ON \
        ..
make

