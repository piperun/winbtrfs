use std::{env, fs, path::PathBuf, process::Command};

fn run(program: &str, args: &[String], commands: &mut Vec<String>) {
    commands.push(format!("{} {}", program, args.join(" ")));
    let status = Command::new(program)
        .args(args)
        .status()
        .unwrap_or_else(|e| panic!("cannot start {program}: {e}"));
    assert!(status.success(), "{program} failed with {status}");
}

fn checked_output(program: &str, args: &[&std::path::Path]) -> String {
    let output = Command::new(program)
        .args(args)
        .output()
        .unwrap_or_else(|e| panic!("cannot start {program}: {e}"));
    assert!(
        output.status.success(),
        "{program} failed: {}",
        String::from_utf8_lossy(&output.stderr)
    );
    String::from_utf8(output.stdout).expect("tool output is not UTF-8")
}

fn assert_export(nm: &str, object: &std::path::Path, symbol: &str) {
    let output = Command::new(nm)
        .args(["-g", "--defined-only"])
        .arg(object)
        .output()
        .unwrap_or_else(|e| panic!("cannot start {nm}: {e}"));
    assert!(
        output.status.success(),
        "{nm} failed: {}",
        String::from_utf8_lossy(&output.stderr)
    );
    let defined = String::from_utf8(output.stdout).expect("nm output is not UTF-8");
    let exports: Vec<_> = defined
        .lines()
        .filter_map(|line| line.split_whitespace().last())
        .collect();
    assert_eq!(
        exports,
        [symbol],
        "reference object must export only {symbol}: {defined}"
    );
}

