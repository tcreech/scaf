scafwrap(1) -- Shell wrapper to transparently invoke OpenMP programs as SCAF clients
==========================================================

## SYNOPSIS
`scafwrap` \[application \[application's arguments\]\]

## DESCRIPTION
`scafwrap` is a wrapper which runs OpenMP programs as SCAF clients without the need for recompilation or modification. Early versions of SCAF required recompiling the GOMP parallel library with a patch and pointing the system's linker to the patched runtime, which proved to be cumbersome. `scafwrap` aims to be a more portable alternative.

## ENVIRONMENT
The underlying SCAF runtime that `scafwrap` uses is affected by the following environment variables:

* `SCAF_COMM_RATE_LIMIT`:
  Setting this to a floating point value overrides the rate (frequency) at which the client will attempt to communicate with `scafd`. E.g., setting `SCAF_COMM_RATE_LIMIT=50` will ensure that the client won't check in faster than 50Hz. The rate limiting is implemented using the token bucket algorithm. Note that setting this to a negative value has the effect of disabling the limit. The default value is 5.0.

* `SCAF_MATH_RATE_LIMIT`:
  Setting this to a floating point value overrides the rate (frequency) at which the client will internally instrument sections and compute efficiency results. E.g., setting `SCAF_MATH_RATE_LIMIT=50` will ensure that sections aren't timed (and their parallel efficiencies computed) faster than 50Hz. In the event that rate limiting requies skipping some sections, the client is essentially *sampling* parallel sections at the specified rate in order to compute its efficiency. The rate limiting is implemented using the token bucket algorithm. Note that setting this to a negative value has the effect of disabling the limit. The default value is 32.0.

* `SCAF_DISABLE_EXPERIMENTS`:
  Setting this to a non-zero integer disables serial experimental entirely. You probably only want to do this to debug serial experiments themselves. Performance feedback is unlikely to be useful with experiments disabled. The default value is 0.

* `SCAF_EXPERIMENT_MIN_USECONDS`:
  Serial experiments will run (if safe) for at least the specified number of microseconds, even if the non-experimental parallel sections are finished and program progress is waiting for the experiment to finish. A value too large will cause an excess of these waits, while a value too small may result in experiment results which are unreliable. The default is 100000 useconds.

* `SCAF_LAZY_EXPERIMENTS`:
  By default, serial experiments do not run unless there are multiple SCAF clients. (A single client should probably just run on the entire machine, so performance feedback would not be useful.) If a second client arrives, then all clients will begin executing experiments. Setting `SCAF_LAZY_EXPERIMENTS=0` disables this behavior, and all clients will run serial experiments even if they are alone on the machine.

* `SCAF_SECTION_DUMPS`:
  Setting this to a non-zero integer enables verbose logging of every parallel section to `/tmp/scaf-sectiondump.${PID}.csv`. This can be used to examine the timing and nature of parallel sections in your program. If enabled, *all* sections will be accounted for in this file regardless of the values of `SCAF_COMM_RATE_LIMIT` and `SCAF_MATH_RATE_LIMIT`. Note that this option may have a significant performance impact.

## EXAMPLES
### Example 1: Running NAS benchmarks
    $ scafwrap ./cg.B.x &
    $ scafwrap ./lu.B.x &
    $ wait

## AUTHOR
Tim Creech <tcreech@umd.edu>

## SEE ALSO
scafd(1)
