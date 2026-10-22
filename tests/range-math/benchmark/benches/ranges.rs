use criterion::{BenchmarkId, Criterion};
#[cfg(has_reference)]
use range_math_benchmark::batch_reference;
use range_math_benchmark::{
    DATASET_SEED, HAS_REFERENCE, backend_order, batch_baseline, batch_c, batch_rust,
    dataset_fingerprint, datasets, preflight,
};
use std::{env, fs, path::PathBuf, time::Duration};

fn main() {
    let output = PathBuf::from(
        env::var_os("CRITERION_HOME").expect("set CRITERION_HOME to a dedicated result directory"),
    );
    let order = env::var("BTRFS_RANGE_ORDER").unwrap_or_else(|_| "c-first".into());
    let registration =
        backend_order(&order, HAS_REFERENCE).unwrap_or_else(|message| panic!("{message}"));
    let sets = datasets();
    preflight(&sets).unwrap_or_else(|message| panic!("preflight failed: {message}"));
    eprintln!(
        "preflight passed: {} cases checked against {}",
        sets.len() * 1024,
        registration[..registration.len() - 1].join(", ")
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
    let backends = registration
        .iter()
        .map(|name| format!("\"{name}\""))
        .collect::<Vec<_>>()
        .join(",");
    fs::write(output.join("inputs.json"), format!(
        "{{\"seed\":\"{DATASET_SEED:016x}\",\"order\":\"{order}\",\"backends\":[{backends}],\"datasets\":[{fingerprints}]}}\n"))
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
        for &backend in registration {
            match backend {
                "c" => {
                    group.bench_function(BenchmarkId::new(dataset.name, backend), |bench| {
                        bench.iter(|| batch_c(&dataset.cases));
                    });
                }
                "rust" => {
                    group.bench_function(BenchmarkId::new(dataset.name, backend), |bench| {
                        bench.iter(|| batch_rust(&dataset.cases));
                    });
                }
                #[cfg(has_reference)]
                "reference" => {
                    group.bench_function(BenchmarkId::new(dataset.name, backend), |bench| {
                        bench.iter(|| batch_reference(&dataset.cases));
                    });
                }
                "baseline" => {
                    group.bench_function(BenchmarkId::new(dataset.name, backend), |bench| {
                        bench.iter(|| batch_baseline(&dataset.cases));
                    });
                }
                _ => unreachable!("validated registration order"),
            }
        }
    }
    group.finish();
    criterion.final_summary();
}
