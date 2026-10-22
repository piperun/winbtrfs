use crate::{Case, Expected, Range, backend_order, baseline_helper, datasets, oracle, preflight};

#[test]
fn backend_orders_balance_positions_and_leave_baseline_last() {
    assert_eq!(
        backend_order("c-first", false).unwrap(),
        ["c", "rust", "baseline"]
    );
    assert_eq!(
        backend_order("rust-first", false).unwrap(),
        ["rust", "c", "baseline"]
    );
    assert_eq!(
        backend_order("c-first", true).unwrap(),
        ["c", "reference", "rust", "baseline"]
    );
    assert_eq!(
        backend_order("reference-first", true).unwrap(),
        ["reference", "rust", "c", "baseline"]
    );
    assert_eq!(
        backend_order("rust-first", true).unwrap(),
        ["rust", "c", "reference", "baseline"]
    );
    assert!(backend_order("reference-first", false).is_err());
    assert!(backend_order("other", true).is_err());
}

#[cfg(has_reference)]
#[test]
fn preflight_checks_reference_object_too() {
    assert!(preflight(&datasets()).is_ok());
}

#[test]
fn corpora_are_repeatable_and_have_1024_cases_each() {
    let first = datasets();
    let second = datasets();
    assert_eq!(first.len(), 4);
    for (a, b) in first.iter().zip(second.iter()) {
        assert_eq!(a.name, b.name);
        assert_eq!(a.cases.len(), 1024, "{}", a.name);
        assert_eq!(a.cases, b.cases, "{}", a.name);
    }
}

#[test]
fn oracle_clips_both_ends_and_clears_misses() {
    let case = Case {
        query_start: 20,
        query_end: 40,
        extent_offset: 10,
        extent_length: 50,
    };
    assert_eq!(
        oracle(case),
        Expected {
            present: 1,
            range: Range { start: 20, end: 40 }
        }
    );
    let before = Case {
        query_start: 20,
        query_end: 40,
        extent_offset: 5,
        extent_length: 15,
    };
    assert_eq!(
        oracle(before),
        Expected {
            present: 0,
            range: Range { start: 0, end: 0 }
        }
    );
}

#[test]
fn oracle_uses_full_unsigned_arithmetic() {
    let case = Case {
        query_start: u64::MAX - 20,
        query_end: u64::MAX,
        extent_offset: u64::MAX - 10,
        extent_length: u64::MAX,
    };
    assert_eq!(
        oracle(case),
        Expected {
            present: 1,
            range: Range {
                start: u64::MAX - 10,
                end: u64::MAX
            },
        }
    );
}

#[test]
fn mixed_corpus_has_documented_categories() {
    let mixed = datasets().into_iter().find(|d| d.name == "mixed").unwrap();
    let mut both = 0;
    let mut left = 0;
    let mut right = 0;
    let mut inside = 0;
    let mut miss = 0;
    for case in mixed.cases {
        let expected = oracle(case);
        if expected.present == 0 {
            miss += 1;
        } else if case.extent_offset < case.query_start && expected.range.end == case.query_end {
            both += 1;
        } else if case.extent_offset < case.query_start {
            left += 1;
        } else if expected.range.end == case.query_end {
            right += 1;
        } else {
            inside += 1;
        }
    }
    assert_eq!((both, left, right, inside, miss), (256, 192, 192, 256, 128));
}

#[test]
fn preflight_checks_all_cases_against_both_production_helpers() {
    assert!(preflight(&datasets()).is_ok());
}

#[test]
fn before_corpus_ends_at_or_before_each_query() {
    let before = datasets().into_iter().find(|d| d.name == "before").unwrap();
    assert!(
        before.cases.iter().all(
            |case| case.extent_offset + case.extent_length <= case.query_start
                && oracle(*case).present == 0
        )
    );
}

#[test]
fn stress_corpus_contains_wrapping_lengths_and_invalid_queries_only_here() {
    let sets = datasets();
    for ordinary in &sets[..3] {
        assert!(
            ordinary
                .cases
                .iter()
                .all(|case| case.query_start < case.query_end
                    && case.extent_length > 0
                    && case.extent_offset.checked_add(case.extent_length).is_some())
        );
    }
    let stress = &sets[3];
    assert_eq!(
        stress
            .cases
            .iter()
            .filter(|case| case.extent_offset.checked_add(case.extent_length).is_none())
            .count(),
        512
    );
    assert_eq!(
        stress
            .cases
            .iter()
            .filter(|case| case.extent_length == 0)
            .count(),
        128
    );
    assert_eq!(
        stress
            .cases
            .iter()
            .filter(|case| case.query_start >= case.query_end)
            .count(),
        128
    );
    assert_eq!(stress.cases.iter().filter(|case|
        case.query_start < case.query_end && case.extent_offset >= case.query_end).count(), 64);
}

#[test]
fn baseline_initializes_both_endpoints_on_every_call() {
    let mut output = Range {
        start: u64::MAX,
        end: u64::MAX,
    };
    let result = unsafe { baseline_helper()(17, 29, 99, 0, &mut output) };
    assert_eq!(result, 1);
    assert_eq!(output, Range { start: 17, end: 29 });
    output = Range {
        start: u64::MAX,
        end: u64::MAX,
    };
    let result = unsafe { baseline_helper()(0, 0, 0, 0, &mut output) };
    assert_eq!(result, 1);
    assert_eq!(output, Range { start: 0, end: 0 });
}
