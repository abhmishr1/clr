## How to build pre-silicon CLR
### Run build_presil_hip_clr.py
- Clones presil HIP git repo and builds presil CLR in build folder in the current directory.
```
python3 build_presil_hip_clr.py
```
- Once build process finishes,
  - The .so libs are in $PWD/build/hipamd/lib/
  - Export this directory to LD_LIBRARY_PATH in the following manner:
  ```
  export LD_LIBRARY_PATH=$PWD/build/hipamd/lib/:$LD_LIBRARY_PATH
  ```
  - Include files are in $PWD/build/hipamd/include/
  - Add this include path during compilation.
  
- Test the presil build using tests in the tests directory.
