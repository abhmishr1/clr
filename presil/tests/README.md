## How to run tests
### Run all tests
- Compiles and runs all tests in the unit_tests directory using the current gfx arch.
```
./run_tests.sh
```
### Run a specific test
- Specify the test name from the unit_tests directory (without the .hip.cpp) using flag -t.
  #### presil_kernel_data
   - run this test on current device
  ```
  ./run_tests.sh -t presil_kernel_data
  ```
  - to run and compile on multiple archs, can pass them as arguments separated by spaces
  ```
  ./run_tests.sh -t presil_kernel_data gfx90a gfx940 gfx1100
  ```
  #### kargs_malloc
  ```
  ./run_tests.sh -t kargs_malloc
  ```
  #### ffm_sim_hook
  - run this test on current device
  ```
  ./run_tests.sh -t ffm_sim_hook
  ```
  - to use a different arch for FFM, pass it as an argument separated by a space
  ```
  ./run_tests.sh -t ffm_sim_hook gfx1200
  ```

