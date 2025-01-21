#!/bin/bash

# Compile http_server.c with GCC and enable all warnings
gcc -Wall -o http_server.exe http_server.c

# Check if the compilation was successful
if [ $? -eq 0 ]; then
    echo "Compilation successful. The executable is named 'http_server'."
else
    echo "Compilation failed. Please check for errors."
fi
