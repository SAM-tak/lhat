#![forbid(unsafe_code)]
use lhat::{Program, Scalar};

fn main() -> lhat::Result<()> {
    let mut program = Program::new(true)?;
    program.register_function("host", "double", "f^number^ -> number^;", |call| match call
        .argument(0)
    {
        Some(Scalar::Integer(value)) => call.answer(value * 2),
        Some(Scalar::Real(value)) => call.answer(value * 2.0),
        _ => Err(lhat::Error::InvalidOperation),
    })?;
    let machine = program.build()?;
    let result = machine.eval("main.lh", "import^ host\nreturn^ host.double(21)")?;
    println!("{}", result.values[0].to_text());
    Ok(())
}
