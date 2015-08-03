scafd(1) -- Scheduling and Allocation with Feedback Daemon
==========================================================

## SYNOPSIS
`scafd` \[-hbaex\] \[-t <seconds>\] \[-u <seconds>\] \[-C <multiple>\] \[-T <maxthreads>\]

## DESCRIPTION
`scafd` is the centralized daemon for the SCAF system. It listens for connections from SCAF clients, which report their efficiency periodically. Based on the reported efficiencies, `scafd` then advises the clients on parallel allocations. In other words, `scafd` coordinates the degree of parallelism that parallel proceses should use, with the goal of maximizing total system speedup as its goal.

## OPTIONS
* `-h`: display this message
* `-b`:     don't monitor background load: assume load of 0
* `-a`:     disable affinity-based parallelism control
* `-e`:     only do strict equipartitioning
* `-t` <n>: use a plain text status interface, printing every n seconds
* `-u` <n>: consider a client unresponsive after n seconds. (default: 10)
* `-C` <n>: only allocate threads in multiples of n. (default: 1)
* `-T` <n>: use n threads for all processes. (default: 8)
* `-x`:     do equipartitioning during experiments. (default: disabled)

## EXAMPLES
### Example 1: default operation
Run `scafd` with all defaults. This will monitor and account for background load, use affinity-based parallelism control for non-malleable programs, optimize for total sum of speedups, and use the total number of available threads.

    $ scafd

### Example 2: equipartitioning with reporting
Run `scafd`, but instruct it to ignore performance results from clients and implement simple equipartitioning among all clients. Print the status of the SCAF system every 2 seconds to STDOUT.

    $ scafd -e -t 2

### Example 3: ignoring background load
Run `scafd` for max-speedup, but don't account for background load. Print the status of the SCAF system every second to STDOUT.

    $ scafd -b -t 1

### Example 4: equipartitioning in chunks of 4 using 32 threads maximum
Say we have a 64-core machine, but we want SCAF and all of its clients to use only 32 of them. In this case, we likely want to ignore background load too, since it will include load from the other half of the machine. Status printed every 60 seconds.

    $ scafd -C 4 -b -T 32 -t 60

## SEE ALSO
scafwrap(1)
