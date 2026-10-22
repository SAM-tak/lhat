use std::{env, path::PathBuf, process::Command};

fn run(command: &mut Command) {
    assert!(
        command.status().expect("cannot run CMake").success(),
        "CMake failed"
    );
}

fn main() {
    let manifest = PathBuf::from(env::var_os("CARGO_MANIFEST_DIR").unwrap());
    let root = manifest.join("../..");
    for path in ["src", "include", "port", "CMakeLists.txt"] {
        println!("cargo:rerun-if-changed={}", root.join(path).display());
    }
    println!("cargo:rerun-if-changed=native");
    for key in ["CMAKE_GENERATOR", "CMAKE_C_COMPILER", "CC"] {
        println!("cargo:rerun-if-env-changed={key}");
    }
    let target = env::var("TARGET").unwrap();
    assert_eq!(
        env::var("HOST").unwrap(),
        target,
        "cross compilation needs a CMake toolchain; currently only native builds are supported"
    );
    let out = PathBuf::from(env::var_os("OUT_DIR").unwrap()).join("native");
    let mut configure = Command::new("cmake");
    configure
        .arg("-S")
        .arg(manifest.join("native"))
        .arg("-B")
        .arg(&out)
        .arg("-DCMAKE_BUILD_TYPE=Release")
        .arg("-DCMAKE_POSITION_INDEPENDENT_CODE=ON");
    if let Some(generator) = env::var_os("CMAKE_GENERATOR") {
        configure.arg("-G").arg(generator);
    }
    if let Some(compiler) = env::var_os("CMAKE_C_COMPILER").or_else(|| env::var_os("CC")) {
        configure.arg(format!("-DCMAKE_C_COMPILER={}", compiler.to_string_lossy()));
    }
    if target.contains("msvc") {
        assert!(
            !env::var("CARGO_CFG_TARGET_FEATURE")
                .unwrap_or_default()
                .split(',')
                .any(|x| x == "crt-static"),
            "static CRT is not supported by the bundled CMake build"
        );
        configure.arg("-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreadedDLL");
    }
    run(&mut configure);
    run(Command::new("cmake").arg("--build").arg(&out).args([
        "--config",
        "Release",
        "--target",
        "lhat_rust_bridge",
        "--parallel",
    ]));
    println!(
        "cargo:rustc-link-search=native={}",
        out.join("lib").display()
    );
    for library in ["lhat_rust_bridge", "lhat", "lhatport"] {
        println!("cargo:rustc-link-lib=static={library}");
    }
    if target.contains("linux") || target.contains("freebsd") {
        println!("cargo:rustc-link-lib=m");
    }
}
