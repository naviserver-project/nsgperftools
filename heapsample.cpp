/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * Copyright (C) 2026 John Buckman
 */

/*
 * heapsample.cpp --
 *
 *      tcmalloc's heap sample, MallocExtension::GetHeapSample(), exists only
 *      as a C++ method. This file calls it for nsgperftools.c, looking the
 *      symbols up at run time like the rest of the module.
 *
 *      The method takes a std::string*, so its symbol name depends on the
 *      C++ library's string ABI. The name used is the one for the ABI this
 *      file is compiled with, so the module and libtcmalloc must agree; with
 *      a distribution's own compiler and gperftools package they do.
 *
 *      The base-class implementation is called directly. It gets the
 *      samples through the virtual ReadStackTraces() of tcmalloc's
 *      MallocExtension instance and appends the address map.
 */

#include <string>
#include <cstring>
#include <cstdlib>
#include <dlfcn.h>

#define NSGP_INSTANCE_SYMBOL "_ZN15MallocExtension8instanceEv"

#if defined(_LIBCPP_VERSION)
# define NSGP_GETHEAPSAMPLE_SYMBOL \
    "_ZN15MallocExtension13GetHeapSampleEPNSt3__112basic_stringIcNS0_11char_traitsIcEENS0_9allocatorIcEEEE"
#elif defined(_GLIBCXX_USE_CXX11_ABI) && _GLIBCXX_USE_CXX11_ABI
# define NSGP_GETHEAPSAMPLE_SYMBOL \
    "_ZN15MallocExtension13GetHeapSampleEPNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEE"
#else
# define NSGP_GETHEAPSAMPLE_SYMBOL "_ZN15MallocExtension13GetHeapSampleEPSs"
#endif

/*
 *----------------------------------------------------------------------
 *
 * NsgpHeapSample --
 *
 *      Get tcmalloc's current heap sample.
 *
 * Results:
 *      The sample in pprof format as a malloc'ed string (free with free()),
 *      its length in *lengthPtr; NULL when the symbols are not found.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

extern "C" char *
NsgpHeapSample(size_t *lengthPtr)
{
    typedef void *(InstanceProc)(void);
    typedef void  (GetHeapSampleProc)(void *self, std::string *writer);

    InstanceProc      *instanceProc = (InstanceProc *) dlsym(RTLD_DEFAULT, NSGP_INSTANCE_SYMBOL);
    GetHeapSampleProc *sampleProc   = (GetHeapSampleProc *) dlsym(RTLD_DEFAULT, NSGP_GETHEAPSAMPLE_SYMBOL);
    std::string        sample;
    char              *result;

    if (instanceProc == NULL || sampleProc == NULL) {
        return NULL;
    }
    (*sampleProc)((*instanceProc)(), &sample);

    result = static_cast<char *>(malloc(sample.size() + 1u));
    if (result != NULL) {
        memcpy(result, sample.data(), sample.size());
        result[sample.size()] = '\0';
        *lengthPtr = sample.size();
    }
    return result;
}

/*
 * Local Variables:
 * mode: c++
 * c-basic-offset: 4
 * fill-column: 78
 * indent-tabs-mode: nil
 * End:
 */
