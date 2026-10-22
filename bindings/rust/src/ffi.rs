//! Private C ABI. No L^ structure layout is duplicated here.
use std::ffi::{c_char, c_void};
pub type Ptr = *mut c_void;
#[repr(C)]
#[derive(Default)]
pub struct Data {
    pub kind: i32,
    pub integer: i64,
    pub real: f64,
    pub bytes: *const u8,
    pub length: usize,
}
pub type Loader = unsafe extern "C" fn(Ptr, *const c_char, *mut usize) -> *mut c_char;
pub type Host = unsafe extern "C" fn(Ptr, Ptr);
extern "C" {
    pub fn lhat_alloc(size: usize) -> Ptr;
    pub fn lhat_free(pointer: Ptr);
    pub fn lhat_load_file(context: Ptr, path: *const c_char, length: *mut usize) -> *mut c_char;
    pub fn lhat_program_new(strict: bool, loader: Option<Loader>, context: Ptr) -> Ptr;
    pub fn lhat_program_free(p: Ptr);
    pub fn lhat_program_set_project_root(p: Ptr, path: *const c_char) -> bool;
    pub fn lhat_program_install(p: Ptr, m: Ptr) -> bool;
    pub fn lhat_program_load_failure(p: Ptr) -> *const c_char;
    pub fn lhat_program_diagnostic_count(p: Ptr) -> usize;
    pub fn lhat_program_units(p: Ptr) -> Ptr;
    pub fn lhat_unit_next(u: Ptr) -> Ptr;
    pub fn lhat_unit_diagnostic_count(u: Ptr) -> usize;
    pub fn lhat_unit_diagnostic_write(
        u: Ptr,
        i: usize,
        rich: bool,
        out: *mut c_char,
        cap: usize,
    ) -> usize;
    pub fn lhat_program_compile_status(p: Ptr) -> i32;
    pub fn lhat_compile_status_message(status: i32) -> *const c_char;
    pub fn lhat_machine_new() -> Ptr;
    pub fn lhat_machine_dispose(m: Ptr);
    pub fn lhat_machine_gc_collect(m: Ptr) -> usize;
    pub fn lhat_machine_set_budget(m: Ptr, turns: i64);
    pub fn lhat_machine_is_suspended(m: Ptr) -> bool;
    pub fn lhat_rs_value_free(m: Ptr, v: Ptr);
    pub fn lhat_rs_value_data(v: Ptr) -> Data;
    pub fn lhat_rs_value_new(m: Ptr, d: Data) -> Ptr;
    pub fn lhat_rs_table_new(m: Ptr) -> Ptr;
    pub fn lhat_rs_table_get(m: Ptr, t: Ptr, k: Ptr) -> Ptr;
    pub fn lhat_rs_table_set(m: Ptr, t: Ptr, k: Ptr, v: Ptr) -> bool;
    pub fn lhat_rs_value_text(v: Ptr, out: *mut c_char, cap: usize) -> usize;
    pub fn lhat_rs_value_equal(a: Ptr, b: Ptr) -> bool;
    pub fn lhat_rs_coroutine_done(v: Ptr) -> bool;
    pub fn lhat_rs_register(
        p: Ptr,
        module: *const c_char,
        name: *const c_char,
        signature: *const c_char,
        call: Host,
        context: Ptr,
    ) -> bool;
    pub fn lhat_rs_register_constant(
        p: Ptr,
        module: *const c_char,
        name: *const c_char,
        data: Data,
    ) -> bool;
    pub fn lhat_rs_program_diagnostic(p: Ptr, i: usize, out: *mut c_char, cap: usize) -> usize;
    pub fn lhat_rs_arg_count(c: Ptr) -> usize;
    pub fn lhat_rs_arg(c: Ptr, i: usize, out: *mut Data) -> bool;
    pub fn lhat_rs_answer(c: Ptr, data: Data) -> bool;
    pub fn lhat_rs_answer_arg(c: Ptr, i: usize) -> bool;
    pub fn lhat_rs_callback_error(c: Ptr, text: *const c_char);
    pub fn lhat_rs_load(
        p: Ptr,
        m: Ptr,
        name: *const c_char,
        bytes: *const u8,
        length: usize,
    ) -> Ptr;
    pub fn lhat_rs_check(p: Ptr, m: Ptr, path: *const c_char) -> Ptr;
    pub fn lhat_rs_run(
        m: Ptr,
        mode: i32,
        callee: Ptr,
        args: *const Ptr,
        count: usize,
        member: *const u8,
        member_len: usize,
    ) -> Ptr;
    pub fn lhat_rs_result_status(r: Ptr) -> i32;
    pub fn lhat_rs_result_suspended(r: Ptr) -> bool;
    pub fn lhat_rs_result_count(r: Ptr) -> usize;
    pub fn lhat_rs_result_value(m: Ptr, r: Ptr, i: usize) -> Ptr;
    pub fn lhat_rs_result_error(m: Ptr, r: Ptr, out: *mut c_char, cap: usize) -> usize;
}
