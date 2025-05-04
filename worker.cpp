// Operating Systems Project 5
// Author: Maija Garson
// Date: 04/29/2025
// Description: Worker process launched by oss. Uses the clock in shared memory and loops continuously. Within the loop, it will randomly generate a time to
// act within BOUND_NS (1000). Once it acts, it will randomly generate a probability to determine if it should request a new resource or release resources
// being held. It sends a message to oss informing if it is a request or release, along with the resource id. It will then wait for a response from oss and will
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
#define MAX_RES 5
#define INST_PER_RES 10
#define BOUND_NS 1000
#define TERM_CHECK_NS 250000000
#define LIFE_NS 2000000000
#define TERM_PROB 40

// Message buffer structure
typedef struct msgbuffer
{
	long mtype; 
	pid_t pid; 
	int resId;
	bool isRelease;
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

	// Represents how many of each resource worker holds
	int held[MAX_RES] = {0};

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
					// Release all resources currently held 
					for (int i = 0; i < MAX_RES; i++)
					{
						while (held[i] > 0)
						{
							held[i]--;
							buf.mtype = 1;
							buf.resId = i;
							buf.isRelease = true;

							// Send message to OSS informing of release
							if (msgsnd(msqid, &buf, sizeof(buf) - sizeof(long), 0) == -1)
							{
								perror("msgsnd release");
								exit(1);
							}
							// Increment time for message sending
							addTime();
							
							// Wait for OSS to acknowledge realease
							if (msgrcv(msqid, &rcvbuf, sizeof(rcvbuf) - sizeof(long), getpid(), 0) == -1)
							{
								perror("msgrcv release ack");
								exit(1);
							}
							// Increment time for message receivingg
							addTime();

						
						}
					}

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
			// Randomly generate number up to 100 to determine if worker will request or release
			int outcome = rand() % 100;
			bool release;
			// If outcome is greater than 5, worker will request. Otherwise worker will release
			if (outcome > 5)
				release = false;
			else
				release = true;


			int r;
			if (release) // Worker is releasing
			{
				// Randomly choose a resource to release
				int tries = 0;
				while (tries < MAX_RES)
				{
					r = rand() % MAX_RES;
					if (held[r] > 0)
						break;
					tries++;
				}
				// Once max resource amount is reached, randomly generate time for next act and continue
				if (tries == MAX_RES)
				{
					nAct = currTimeNs + (rand() % BOUND_NS);
					continue;
				}
			}
			else // Worker is requestin
			{
				// Randomly choose a resource to request
				int tries = 0;
				while (tries < MAX_RES)
				{
					r = rand() % MAX_RES;
					if (held[r] < INST_PER_RES)
						break;
					tries++;
				}
				// Once max resource amount is reached, randomly generate time for next act and continue
				if (tries == MAX_RES)
				{
					nAct = currTimeNs + (rand() % BOUND_NS);
					continue;
				}
			}
		

			// Prepare info to send message to OSS, informing if it is a release or request and what resource is selected
			buf.mtype = 1;
			buf.pid = getpid();
			buf.resId = r;
			buf.isRelease = release;
			buf.granted = false; 
			// Send request/release message to OSS
			if (msgsnd(msqid, &buf, sizeof(buf) - sizeof(long), 0) == -1)
			{
				perror("msgsnd request");
				exit(1);
			}

			// Increment time for message sending
			addTime();
		
			// Wait until OSS sends a message back
			if (msgrcv(msqid, &rcvbuf, sizeof(rcvbuf) - sizeof(long), getpid(), 0) == -1)
			{
				perror("msgrcv grant");
				exit(1);
			}

			// Increment time for message receiving
			addTime();

			if (rcvbuf.granted) // If new resource was received
			{
				if (release)
					held[r]--;
				else
					// Increment held at resoure's location 
					held[r]++;
			}

			// Randomly generate time for next act
			nAct = currTimeNs + (rand() % BOUND_NS);
		}
	}


	return 0;
}
