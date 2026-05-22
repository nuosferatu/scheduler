cd examples
rm -rf build
mkdir build
cd build
cmake .. -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
make
ln -sf "$(pwd)/compile_commands.json" ../../compile_commands.json
