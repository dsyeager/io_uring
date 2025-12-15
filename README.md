# io_uring
Sandbox for learning / testing io_uring

This is a sandbox, expect lots of dirty output and maybe some half finished thoughts.

Keeping the build simple for now using an MK file. To build just run MK.

copy_file: 
    Description: copies one file N times. Tests multiple simultaneous readers/writers.
    cmd line: copy_file --cnt=5 --input=io_uring_copy_file.cc --output=tmp/out
