- **Docs:** the Python library page now activates the virtualenv with the
  POSIX `.` builtin instead of `source`, which POSIX `sh` does not define;
  bash and zsh accept either form (follow-up to #3456).
