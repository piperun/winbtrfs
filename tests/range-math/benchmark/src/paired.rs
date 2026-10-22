//! Balanced chunk schedules and exact collection for the interleaved runner.

/// Protocol identifier; sequential evidence cannot satisfy this contract.
pub const PROTOCOL: &str = "interleaved-v2";
/// Algorithm identifier for independently checking each invocation's digest.
pub const SCHEDULE: &str = "splitmix64-fisher-yates-fnv1a64-v1";
/// Stable label IDs used in schedules, duration arrays and metadata.
pub const LABELS: [&str; 5] = ["c", "reference", "rust", "baseline-a", "baseline-b"];
/// Direct batch calls within one timed chunk.
pub const BATCHES_PER_CHUNK: u64 = 16384;
/// Helper calls per label per complete cycle.
pub const CALLS_PER_LABEL_CYCLE: u64 = 10 * BATCHES_PER_CHUNK * 1024;
/// Ten balanced rows: five cyclic label rotations and their reversals.
pub const ROWS: [[u8; 5]; 10] = [
    [0, 1, 4, 2, 3],
    [1, 2, 0, 3, 4],
    [2, 3, 1, 4, 0],
    [3, 4, 2, 0, 1],
    [4, 0, 3, 1, 2],
    [3, 2, 4, 1, 0],
    [4, 3, 0, 2, 1],
    [0, 4, 1, 3, 2],
    [1, 0, 2, 4, 3],
    [2, 1, 3, 0, 4],
];

fn splitmix(state: &mut u64) -> u64 {
    *state = state.wrapping_add(0x9e37_79b9_7f4a_7c15);
    let mut value = *state;
    value = (value ^ (value >> 30)).wrapping_mul(0xbf58_476d_1ce4_e5b9);
    value = (value ^ (value >> 27)).wrapping_mul(0x94d0_49bb_1331_11eb);
    value ^ (value >> 31)
}

/// Shuffle row IDs using the order index and absolute cycle index.
///
/// Arithmetic wraps modulo 2^64. Seed = DATASET_SEED XOR
/// ((order + 1) * d1b54a32d192ed03) XOR (cycle * 94d049bb133111eb).
/// Descending Fisher-Yates uses SplitMix64 output modulo (index + 1).
pub fn row_order(order: u64, cycle: u64) -> [usize; 10] {
    assert!(order < 3, "invalid order index");
    let mut state = crate::DATASET_SEED
        ^ (order + 1).wrapping_mul(0xd1b5_4a32_d192_ed03)
        ^ cycle.wrapping_mul(0x94d0_49bb_1331_11eb);
    let mut result = [0, 1, 2, 3, 4, 5, 6, 7, 8, 9];
    for index in (1..10).rev() {
        result.swap(index, (splitmix(&mut state) % (index as u64 + 1)) as usize);
    }
    result
}

/// One invocation of Criterion's custom timer, including warmup invocations.
#[derive(Debug)]
pub struct Record {
    /// Number of full balanced cycles requested by Criterion.
    pub iterations: u64,
    /// First absolute cycle index for this invocation within its dataset.
    pub cycle_start: u64,
    /// Integer duration totals in stable label order.
    pub ns: [u64; 5],
    /// Smallest individual chunk duration for each label.
    pub min_chunk_ns: [u64; 5],
    /// FNV-1a 64-bit digest of all scheduled label bytes in execution order.
    pub schedule_digest: u64,
}

impl Record {
    /// Exact sum returned to Criterion; no baseline or timer subtraction.
    pub fn total_ns(&self) -> u64 {
        self.ns
            .iter()
            .try_fold(0u64, |total, value| total.checked_add(*value))
            .expect("duration overflow")
    }

    /// Serialize exact integer evidence without converting durations to floats.
    pub fn json(&self) -> String {
        format!(
            "{{\"iterations\":{},\"cycle_start\":{},\"ns\":{:?},\"min_chunk_ns\":{:?},\"schedule_digest\":\"{:016x}\"}}",
            self.iterations, self.cycle_start, self.ns, self.min_chunk_ns, self.schedule_digest
        )
    }
}

/// Collect equally many chunks per label; the closure times one whole chunk.
///
/// Scheduling, digest updates and collection occur outside the closure's timer.
pub fn collect(
    iterations: u64,
    cycle_start: u64,
    order: u64,
    mut chunk: impl FnMut(u8) -> u64,
) -> Record {
    assert!(iterations > 0, "positive iterations required");
    let cycle_end = cycle_start.checked_add(iterations).expect("cycle overflow");
    let mut record = Record {
        iterations,
        cycle_start,
        ns: [0; 5],
        min_chunk_ns: [u64::MAX; 5],
        schedule_digest: 0xcbf2_9ce4_8422_2325,
    };
    for cycle in cycle_start..cycle_end {
        for row in row_order(order, cycle) {
            for label in ROWS[row] {
                let ns = chunk(label);
                let index = label as usize;
                record.ns[index] = record.ns[index].checked_add(ns).expect("duration overflow");
                record.min_chunk_ns[index] = record.min_chunk_ns[index].min(ns);
                record.schedule_digest =
                    (record.schedule_digest ^ u64::from(label)).wrapping_mul(0x100_0000_01b3);
            }
        }
    }
    record.total_ns();
    record
}

#[cfg(test)]
mod tests;
