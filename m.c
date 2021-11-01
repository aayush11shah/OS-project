#include "c.h"
#include <sys/wait.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/types.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <pthread.h>

#include <time.h>
#include <stdint.h>
#include <inttypes.h>
#define BILLION  1000000000L
uint64_t nanotime(void)
{
  long int ns;
  uint64_t all;
  time_t sec;
  struct timespec spec;

  clock_gettime(CLOCK_REALTIME, &spec);
  sec = spec.tv_sec;
  ns = spec.tv_nsec;

  all = (uint64_t) sec * BILLION + (uint64_t) ns;

  return all;
}


typedef struct proc_list {
	pid_t pid;
	int proc_type;
	int read_pipe;
	sh_seg_t *shseg;
	int shmid;
	struct proc_list* next;
	
} plist_t;

plist_t *p_list, *tail;

char* itos(int x) {
	int length = snprintf(NULL, 0, "%d", x);
	char* str = malloc(sizeof(int) + 1);
	snprintf(str, length + 1, "%d", x );
	return str;
}

typedef enum sched {
	RR, FCFS
} sch_alg;

int instr = 1;

// updates plist in case of finish
// sends "finish" if process has finished, regardless of str
// returns 1 on finish
int send_instruction(plist_t **pp_list, char *str) {
	plist_t *p_list = *pp_list;

	while(!p_list->shseg->waiting);
	
	char finished = 0;
	if(p_list->shseg->finished) {
		strcpy(str, "finish");
		finished = 1;
	}
	
	pthread_mutex_lock(&p_list->shseg->instr_ready_mutex);
	p_list->shseg->executing = 1;
	
	strcpy(p_list->shseg->sched_instr, str);
	
	pthread_cond_signal(&p_list->shseg->instr_ready_cond);
	pthread_mutex_unlock(&p_list->shseg->instr_ready_mutex);
	while(p_list->shseg->executing);
	
	if(finished) {
		shmdt(p_list->shseg);
		shmctl(p_list->shmid, IPC_RMID, NULL);

		plist_t *prev = p_list;
		while(prev->next != p_list) prev = prev->next;
		
		prev->next = p_list->next;
		free(p_list);
		
		*pp_list = prev->next;
		return 1;
	}
	
	return 0;
}

int main() {
	int n1 = 10, n2 = 10, n3 = 10;
	sch_alg sh = RR;
	p_list = NULL;
	pid_t last_created = getpid();
	int is_child = 0;
	for(int i = 0; i < 3; i++) {
		if(last_created) {
			plist_t* new_proc;
			if(!p_list) {
				new_proc = p_list = tail = (plist_t*) malloc(sizeof(plist_t));
				p_list->next = tail;
			}
			else {
				new_proc = (plist_t*) malloc(sizeof(plist_t));
				new_proc->next = p_list;
				tail->next = new_proc;
				tail = tail->next;
			}
			
			new_proc->pid = last_created;
			
			int p[2];
			pipe(p);
			new_proc->read_pipe = p[READ_END];
			
			key_t shmkey = ftok("m.c", i);
			int shmid = shmget(shmkey, 1024, 0666 | IPC_CREAT);
			sh_seg_t *shseg = shmat(shmid, (void*)0, 0);
			new_proc->shseg = shseg;
			new_proc->shmid = shmid;
			new_proc->proc_type = i;

			pthread_mutexattr_init(&shseg->instr_mutex_attr);
			pthread_mutexattr_setpshared(&shseg->instr_mutex_attr, PTHREAD_PROCESS_SHARED);
			pthread_mutex_init(&shseg->instr_ready_mutex, &shseg->instr_mutex_attr);
			pthread_mutexattr_destroy(&shseg->instr_mutex_attr);
			
			pthread_condattr_init(&shseg->instr_cond_attr);
			pthread_condattr_setpshared(&shseg->instr_cond_attr, PTHREAD_PROCESS_SHARED);
			pthread_cond_init(&shseg->instr_ready_cond, &shseg->instr_cond_attr);
			pthread_condattr_destroy(&shseg->instr_cond_attr);

			shseg->executing = 1;
			shseg->waiting = 0;
			
			last_created = fork();
			if(!last_created) {
				close(p[READ_END]);
				char* args[] = {"./c", itos(p[WRITE_END]), itos(shmkey), itos(i), NULL};
				execvp(args[0], args);
			}
			else {
				close(p[WRITE_END]);
			}
		}
	}
	
	struct timespec ts;
	ts.tv_sec = 0;
	ts.tv_nsec = 10000000;
	
	int total = 3;
	int instr = 1;
	
	while(total) {
		char str[10];
		strcpy(str, "start");
		
		if(send_instruction(&p_list, str)) {
			total--;
			continue;
		}

		printf("C%c starts at %lu\n", p_list->proc_type+'1', nanotime());
		
		if(sh == RR) {
			nanosleep(&ts, &ts);
			
			strcpy(str, "sleep");
			if(send_instruction(&p_list, str)) {
				total--;
				continue;
			}
		}
		else {
			while(!p_list->shseg->finished);
			
			strcpy(str, "finish");
			send_instruction(&p_list, str);
			
			total--;
			continue;
		}
		
		p_list = p_list->next;
	}
	
	int procs = 3;
	while(procs--) printf("Child destroyed: %d\n", wait(NULL));
	printf("All done\n");
	
	return 0;
}