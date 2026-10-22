#![forbid(unsafe_code)]
use lhat::{Error, Machine, Program, Scalar, ValueKind};
use std::{cell::RefCell, rc::Rc};

fn vm() -> Machine {
    Program::new(true).unwrap().build().unwrap()
}

#[test]
fn evaluate_and_retain_values_across_runs_and_collection() {
    let machine = vm();
    let function = machine
        .eval(
            "closure.lh",
            "let^ n = 40\nreturn^ f^x:number^ -> number^ { return^ n + x }",
        )
        .unwrap()
        .values
        .remove(0);
    for _ in 0..10 {
        machine
            .eval("garbage.lh", "return^ {a := \"temporary\"}")
            .unwrap();
        machine.collect_garbage().unwrap();
    }
    let two = machine.value(2_i64).unwrap();
    assert_eq!(
        machine.call(&function, &[two]).unwrap().values[0].scalar(),
        Scalar::Integer(42)
    );
    drop(machine);
    assert_eq!(function.kind(), ValueKind::Other);
    assert!(!function.to_text().is_empty());
}

#[test]
fn tables_and_binary_strings() {
    let machine = vm();
    let table = machine.table().unwrap();
    let key = machine.value("key").unwrap();
    let value = machine.value(vec![0, 255, b'a']).unwrap();
    machine.table_set(&table, &key, &value).unwrap();
    machine.collect_garbage().unwrap();
    assert_eq!(
        machine.table_get(&table, &key).unwrap().scalar(),
        Scalar::Bytes(vec![0, 255, b'a'])
    );
    assert!(value.equals(&value.clone()).unwrap());
    assert_eq!(
        machine.table_set(&table, &machine.value(Scalar::Nil).unwrap(), &value),
        Err(Error::InvalidOperation)
    );
    assert!(matches!(
        machine.table_get(&key, &key),
        Err(Error::InvalidOperation)
    ));
}

#[test]
fn rust_callbacks_capture_state_and_return_tuples() {
    let mut program = Program::new(true).unwrap();
    let count = Rc::new(RefCell::new(0));
    let captured = count.clone();
    program
        .register_function(
            "host",
            "pair",
            "f^number^ -> (number^, string^);",
            move |call| {
                *captured.borrow_mut() += 1;
                assert_eq!(call.len(), 1);
                assert_eq!(call.argument(0), Some(Scalar::Integer(21)));
                assert_eq!(call.argument(1), None);
                call.answer(42_i64)?;
                call.answer("answer")
            },
        )
        .unwrap();
    let machine = program.build().unwrap();
    let result = machine
        .eval("tuple.lh", "import^ host\nreturn^ host.pair(21)")
        .unwrap();
    assert_eq!(result.values.len(), 2);
    assert_eq!(result.values[0].scalar(), Scalar::Integer(42));
    assert_eq!(result.values[1].scalar(), Scalar::Bytes(b"answer".to_vec()));
    machine.collect_garbage().unwrap();
    assert_eq!(*count.borrow(), 1);
}

#[test]
fn callback_panics_and_errors_are_vm_faults() {
    let mut program = Program::new(true).unwrap();
    program
        .register_function("host", "panic", "f^ -> number^;", |_| panic!("test panic"))
        .unwrap();
    program
        .register_function("host", "error", "f^ -> number^;", |_| {
            Err(Error::InvalidOperation)
        })
        .unwrap();
    let machine = program.build().unwrap();
    for function in ["panic", "error"] {
        let error = machine.eval(
            "fault.lh",
            format!("import^ host\nreturn^ host.{function}()"),
        );
        assert!(matches!(error, Err(Error::Runtime { .. })), "{error:?}");
    }
    assert_eq!(
        machine.eval("recovery.lh", "return^ 42").unwrap().values[0].scalar(),
        Scalar::Integer(42)
    );
}

#[test]
fn foreign_values_are_rejected_before_ffi() {
    let a = vm();
    let b = vm();
    let foreign = b.value(1_i64).unwrap();
    let table = a.table().unwrap();
    assert!(matches!(a.call(&foreign, &[]), Err(Error::ForeignValue)));
    assert!(matches!(
        a.table_get(&table, &foreign),
        Err(Error::ForeignValue)
    ));
    assert_eq!(
        a.table_set(&table, &foreign, &foreign),
        Err(Error::ForeignValue)
    );
    assert_eq!(table.equals(&foreign), Err(Error::ForeignValue));
    let function = a.load("id.lh", "return^ 1").unwrap();
    assert!(matches!(
        a.call(&function, &[foreign]),
        Err(Error::ForeignValue)
    ));
}

