#include <stdio.h>
#include <stdlib.h>
#include "so_scheduler.h"

#define CMP_ST(a, b) ((a) < (b) ? (-1) : (1))
#define CMP_EQ(a, b) ((a) <= (b) ? (-1) : (1))

typedef struct {
    unsigned int time_quantum;
    unsigned int io;
} scheduler;

typedef struct thread {
	unsigned int priority;
	pthread_t tid;
	pthread_mutex_t mutex_pl;
	unsigned int time_quantum;
	struct thread *urm;
} ThreadInfo;

typedef struct {
	so_handler *func;
	unsigned int priority;
	ThreadInfo *t;
} argum;

ThreadInfo **readyQueue, **blockingQueue;
ThreadInfo *running, *terminated, *last_thread;
scheduler *sch;
pthread_key_t *myThread;

void insertQueue(ThreadInfo *ti, int where)
{
	ThreadInfo **tl = &readyQueue[ti->priority];

	if (where == 1) {
	//pun ti la finalul listei
		while (*tl != NULL)
			tl = &((*tl)->urm);
	}
	ti->urm = *tl;
	*tl = ti;
}

ThreadInfo *extractQueue(void)
{
	ThreadInfo *p;
	int i = SO_MAX_PRIO, check = 0;

    //extrag primul element din lista cu prio cea mai mare
	for (; i >= 0; i--) {
		if (readyQueue[i] == NULL)
			continue;
		else {
			check = 1;
			break;
		}
	}

	if (check == 1) {
		p = readyQueue[i];
		readyQueue[i] = readyQueue[i]->urm;
		return p;
	}
	//daca nu gasesc nimic (READY e gol) intorc NULL
	return NULL;
}

void setMutex(ThreadInfo *t1, ThreadInfo *t2)
{
	pthread_mutex_unlock(&t1->mutex_pl);
	pthread_mutex_lock(&t2->mutex_pl);
}

void insertByPriority(ThreadInfo *thread, ThreadInfo *extr, int eq)
{
	if (extr != NULL && eq == 0) {
		if (CMP_ST(running->priority, extr->priority) == -1) {
			ThreadInfo *t = running;

			running = extr;
			insertQueue(t, 1);
		} else if (CMP_ST(running->priority, extr->priority) == 1)
			insertQueue(extr, 0);
	} else if (extr != NULL && eq == 1) {
		if (CMP_EQ(running->priority, extr->priority) == -1) {
			ThreadInfo *t = running;

			running = extr;
			insertQueue(t, 1);
		} else if (CMP_EQ(running->priority, extr->priority) == 1)
			insertQueue(extr, 0);
	}
	setMutex(running, thread);
}

void reschedule(void)
{
	ThreadInfo *thread = pthread_getspecific(*myThread), *extr;

	if ((--thread->time_quantum) > 0) {
	//daca a venit un thread cu prioritate mai mare
		ThreadInfo *extr = extractQueue();

		insertByPriority(thread, extr, 0);
		return;
	}

    //daca thread-ului curent i-a expirat cuanta
	thread->time_quantum = sch->time_quantum;

    //apare in coada READY o prioritate mai mare
	extr = extractQueue();
	insertByPriority(thread, extr, 1);
}


int so_init(unsigned int time_quantum, unsigned int io)
{
	if (sch != NULL)
		return -1;

	if (time_quantum == 0 || io > SO_MAX_NUM_EVENTS)
		return -1;

	sch = malloc(sizeof(scheduler));
	if (sch == NULL)
		return -1;

	running = malloc(sizeof(ThreadInfo));
	if (running == NULL) {
		free(sch);
		sch = NULL;
	}

	sch->time_quantum = time_quantum;
	running->time_quantum = sch->time_quantum;
	sch->io = io;

	running->tid = pthread_self();
	running->priority = 0;
	running->urm = NULL;

	pthread_mutex_init(&running->mutex_pl, NULL);

	myThread = malloc(sizeof(pthread_key_t));
	pthread_key_create(myThread, NULL);
	pthread_setspecific(*myThread, running);

	readyQueue = malloc((1 + SO_MAX_PRIO) * sizeof(ThreadInfo *));
	blockingQueue = malloc(io * sizeof(ThreadInfo *));
	reschedule();
	return 0;
}

