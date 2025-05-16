
// Operating Systems Project 6
// Author: Maija Garson
// Date: 05/15/2025
// Description: A program that simulates a paging memory management operating system.
// This program runs until it has forked the total amount of processes specified in the command line, while allowing a
// specified amount of processes to run simultaneously. It will allocate shared memory to represent a system clock. It will
// also keep track of a process control block table for all processes, each with a 32 entry page table, and a page frame 
// table of 256 frames. It will receive messages from child processes that represent a memory request to read or write. If
// the requested page is in the frame table, it will grant the request and update the PCB and frame table to reflect this. 
// In the case of a page fault, it will add the worker to a wait queue, and add the required latency. Once this time has passed,
// it will load the page using the least recently used algorithm and update all tables to reflect this. It will print all tables 
// every 1 sec of system time. It will calculate and print final statistics at the end of each run.
// The program will send a kill signal to all processes and terminate if 5 real-life seconds are reached.

#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <sys/msg.h>
#include <cstring>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>
#include <string>
#include <queue>

#define PERMS 0644
#define MAX_PROC 18
#define FRAME_NUM 256

using namespace std;

// Structure for command line options
typedef struct
{
	int proc;
	int simul;
	long long interval;
	string logfile;
} options_t;

// Structure for Process Control Block
typedef struct
{
        int occupied; // Either true or false, determines if slot is occupied
        pid_t pid; // Process ID of this child
        int startSeconds; // Second time when it was forked
        int startNano; // Nanosecond time when it was forked
	int pageTable [32]; // Process's 32 pages for frame
	bool waiting; // True if process is currently waiting due to page fault
	int waitPage; // Page number processes is waiting to be loaded
	bool waitIsWrite; // True if waiting reference is a write
	long long waitSec; // Second time of page fault
	long long waitNano; // Nanosecond time of page fault
} PCB;

// Structure for frame table
typedef struct
{
	bool occupied; // Either true or false, determines if frame is free to use
	bool dirty; // True if frame has been written
	pid_t ownerPid; // PID of process that owns page in frame
	int pageNum; // Page number in frame
	long long lastRefSec; // Second time of access
	long long lastRefNano; // Nanosecond time of access
} Frame;

// Message buffer for communication between OSS and child processes
typedef struct msgbuffer 
{
	long mtype; // Message type used for message queue
	pid_t pid;
	unsigned address;
	bool isWrite;
	bool granted; // Grant resources to worker
} msgbuffer;

// Global variables
PCB* processTable; // Process control block table to track child processes
Frame* frameTable;
queue<int> waitQueue;

int running; // Amount of running processes in system

int *shm_ptr; // Shared memory pointer to store system clock
int shm_id; // Shared memory ID

int msqid; // Queue ID for communication
msgbuffer buf; // Message buffer to send messages
msgbuffer rcvbuf; // Message buffer to receive messages

bool logging = false; // Bool to determine if output should also print to logfile
FILE* logfile = NULL; // Pointer to logfile

void print_usage(const char * app)
{
	fprintf(stdout, "usage: %s [-h] [-n proc] [-s simul] [-i intervalInMsToLaunchChildren] [-f]\n", app);
	fprintf(stdout, "      proc is the number of total children to launch\n");
	fprintf(stdout, "      simul indicates how many children are to be allowed to run simultaneously\n");
	fprintf(stdout, "      itnterval is the time between launching children\n");
	fprintf(stdout, "      selecting f will output to a logfile as well\n");
}

// Function to increment system clock in seconds and nanoseconds
void incrementClock()
{
	// Update nanoseconds and check for overflow
	shm_ptr[1] += 10000000;
	if (shm_ptr[1] >= 1000000000) // Determines if nanosec is gt 1 billion, meaning it should convert to 1 second
	{
		shm_ptr[1] -= 1000000000;
		shm_ptr[0]++;
	}

}

// Function to add a small overhead of 1000 ns to the clock (less amount than incrmenting clock)
void addOverhead()
{
	// Increment ns in shared memor
	shm_ptr[1] += 1000;
	// Check for overflow
	if (shm_ptr[1] >= 1000000000)
	{
		shm_ptr[1] -= 1000000000;
		shm_ptr[0]++;
	}
}

