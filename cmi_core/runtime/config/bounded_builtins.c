/*
 * Input for Berry's const-object generator. The generated m_builtin table is
 * shared by the host compiler and runtime so bytecode builtin indices match.
 * Language operations remain available, but only numeric conversions are
 * exposed as general-purpose builtins; the runtime ABI functions are registered
 * separately as globals by the backend.
 */

/* @const_object_info_begin
vartab m_builtin (scope: local) {
    number, func(be_baselib_number)
    int, func(be_baselib_int)
    real, func(be_baselib_real)
    bool, func(l_bool)
}
@const_object_info_end */
