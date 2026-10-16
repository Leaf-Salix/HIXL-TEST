# HIXL-TEST Agent Guide

- User-facing documentation is Chinese.
- Keep installed HIXL tests separate from optional experimental patch tests.
- Do not claim multi-node hardware validation without a matching real run.
- Tests must never transport tested buffer contents through the control socket.
- Fail on data, guard, synchronization, cleanup, or child-process errors.
- Run `python3 scripts/check.py` and `python3 -m unittest discover -s tests -v`.
- Hardware runs on shared systems must use the site's scheduler and reserved devices.
- Do not publish credentials, private addresses, machine identifiers, raw runtime handles,
  or site-specific user paths. Generated results are ignored by Git.
