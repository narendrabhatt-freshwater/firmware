# Coding and debugging rules

## Establish the root cause before changing behavior

- Reproduce the failure and trace the actual cause using code, logs, measurements,
  or a focused test. Clearly distinguish confirmed findings from hypotheses.
- Fix the underlying defect. Do not hide symptoms with arbitrary delays, larger
  timeouts or buffers, retries, dropped errors, silent fallbacks, forced resets,
  or reconnect requirements. A parameter change needs evidence that the parameter
  itself violates the actual timing, capacity, or protocol requirement.
- Before changing production behavior, explain the problem, supporting evidence,
  proposed fix, and consequences to the user and agree on the solution. Permission
  to investigate or test is not permission to introduce a new recovery policy or
  workaround. Existing approval for a specific fix remains valid.
- If testing uncovers another defect, report it and discuss the solution before
  implementing another behavior change. Keep diagnostic experiments separate
  from production code and binaries.
- If the root cause is still unknown, say so and continue investigating. Do not
  present a plausible explanation or a passing workaround as a confirmed fix.
- Verify that the fix addresses the reproduced failure and test relevant adjacent
  behavior and failure paths. Report exactly what passed and what remains
  untested; do not claim that all regressions have been ruled out.
- If the user explicitly requests a temporary mitigation, identify it as such
  and keep the unresolved root cause visible.
