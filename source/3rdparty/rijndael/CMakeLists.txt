file(GLOB rijndael_src
    "source/rijndael-alg-fst.h"
    "source/rijndael-alg-fst.c"
)

#add_library(rijndael STATIC ${rijndael_src})
add_library(rijndael_obj OBJECT ${rijndael_src})
add_static_library_from_objects(rijndael rijndael_obj)