// Function to access and add to shared memory
void shareMem()
{
	// Generate key
	const int sh_key = ftok("main.c", 0);
	// Create shared memory
	shm_id = shmget(sh_key, sizeof(int) * 2, IPC_CREAT | 0666);
	if (shm_id <= 0) // Check if shared memory get failed
	{
		// If true, print error message and exit
		fprintf(stderr, "Shared memory get failed\n");
		exit(1);
	}
	
	// Attach shared memory
	shm_ptr = (int*)shmat(shm_id, 0, 0);
	if (shm_ptr <= 0)
	{
		fprintf(stderr, "Shared memory attach failed\n");
		exit(1);
	}
	// Initialize shared memory pointers to represent clock
	// Index 0 represents seconds, index 1 represents nanoseconds
	shm_ptr[0] = 0;
	shm_ptr[1] = 0;
}

// Function to print formatted process table, each process's page table,  and frame table to console. Will also print to logfile if necessary.
void printInfo(int n)
{

	printf("\n");
	if (logging) fprintf(logfile, "\n");

	// Print process control block 	
	printf("OSS PID: %d SysClockS: %u SysClockNano: %u\n Process Table:\n", getpid(), shm_ptr[0], shm_ptr[1]);
	printf("Entry\tOccupied\tPID\tStartS\tStartNs\n");
	
	if(logging) fprintf(logfile, "OSS PID: %d SysClockS: %u SysClockNano: %u\n Process Table:\n", getpid(), shm_ptr[0], shm_ptr[1]);
	if (logging) fprintf(logfile,"Entry\tOccupied\tPID\tStartS\tStartNs\n");

	for (int i = 0; i < n; i++)
	{
		// Print table only if occupied by process
		if (processTable[i].occupied == 1)
		{
			printf("%d\t%d\t\t%d\t%u\t%u\n", i, processTable[i].occupied, processTable[i].pid, processTable[i].startSeconds, processTable[i].startNano);
			if (logging) fprintf(logfile, "%d\t%d\t\t%d\t%u\t%u\n", i, processTable[i].occupied, processTable[i].pid, processTable[i].startSeconds, processTable[i].startNano);
		}
	}
	printf("\n");
	if (logging) fprintf(logfile, "\n");

	// Print frame table
	printf("Current memory layout at time %u:%09u is:\n", shm_ptr[0], shm_ptr[1]);
	if (logging) fprintf(logfile, "Current memory layout at time %u:%09u is:\n", shm_ptr[0], shm_ptr[1]);

	printf("      %-8s %-8s %-8s %-12s\n", "Occupied", "DirtyBit", "LastRefS", "LastRefNano");
	if (logging) fprintf(logfile, "      %-8s %-8s %-8s %-12s\n", "Occupied", "DirtyBit", "LastRefS", "LastRefNano");

	for (int i = 0; i < FRAME_NUM; i++)
	{
		string occ = "No";
		if (frameTable[i].occupied)
			occ = "Yes";
		printf("Frame %d: %-8s %-8d %-8lld %-12lld\n", i, occ.c_str(), frameTable[i].dirty, frameTable[i].lastRefSec, frameTable[i].lastRefNano);
		if (logging)
			fprintf(logfile, "Frame %d: %-8s %-8d %-8lld %-12lld\n", i, occ.c_str(), frameTable[i].dirty, frameTable[i].lastRefSec, frameTable[i].lastRefNano);
	}
	printf("\n");
	if (logging) fprintf(logfile, "\n");

	// Print each process's page table
	for (int i = 0; i < n; i++)
	{
		if(!processTable[i].occupied) continue;
		printf("P%d page table: [", i);
		if (logging) fprintf(logfile,"P%d page table: [", i);
		for (int j = 0; j < 32; j++)
		{
			printf(" %d", processTable[i].pageTable[j]);
			if (logging) fprintf(logfile, " %d", processTable[i].pageTable[j]);
		}
		printf(" ]\n");
		if (logging) fprintf(logfile, " ]\n");
	}

	printf("\n");
	if (logging) fprintf(logfile, "\n");

}

