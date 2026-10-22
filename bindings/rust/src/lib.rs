//! Safe Rust embedding for L^.
//!
//! ```
//! use lhat::{Program, Scalar};
//! let vm = Program::new(true)?.build()?;
//! let result = vm.eval("example.lh", "return^ 6 * 7")?;
//! assert_eq!(result.values[0].scalar(), Scalar::Integer(42));
//! # Ok::<(), lhat::Error>(())
//! ```
//!
//! Values root their objects and keep their VM (and program) alive. Cloning a
//! value shares the root. Programs, VMs and values are confined to their thread;
//! calls into the C library are serialized across independent Rust threads.
//!
//! A VM cannot be moved to a different thread:
//! ```compile_fail
//! let machine = lhat::Program::new(true).unwrap().build().unwrap();
//! std::thread::spawn(move || machine.collect_garbage());
//! ```
mod ffi;
use std::{
    cell::{Cell, RefCell},
    ffi::{CStr, CString},
    fmt,
    marker::PhantomData,
    panic::{catch_unwind, AssertUnwindSafe},
    ptr::{self, NonNull},
    rc::Rc,
    sync::Mutex,
};

pub type Result<T> = std::result::Result<T, Error>;

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Error {
    OutOfMemory,
    InvalidString,
    Registration,
    Compile(String),
    Runtime { status: i32, message: String },
    ForeignValue,
    Busy,
    Suspended,
    InvalidOperation,
}
impl fmt::Display for Error {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::Compile(s) | Self::Runtime { message: s, .. } => f.write_str(s),
            other => f.write_str(match other {
                Self::OutOfMemory => "L^ allocation failed or value cannot be retained",
                Self::InvalidString => "a name or path contains NUL",
                Self::Registration => "L^ rejected the host registration",
                Self::ForeignValue => "value belongs to a different VM",
                Self::Busy => "VM is already executing a Rust API operation",
                Self::Suspended => "continue the suspended run before starting another operation",
                _ => "operation is not supported for this value",
            }),
        }
    }
}
impl std::error::Error for Error {}

