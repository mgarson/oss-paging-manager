Operating Systems Project 6
Author: Maija Garson
Date: 05/15/2025

Description
This project will compile two programs into two executables using the makefile provided. One of the executables, oss, is generated from oss.cpp. The other executable,
worker, is generated from worker.cpp. The oss program will allocate a shared memory clock, keep track of a process control block table and page frame table of 256 frames,
and will launch the worker program as its child up to a specified amount of times. The worker child will randomly generate a probability to request to read or write to memory.
The worker will send messages to oss representing a request to read or write. Oss will attempt to grant requests if possible, or will add the child to a wait queue.
Oss will also update all values in the tables to reflect memory request from child processes. Every 1 sec of system time, oss will print both the frame table and process table,
including each process's current pages. Oss will print final statistics at the end of each run.

Compilation
These programs will compile using the included makefile. In the command line it will compile if given:
make

Running the Program:
Once compiled, the oss program can be run with 5 options that are optional:
oss [-h] [-n proc] [-s simul] [-i intervalInMsToLaunchChildren] [-f]
Where
        -h: Display help message
        -n proc: Proc represents the amount of total child processes to launch
        -s simul: Simul represents the amount of child processes that can run simultaneously
        -i intervalInMsToLaunchChildren: Represents the interval in ms to launch the next child process
	-f logfile: Will print output from oss to logfile, while still printing to console
Default values for options n and s will be 1 and for i will be 0 if not specified in the command line

Problems Encountered:
I did not have many issues with this project. A lot of the structure was similar to Project 5, so it was not difficult to change it from resource management to memory management.

Known Bugs:
I currently do not know of any bugs in this project.