#[test]
fn loader_units_and_diagnostics() {
    let program = Program::with_loader(true, |path| match path {
        "main.lh" => Some(b"require^\"lib.lh\"\nreturn^ 42".to_vec()),
        "lib.lh" => Some(b"module^ lib\npublic^let^answer = 42".to_vec()),
        _ => None,
    })
    .unwrap();
    let machine = program.build().unwrap();
    let body = machine.check("main.lh").unwrap();
    assert_eq!(
        machine.call(&body, &[]).unwrap().values[0].scalar(),
        Scalar::Integer(42)
    );
    assert!(matches!(
        machine.check("missing.lh"),
        Err(Error::Compile(_))
    ));
    assert!(matches!(
        machine.eval("bad.lh", "let^ x:number^ = \"wrong\""),
        Err(Error::Compile(_))
    ));
    assert!(matches!(
        machine.eval("bad\0.lh", ""),
        Err(Error::InvalidString)
    ));
}

#[test]
fn loader_panics_do_not_unwind_into_c() {
    let machine = Program::with_loader(true, |_| panic!("loader panic"))
        .unwrap()
        .build()
        .unwrap();
    assert!(matches!(
        machine.check("missing.lh"),
        Err(Error::Compile(_))
    ));
}

#[test]
fn coroutine_and_budget() {
    let machine = vm();
    let coroutine = machine
        .eval("yield.lh", "yield^ 10\nreturn^ 42")
        .unwrap()
        .values
        .remove(0);
    assert_eq!(coroutine.kind(), ValueKind::Coroutine);
    assert!(!coroutine.coroutine_done());
    assert_eq!(
        machine.resume(&coroutine, &[]).unwrap().values[0].scalar(),
        Scalar::Integer(10)
    );
    machine.collect_garbage().unwrap();
    assert_eq!(
        machine.resume(&coroutine, &[]).unwrap().values[0].scalar(),
        Scalar::Integer(42)
    );
    assert!(coroutine.coroutine_done());
    assert!(matches!(
        machine.resume(&coroutine, &[]),
        Err(Error::Runtime { .. })
    ));
    machine.set_budget(1).unwrap();
    let mut result = machine
        .eval(
            "loop.lh",
            "var^ n = 0\nfor^ i from^ 1 to^ 100 { n := n + i }\nreturn^ n",
        )
        .unwrap();
    assert!(result.suspended);
    assert!(matches!(
        machine.eval("other.lh", "return^ 1"),
        Err(Error::Suspended)
    ));
    for _ in 0..200 {
        if !result.suspended {
            break;
        }
        result = machine.continue_run().unwrap();
    }
    assert!(!result.suspended);
    assert_eq!(result.values[0].scalar(), Scalar::Integer(5050));
}

#[test]
fn reentrant_vm_execution_is_rejected() {
    let mut program = Program::new(true).unwrap();
    let handle = Rc::new(RefCell::new(None::<Machine>));
    let callback_handle = handle.clone();
    program
        .register_function("host", "check", "f^ -> number^;", move |call| {
            let nested = callback_handle
                .borrow()
                .as_ref()
                .unwrap()
                .eval("nested.lh", "return^ 0");
            assert!(matches!(nested, Err(Error::Busy)));
            call.answer(42_i64)
        })
        .unwrap();
    let machine = program.build().unwrap();
    *handle.borrow_mut() = Some(machine.clone());
    assert_eq!(
        machine
            .eval("outer.lh", "import^ host\nreturn^ host.check()")
            .unwrap()
            .values[0]
            .scalar(),
        Scalar::Integer(42)
    );
    // Capturing a VM strongly in its own callback forms an ordinary Rc cycle.
    handle.borrow_mut().take();
}