// Function to perform least recently used algorithm on frame table, passing in process's PCB index as parameter
int lruReplacement(int slot)
{
	// Get page number that process is waiting to load
	unsigned page = processTable[slot].waitPage;
	
	// Attempt to find free frame
	int frame = -1;
	for (int i = 0; i < FRAME_NUM; i++)
	{
		if (!frameTable[i].occupied)
		{
			frame = i;
			break;
		}
	}

	if (frame < 0) // If true, no free frame found
	{
		// Take first frame in table and calculate time referenced
		frame = 0;
		long long oldest = (long long)frameTable[0].lastRefSec * 1000000000 + (long long)frameTable[0].lastRefNano;
		// Increment through rest of frame table, updating oldest value to find frame used the longest time ago
		for (int i = 1; i < FRAME_NUM; i++)
		{
			long long t = (long long)frameTable[i].lastRefSec * 1000000000 + (long long)frameTable[i].lastRefNano;
			if (t < oldest)
			{
				oldest = t;
				frame = i;
			}
		}


		// Print frame swap
		printf("oss: Clearing frame %d and swapping in p%d page %u\n", frame, slot, page);
		if (logging)
			fprintf(logfile, "oss: Clearing frame %d and swapping in p%d page %u\n", frame, slot, page);

		// Find pid of process who the frame belonged to, and remove page from process's table
		pid_t victim = frameTable[frame].ownerPid;
		int vicPage = frameTable[frame].pageNum;
		for (int i = 0; i < MAX_PROC; i++)
		{
			if (processTable[i].occupied && processTable[i].pid == victim)
			{
				processTable[i].pageTable[vicPage] = -1;
				break;
			}
		}
	}

	// Update PCB and frame table to add new frame for process
	processTable[slot].pageTable[page] = frame;
	frameTable[frame].occupied = true;
	frameTable[frame].ownerPid = processTable[slot].pid;
	frameTable[frame].pageNum = page;
	// Set dirty bit based on whether request was read or write
	frameTable[frame].dirty = processTable[slot].waitIsWrite;
	// Update time last referenced in frame table
	frameTable[frame].lastRefSec = shm_ptr[0];
	frameTable[frame].lastRefNano = shm_ptr[1];

	// Return found frame
	return frame;
}

// Signal handler to terminate all processes after 5 seconds in real time
void signal_handler(int sig)
{
	printf("5 seconds have passed, process(es) will now terminate.\n");
	pid_t pid;

	// Loop through process table to find all processes still running and terminate
	for (int i = 0; i < 18; i++)
	{
		if(processTable[i].occupied)
		{
			pid = processTable[i].pid;
			if (pid > 0)
				kill(pid, SIGKILL);
		}
	}
	 // Detach from shared memory and remove it
        if(shmdt(shm_ptr) == -1)
        {
                perror("shmdt failed");
                exit(1);
        }
        if (shmctl(shm_id, IPC_RMID, NULL) == -1)
        {
                perror("shmctl failed");
                exit(1);
        }

        // Remove the message queue
        if (msgctl(msqid, IPC_RMID, NULL) == -1)
        {
                perror("msgctl failed");
                exit(1);
        }


	exit(1);
}

