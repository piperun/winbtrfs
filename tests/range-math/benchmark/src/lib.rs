//! Fixed benchmark cases and the correctness gate for the range helpers.
//!
//! The timed runner consumes pre-generated cases; no data generation occurs in
//! a measured iteration.
#![warn(missing_docs)]

/// One query and extent pair supplied to a helper call.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Case {
    /// Inclusive query start.
    pub query_start: u64,
    /// Exclusive query end.
    pub query_end: u64,
    /// Inclusive extent start.
    pub extent_offset: u64,
    /// Length of the extent before clipping.
    pub extent_length: u64,
}

/// The two fixed-width output endpoints shared with C.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
#[repr(C)]
pub struct Range {
    /// Inclusive clipped start, or zero for an empty intersection.
    pub start: u64,
    /// Exclusive clipped end, or zero for an empty intersection.
    pub end: u64,
}

/// Oracle result for one helper call.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Expected {
    /// One for a nonempty intersection, zero otherwise.
    pub present: u32,
    /// Clipped endpoints, or both zero for an empty result.
    pub range: Range,
}

/// A named, pre-generated sequence of 1024 calls.
pub struct Dataset {
    /// Criterion dataset identifier.
    pub name: &'static str,
    /// Calls, in their fixed benchmark order.
    pub cases: Vec<Case>,
}

/// Fixed seed used for the ordinary offsets and stress variations.
pub const DATASET_SEED: u64 = SEED;

/// FNV-1a digest of all case words serialized as little-endian bytes.
pub fn dataset_fingerprint(dataset: &Dataset) -> u64 {
    let mut hash = 0xcbf2_9ce4_8422_2325u64;
    for case in &dataset.cases {
        for field in [
            case.query_start,
            case.query_end,
            case.extent_offset,
            case.extent_length,
        ] {
            for byte in field.to_le_bytes() {
                hash = (hash ^ u64::from(byte)).wrapping_mul(0x100_0000_01b3);
            }
        }
    }
    hash
}

const SEED: u64 = 0x6e5a_4b13_9c20_d7f1;

fn next(state: &mut u64) -> u64 {
    *state = state.wrapping_add(0x9e37_79b9_7f4a_7c15);
    let mut value = *state;
    value = (value ^ (value >> 30)).wrapping_mul(0xbf58_476d_1ce4_e5b9);
    value = (value ^ (value >> 27)).wrapping_mul(0x94d0_49bb_1331_11eb);
    value ^ (value >> 31)
}

fn ordinary(state: &mut u64, kind: u8) -> Case {
    let base = 1024 + next(state) % 1_000_000;
    let (offset, length) = match kind {
        0 => (base + 50, 200),  // clip at both query ends
        1 => (base + 50, 100),  // clip at query start
        2 => (base + 150, 100), // clip at query end
        3 => (base + 120, 60),  // fully inside
        _ => (base, 100),       // ends exactly at query start
    };
    Case {
        query_start: base + 100,
        query_end: base + 200,
        extent_offset: offset,
        extent_length: length,
    }
}

/// Generate four deterministic 1024-entry benchmark datasets.
pub fn datasets() -> Vec<Dataset> {
    let mut state = SEED;
    let overlap = (0..1024).map(|_| ordinary(&mut state, 0)).collect();
    let before = (0..1024).map(|_| ordinary(&mut state, 4)).collect();
    let mut mixed = Vec::with_capacity(1024);
    for (kind, count) in [(0, 256), (1, 192), (2, 192), (3, 256), (4, 128)] {
        for _ in 0..count {
            mixed.push(ordinary(&mut state, kind));
        }
    }
    for index in (1..mixed.len()).rev() {
        let other = (next(&mut state) % (index as u64 + 1)) as usize;
        mixed.swap(index, other);
    }
    let mut stress = Vec::with_capacity(1024);
    for _ in 0..512 {
        let delta = 1 + next(&mut state) % 32;
        stress.push(Case {
            query_start: u64::MAX - 100,
            query_end: u64::MAX,
            extent_offset: u64::MAX - delta,
            extent_length: u64::MAX,
        });
    }
    for _ in 0..192 {
        let delta = 1 + next(&mut state) % 50;
        stress.push(Case {
            query_start: u64::MAX - 100,
            query_end: u64::MAX,
            extent_offset: u64::MAX - 100 + delta,
            extent_length: 20,
        });
    }
    for _ in 0..64 {
        let gap = next(&mut state) % 32;
        stress.push(Case {
            query_start: 100,
            query_end: 200,
            extent_offset: 200 + gap,
            extent_length: 40,
        });
    }
    for _ in 0..128 {
        stress.push(Case {
            query_start: 100,
            query_end: 200,
            extent_offset: 150,
            extent_length: 0,
        });
    }
    for _ in 0..128 {
        stress.push(Case {
            query_start: 200,
            query_end: 100,
            extent_offset: 150,
            extent_length: 100,
        });
    }
    vec![
        Dataset {
            name: "overlap",
            cases: overlap,
        },
        Dataset {
            name: "before",
            cases: before,
        },
        Dataset {
            name: "mixed",
            cases: mixed,
        },
        Dataset {
            name: "u64-stress",
            cases: stress,
        },
    ]
}