#[test]
fn independent_rust_threads_are_serialized() {
    let threads: Vec<_> = (0..4)
        .map(|_| {
            std::thread::spawn(|| {
                for _ in 0..4 {
                    let machine = vm();
                    assert_eq!(
                        machine.eval("thread.lh", "return^ 42").unwrap().values[0].scalar(),
                        Scalar::Integer(42)
                    );
                }
            })
        })
        .collect();
    for thread in threads {
        thread.join().unwrap();
    }
}

#[test]
fn captured_state_lives_until_last_value_is_dropped() {
    struct OnDrop(Rc<RefCell<bool>>);
    impl Drop for OnDrop {
        fn drop(&mut self) {
            *self.0.borrow_mut() = true;
        }
    }
    let dropped = Rc::new(RefCell::new(false));
    let state = OnDrop(dropped.clone());
    let mut program = Program::new(true).unwrap();
    program
        .register_function("host", "answer", "f^ -> number^;", move |call| {
            let _ = &state;
            call.answer(42_i64)
        })
        .unwrap();
    let machine = program.build().unwrap();
    let value = machine
        .eval("keep.lh", "import^ host\nreturn^ host.answer")
        .unwrap()
        .values
        .remove(0);
    drop(machine);
    assert!(!*dropped.borrow());
    drop(value);
    assert!(*dropped.borrow());
}

#[test]
fn primitive_constants_and_member_calls() {
    let mut program = Program::new(true).unwrap();
    program.register_constant("host", "answer", 42_i64).unwrap();
    program.register_constant("host", "enabled", true).unwrap();
    program.register_constant("host", "ratio", 0.5_f64).unwrap();
    program.register_constant("host", "name", "L^").unwrap();
    assert_eq!(
        program.register_constant("host", "nul", vec![0]),
        Err(Error::InvalidString)
    );
    let machine = program.build().unwrap();
    let result = machine
        .eval(
            "constants.lh",
            "import^ host\nreturn^ host.answer, host.enabled, host.ratio, host.name",
        )
        .unwrap();
    assert_eq!(
        result.values.iter().map(|v| v.scalar()).collect::<Vec<_>>(),
        vec![
            Scalar::Integer(42),
            Scalar::Bool(true),
            Scalar::Real(0.5),
            Scalar::from("L^")
        ]
    );
    let table = machine
        .eval(
            "member.lh",
            "return^ { double := f^x:number^ -> number^ { return^ x * 2 } }",
        )
        .unwrap()
        .values
        .remove(0);
    let result = machine
        .call_member(&table, "double", &[machine.value(21_i64).unwrap()])
        .unwrap();
    assert_eq!(result.values[0].scalar(), Scalar::Integer(42));
    assert!(matches!(
        machine.call_member(&table, "absent", &[]),
        Err(Error::Runtime { .. })
    ));
}

#[test]
fn callback_forwards_tables_without_exposing_raw_values() {
    let mut program = Program::new(true).unwrap();
    program
        .register_function("host", "identity", "f^t^{} -> t^{};", |call| {
            assert_eq!(call.argument(0), Some(Scalar::Other(ValueKind::Table)));
            assert_eq!(call.answer_argument(1), Err(Error::InvalidOperation));
            call.answer_argument(0)
        })
        .unwrap();
    let machine = program.build().unwrap();
    let value = machine
        .eval(
            "identity.lh",
            "import^ host\nreturn^ host.identity({ answer := 42 })",
        )
        .unwrap()
        .values
        .remove(0);
    machine.collect_garbage().unwrap();
    assert_eq!(
        machine
            .table_get(&value, &machine.value("answer").unwrap())
            .unwrap()
            .scalar(),
        Scalar::Integer(42)
    );
}

#[test]
fn project_root_is_used_by_rust_loader() {
    let mut program = Program::with_loader(true, |path| match path {
        "main.lh" => Some(b"require^\"$src/lib.lh\"\nreturn^ 42".to_vec()),
        "project/src/lib.lh" => Some(b"module^ lib\npublic^let^answer = 42".to_vec()),
        _ => None,
    })
    .unwrap();
    program.set_project_root("project").unwrap();
    let machine = program.build().unwrap();
    let script = machine.check("main.lh").unwrap();
    assert_eq!(
        machine.call(&script, &[]).unwrap().values[0].scalar(),
        Scalar::Integer(42)
    );
}
