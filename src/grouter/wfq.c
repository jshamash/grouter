#include <slack/std.h>
#include <slack/map.h>
#include <slack/list.h>
#include <pthread.h>
#include "protocols.h"
#include "packetcore.h"
#include "message.h"
#include "grouter.h"

// WCWeightedFairScheduler: is one part of the W+FQ scheduler.
// It picks the appropriate job from the system of queues.
// If no job in packet core.. the thread waits.
// If there is job, select a Queue and then the job at the head of the Queue
// for running. Update the algorithm parameters and store them back on the queue
// datastructure: start, finish times for the queue and virtual time for the system.

// TODO: Debug this function..

extern router_config rconfig;

void *weightedFairScheduler(void *pc)
{
	pktcore_t *pcore = (pktcore_t *)pc;
	List *keylst;
	simplequeue_t *nxtq, *thisq;
	char *nxtkey, *savekey;
	double minftime, minstime, tweight;
	int pktsize, npktsize;
	gpacket_t *in_pkt, *nxt_pkt;

	pthread_setcanceltype(PTHREAD_CANCEL_ASYNCHRONOUS, NULL);       // die as soon as cancelled
	while (1)
	{
		verbose(2, "[weightedFairScheduler]:: Worst-case weighted fair queuing scheduler processing..");

		savekey = NULL;
		minftime = INFINITY;

		// Pause until there are packets to be processed
		pthread_mutex_lock(&(pcore->qlock));
		if (pcore->packetcnt == 0)
			pthread_cond_wait(&(pcore->schwaiting), &(pcore->qlock));
		pthread_mutex_unlock(&(pcore->qlock));

		pthread_testcancel();

		keylst = map_keys(pcore->queues);
		while (list_has_next(keylst) == 1)
		{
			nxtkey = list_next(keylst);
			nxtq = map_get(pcore->queues, nxtkey);
			if (nxtq->cursize == 0) {
				//printf("The queue %s is empty\n", nxtkey);
				continue;
			}
			//if ((nxtq->stime <= pcore->vclock) && (nxtq->ftime < minftime))
			if (nxtq->ftime < minftime)
			{
				savekey = nxtkey;
				minftime = nxtq->ftime;
			}
		}
		list_release(keylst);

		// if savekey is NULL then release the lock..
		if (savekey == NULL)
			continue;
		else
		{
			printf("Reading from %s, ftime is %f\n", savekey, minftime);
			thisq = map_get(pcore->queues, savekey);
			readQueue(thisq, (void **)&in_pkt, &pktsize);
			writeQueue(pcore->workQ, in_pkt, pktsize);
			pthread_mutex_lock(&(pcore->qlock));
			pcore->packetcnt--;
			pthread_mutex_unlock(&(pcore->qlock));

			peekQueue(thisq, (void **)&nxt_pkt, &npktsize);
			if (npktsize)
			{
				thisq->stime = minftime;
				thisq->ftime = thisq->stime + npktsize/thisq->weight;
			}

//			minstime = thisq->stime;
//			tweight = 0.0;
//
//			keylst = map_keys(pcore->queues);
//			while (list_has_next(keylst) == 1)
//			{
//				nxtkey = list_next(keylst);
//				nxtq = map_get(pcore->queues, nxtkey);
//				tweight += nxtq->weight;
//				if ((nxtq->cursize > 0) && (nxtq->stime < minstime))
//					minstime = nxtq->stime;
//			}
//			list_release(keylst);
//			pcore->vclock = max(minstime, (pcore->vclock + ((double)pktsize)/tweight));
			// or should we just update the vclock by adding the length of the packet we just sent?
			pcore->vclock = minftime;
			printf("vclock is now %f\n", pcore->vclock);
		}
	}
}




// WCWeightFairQueuer: function called by the classifier to enqueue
// the packets.. 
// TODO: Debug this function...
int weightedFairQueuer(pktcore_t *pcore, gpacket_t *in_pkt, int pktsize, char *qkey)
{
	simplequeue_t *thisq, *nxtq;
	double minftime, minstime, tweight;
	List *keylst;
	char *nxtkey, *savekey;

	verbose(2, "[weightedFairQueuer]:: Worst-case weighted fair queuing scheduler processing..");

	pthread_mutex_lock(&(pcore->qlock));

	thisq = map_get(pcore->queues, qkey);
	if (thisq == NULL)
	{
		fatal("[weightedFairQueuer]:: Invalid %s key presented for queue addition", qkey);
		pthread_mutex_unlock(&(pcore->qlock));
		return EXIT_FAILURE;             // packet dropped..
	}

	verbose(2, "[weightedFairQueuer]:: Checking the queue size \n");
	if (thisq->cursize == 0)
	{
		// If it's the first element we need to set the stime and ftime of this queue.
		verbose(2, "[weightedFairQueuer]:: inserting the first element.. ");
		thisq->stime = pcore->vclock;
		//thisq->ftime = thisq->stime + pktsize/thisq->weight;
		thisq->ftime = thisq->stime + pktsize/thisq->weight;

//		minstime = thisq->stime;
//
//		keylst = map_keys(pcore->queues);
//
//		while (list_has_next(keylst) == 1)
//		{
//			nxtkey = list_next(keylst);
//
//			nxtq = map_get(pcore->queues, nxtkey);
//
//			if ((nxtq->cursize > 0) && (nxtq->stime < minstime))
//				minstime = nxtq->stime;
//		}
//		list_release(keylst);
//
//		pcore->vclock = max(minstime, pcore->vclock);
		
		// insert the packet... and increment variables..
		printf("Writing to queue\n");
		writeQueue(thisq, in_pkt, pktsize);
		pcore->packetcnt++;

		// wake up scheduler if it was waiting..
		if (pcore->packetcnt == 1)
			pthread_cond_signal(&(pcore->schwaiting));
		pthread_mutex_unlock(&(pcore->qlock));
		return EXIT_SUCCESS;
	} else if (thisq->cursize < thisq->maxsize)
	{
		printf("Writing to queue\n");
		writeQueue(thisq, in_pkt, pktsize);
		pcore->packetcnt++;
		pthread_mutex_unlock(&(pcore->qlock));
		return EXIT_SUCCESS;
	} else {
		// Should already be handled by packetcore
		verbose(2, "[weightedFairQueuer]:: Packet dropped.. Queue for %s is full ", qkey);
		pthread_mutex_unlock(&(pcore->qlock));
		return EXIT_SUCCESS;
	}
}