int main(int argc, char* argv[])
{
	// Signal that will terminate program after 5 sec (real time)
	signal(SIGALRM, signal_handler);
	alarm(5);

	key_t key; // Key to access queue

	// Create file to track message queue
	system("touch msgq.txt");

	// Get key for message queue
	if ((key = ftok("msgq.txt", 1)) == -1)
	{
		perror("ftok");
		exit(1);
	}

	// Create message queue
	if ((msqid = msgget(key, PERMS | IPC_CREAT)) == -1)
	{
		perror("msgget in parent\n");
		exit(1);
	}	

	printf("Message queue set up\n");

	// Structure to hold values for options in command line argument
	options_t options;

	// Set default values
	options.proc = 1;
	options.simul = 1;
	options.interval = 0;


	// Values to keep track of child iterations
	int total = 0; // Total number of child processes spawned
	running = 0; // Number of simultaneous processes in system
	int totRefs = 0; // Total number of memory reference requests received
	int totFaults = 0; // Total number of page faults

	const char optstr[] = "hn:s:i:f"; // Options h, n, s, t, i, f
	char opt;
	
	// Parse command line arguments with getopt
	while ( (opt = getopt(argc, argv, optstr)) != -1)
	{
		switch(opt)
		{
			case 'h': // Help
				// Prints usage
				print_usage(argv[0]);
				return EXIT_SUCCESS;
			case 'n': // Total amount of processes
				// Check if n's argument starts with '-'
				if (optarg[0] == '-')
				{
					// Check if next character starts with other option, meaning no argument given for n and another option given
					if (optarg[1] == 's' || optarg[1] == 'i' || optarg[1] == 'f' ||  optarg[1] == 'h')
					{
						// Print an error statement, print usage, and exit program
						fprintf(stderr, "Error! Option n requires an argument.\n");
						print_usage(argv[0]);
						return EXIT_FAILURE;
					}
					else // Means argument is not another option, but is still invalid input
					{
						fprintf(stderr, "Error! Invalid input.\n");
						print_usage(argv[0]);
						return EXIT_FAILURE;
					}
				}
				// Loop to ensure all characters in n's argument are digits
				for (int i = 0; optarg[i] != '\0'; i++)
				{
					if (!isdigit(optarg[i]))
					{
						// If non digit is found, print error statement, print usage, and exit program
						fprintf(stderr, "Error! %s is not a valid number.\n", optarg);
						print_usage(argv[0]);
						return EXIT_FAILURE;
					}
				}

				// Set proc to optarg, ensure no more than 100 proc,  and break
				options.proc = atoi(optarg);
				if (options.proc > 100)
				{
					fprintf(stderr, "Warning: no more than 100 total processes allowed, -n will be set to 100\n");
					options.proc = 100;
				}
				break;
			
			case 's': // Total amount of processes that can run simultaneously
				// Checks if s's argument starts with '-'
				if (optarg[0] == '-')
				{
					// Check if next character starts with other option, meaning no argument given for n and another option given
					if (optarg[1] == 'n' || optarg[1] == 'i' || optarg[1] == 'f' ||  optarg[1] == 'h')
					{
						// Print an error statement, print usage, and exit program
						fprintf(stderr, "Error! Option s requires an argument.\n");
						print_usage(argv[0]);
						return EXIT_FAILURE;
					}
					else // Means argument is not another option, but is still invalid input
					{
						fprintf(stderr, "Error! Invalid input.\n");
						print_usage(argv[0]);
						return EXIT_FAILURE;
					}
				}
				// Loop to ensure all characters in s's argument are digits
				for (int i = 0; optarg[i] != '\0'; i++)
				{
					if (!isdigit(optarg[i]))
					{
						fprintf(stderr, "Error! %s is not a valid number.\n", optarg);
						print_usage(argv[0]);
						return EXIT_FAILURE;
					}
				}

				// Set simul to optarg and break
				options.simul = atoi(optarg);
				if (options.simul > 18)
				{
					fprintf(stderr, "Error! Value entered for options s cannot exceed 18. %d > 18.\n", options.simul);
					print_usage(argv[0]);
					return EXIT_FAILURE;
				}
				break;

			case 'i':
				// Checks if i's argument starts with '-'
				if (optarg[0] == '-')
				{
					// Checks if next character is character of other option, meaning no argument given for i and another option given
					if (optarg[1] == 'n' || optarg[1] == 's' || optarg[1] == 'f' ||  optarg[1] == 'h')
					{
						// Print error statement, print usage, and exit program
						fprintf(stderr, "Error! Option i requires an argument.\n");
						print_usage(argv[0]);
						return EXIT_FAILURE;
					}
					else // Means argument is not another option, but is invalid input
					{
						// Print error statement, print usage, and exit program
						fprintf(stderr, "Error! Invalid input.\n");
						print_usage(argv[0]);
						return EXIT_FAILURE;
					}
				}
				// Loop to ensure all characters in i's argument are digits
				for (int i = 0; optarg[i] != '\0'; i++)
				{
					if (!isdigit(optarg[i]))
					{
						fprintf(stderr, "Error! %s is not a valid number.\n", optarg);
						print_usage(argv[0]);
						return EXIT_FAILURE;
					}
				}

				// Set interval to optarg, converting to ns,  and break
				options.interval = atoll(optarg) * 1000000;
				break;

			case 'f': // Print output also to logfile if option is passed
				logging = true;
				// Open logfile
				logfile = fopen("ossLog.txt", "w");
				if (logfile == NULL)
				{
					fprintf(stderr, "Error! Failed to open logfile.\n");
					return EXIT_FAILURE;
				}
				break;

			default:
				// Prints message that option given is invalid, prints usage, and exits program
				fprintf(stderr, "Error! Invalid option %c.\n", optopt);
				print_usage(argv[0]);
				return EXIT_FAILURE;
		}
	}
			

	// Set up shared memory for clock
	shareMem();

	// Allocate memory for process table based on total processes
	processTable = new PCB[MAX_PROC];
	// Initialize process table, all values set to empty
	for (int i = 0; i < MAX_PROC; i++)
	{
		// Set occupied to 0
		processTable[i].occupied = 0;
		// Set pid to -1 
		processTable[i].pid = -1;
		processTable[i].startSeconds = 0;
		processTable[i].startNano = 0;
		processTable[i].waiting = false;
		processTable[i].waitPage = -1;
		processTable[i].waitSec = 0;
		processTable[i].waitNano = 0;
		for (int j = 0; j < 32; j++)
		{
			processTable[i].pageTable[j] = -1;
		}
	}

	// Allocate memory for frame table based on total frames
	frameTable = new Frame[FRAME_NUM];
	// Initialize frame table, all values set to empty
	for (int i = 0; i < FRAME_NUM; i++)
	{
		frameTable[i].occupied = false;
		frameTable[i].dirty = false;
		frameTable[i].ownerPid = -1;
		frameTable[i].pageNum = -1;
		frameTable[i].lastRefSec = 0;
		frameTable[i].lastRefNano = 0;
	}

	// Variables to track last printed time
        long long int lastPrintSec = shm_ptr[0];
        long long int lastPrintNs = shm_ptr[1];

	// Calculate current system time in ns
	long long currTimeNs = (long long)shm_ptr[0] * 1000000000 + shm_ptr[1];
	// Calculate next time to spawn a process based on command line value given for interval
	long long nSpawnT = currTimeNs + options.interval;

	// Loop that will continue until total amount of processes given are launched and all running processes are terminated
	while (total < options.proc ||  running > 0)
	{
		// Update system clock
		incrementClock();

		// Loop through and terminate any process that are finished
		pid_t pid;
		int status;
		while ((pid = waitpid(-1, &status, WNOHANG)) > 0)
		{
			// Find process's location in process table
			int indx;
			for (int i = 0; i < options.proc; i++)
			{
				if (processTable[i].occupied == 1 && processTable[i].pid == pid)
				{
					indx = i;
					break;
				}
			}

			// Clear process's entires in PCB and frame table
			for (int i = 0; i < 32; i++)
			{
				processTable[indx].waiting = false;
			}

			for (int i = 0; i < FRAME_NUM; i++)
			{
				if (frameTable[i].ownerPid == pid)
				{
					frameTable[i].occupied = false;
					frameTable[i].ownerPid = -1;
					frameTable[i].pageNum = -1;
					frameTable[i].dirty = false;
				}
			}

			// Mark finished process as unoccupied in process table
			processTable[indx].occupied = 0;
			// Decrement total processes running
			running--;
		}

		// Calculate time since last print for sec and ns
		long long int printDiffSec = shm_ptr[0] - lastPrintSec;
		long long int printDiffNs = shm_ptr[1] - lastPrintNs;
		// Adjust ns value for subtraction resulting in negative value
		if (printDiffNs < 0)
		{
			printDiffSec--;
			printDiffNs += 1000000000;
		}
		// Calculate total time sincd last print in ns
		long long int printTotDiff = printDiffSec * 1000000000 + printDiffNs;

		if (printTotDiff >= 1000000000) // Determine if time of last print surpasssed .5 sec system time
		{
			// If true, print table and update time since last print in sec and ns
			printInfo(18);
			lastPrintSec = shm_ptr[0];
			lastPrintNs = shm_ptr[1];
		}

		currTimeNs = (long long)shm_ptr[0] * 1000000000 + shm_ptr[1];
		// Determine if a new child process can be spawned
		// Must be greater than next spawn time, less than total process allowed (100), and less than simultanous processes allowed (18)
		if (currTimeNs >= nSpawnT && total < options.proc  && running < options.simul)
		{
			//Fork new child
			pid_t childPid = fork();
			if (childPid == 0) // Child process
			{
				// Create array of arguments to pass to exec. "./worker" is the program to execute, arg is the command line argument
				// to be passed to "./worker", and NULL shows it is the end of the argument list
				char* args[] = {"./worker",  NULL};
				// Replace current process with "./worker" process and pass iteration amount as parameter
				execvp(args[0], args);
				// If this prints, means exec failed
				// Prints error message and exits
				fprintf(stderr, "Exec failed, terminating!\n");
				exit(1);
			}
			else // Parent process
			{
				// Increment total created processes and running processes
				total++;
				running++;
					
				// Increment clock
				incrementClock();

				// Update table with new child info
				for (int i = 0; i < 18; i++)
				{
					if (processTable[i].occupied == 0)
					{
						processTable[i].occupied = 1;
						processTable[i].pid = childPid;
						processTable[i].startSeconds = shm_ptr[0];
						processTable[i].startNano = shm_ptr[1];
						break;
					}
				}
				// Calculate current time and ns and determine next spawn time
				currTimeNs = (shm_ptr[0] * 1000000000) + shm_ptr[1];
				nSpawnT = currTimeNs + options.interval;

			}
		}

		// Check for message from worker process
		if (msgrcv(msqid, &rcvbuf, sizeof(msgbuffer) - sizeof(long), 1, IPC_NOWAIT) > 0)
		{
			// Increment total reference requests
			totRefs++;

			// Find process who sent message in PCB
			int slot = -1;
			for (int i = 0; i < MAX_PROC; i++)
			{
				if (processTable[i].occupied && processTable[i].pid == rcvbuf.pid)
				{
					slot = i;
					break;
				}
			}

			// Calculate page number from address sent, ensuring it is not greater than max amount of entries
			unsigned page = rcvbuf.address / 1024;
			if (page >= 32)
			{
				fprintf(stderr, "ERROR! OSS: bad address %u. Page %u out of range.\n", rcvbuf.address, page);
				exit(1);
			}

			// Determine if request was read or write and set to string for printing
			string op;
			if (rcvbuf.isWrite) op = "write";
			else op = "read";

			// Print incoming request
			printf("oss: P%d requesting %s of address %u at time %d:%09d\n", slot, op.c_str(), rcvbuf.address, shm_ptr[0], shm_ptr[1]);
			if (logging)
				fprintf(logfile, "oss: P%d requesting %s of address %u at time %d:%09d\n", slot, op.c_str(), rcvbuf.address, shm_ptr[0], shm_ptr[1]);

			
			// Check page table entry 
			int frame = processTable[slot].pageTable[page];
			if (frame != -1) // Determine if frame found in table
			{
				// Add overhead
				addOverhead();
				// Add additional 100ns for accessing, ensuring no overflow
				shm_ptr[1] += 100;
				if (shm_ptr[1] >= 1000000000)
				{
					shm_ptr[1] -= 1000000000;
					shm_ptr[0]++;
				}


				// Update last reference time in frame table
				frameTable[frame].lastRefSec = shm_ptr[0];
				frameTable[frame].lastRefNano = shm_ptr[1];

				// Prepare and send message to worker, granting requst
				buf.mtype = rcvbuf.pid;
				buf.granted = true;
				if (msgsnd(msqid, &buf, sizeof(msgbuffer) - sizeof(long), 0) == -1)
				{
					perror("msgsnd grant");
					exit(1);
				}
				else
				{
					// Determine whether it is a read or write
					if (rcvbuf.isWrite)
					{
						// Update dirty bit if it is a write
						frameTable[frame].dirty = true;
						// Print write
						printf("oss: Address %u in frame %d, writing data to frame at time %d:%09d\n", rcvbuf.address, frame, shm_ptr[0], shm_ptr[1]);
						if (logging)
							fprintf(logfile, "oss: Address %u in frame %d, writing data to frame at time %d:%09d\n", rcvbuf.address, frame, shm_ptr[0], shm_ptr[1]);
					}
					else
					{
						// Print read
						printf("oss: Address %u in frame %d, giving data to P%d at time %d:%09d\n", rcvbuf.address, frame, slot, shm_ptr[0], shm_ptr[1]);
						if (logging)
							fprintf(logfile, "oss: Address %u in frame %d, giving data to P%d at time %d:%09d\n", rcvbuf.address, frame, slot, shm_ptr[0], shm_ptr[1]);
					}
					
				}
			}
			else // Page fault
			{
				// Increment total page faults
				totFaults++;

				// Print page fault
				printf("oss: Address %u is not in a frame, pagefault\n", rcvbuf.address);
				if (logging)
					fprintf(logfile, "oss: Address %u is not in a frame, pagefault\n", rcvbuf.address);

				// Mark process in PCB table as waiting
				processTable[slot].waiting = true;
				processTable[slot].waitPage = page;
				processTable[slot].waitIsWrite = rcvbuf.isWrite;
				processTable[slot].waitSec = shm_ptr[0];
				processTable[slot].waitNano = shm_ptr[1];

				// Add process to wait queue
				waitQueue.push(slot);
			}
		}

		// Check for any waiting processes in the queue
		if (!waitQueue.empty())
		{
			// Get index of next waiting process from queue 
			int slot = waitQueue.front();
			
			// Compute elapsed system time since fault
			currTimeNs = ((long long)shm_ptr[0] * 1000000000) + (long long)shm_ptr[1];
			long long faultNs = (long long)processTable[slot].waitSec * 1000000000 + (long long)processTable[slot].waitNano;
			// Add latency for page fault
			long long latNs = 14 * 1000000;
			if (processTable[slot].waitIsWrite)
				latNs += 1000000;

			// Determine if enough time has passed to service page fault
			if (currTimeNs - faultNs >= latNs)
			{
				// Remove process from wait queue
				waitQueue.pop();


				// Load faulted page into frame using LRU replacement algorithm
				unsigned page = processTable[slot].waitPage;
				int frame = lruReplacement(slot);

				// Update info in process table to reflect loaded page
				processTable[slot].pageTable[page] = frame;
				processTable[slot].waiting = false;

				// Update info in frame table to reflect loaded page
				frameTable[frame].occupied = true;
				frameTable[frame].ownerPid = processTable[slot].pid;
				frameTable[frame].pageNum = page;
				frameTable[frame].dirty = processTable[slot].waitIsWrite;
				frameTable[frame].lastRefSec = shm_ptr[0];
				frameTable[frame].lastRefNano = shm_ptr[1];

				// Add overhead of loading page
				addOverhead();

				// Prepare and send message to worker, granting requesst
				buf.mtype = processTable[slot].pid;
				buf.granted = true;
				if (msgsnd(msqid, &buf, sizeof(buf) - sizeof(long), 0) == -1)
				{
					perror("msgsnd queue grant");
					exit(1);
				}

				// Determine if read or write for printing
				string opr = "read";
				if(processTable[slot].waitIsWrite) 
				{
					// If write, print and add additional time (dirty bit set in LRU algorithm)
					opr = "write";
					printf("oss: Dirty bit of frame %d set, adding additional time to the clock\n", frame);
					if (logging)
						fprintf(logfile, "oss: Dirty bit of frame %d set, adding additional time to the clock\n", frame);
					addOverhead();
				}

				// Determine address of process and print
				unsigned addr = processTable[slot].waitPage * 1024;
				printf("oss: Indicating to P%d that %s has happened to the address %u\n", slot, opr.c_str(), addr);
				if (logging)
					fprintf(logfile, "oss: Indicating to P%d that %s has happened to the address %u\n", slot, opr.c_str(), addr);
			}
		}

	}

	// Update time for statistics
	currTimeNs = ((long long)shm_ptr[0] * 1000000000) + (long long)shm_ptr[1];

	// Calculate and print statistics
	double refsPerSec = ((double)totRefs * 1000000000) / currTimeNs;

	double faultRate;
	if (totRefs > 0)
		faultRate = (100.0 * totFaults) / totRefs;
	else faultRate = 0.0;

	printf("\n----Simulation Statistics----\n");
	if (logging) fprintf(logfile, "\n----Simulation Statistics----\n");
	printf("Total memory references: %d\n", totRefs);
	if (logging) fprintf(logfile, "Total memory references: %d\n", totRefs);
	printf("Total page faults: %d\n", totFaults);
	if (logging) fprintf(logfile, "Total page faults: %d\n", totFaults);
	printf("Fault rate: %.2f%%\n", faultRate);
	if (logging) fprintf(logfile, "Fault rate: %.2f%%\n", faultRate);
	printf("References per sec of system time: %.2f\n", refsPerSec);
	if (logging) fprintf(logfile, "References per sec of system time: %.2f\n", refsPerSec);

	// Detach from shared memory and remove it
	if(shmdt(shm_ptr) == -1)
	{
		perror("shmdt failed");
		exit(1);
	}
	if (shmctl(shm_id, IPC_RMID, NULL) == -1)
	{
		perror("shmctl failed");
		exit(1);
	}

	// Remove the message queue
	if (msgctl(msqid, IPC_RMID, NULL) == -1)
	{
		perror("msgctl failed");
		exit(1);
	}

	return 0;

}

