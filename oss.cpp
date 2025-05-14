
// Operating Systems Project 5
// Author: Maija Garson
// Date: 04/29/2025
// Description: A program that simulates a resource management operating system with deadlock detection and recovery.
// This program runs until it has forked the total amount of processes specified in the command line, while allowing a
// specified amount of processes to run simultanously. It will allocate shared memory to represent a system clock. It will
// also keep track of a process control block table for all processes and a resource table for 5 resource types with 10 
// instances each. It will receive messages from child processes that represent a resource request or release. If it 
// receives a request, it will grant the request if possible or it will add the child to a wait queue if not possible.
// When requests/releases are received, it will update values in both tables to reflect this. It will print both tables 
// every .5 sec of system time. It will run a deadlock detection algorithm every 1 sec of system time. If a deadlock is
// found, it will run a recovery algorithm, incrementally killing processes, until the system is no longer in a deadlock.
// It will calculate and print final statistics at the end of each run.
// The program will send a kill signal to all processes and terminate if 3 real-life seconds are reached.

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
#define FRAME_NUM 8

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
        int occupied; // Either true or false
        pid_t pid; // Process ID of this child
        int startSeconds; // Time when it was forked
        int startNano; // Time when it was forked
	int pageTable [32];
	bool waiting;
	int waitPage;
	bool waitIsWrite;
	long long waitSec;
	long long waitNano;
} PCB;

typedef struct
{
	bool occupied;
	bool dirty;
	pid_t ownerPid;
	int pageNum;
	long long lastRefSec;
	long long lastRefNano;
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

// FUnction to print formatted process table and resource table to console. Will also print to logfile if necessary.
void printInfo(int n)
{

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
}

int lruReplacement(int slot)
{
	unsigned page = processTable[slot].waitPage;
	
	int frame = -1;
	for (int i = 0; i < FRAME_NUM; i++)
	{
		if (!frameTable[i].occupied)
		{
			frame = i;
			break;
		}
	}

	if (frame < 0)
	{
		frame = 0;
		long long oldest = (long long)frameTable[0].lastRefSec * 1000000000 + (long long)frameTable[0].lastRefNano;
		for (int i = 1; i < FRAME_NUM; i++)
		{
			long long t = (long long)frameTable[i].lastRefSec * 1000000000 + (long long)frameTable[i].lastRefNano;
			if (t < oldest)
			{
				oldest = t;
				frame = i;
			}
		}

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

	processTable[slot].pageTable[page] = frame;
	frameTable[frame].occupied = true;
	frameTable[frame].ownerPid = processTable[slot].pid;
	frameTable[frame].pageNum = page;
	frameTable[frame].dirty = processTable[slot].waitIsWrite;
	frameTable[frame].lastRefSec = shm_ptr[0];
	frameTable[frame].lastRefNano = shm_ptr[1];

	return frame;
}

// Signal handler to terminate all processes after 3 seconds in real time
void signal_handler(int sig)
{
	printf("3 seconds have passed, process(es) will now terminate.\n");
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
	// Signal that will terminate program after 3 sec (real time)
	signal(SIGALRM, signal_handler);
	alarm(3);

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
	int total = 0; // Total amount of processes
	running = 0;
	int msgsnt = 0;

	const char optstr[] = "hn:s:t:i:f"; // Options h, n, s, t, i, f
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

				// Set proc to optarg and break
				options.proc = atoi(optarg);
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
	processTable = new PCB[20];
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

	frameTable = new Frame[FRAME_NUM];
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

		if (printTotDiff >= 500000000) // Determine if time of last print surpasssed .5 sec system time
		{
			// If true, print table and update time since last print in sec and ns
			//printInfo(18);
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

		if (msgrcv(msqid, &rcvbuf, sizeof(msgbuffer) - sizeof(long), 1, IPC_NOWAIT) > 0)
		{
			int slot = -1;
			for (int i = 0; i < MAX_PROC; i++)
			{
				if (processTable[i].occupied && processTable[i].pid == rcvbuf.pid)
				{
					slot = i;
					break;
				}
			}

			unsigned page = rcvbuf.address / 1024;
			if (page >= 32)
			{
				fprintf(stderr, "ERROR! OSS: bad address %u. Page %u out of range.\n", rcvbuf.address, page);
				exit(1);
			}

			
			// Check page table entry
			int frame = processTable[slot].pageTable[page];
			if (frame != -1)
			{
				addOverhead();
				frameTable[frame].lastRefSec = shm_ptr[0];
				frameTable[frame].lastRefNano = shm_ptr[1];

				buf.mtype = rcvbuf.pid;
				buf.granted = true;
				if (msgsnd(msqid, &buf, sizeof(msgbuffer) - sizeof(long), 0) == -1)
				{
					perror("msgsnd grant");
					exit(1);
				}
				else printf("OSS: P%d page %u HIT in frame %d at time %d:%09d\n", slot, page, frame, shm_ptr[0], shm_ptr[1]);
			}
			else // Page fault
			{
				printf("OSS: P%d page %u FAULT at time %d:%09d... queueing\n", slot, page, shm_ptr[0], shm_ptr[1]);

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

		if (!waitQueue.empty())
		{
			int slot = waitQueue.front();
			
			currTimeNs = ((long long)shm_ptr[0] * 1000000000) + (long long)shm_ptr[1];
			long long faultNs = (long long)processTable[slot].waitSec * 1000000000 + (long long)processTable[slot].waitNano;
			long long latNs = 14 * 1000000;
			if (processTable[slot].waitIsWrite)
				latNs += 1000000;

			if (currTimeNs - faultNs >= latNs)
			{
				waitQueue.pop();
				unsigned page = processTable[slot].waitPage;
				
				int frame = lruReplacement(slot);

				processTable[slot].pageTable[page] = frame;
				processTable[slot].waiting = false;

				frameTable[frame].occupied = true;
				frameTable[frame].ownerPid = processTable[slot].pid;
				frameTable[frame].pageNum = page;
				frameTable[frame].dirty = processTable[slot].waitIsWrite;
				frameTable[frame].lastRefSec = shm_ptr[0];
				frameTable[frame].lastRefNano = shm_ptr[1];

				addOverhead();

				buf.mtype = processTable[slot].pid;
				buf.granted = true;
				if (msgsnd(msqid, &buf, sizeof(buf) - sizeof(long), 0) == -1)
				{
					perror("msgsnd queue grant");
					exit(1);
				}
				else printf("OSS: serviced P%d page %u in frame %d at time %d:%09d\n", slot, page, processTable[slot].pageTable[page], shm_ptr[0], shm_ptr[1]);
			}
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

	return 0;

}

