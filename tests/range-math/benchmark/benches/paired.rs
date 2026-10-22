use criterion::{Criterion, SamplingMode};
#[cfg(has_reference)]
use range_math_benchmark::batch_reference;
use range_math_benchmark::{
    Case, DATASET_SEED, HAS_REFERENCE, batch_baseline, batch_c, batch_rust, dataset_fingerprint,
    datasets,
    paired::{self, BATCHES_PER_CHUNK, LABELS, PROTOCOL, SCHEDULE},
    preflight,
};
use std::{
    env, fs,
    path::PathBuf,
    time::{Duration, Instant},
};

fn chunk(mut batch: impl FnMut()) -> u64 {
    let start = Instant::now();
    for _ in 0..BATCHES_PER_CHUNK {
        batch();
    }
    start
        .elapsed()
        .as_nanos()
        .try_into()
        .expect("chunk duration overflow")
}

// Each branch enters its own monomorphized timer. Label dispatch is outside
// that timer, and each closure calls the existing direct-call batch function.
fn time_label(label: u8, cases: &[Case]) -> u64 {
    match label {
        0 => chunk(|| batch_c(cases)),
        #[cfg(has_reference)]
        1 => chunk(|| batch_reference(cases)),
        2 => chunk(|| batch_rust(cases)),
        3 | 4 => chunk(|| batch_baseline(cases)),
        _ => unreachable!("validated label with linked reference"),
    }
}

fn main() {
    assert!(
        HAS_REFERENCE,
        "paired mode requires the historical Rust reference object"
    );
    let output = PathBuf::from(
        env::var_os("CRITERION_HOME")
            .expect("set CRITERION_HOME to a fresh dedicated result directory"),
    );
    let order = env::var("BTRFS_RANGE_ORDER").unwrap_or_else(|_| "c-first".into());
    let order_index = match order.as_str() {
        "c-first" => 0,
        "reference-first" => 1,
        "rust-first" => 2,
        _ => panic!("invalid BTRFS_RANGE_ORDER={order}"),
    };
    let sets = datasets();
    preflight(&sets).unwrap_or_else(|message| panic!("preflight failed: {message}"));
    eprintln!(
        "preflight passed: {} cases checked against C, reference, Rust",
        sets.len() * 1024
    );
    fs::create_dir_all(&output).expect("cannot create CRITERION_HOME");
    assert!(
        !output.join("inputs.json").exists(),
        "CRITERION_HOME already contains a run"
    );
    let probe_ns: Vec<u64> = (0..10000)
        .map(|_| {
            let start = Instant::now();
            start
                .elapsed()
                .as_nanos()
                .try_into()
                .expect("probe duration overflow")
        })
        .collect();
    let mut sorted = probe_ns.clone();
    sorted.sort_unstable();
    let p99 = sorted[9899];
    assert!(p99 > 0, "timer probe p99 must be positive");
    let timer_only = env::args().any(|arg| arg == "--timer-probe");
    let smoke = env::args().any(|arg| arg == "--test");
    let mode = if timer_only {
        "timer-probe"
    } else if smoke {
        "smoke"
    } else {
        "measurement"
    };
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
    let backends = LABELS
        .iter()
        .map(|label| format!("\"{label}\""))
        .collect::<Vec<_>>()
        .join(",");
    fs::write(output.join("inputs.json"), format!(
        "{{\"protocol\":\"{PROTOCOL}\",\"schedule\":\"{SCHEDULE}\",\"mode\":\"{mode}\",\"seed\":\"{DATASET_SEED:016x}\",\"order\":\"{order}\",\"order_index\":{order_index},\"backends\":[{backends}],\"datasets\":[{fingerprints}],\"batches_per_chunk\":16384,\"cases_per_batch\":1024,\"chunks_per_label_cycle\":10,\"calls_per_label_cycle\":167772160,\"criterion\":{{\"samples\":10,\"warmup_ns\":2000000000,\"measurement_ns\":5000000000,\"confidence_level\":0.95,\"sampling_mode\":\"Flat\"}},\"timer_probe\":{{\"count\":10000,\"p99_ns\":{p99},\"ns\":{probe_ns:?}}}}}\n"))
        .expect("cannot write paired metadata");
    if timer_only {
        println!(
            "{{\"protocol\":\"{PROTOCOL}\",\"count\":10000,\"p99_ns\":{p99},\"min_ns\":{},\"max_ns\":{}}}",
            sorted[0], sorted[9999]
        );
        return;
    }
    let mut criterion = Criterion::default()
        .configure_from_args()
        .sample_size(10)
        .warm_up_time(Duration::from_secs(2))
        .measurement_time(Duration::from_secs(5))
        .confidence_level(0.95)
        .output_directory(&output);
    let mut group = criterion.benchmark_group("paired");
    group.sampling_mode(SamplingMode::Flat);
    for dataset in &sets {
        let mut records = Vec::new();
        let mut cycle = 0u64;
        group.bench_function(dataset.name, |bench| {
            bench.iter_custom(|iterations| {
                let record = paired::collect(iterations, cycle, order_index, |label| {
                    time_label(label, &dataset.cases)
                });
                cycle = cycle.checked_add(iterations).expect("cycle overflow");
                let duration = Duration::from_nanos(record.total_ns());
                records.push(record);
                duration
            });
        });
        let rows = records
            .iter()
            .map(|record| record.json())
            .collect::<Vec<_>>()
            .join(",\n");
        fs::write(output.join(format!("paired-{}.json", dataset.name)), format!(
            "{{\"protocol\":\"{PROTOCOL}\",\"dataset\":\"{}\",\"order\":\"{order}\",\"records\":[{rows}]}}\n", dataset.name))
            .expect("cannot write paired invocation evidence");
    }
    group.finish();
    criterion.final_summary();
}