// The C registry has process-wide mutable identities. Nested calls from a
// callback already hold this lock on the same thread. No pointer escapes it
// except program/VM/value handles whose owners enforce their lifetimes.
static API: Mutex<()> = Mutex::new(());
thread_local! { static IN_API: Cell<bool> = const { Cell::new(false) }; }
struct Reset<'a>(&'a Cell<bool>);
impl Drop for Reset<'_> {
    fn drop(&mut self) {
        self.0.set(false);
    }
}
fn api<T>(f: impl FnOnce() -> T) -> T {
    IN_API.with(|entered| {
        if entered.get() {
            return f();
        }
        let _lock = API.lock().unwrap_or_else(|e| e.into_inner());
        entered.set(true);
        let _reset = Reset(entered);
        f()
    })
}
fn name(s: &str) -> Result<CString> {
    CString::new(s).map_err(|_| Error::InvalidString)
}
// SAFETY: callers supply a live C string or NULL, and copy before returning.
unsafe fn string(p: *const std::ffi::c_char) -> String {
    if p.is_null() {
        String::new()
    } else {
        CStr::from_ptr(p).to_string_lossy().into_owned()
    }
}
fn text(mut write: impl FnMut(*mut std::ffi::c_char, usize) -> usize) -> String {
    let length = write(ptr::null_mut(), 0);
    let mut bytes = vec![0; length + 1];
    write(bytes.as_mut_ptr().cast(), bytes.len());
    bytes.truncate(length);
    String::from_utf8_lossy(&bytes).into_owned()
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ValueKind {
    Nil,
    Bool,
    Integer,
    Real,
    Bytes,
    Table,
    Coroutine,
    Other,
}

/// Owned primitive values for host arguments and answers. Strings are bytes,
/// so embedded NUL and non-UTF-8 data survive crossing the boundary.
#[derive(Debug, Clone, PartialEq)]
pub enum Scalar {
    Nil,
    Bool(bool),
    Integer(i64),
    Real(f64),
    Bytes(Vec<u8>),
    /// A non-primitive argument. Use `Callback::answer_argument` to forward it.
    Other(ValueKind),
}
impl From<i64> for Scalar {
    fn from(v: i64) -> Self {
        Self::Integer(v)
    }
}
impl From<f64> for Scalar {
    fn from(v: f64) -> Self {
        Self::Real(v)
    }
}
impl From<bool> for Scalar {
    fn from(v: bool) -> Self {
        Self::Bool(v)
    }
}
impl From<&str> for Scalar {
    fn from(v: &str) -> Self {
        Self::Bytes(v.as_bytes().to_vec())
    }
}
impl From<Vec<u8>> for Scalar {
    fn from(v: Vec<u8>) -> Self {
        Self::Bytes(v)
    }
}
impl From<String> for Scalar {
    fn from(v: String) -> Self {
        Self::Bytes(v.into_bytes())
    }
}
impl From<&[u8]> for Scalar {
    fn from(v: &[u8]) -> Self {
        Self::Bytes(v.to_vec())
    }
}
impl Scalar {
    fn data(&self) -> Result<ffi::Data> {
        let mut d = ffi::Data::default();
        match self {
            Self::Nil => (),
            Self::Bool(v) => {
                d.kind = 1;
                d.integer = i64::from(*v);
            }
            Self::Integer(v) => {
                d.kind = 2;
                d.integer = *v;
            }
            Self::Real(v) => {
                d.kind = 3;
                d.real = *v;
            }
            Self::Bytes(v) => {
                d.kind = 4;
                d.bytes = v.as_ptr();
                d.length = v.len();
            }
            Self::Other(_) => return Err(Error::InvalidOperation),
        }
        Ok(d)
    }
}
fn kind(d: &ffi::Data) -> ValueKind {
    match d.kind {
        0 => ValueKind::Nil,
        1 => ValueKind::Bool,
        2 => ValueKind::Integer,
        3 => ValueKind::Real,
        4 => ValueKind::Bytes,
        6 => ValueKind::Table,
        7 => ValueKind::Coroutine,
        _ => ValueKind::Other,
    }
}
// SAFETY: byte data is borrowed from a rooted value or the current callback.
unsafe fn scalar(d: ffi::Data) -> Scalar {
    match d.kind {
        0 => Scalar::Nil,
        1 => Scalar::Bool(d.integer != 0),
        2 => Scalar::Integer(d.integer),
        3 => Scalar::Real(d.real),
        4 => Scalar::Bytes(std::slice::from_raw_parts(d.bytes, d.length).to_vec()),
        _ => Scalar::Other(kind(&d)),
    }
}

type HostFn = dyn FnMut(&mut Callback<'_>) -> Result<()>;
struct Host(RefCell<Box<HostFn>>);
type LoadFn = dyn FnMut(&str) -> Option<Vec<u8>>;
struct Loader(RefCell<Box<LoadFn>>);

/// Builder for an L^ program. Register host functions before `build`.
pub struct Program {
    raw: NonNull<std::ffi::c_void>,
    // Boxes keep callback addresses stable when the vector grows.
    #[allow(clippy::vec_box)]
    hosts: Vec<Box<Host>>,
    _loader: Option<Box<Loader>>,
    _local: PhantomData<Rc<()>>,
}
impl Program {
    /// Creates a program with no filesystem access. `strict` enables checking.
    pub fn new(strict: bool) -> Result<Self> {
        Self::create(strict, None, ptr::null_mut(), None)
    }
    /// Uses the default UTF-8 filesystem loader for `require^` and `check`.
    pub fn with_filesystem(strict: bool) -> Result<Self> {
        Self::create(strict, Some(ffi::lhat_load_file), ptr::null_mut(), None)
    }
    /// Loader paths are normalized by L^. Panics are contained and reported as
    /// an unreadable unit; returned bytes are copied to the C allocator.
    pub fn with_loader(
        strict: bool,
        loader: impl FnMut(&str) -> Option<Vec<u8>> + 'static,
    ) -> Result<Self> {
        let mut loader = Box::new(Loader(RefCell::new(Box::new(loader))));
        let context = (&mut *loader as *mut Loader).cast();
        Self::create(strict, Some(load), context, Some(loader))
    }
    fn create(
        strict: bool,
        load: Option<ffi::Loader>,
        context: ffi::Ptr,
        loader: Option<Box<Loader>>,
    ) -> Result<Self> {
        api(|| {
            // SAFETY: loader context is boxed and owned until after program_free.
            let raw = NonNull::new(unsafe { ffi::lhat_program_new(strict, load, context) })
                .ok_or(Error::OutOfMemory)?;
            Ok(Self {
                raw,
                hosts: Vec::new(),
                _loader: loader,
                _local: PhantomData,
            })
        })
    }
    pub fn set_project_root(&mut self, path: &str) -> Result<()> {
        let path = name(path)?;
        api(|| {
            if unsafe { ffi::lhat_program_set_project_root(self.raw.as_ptr(), path.as_ptr()) } {
                Ok(())
            } else {
                Err(Error::InvalidOperation)
            }
        })
    }
    /// Registers a Rust closure using an L^ signature, e.g.
    /// `f^x:number^ -> number^`. Captured Rust state lives with the program.
    pub fn register_function(
        &mut self,
        module: &str,
        function: &str,
        signature: &str,
        callback: impl FnMut(&mut Callback<'_>) -> Result<()> + 'static,
    ) -> Result<()> {
        let (module, function, signature) = (name(module)?, name(function)?, name(signature)?);
        let mut host = Box::new(Host(RefCell::new(Box::new(callback))));
        let context = (&mut *host as *mut Host).cast();
        // Keep the context even if C rejects a partially installed registration.
        self.hosts.push(host);
        api(|| {
            if unsafe {
                ffi::lhat_rs_register(
                    self.raw.as_ptr(),
                    module.as_ptr(),
                    function.as_ptr(),
                    signature.as_ptr(),
                    call_host,
                    context,
                )
            } {
                Ok(())
            } else {
                Err(Error::Registration)
            }
        })
    }
    /// Registers a primitive module constant. The C constant API accepts only
    /// NUL-free strings; use `Machine::value` for arbitrary byte strings.
    pub fn register_constant(
        &mut self,
        module: &str,
        constant: &str,
        value: impl Into<Scalar>,
    ) -> Result<()> {
        let (module, constant) = (name(module)?, name(constant)?);
        let value = value.into();
        if matches!(&value, Scalar::Bytes(bytes) if bytes.contains(&0)) {
            return Err(Error::InvalidString);
        }
        api(|| {
            if unsafe {
                ffi::lhat_rs_register_constant(
                    self.raw.as_ptr(),
                    module.as_ptr(),
                    constant.as_ptr(),
                    value.data()?,
                )
            } {
                Ok(())
            } else {
                Err(Error::Registration)
            }
        })
    }
    pub fn build(self) -> Result<Machine> {
        api(|| {
            let raw = NonNull::new(unsafe { ffi::lhat_machine_new() }).ok_or(Error::OutOfMemory)?;
            if !unsafe { ffi::lhat_program_install(self.raw.as_ptr(), raw.as_ptr()) } {
                unsafe { ffi::lhat_machine_dispose(raw.as_ptr()) };
                return Err(Error::OutOfMemory);
            }
            Ok(Machine(Rc::new(Engine {
                raw,
                program: self,
                busy: Cell::new(false),
            })))
        })
    }
}
impl Drop for Program {
    fn drop(&mut self) {
        // SAFETY: Engine disposes its VM first; callback boxes are still alive.
        api(|| unsafe { ffi::lhat_program_free(self.raw.as_ptr()) });
    }
}

unsafe extern "C" fn load(
    context: ffi::Ptr,
    path: *const std::ffi::c_char,
    length: *mut usize,
) -> *mut std::ffi::c_char {
    catch_unwind(AssertUnwindSafe(|| {
        // SAFETY: C calls only while Program owns this loader and output slot.
        let loader = &*(context as *const Loader);
        let bytes = (loader.0.borrow_mut())(&string(path))?;
        let memory = ffi::lhat_alloc(bytes.len().checked_add(1)?).cast::<u8>();
        if memory.is_null() {
            return None;
        }
        ptr::copy_nonoverlapping(bytes.as_ptr(), memory, bytes.len());
        *memory.add(bytes.len()) = 0;
        *length = bytes.len();
        Some(memory.cast())
    }))
    .ok()
    .flatten()
    .unwrap_or(ptr::null_mut())
}

/// A host invocation. Borrowed VM arguments cannot escape this callback.
pub struct Callback<'a> {
    raw: ffi::Ptr,
    _call: PhantomData<&'a mut ()>,
}
impl Callback<'_> {
    pub fn len(&self) -> usize {
        unsafe { ffi::lhat_rs_arg_count(self.raw) }
    }
    pub fn is_empty(&self) -> bool {
        self.len() == 0
    }
    pub fn argument(&self, index: usize) -> Option<Scalar> {
        let mut d = ffi::Data::default();
        unsafe { ffi::lhat_rs_arg(self.raw, index, &mut d).then(|| scalar(d)) }
    }
    pub fn answer(&mut self, value: impl Into<Scalar>) -> Result<()> {
        let value = value.into();
        if unsafe { ffi::lhat_rs_answer(self.raw, value.data()?) } {
            Ok(())
        } else {
            Err(Error::InvalidOperation)
        }
    }
    /// Forwards a rooted VM argument, including a table or closure.
    pub fn answer_argument(&mut self, index: usize) -> Result<()> {
        if unsafe { ffi::lhat_rs_answer_arg(self.raw, index) } {
            Ok(())
        } else {
            Err(Error::InvalidOperation)
        }
    }
}
unsafe extern "C" fn call_host(context: ffi::Ptr, raw: ffi::Ptr) {
    // No Rust unwind may cross C frames, including a RefCell reentry panic.
    let result = catch_unwind(AssertUnwindSafe(|| {
        let host = &*(context as *const Host);
        (host.0.borrow_mut())(&mut Callback {
            raw,
            _call: PhantomData,
        })
    }));
    let message = match result {
        Ok(Ok(())) => return,
        Ok(Err(e)) => e.to_string(),
        Err(_) => "Rust host callback panicked".into(),
    };
    let message = CString::new(message.replace('\0', "\\0")).unwrap();
    ffi::lhat_rs_callback_error(raw, message.as_ptr());
}

struct Engine {
    raw: NonNull<std::ffi::c_void>,
    program: Program,
    busy: Cell<bool>,
}
impl Drop for Engine {
    fn drop(&mut self) {
        // SAFETY: all Value roots have gone; program and callbacks remain alive.
        api(|| unsafe { ffi::lhat_machine_dispose(self.raw.as_ptr()) });
    }
}
/// An L^ VM and its program. Clones share the same VM.
#[derive(Clone)]
pub struct Machine(Rc<Engine>);
/// Results are rooted immediately, including every tuple position.
#[derive(Debug)]
pub struct RunResult {
    pub values: Vec<Value>,
    pub suspended: bool,
}
impl Machine {
    fn enter<T>(&self, allow_suspended: bool, f: impl FnOnce(ffi::Ptr) -> Result<T>) -> Result<T> {
        api(|| {
            if self.0.busy.replace(true) {
                return Err(Error::Busy);
            }
            let _reset = Reset(&self.0.busy);
            let m = self.0.raw.as_ptr();
            if !allow_suspended && unsafe { ffi::lhat_machine_is_suspended(m) } {
                return Err(Error::Suspended);
            }
            f(m)
        })
    }
    fn own(&self, raw: ffi::Ptr) -> Result<Value> {
        let raw = NonNull::new(raw).ok_or(Error::OutOfMemory)?;
        Ok(Value(Rc::new(Root {
            raw,
            engine: self.0.clone(),
        })))
    }
    fn check_value(&self, value: &Value) -> Result<()> {
        if Rc::ptr_eq(&self.0, &value.0.engine) {
            Ok(())
        } else {
            Err(Error::ForeignValue)
        }
    }
    pub fn value(&self, value: impl Into<Scalar>) -> Result<Value> {
        let value = value.into();
        self.enter(false, |m| {
            self.own(unsafe { ffi::lhat_rs_value_new(m, value.data()?) })
        })
    }
    pub fn table(&self) -> Result<Value> {
        self.enter(false, |m| self.own(unsafe { ffi::lhat_rs_table_new(m) }))
    }
    /// Checks and compiles a script. Dependencies use the program's loader.
    pub fn load(&self, script_name: &str, source: impl AsRef<[u8]>) -> Result<Value> {
        let script_name = name(script_name)?;
        let bytes = source.as_ref();
        self.enter(false, |m| {
            let raw = unsafe {
                ffi::lhat_rs_load(
                    self.0.program.raw.as_ptr(),
                    m,
                    script_name.as_ptr(),
                    bytes.as_ptr(),
                    bytes.len(),
                )
            };
            if raw.is_null() {
                Err(Error::Compile(unsafe {
                    string(ffi::lhat_program_load_failure(self.0.program.raw.as_ptr()))
                }))
            } else {
                self.own(raw)
            }
        })
    }
    /// Checks a loader-provided unit and returns its compiled callable body.
    pub fn check(&self, path: &str) -> Result<Value> {
        let path = name(path)?;
        self.enter(false, |m| {
            let raw = unsafe { ffi::lhat_rs_check(self.0.program.raw.as_ptr(), m, path.as_ptr()) };
            if raw.is_null() {
                Err(Error::Compile(self.diagnostics_inner()))
            } else {
                self.own(raw)
            }
        })
    }
    fn diagnostics_inner(&self) -> String {
        unsafe {
            let p = self.0.program.raw.as_ptr();
            let mut reports = Vec::new();
            for i in 0..ffi::lhat_program_diagnostic_count(p) {
                reports.push(text(|out, cap| {
                    ffi::lhat_rs_program_diagnostic(p, i, out, cap)
                }));
            }
            let mut u = ffi::lhat_program_units(p);
            while !u.is_null() {
                for i in 0..ffi::lhat_unit_diagnostic_count(u) {
                    reports.push(text(|out, cap| {
                        ffi::lhat_unit_diagnostic_write(u, i, false, out, cap)
                    }));
                }
                u = ffi::lhat_unit_next(u);
            }
            if reports.is_empty() {
                reports.push(format!(
                    "unit could not be checked ({} loader diagnostics); {}",
                    ffi::lhat_program_diagnostic_count(p),
                    string(ffi::lhat_compile_status_message(
                        ffi::lhat_program_compile_status(p)
                    ))
                ));
            }
            reports.join("\n")
        }
    }
    pub fn eval(&self, name: &str, source: impl AsRef<[u8]>) -> Result<RunResult> {
        self.call(&self.load(name, source)?, &[])
    }
    pub fn call(&self, callee: &Value, arguments: &[Value]) -> Result<RunResult> {
        self.run(0, Some(callee), arguments, "")
    }
    pub fn call_member(
        &self,
        receiver: &Value,
        member: &str,
        arguments: &[Value],
    ) -> Result<RunResult> {
        self.run(3, Some(receiver), arguments, member)
    }
    pub fn resume(&self, coroutine: &Value, sent: &[Value]) -> Result<RunResult> {
        self.run(1, Some(coroutine), sent, "")
    }
    pub fn continue_run(&self) -> Result<RunResult> {
        self.run(2, None, &[], "")
    }
    fn run(
        &self,
        mode: i32,
        callee: Option<&Value>,
        arguments: &[Value],
        member: &str,
    ) -> Result<RunResult> {
        if let Some(v) = callee {
            self.check_value(v)?;
        }
        for v in arguments {
            self.check_value(v)?;
        }
        let args: Vec<_> = arguments.iter().map(|v| v.0.raw.as_ptr()).collect();
        self.enter(mode == 2, |m| unsafe {
            let raw = ffi::lhat_rs_run(
                m,
                mode,
                callee.map_or(ptr::null_mut(), |v| v.0.raw.as_ptr()),
                args.as_ptr(),
                args.len(),
                member.as_ptr(),
                member.len(),
            );
            let raw = NonNull::new(raw).ok_or(Error::OutOfMemory)?;
            struct Free(NonNull<std::ffi::c_void>);
            impl Drop for Free {
                fn drop(&mut self) {
                    unsafe { ffi::lhat_free(self.0.as_ptr()) };
                }
            }
            let _free = Free(raw);
            let raw = raw.as_ptr();
            let suspended = ffi::lhat_rs_result_suspended(raw);
            let status = ffi::lhat_rs_result_status(raw);
            if status != 0 && !suspended {
                return Err(Error::Runtime {
                    status,
                    message: text(|out, cap| ffi::lhat_rs_result_error(m, raw, out, cap)),
                });
            }
            let mut values = Vec::new();
            if !suspended {
                for i in 0..ffi::lhat_rs_result_count(raw) {
                    values.push(self.own(ffi::lhat_rs_result_value(m, raw, i))?);
                }
            }
            Ok(RunResult { values, suspended })
        })
    }
    pub fn set_budget(&self, turns: u32) -> Result<()> {
        self.enter(true, |m| {
            unsafe { ffi::lhat_machine_set_budget(m, turns.into()) };
            Ok(())
        })
    }
    pub fn collect_garbage(&self) -> Result<usize> {
        self.enter(false, |m| Ok(unsafe { ffi::lhat_machine_gc_collect(m) }))
    }
    pub fn table_get(&self, table: &Value, key: &Value) -> Result<Value> {
        self.check_value(table)?;
        self.check_value(key)?;
        if table.kind() != ValueKind::Table {
            return Err(Error::InvalidOperation);
        }
        self.enter(false, |m| {
            self.own(unsafe { ffi::lhat_rs_table_get(m, table.0.raw.as_ptr(), key.0.raw.as_ptr()) })
        })
    }
    pub fn table_set(&self, table: &Value, key: &Value, value: &Value) -> Result<()> {
        for v in [table, key, value] {
            self.check_value(v)?;
        }
        self.enter(false, |m| {
            if unsafe {
                ffi::lhat_rs_table_set(
                    m,
                    table.0.raw.as_ptr(),
                    key.0.raw.as_ptr(),
                    value.0.raw.as_ptr(),
                )
            } {
                Ok(())
            } else {
                Err(Error::InvalidOperation)
            }
        })
    }
}

struct Root {
    raw: NonNull<std::ffi::c_void>,
    engine: Rc<Engine>,
}
impl Drop for Root {
    fn drop(&mut self) {
        // SAFETY: this is the final clone; Engine is retained by this root.
        api(|| unsafe { ffi::lhat_rs_value_free(self.engine.raw.as_ptr(), self.raw.as_ptr()) });
    }
}
/// A GC root owned by Rust. May outlive every `Machine` handle safely.
#[derive(Clone)]
pub struct Value(Rc<Root>);
impl Value {
    pub fn kind(&self) -> ValueKind {
        api(|| kind(&unsafe { ffi::lhat_rs_value_data(self.0.raw.as_ptr()) }))
    }
    pub fn scalar(&self) -> Scalar {
        api(|| unsafe { scalar(ffi::lhat_rs_value_data(self.0.raw.as_ptr())) })
    }
    pub fn to_text(&self) -> String {
        api(|| text(|out, cap| unsafe { ffi::lhat_rs_value_text(self.0.raw.as_ptr(), out, cap) }))
    }
    pub fn coroutine_done(&self) -> bool {
        api(|| unsafe { ffi::lhat_rs_coroutine_done(self.0.raw.as_ptr()) })
    }
    pub fn equals(&self, other: &Value) -> Result<bool> {
        if !Rc::ptr_eq(&self.0.engine, &other.0.engine) {
            return Err(Error::ForeignValue);
        }
        Ok(api(|| unsafe {
            ffi::lhat_rs_value_equal(self.0.raw.as_ptr(), other.0.raw.as_ptr())
        }))
    }
}
impl fmt::Debug for Value {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_tuple("Value").field(&self.to_text()).finish()
    }
}
