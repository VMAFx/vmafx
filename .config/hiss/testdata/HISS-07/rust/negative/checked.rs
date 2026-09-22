fn f(x: Option<i32>) -> Result<i32, ()> {
	x.ok_or(())
}
