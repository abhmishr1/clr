## How to run tests
### Run all tests
- Compiles and runs all tests in the unit_tests directory using the current gfx arch.
```
./run_tests.sh
```
### Run a specific test
- Specify the test name from the unit_tests directory (without the .hip.cpp) using flag -t.
  #### presil_kernel_data
  - can pass multiple archs as arguments separated by spaces.
  ```
  ./run_tests.sh -t presil_kernel_data gfx90a gfx940 gfx1100
  ```
  #### kargs_malloc
  ```
  ./run_tests.sh -t kargs_malloc
  ```

