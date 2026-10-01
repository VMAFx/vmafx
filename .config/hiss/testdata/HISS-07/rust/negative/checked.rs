// SPDX-License-Identifier: EUPL-1.2
fn f(x: Option<i32>) -> Result<i32, ()> {
	x.ok_or(())
}
