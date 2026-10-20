use std::{env, fs, path::PathBuf, process::Command};

fn run(program: &str, args: &[String], commands: &mut Vec<String>) {
    commands.push(format!("{} {}", program, args.join(" ")));
    let status = Command::new(program)
        .args(args)
        .status()
        .unwrap_or_else(|e| panic!("cannot start {program}: {e}"));
    assert!(status.success(), "{program} failed with {status}");
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
    let rustc = env::var("BTRFS_RANGE_RUSTC").unwrap_or_else(|_| env::var("RUSTC").unwrap());
    let out = PathBuf::from(env::var("OUT_DIR").unwrap());
    let c_obj = out.join("range-c.o");
    let baseline_obj = out.join("baseline.o");
    let rust_obj = out.join("range-rust.o");
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
    run(
        &ar,
        &[
            "crs".into(),
            archive.display().to_string(),
            c_obj.display().to_string(),
            baseline_obj.display().to_string(),
            rust_obj.display().to_string(),
        ],
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
    ] {
        println!("cargo:rerun-if-env-changed={key}");
    }
}