void so_end(void)
{
	if (sch == NULL)
		return;

    //last_thread asteapta ca READY sa se goleasca
	last_thread = pthread_getspecific(*myThread);

	running = extractQueue();
	if (running != NULL)
		setMutex(running, last_thread);

	while (terminated != NULL) {
		pthread_join(terminated->tid, NULL);
		pthread_mutex_destroy(&terminated->mutex_pl);
		terminated = terminated->urm;
	}

	pthread_mutex_destroy(&last_thread->mutex_pl);

	free(readyQueue);
	free(blockingQueue);
	free(sch);
	sch = NULL;
	pthread_key_delete(*myThread);
	free(myThread);
}

void so_exec(void)
{
	setMutex(pthread_getspecific(*myThread),
			pthread_getspecific(*myThread));
	reschedule();
}

int so_wait(unsigned int io)
{
	ThreadInfo *thread = pthread_getspecific(*myThread);

	setMutex(thread, thread);
	if (CMP_ST(io, 0) == -1 || CMP_EQ(sch->io, io) == -1)
		return -1;

	//inserare thread in blockingQueue[io]
	thread->urm = blockingQueue[io];
	blockingQueue[io] = thread;

	//se blocheaza thread-ul curent si se pune
	//altul in starea running
	running = extractQueue();
	setMutex(running, thread);

	return 0;
}

void *start_thread(void *arg)
{
    //asteapta planificare
	argum *a = (argum *)arg;
	int prior = a->priority;

	pthread_setspecific(*myThread, a->t);
	terminated = pthread_getspecific(*myThread);

	//asteapta sa fie planificat
	pthread_mutex_lock(&terminated->mutex_pl);
	a->func(prior);
	running = extractQueue();

	if (running != NULL)
		pthread_mutex_unlock(&running->mutex_pl);
	else
		pthread_mutex_unlock(&last_thread->mutex_pl);

	free(a);
	return NULL;
}

tid_t so_fork(so_handler *func, unsigned int priority)
{
	if (CMP_ST(SO_MAX_PRIO, priority) == -1  || func == NULL)
		return INVALID_TID;

	ThreadInfo *thread = pthread_getspecific(*myThread);
	argum *p = malloc(sizeof(argum));

	pthread_mutex_unlock(&thread->mutex_pl);

	p->func = func;
	p->priority = priority;
	p->t = malloc(sizeof(ThreadInfo));

	if (!p->t)
		return INVALID_TID;
	p->t->priority = priority;
	p->t->time_quantum = sch->time_quantum;
	pthread_mutex_init(&p->t->mutex_pl, NULL);
	pthread_mutex_lock(&p->t->mutex_pl);

	pthread_create(&p->t->tid, NULL, start_thread, p);

	//pun thread-ul in READY
	insertQueue(p->t, 1);

	pthread_mutex_lock(&thread->mutex_pl);
	reschedule();
	return p->t->tid;
}

int so_signal(unsigned int io)
{
	int nr;

	if (CMP_ST(io, 0) == -1 || CMP_EQ(sch->io, io) == -1)
		return -1;
	ThreadInfo *thread = pthread_getspecific(*myThread);

	pthread_mutex_unlock(&thread->mutex_pl);
	ThreadInfo *p = blockingQueue[io];

	for (nr = 0; p != NULL; nr++) {
		ThreadInfo *aux = p;

		p = p->urm;
		insertQueue(aux, 1);
	}
	blockingQueue[io] = NULL;

	pthread_mutex_lock(&thread->mutex_pl);
	reschedule();

	return nr;
}

