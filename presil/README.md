## How to build pre-silicon CLR
### Run build_presil_hip_clr.py
- Clones presil HIP git repo and builds presil CLR in build folder in the current directory.
```
python3 build_presil_hip_clr.py
```
- Once build process finishes,
  - The .so libs are in /path/to/clr/presil/build/hipamd/lib/
  - Export this directory to LD_LIBRARY_PATH in the following manner:
  ```
  export LD_LIBRARY_PATH=/path/to/clr/presil/build/hipamd/lib/:$LD_LIBRARY_PATH
  ```
  - Include files are in /path/to/clr/presil/build/hipamd/include/
  - Add this include path during compilation.
  
- Test the presil build using tests in the tests directory.
  ```
  cd /path/to/clr/presil/tests
  ./run_tests.sh
  ```
  - More information on how to run tests provided in README in tests directory.
- For incremental builds, or any code changes in clr, simply run make in /path/to/clr/presil/build/ to update the .so
  ```
  cd /path/to/clr/presil/build
  make -j$(nproc)
  ```
