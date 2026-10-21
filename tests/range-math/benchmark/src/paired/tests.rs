use super::*;

#[test]
fn rows_balance_every_position_and_directed_within_row_neighbor() {
    let mut positions = [[0; 5]; 5];
    let mut pairs = [[0; 5]; 5];
    for row in ROWS {
        let mut seen = [false; 5];
        for (position, label) in row.into_iter().enumerate() {
            assert!(!seen[label as usize]);
            seen[label as usize] = true;
            positions[label as usize][position] += 1;
        }
        for pair in row.windows(2) {
            pairs[pair[0] as usize][pair[1] as usize] += 1;
        }
    }
    assert_eq!(positions, [[2; 5]; 5]);
    for (left, row) in pairs.iter().enumerate() {
        for (right, &count) in row.iter().enumerate() {
            assert_eq!(count, if left == right { 0 } else { 2 });
        }
    }
}

#[test]
fn schedules_are_row_permutations_with_distinct_order_and_cycle_seeds() {
    let first = row_order(0, 0);
    let mut sorted = first;
    sorted.sort();
    assert_eq!(sorted, [0, 1, 2, 3, 4, 5, 6, 7, 8, 9]);
    assert_eq!(first, row_order(0, 0));
    assert_ne!(first, row_order(1, 0));
    assert_ne!(first, row_order(2, 0));
    assert_ne!(first, row_order(0, 1));
}

#[test]
fn collection_preserves_equal_label_work_minima_and_contiguous_cycles() {
    let mut counts = [0; 5];
    let record = collect(3, 7, 1, |label| {
        counts[label as usize] += 1;
        100 + u64::from(label)
    });
    assert_eq!(counts, [30; 5]);
    assert_eq!(record.iterations, 3);
    assert_eq!(record.cycle_start, 7);
    assert_eq!(record.ns, [3000, 3030, 3060, 3090, 3120]);
    assert_eq!(record.min_chunk_ns, [100, 101, 102, 103, 104]);
    assert_eq!(record.total_ns(), 15300);
    assert_eq!(record.schedule_digest, 0xfba4_c0f9_dc77_3d2d);
    assert_eq!(CALLS_PER_LABEL_CYCLE, 167772160);
    assert_ne!(
        record.schedule_digest,
        collect(3, 8, 1, |_| 1).schedule_digest
    );
}

#[test]
fn serialized_schedules_match_independently_recomputed_label_digest_vectors() {
    for (order, digest) in [
        (0, 0x1584_99ad_158e_e825),
        (1, 0x0727_b6e5_cf22_add9),
        (2, 0xc5bc_e027_9f3b_92d1),
    ] {
        assert_eq!(collect(1, 0, order, |_| 1).schedule_digest, digest);
    }
}

#[test]
#[should_panic(expected = "positive iterations")]
fn zero_iterations_cannot_create_an_empty_record() {
    collect(0, 0, 0, |_| 1);
}

#[test]
#[should_panic(expected = "cycle overflow")]
fn cycle_index_cannot_wrap() {
    collect(2, u64::MAX, 0, |_| 1);
}

#[test]
#[should_panic(expected = "duration overflow")]
fn duration_sum_cannot_wrap() {
    collect(1, 0, 0, |_| u64::MAX);
}