fn main() {
    let crate_dir = PathBuf::from(env::var("CARGO_MANIFEST_DIR").unwrap());
    let repo = crate_dir.ancestors().nth(3).unwrap();
    let target = env::var("TARGET").unwrap();
    let windows = target == "x86_64-pc-windows-gnu";
    assert!(
        windows || target == "x86_64-unknown-linux-gnu",
        "unsupported target: {target}"
    );
    let cc = env::var("BTRFS_RANGE_CC").unwrap_or_else(|_| {
        if windows {
            "x86_64-w64-mingw32-gcc".into()
        } else {
            "gcc".into()
        }
    });
    let ar = env::var("BTRFS_RANGE_AR").unwrap_or_else(|_| {
        if windows {
            "x86_64-w64-mingw32-ar".into()
        } else {
            "ar".into()
        }
    });
    let nm = env::var("BTRFS_RANGE_NM").unwrap_or_else(|_| {
        if windows {
            "x86_64-w64-mingw32-nm".into()
        } else {
            "nm".into()
        }
    });
    let objcopy = env::var("BTRFS_RANGE_OBJCOPY").unwrap_or_else(|_| {
        if windows {
            "x86_64-w64-mingw32-objcopy".into()
        } else {
            "objcopy".into()
        }
    });
    let objdump = env::var("BTRFS_RANGE_OBJDUMP").unwrap_or_else(|_| {
        if windows {
            "x86_64-w64-mingw32-objdump".into()
        } else {
            "objdump".into()
        }
    });
    let rustc = env::var("BTRFS_RANGE_RUSTC").unwrap_or_else(|_| env::var("RUSTC").unwrap());
    let out = PathBuf::from(env::var("OUT_DIR").unwrap());
    let c_obj = out.join("range-c.o");
    let baseline_obj = out.join("baseline.o");
    let rust_obj = out.join("range-rust.o");
    let reference_obj = out.join("range-reference.o");
    let archive = out.join("librange_helpers.a");
    let mut commands = Vec::new();
    run(
        &cc,
        &[
            "-std=c11".into(),
            "-O2".into(),
            "-fno-lto".into(),
            "-Wall".into(),
            "-Wextra".into(),
            "-Werror".into(),
            "-Dbtrfs_clip_byte_range=btrfs_clip_byte_range_c".into(),
            "-I".into(),
            repo.join("src").display().to_string(),
            "-c".into(),
            repo.join("src/range-math.c").display().to_string(),
            "-o".into(),
            c_obj.display().to_string(),
        ],
        &mut commands,
    );
    run(
        &cc,
        &[
            "-std=c11".into(),
            "-O2".into(),
            "-fno-lto".into(),
            "-Wall".into(),
            "-Wextra".into(),
            "-Werror".into(),
            "-c".into(),
            crate_dir.join("baseline.c").display().to_string(),
            "-o".into(),
            baseline_obj.display().to_string(),
        ],
        &mut commands,
    );
    run(
        &rustc,
        &[
            "--crate-name".into(),
            "btrfs_range_math".into(),
            "--crate-type".into(),
            "rlib".into(),
            "--edition=2024".into(),
            "--target".into(),
            target,
            "-C".into(),
            "opt-level=2".into(),
            "-C".into(),
            "panic=abort".into(),
            "-C".into(),
            "overflow-checks=on".into(),
            "-C".into(),
            "codegen-units=1".into(),
            "-C".into(),
            "no-redzone=yes".into(),
            "-C".into(),
            "lto=off".into(),
            "-C".into(),
            "debuginfo=0".into(),
            "--emit=obj".into(),
            "-o".into(),
            rust_obj.display().to_string(),
            repo.join("rust/range-math/lib.rs").display().to_string(),
        ],
        &mut commands,
    );
    let symbols = Command::new(&nm)
        .arg("-u")
        .arg(&rust_obj)
        .output()
        .unwrap_or_else(|e| panic!("cannot start {nm}: {e}"));
    assert!(symbols.status.success(), "{nm} failed");
    assert!(
        symbols.stdout.iter().all(u8::is_ascii_whitespace),
        "Rust leaf has unresolved symbols: {}",
        String::from_utf8_lossy(&symbols.stdout)
    );
    commands.push(format!("{nm} -u {}", rust_obj.display()));
    println!("cargo:rustc-check-cfg=cfg(has_reference)");
    if let Some(source) = env::var_os("BTRFS_RANGE_REFERENCE_OBJECT") {
        let source = PathBuf::from(source);
        assert!(
            source.is_file(),
            "BTRFS_RANGE_REFERENCE_OBJECT is not a file: {}",
            source.display()
        );
        println!("cargo:rerun-if-changed={}", source.display());
        let header = Command::new(&objdump)
            .arg("-f")
            .arg(&source)
            .output()
            .unwrap_or_else(|e| panic!("cannot start {objdump}: {e}"));
        assert!(
            header.status.success(),
            "invalid reference object: {}",
            String::from_utf8_lossy(&header.stderr)
        );
        let header = String::from_utf8(header.stdout).expect("objdump output is not UTF-8");
        let format = if windows {
            "file format pe-x86-64"
        } else {
            "file format elf64-x86-64"
        };
        assert!(
            header.contains(format) && header.contains("architecture: i386:x86-64"),
            "reference object has wrong target architecture or format: {header}"
        );
        assert_export(&nm, &source, "btrfs_clip_byte_range");
        let undefined = checked_output(&nm, &[std::path::Path::new("-u"), &source]);
        assert!(
            undefined.trim().is_empty(),
            "reference object has unresolved symbols: {undefined}"
        );
        fs::copy(&source, &reference_obj).expect("cannot copy reference object");
        run(
            &objcopy,
            &[
                "--redefine-sym".into(),
                "btrfs_clip_byte_range=btrfs_clip_byte_range_reference".into(),
                reference_obj.display().to_string(),
            ],
            &mut commands,
        );
        assert_export(&nm, &reference_obj, "btrfs_clip_byte_range_reference");
        println!("cargo:rustc-cfg=has_reference");
    }
    let mut members = vec![
        c_obj.display().to_string(),
        baseline_obj.display().to_string(),
        rust_obj.display().to_string(),
    ];
    if env::var_os("BTRFS_RANGE_REFERENCE_OBJECT").is_some() {
        members.push(reference_obj.display().to_string());
    }
    if archive.exists() {
        fs::remove_file(&archive).expect("cannot replace helper archive");
    }
    run(
        &ar,
        &[vec!["crs".into(), archive.display().to_string()], members].concat(),
        &mut commands,
    );
    fs::write(out.join("build-commands.txt"), commands.join("\n") + "\n").unwrap();
    println!("cargo:rustc-link-search=native={}", out.display());
    println!("cargo:rustc-link-lib=static=range_helpers");
    for source in [
        crate_dir.join("baseline.c"),
        repo.join("src/range-math.c"),
        repo.join("src/range-math.h"),
        repo.join("rust/range-math/lib.rs"),
    ] {
        println!("cargo:rerun-if-changed={}", source.display());
    }
    for key in [
        "BTRFS_RANGE_CC",
        "BTRFS_RANGE_AR",
        "BTRFS_RANGE_NM",
        "BTRFS_RANGE_RUSTC",
        "BTRFS_RANGE_OBJCOPY",
        "BTRFS_RANGE_OBJDUMP",
        "BTRFS_RANGE_REFERENCE_OBJECT",
    ] {
        println!("cargo:rerun-if-env-changed={key}");
    }
}
