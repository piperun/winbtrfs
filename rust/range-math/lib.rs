//! Allocation-free clipping of half-open byte intervals for the C driver.
//!
//! This leaf owns no filesystem state and calls no Windows APIs. The C caller
//! validates requests and retains all traversal, locking and output handling.
#![no_std]
#![warn(missing_docs)]
#![deny(unsafe_op_in_unsafe_fn)]

/// Fixed-width interval exchanged with the C caller.
#[repr(C)]
pub struct BtrfsByteRange {
    /// Inclusive byte offset, or zero for an empty result.
    pub start: u64,
    /// Exclusive byte offset, or zero for an empty result.
    pub end: u64,
}

const _: [(); 16] = [(); core::mem::size_of::<BtrfsByteRange>()];
const _: [(); 8] = [(); core::mem::align_of::<BtrfsByteRange>()];
const _: [(); 0] = [(); core::mem::offset_of!(BtrfsByteRange, start)];
const _: [(); 8] = [(); core::mem::offset_of!(BtrfsByteRange, end)];

fn clip(query_start: u64, query_end: u64, extent_offset: u64, extent_length: u64) -> BtrfsByteRange {
    let empty = BtrfsByteRange { start: 0, end: 0 };
    if query_start >= query_end || extent_offset >= query_end {
        return empty;
    }

    let available = match query_end.checked_sub(extent_offset) {
        Some(value) => value,
        None => return empty,
    };
    let length = if extent_length < available {
        extent_length
    } else {
        available
    };
    let end = match extent_offset.checked_add(length) {
        Some(value) => value,
        None => return empty,
    };
    let start = if extent_offset < query_start {
        query_start
    } else {
        extent_offset
    };
    if start >= end {
        empty
    } else {
        BtrfsByteRange { start, end }
    }
}

/// Clip an extent to a query, returning one for a nonempty intersection.
///
/// Empty intersections and reversed or empty queries return zero and clear both
/// output endpoints. Extent lengths may span the unsigned 64-bit boundary;
/// clipping never forms their unbounded end in a 64-bit integer.
///
/// # Safety
///
/// `out` must be non-null, properly aligned, and valid for writing one
/// `BtrfsByteRange`. The caller must provide exclusive access for this call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn btrfs_clip_byte_range(
    query_start: u64,
    query_end: u64,
    extent_offset: u64,
    extent_length: u64,
    out: *mut BtrfsByteRange,
) -> u32 {
    let result = clip(query_start, query_end, extent_offset, extent_length);
    let present = if result.start < result.end { 1 } else { 0 };
    // SAFETY: the C ABI requires an aligned, exclusively writable output slot.
    unsafe { out.write(result) };
    present
}
