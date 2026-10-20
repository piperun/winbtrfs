use criterion::{BenchmarkId, Criterion};
use range_math_benchmark::{
    DATASET_SEED, batch_baseline, batch_c, batch_rust, dataset_fingerprint, datasets, preflight,
};
use std::{env, fs, path::PathBuf, time::Duration};

fn main() {
    let output = PathBuf::from(
        env::var_os("CRITERION_HOME").expect("set CRITERION_HOME to a dedicated result directory"),
    );
    let order = env::var("BTRFS_RANGE_ORDER").unwrap_or_else(|_| "c-first".into());
    assert!(
        order == "c-first" || order == "rust-first",
        "BTRFS_RANGE_ORDER must be c-first or rust-first"
    );
    let sets = datasets();
    preflight(&sets).unwrap_or_else(|message| panic!("preflight failed: {message}"));
    eprintln!(
        "preflight passed: {} cases checked against C and Rust",
        sets.len() * 1024
    );
    fs::create_dir_all(&output).expect("cannot create CRITERION_HOME");
    let fingerprints = sets
        .iter()
        .map(|dataset| {
            format!(
                "{{\"name\":\"{}\",\"cases\":{},\"fnv1a64_le\":\"{:016x}\"}}",
                dataset.name,
                dataset.cases.len(),
                dataset_fingerprint(dataset)
            )
        })
        .collect::<Vec<_>>()
        .join(",");
    fs::write(output.join("inputs.json"), format!(
        "{{\"seed\":\"{DATASET_SEED:016x}\",\"order\":\"{order}\",\"datasets\":[{fingerprints}]}}\n"))
        .expect("cannot write dataset metadata");

    let mut criterion = Criterion::default()
        .sample_size(100)
        .warm_up_time(Duration::from_secs(2))
        .measurement_time(Duration::from_secs(5))
        .confidence_level(0.95)
        .output_directory(&output)
        .configure_from_args();
    let mut group = criterion.benchmark_group("range");
    for dataset in &sets {
        if order == "rust-first" {
            group.bench_function(BenchmarkId::new(dataset.name, "rust"), |bench| {
                bench.iter(|| batch_rust(&dataset.cases));
            });
            group.bench_function(BenchmarkId::new(dataset.name, "c"), |bench| {
                bench.iter(|| batch_c(&dataset.cases));
            });
        } else {
            group.bench_function(BenchmarkId::new(dataset.name, "c"), |bench| {
                bench.iter(|| batch_c(&dataset.cases));
            });
            group.bench_function(BenchmarkId::new(dataset.name, "rust"), |bench| {
                bench.iter(|| batch_rust(&dataset.cases));
            });
        }
        group.bench_function(BenchmarkId::new(dataset.name, "baseline"), |bench| {
            bench.iter(|| batch_baseline(&dataset.cases));
        });
    }
    group.finish();
    criterion.final_summary();
}
