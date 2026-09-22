// Cyclomatic complexity 16 in 48 lines: over the HISS-04 cap of 10 and under the length
// cap, which is the only thing the scanner measures for Rust. No clippy configuration or
// Rust lint step exists in the Makefile, lefthook or CI.
fn branchy(mut count: i32) -> i32 {
    if count > 0 {
        count += 1;
    }
    if count > 1 {
        count += 1;
    }
    if count > 2 {
        count += 1;
    }
    if count > 3 {
        count += 1;
    }
    if count > 4 {
        count += 1;
    }
    if count > 5 {
        count += 1;
    }
    if count > 6 {
        count += 1;
    }
    if count > 7 {
        count += 1;
    }
    if count > 8 {
        count += 1;
    }
    if count > 9 {
        count += 1;
    }
    if count > 10 {
        count += 1;
    }
    if count > 11 {
        count += 1;
    }
    if count > 12 {
        count += 1;
    }
    if count > 13 {
        count += 1;
    }
    if count > 14 {
        count += 1;
    }
    count
}
