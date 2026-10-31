# Binding examples from the standard library

`stdlib/` is implemented using the public L^ C API.
The same registration APIs and callbacks expose functions and types from your own application.

| What to expose | Implementation to read |
| --- | --- |
| Host functions | [math.c](https://github.com/SAM-tak/lhat/blob/main/stdlib/math.c): reading numeric arguments, returning values, and registering function tables |
| hostvalue (value types) | [mathcomplex.c](https://github.com/SAM-tak/lhat/blob/main/stdlib/mathcomplex.c) and [mathvalue_impl.h](https://github.com/SAM-tak/lhat/blob/main/stdlib/mathvalue_impl.h): a complex type, fields, operators, and constructor |
| hostdata (references to host objects) | [io.c](https://github.com/SAM-tak/lhat/blob/main/stdlib/io.c): wrapping `FILE *`, methods, and releasing resources through `dispose` |

## A complete small module

This is the actual source of `stdlib/mathcomplex.c`.

<<< @/../stdlib/mathcomplex.c{c}

The file specifies a module name, type name, component count, and field names,
then uses the shared callbacks and registration code in `mathvalue_impl.h`.
The macros and shared header are how the standard library reuses its math implementations;
your own bindings do not need to follow this file structure.

Calling `lhatstdlib_mathcomplex_register(program)` before type checking registers
`std.math.complex.Complex`, its members, and `new`, `zero`, `one`, and `i`.
`zero`, `one`, and `i` are functions returning values, so scripts call them as `complex.i()`.

The following excerpts from `mathvalue_impl.h` show the actual C API calls.

## 1. Give C data a value-type tag

The shared implementation stores the value and registration context as follows:

```c
typedef struct {
    const LhatHostValueTag *tag;
} MathModule;

typedef struct {
    float component[MATH_DIM];
} MathValue;
```

For complex numbers, `MATH_DIM` is 2, so the C data contains two floats for the real and imaginary parts.
The beginning of `math_register` registers the data size and its components:

```c
    module->tag = lhat_register_hostvalue_type(program, MATH_MODULE, MATH_TYPE,
                                               sizeof(MathValue));
    if (module->tag == NULL) {
        return false;
    }
    for (size_t i = 0; i < MATH_DIM; i++) {
        if (!lhat_register_hostvalue_field(program, MATH_MODULE, MATH_TYPE,
                                           math_names[i], i * sizeof(float),
                                           LHAT_HVFIELD_F32)) {
            return false;
        }
    }
```

Here `re` and `im` become fields at offsets 0 and `sizeof(float)`.
The tag identifies the host value type when callbacks later read or produce values.

## 2. Read arguments and return a value

The callback implementing `new(re, im)` is:

```c
static void math_new(LhatMachine *machine, void *context,
                     const LhatValue *args, size_t count,
                     LhatValue *answers, int *answer_count)
{
    if (count < MATH_DIM) {
        return;
    }
    MathValue v = { { 0 } };
    for (size_t i = 0; i < MATH_DIM; i++) {
        if (!lhat_is_number(args[i])) {
            return;
        }
        v.component[i] = (float)math_real(args[i]);
    }
    answers[0] = math_value(machine, (const MathModule *)context, v);
    *answer_count = 1;
}
```

`args` and `count` are the script's arguments; `context` is the `MathModule *` supplied during registration.
`math_real` reads either integer or real numeric representation.
The callback writes `answers[0]` and sets `*answer_count = 1` to report one return value.

The `math_value` helper returns C data as an L^ value type:

```c
static LhatValue math_value(LhatMachine *machine, const MathModule *module,
                            MathValue v)
{
    LhatValue out = lhat_nil();
    return lhat_make_hostvalue(machine, module->tag, &v, &out) ? out : lhat_nil();
}
```

`lhat_make_hostvalue` copies the C data to return-value storage; it does not retain a pointer to local variable `v`.

## 3. Register the name, signature, and implementation

The `new` registration at the end of `math_register` uses this C API call:

```c
           lhat_register_func(program, MATH_MODULE, "new", constructor_signature,
                              math_new, module);
```

| Argument | Meaning for the complex module |
| --- | --- |
| `program` | Program that checks and runs code using this API |
| `MATH_MODULE` | `std.math.complex` |
| `"new"` | Function name visible to scripts |
| `constructor_signature` | `f^number^, number^ -> std.math.complex.Complex;` |
| `math_new` | C function called at runtime |
| `module` | Context holding the type tag and supplied to the callback |

Operator registration follows the same pattern. `math_register` uses `lhat_register_hostvalue_member`
to connect names such as `+` and their types to C functions such as `math_add`.
The receiver of a member taking `self^` is passed as a callback argument too.

For your application, substitute the names, signatures, C implementations, and context for the functionality you want to expose.
Register before type checking, then use `lhat_program_install` to place the registrations on the machine before execution.

## Read a hostdata example too

In `stdlib/io.c`, `lhatstdlib_io_register` registers `std.io.File` with `lhat_register_hostdata_type`
and members such as `readLine`, `write`, and `dispose` with `lhat_register_member`.
Callbacks check the tag and retrieve a `File *` with `lhat_hostdata_pointer`, then use its `FILE *`.
`file_dispose` closes the stream and frees the wrapper's data.

Use the complex hostvalue example for inline numeric data and the File hostdata example for references to host resources,
adapting their lifetime choices to your own types.