/// Intersect the query and extent using independent 128-bit arithmetic.
pub fn oracle(case: Case) -> Expected {
    let start = u128::from(case.query_start).max(u128::from(case.extent_offset));
    let end = u128::from(case.query_end)
        .min(u128::from(case.extent_offset) + u128::from(case.extent_length));
    if case.query_start >= case.query_end || start >= end {
        Expected {
            present: 0,
            range: Range::default(),
        }
    } else {
        Expected {
            present: 1,
            range: Range {
                start: start as u64,
                end: end as u64,
            },
        }
    }
}

unsafe extern "C" {
    fn btrfs_clip_byte_range(
        query_start: u64,
        query_end: u64,
        extent_offset: u64,
        extent_length: u64,
        out: *mut Range,
    ) -> u32;
    fn btrfs_clip_byte_range_c(
        query_start: u64,
        query_end: u64,
        extent_offset: u64,
        extent_length: u64,
        out: *mut Range,
    ) -> u32;
    fn btrfs_clip_byte_range_baseline(
        query_start: u64,
        query_end: u64,
        extent_offset: u64,
        extent_length: u64,
        out: *mut Range,
    ) -> u32;
    #[cfg(has_reference)]
    fn btrfs_clip_byte_range_reference(
        query_start: u64,
        query_end: u64,
        extent_offset: u64,
        extent_length: u64,
        out: *mut Range,
    ) -> u32;
}

/// ABI of the production helpers and call-only baseline.
pub type Helper = unsafe extern "C" fn(u64, u64, u64, u64, *mut Range) -> u32;

/// Address of the production Rust helper for preflight checks.
pub fn rust_helper() -> Helper {
    btrfs_clip_byte_range
}
/// Address of the renamed production C helper for preflight checks.
pub fn c_helper() -> Helper {
    btrfs_clip_byte_range_c
}
/// Address of the C call-only baseline for contract tests.
pub fn baseline_helper() -> Helper {
    btrfs_clip_byte_range_baseline
}

/// Whether a historical Rust reference object was linked into this runner.
pub const HAS_REFERENCE: bool = cfg!(has_reference);

/// Names in registration order, with the call-only baseline always last.
pub fn backend_order(order: &str, reference: bool) -> Result<&'static [&'static str], String> {
    match (order, reference) {
        ("c-first", false) => Ok(&["c", "rust", "baseline"]),
        ("rust-first", false) => Ok(&["rust", "c", "baseline"]),
        ("c-first", true) => Ok(&["c", "reference", "rust", "baseline"]),
        ("reference-first", true) => Ok(&["reference", "rust", "c", "baseline"]),
        ("rust-first", true) => Ok(&["rust", "c", "reference", "baseline"]),
        _ => Err(format!(
            "invalid BTRFS_RANGE_ORDER={order} for reference={reference}"
        )),
    }
}

/// Address of the historical Rust reference helper when linked.
#[cfg(has_reference)]
pub fn reference_helper() -> Helper {
    btrfs_clip_byte_range_reference
}

/// Check every case against the oracle using every linked clipping helper.
pub fn preflight(sets: &[Dataset]) -> Result<(), String> {
    let mut helpers = Vec::with_capacity(3);
    helpers.push(("C", c_helper()));
    helpers.push(("Rust", rust_helper()));
    #[cfg(has_reference)]
    helpers.push(("reference", reference_helper()));
    for dataset in sets {
        if dataset.cases.len() != 1024 {
            return Err(format!(
                "{} has {} cases, expected 1024",
                dataset.name,
                dataset.cases.len()
            ));
        }
        for (index, &case) in dataset.cases.iter().enumerate() {
            let expected = oracle(case);
            for &(name, helper) in &helpers {
                let mut actual = Range {
                    start: u64::MAX,
                    end: u64::MAX,
                };
                // SAFETY: `actual` is aligned, writable, and exclusively borrowed.
                let present = unsafe {
                    helper(
                        case.query_start,
                        case.query_end,
                        case.extent_offset,
                        case.extent_length,
                        &mut actual,
                    )
                };
                if present != expected.present || actual != expected.range {
                    return Err(format!(
                        "{} case {} {}: got ({}, {:?}), expected {:?}",
                        dataset.name, index, name, present, actual, expected
                    ));
                }
            }
        }
    }
    Ok(())
}

macro_rules! batch {
    ($name:ident, $helper:ident) => {
        #[doc = "Run a pre-generated batch with direct calls to its leaf helper."]
        pub fn $name(cases: &[Case]) {
            for &case in cases {
                let case = std::hint::black_box(case);
                let mut out = Range {
                    start: u64::MAX,
                    end: u64::MAX,
                };
                // SAFETY: `out` is aligned, writable, and exclusively borrowed.
                let present = unsafe {
                    $helper(
                        case.query_start,
                        case.query_end,
                        case.extent_offset,
                        case.extent_length,
                        &mut out,
                    )
                };
                std::hint::black_box((present, out.start, out.end));
            }
        }
    };
}

batch!(batch_c, btrfs_clip_byte_range_c);
batch!(batch_rust, btrfs_clip_byte_range);
#[cfg(has_reference)]
batch!(batch_reference, btrfs_clip_byte_range_reference);
batch!(batch_baseline, btrfs_clip_byte_range_baseline);

#[cfg(test)]
mod tests;
