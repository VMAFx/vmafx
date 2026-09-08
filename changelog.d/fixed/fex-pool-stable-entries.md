- Keep internal feature-context pool entries at stable addresses when their
  pointer table grows, so waiting acquisitions are signaled correctly. Check
  allocation sizes and release entry options even after context creation fails.
