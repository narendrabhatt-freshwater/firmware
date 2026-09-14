#include "be_constobj.h"

static be_define_const_map_slots(m_builtin_map) {
    { be_const_key(number, -1), be_const_int(0) },
    { be_const_key(bool, 3), be_const_int(3) },
    { be_const_key(int, -1), be_const_int(1) },
    { be_const_key(real, -1), be_const_int(2) },
};

static be_define_const_map(
    m_builtin_map,
    4
);

static const bvalue __vlist_array[] = {
    be_const_func(be_baselib_number),
    be_const_func(be_baselib_int),
    be_const_func(be_baselib_real),
    be_const_func(l_bool),
};

static be_define_const_vector(
    m_builtin_vector,
    __vlist_array,
    4
);
