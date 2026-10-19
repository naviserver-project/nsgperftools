#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this file,
# You can obtain one at https://mozilla.org/MPL/2.0/.
#
# Copyright (C) 2026 John Buckman
#

ifndef NAVISERVER
    NAVISERVER  = /usr/local/ns
endif

MODNAME = nsgperftools
MOD     = nsgperftools.so
MODOBJS = nsgperftools.o heapsample.o
MODLIBS = -ldl -lstdc++

include  $(NAVISERVER)/include/Makefile.module

#
# With MODNAME set, the first rule in Makefile.module is an object-file
# dependency, which would make a plain "make" build only that object.
#
.DEFAULT_GOAL := all

#
# MallocExtension::GetHeapSample() exists only in C++ (see heapsample.cpp).
#
heapsample.o: heapsample.cpp
	$(CXX) -fPIC -O2 -g -Wall -c -o $@ heapsample.cpp
