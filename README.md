# io_uring
Sandbox for learning / testing io_uring

This is a sandbox, expect lots of dirty output and maybe some half finished thoughts.

Keeping the build simple for now using an MK file. To build just run MK.

copy_file: 
    Description: copies one file N times. Tests multiple simultaneous readers/writers.
                 modified from the original man page I found. Does it's own mmaping, etc, a bit over complicated.
    cmd line: copy_file --cnt=5 --input=io_uring_copy_file.cc --output=tmp/out

copy_file_simple:
    Description: copies one file N times. Tests multiple simultaneous readers/writers.
                 uses the iouring helper routines, much cleaner than the copy_file.cc logic
    strace -c copy_file_simple --cnt=500 --input=copy_file_simple.cc --output=tmp/out --debug=0
