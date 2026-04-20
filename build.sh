#!/bin/sh
set -eu

c++ -std=c++20 -O2 -Wall -Wextra -Wpedantic -Wno-register \
  toyger-wardrobe.cpp -o toyger-wardrobe \
  -I/usr/local/include -I/usr/X11R6/include \
  -L/usr/local/lib -L/usr/X11R6/lib \
  -lXm -lXt -lX11
