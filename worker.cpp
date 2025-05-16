// Operating Systems Project 6
// Author: Maija Garson
// Date: 05/15/2025
// Description: Worker process launched by oss. Uses the clock in shared memory and loops continuously. Within the loop, it will randomly generate a time to
// act within BOUND_NS (1000). Once it acts, it will randomly generate a probability to determine if it should request to read or write to a memory reference.
// It will randomly generate an address to request to. It will then send a message to oss reflecting this. It will then wait for a response from oss and will
// update its values if the message was granted. Each time it sends/receives a message it will increment the system clock. It will also continuously check every
// 250000000 ns if it has run for 1 sec. If it has run for that time, it will randomly generate a probability to determine if it should terminate or continue looping.
// Once it terminates, it will release all held resources, detaches from shared memory, and exit.

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <unistd.h>
#include <sys/shm.h>
#include <sys/types.h>
#include <sys/msg.h>
#include <cstdio>
#include <cstdlib>

#define PERMS 0644
#define BOUND_NS 1000
#define TERM_CHECK_NS 250000000
#define LIFE_NS 2000000000
#define TERM_PROB 40

// Message buffer structure
typedef struct msgbuffer
{
	long mtype; 
	pid_t pid;
	unsigned address;
	bool isWrite;	
	bool granted;
} msgbuffer;

// Shared memory pointers for system clock
int *shm_ptr;
int shm_id;

// Function to attach to shared memory
void shareMem()
{
	// Generate key
	const int sh_key = ftok("main.c", 0);
	// Access shared memory
	shm_id = shmget(sh_key, sizeof(int) * 2, 0666);

	// Determine if shared memory access not successful
	if (shm_id == -1)
	{
		// If true, print error message and exit 
		fprintf(stderr, "Child: Shared memory get failed.\n");
		exit(1);
	}

	// Attach shared memory
	shm_ptr = (int *)shmat(shm_id, 0, 0);
	//Determine if insuccessful
	if (shm_ptr == (int *)-1)
	{
		// If true, print error message and exit
		fprintf(stderr, "Child: Shared memory attach failed.\n");
		exit(1);
	}
}

// Function to increment time by 1000 ns 
void addTime()
{
	shm_ptr[1] += 1000;
	// Ensure ns did not overflow 
	if (shm_ptr[1] > 1000000000) 
	{
		shm_ptr[1] -= 1000000000;
		shm_ptr[0]++;
	}
}

int main(int argc, char* argv[])
{
	shareMem();
	
	// Info needed for message sending/receiving
	msgbuffer buf;
	msgbuffer rcvbuf;
	buf.mtype = 1;
	buf.pid = getpid();
	int msqid = 0;
	key_t key;

	// Get key for message queue
	if ((key = ftok("msgq.txt", 1)) == -1)
	{
		perror("ftok");
		exit(1);
	}

	// Create message queue
	if ((msqid = msgget(key, PERMS)) == -1)
	{
		perror("msgget in child\n");
		exit(1);
	}

	// Represents time process started in ns
	long long startTimeNs = (long long)shm_ptr[0] * 1000000000 + shm_ptr[1];
	long long lastTermChk = startTimeNs;

	// Randomly generate a number within bound ns to determine when worker will act 
	srand(getpid());
	long long nAct = startTimeNs + (rand() % BOUND_NS);

	while(true)
	{
		// Calculate current system time in ns
		long long currTimeNs = (long long)shm_ptr[0] * 1000000000 + shm_ptr[1];

		// Determine if worker should terminate every time it reaches term check (25000000 ns)
		if (currTimeNs - lastTermChk >= TERM_CHECK_NS)
		{
			lastTermChk = currTimeNs;
			// Once lifetime (1 sec)  is reached, worker will determine if it should terminate
			if (currTimeNs - startTimeNs >= LIFE_NS)
			{
				// Randomly generate number up to 100 to determine if worker will terminate
				int die = rand() % 100;
				// If randomly generated number is less than term probability (40), it will terminate
				if (die < TERM_PROB)
				{
					// Detach from shared memory and exit
					if (shmdt(shm_ptr) == -1)
					{
						perror("shmdt failed");
						exit(1);
					}
					exit(0);
				}
			}
		}

		// Determine if current time has reached time for worker to act
		if (currTimeNs >= nAct)
		{

			// Randomly generate address within allowed range
			buf.address = rand() % 32768;
			// Randomly determine if process will request read or write
			buf.isWrite = rand() % 2;
			// Clear granted flag before sending
			buf.granted = false;

			// Send message to oss
			if (msgsnd(msqid, &buf, sizeof(buf) - sizeof(long), 0) == -1)
			{
				perror("child msgsnd");
				exit(1);
			}
			// Add overhead for sending message
			addTime();

			// Wait to receive message back from oss
			if (msgrcv(msqid, &rcvbuf, sizeof(rcvbuf) - sizeof(long), getpid(), 0) == -1)
			{
				perror("child msgrcv");
				exit(1);
			}
			// Add overhead for receiving message
			addTime();

			// Randomly generate time for next act
			nAct = currTimeNs + (rand() % BOUND_NS);
		}
	}


	return 0;
}
