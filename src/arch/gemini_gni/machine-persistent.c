/** @file
 * Elan persistent communication
 * @ingroup Machine
*/

/*
  included in machine.c
  Gengbin Zheng, 9/6/2011
*/

/*
  machine specific persistent comm functions:
  * LrtsSendPersistentMsg
  * CmiSyncSendPersistent
  * PumpPersistent
  * PerAlloc PerFree      // persistent message memory allocation/free functions
  * persist_machine_init  // machine specific initialization call
*/

void LrtsSendPersistentMsg(PersistentHandle h, int destNode, int size, void *m)
{
    gni_post_descriptor_t *pd;
    gni_return_t status;
    RDMA_REQUEST        *rdma_request_msg;
    
    PersistentSendsTable *slot = (PersistentSendsTable *)h;
    if (h==NULL) CmiAbort("LrtsSendPersistentMsg: not a valid PersistentHandle");
    CmiAssert(CmiNodeOf(slot->destPE) == destNode);
    if (size > slot->sizeMax) {
        CmiPrintf("size: %d sizeMax: %d mype=%d destPe=%d\n", size, slot->sizeMax, CmiMyPe(), destNode);
        CmiAbort("Abort: Invalid size\n");
    }

    if (slot->destBuf[0].destAddress) {
        // CmiPrintf("[%d] LrtsSendPersistentMsg h=%p hdl=%d destNode=%d destAddress=%p size=%d\n", CmiMyPe(), h, CmiGetHandler(m), destNode, slot->destBuf[0].destAddress, size);

        // uGNI part
        START_EVENT();
        MallocPostDesc(pd);
        if(size <= LRTS_GNI_RDMA_THRESHOLD) {
            pd->type            = GNI_POST_FMA_PUT;
        }
        else
        {
            pd->type            = GNI_POST_RDMA_PUT;
        }
        pd->cq_mode         = GNI_CQMODE_GLOBAL_EVENT;
        pd->dlvr_mode       = GNI_DLVMODE_PERFORMANCE;
        pd->length          = ALIGN64(size);
        pd->local_addr      = (uint64_t) m;
       
        pd->remote_addr     = (uint64_t)slot->destBuf[0].destAddress;
        pd->remote_mem_hndl = slot->destBuf[0].mem_hndl;
        pd->src_cq_hndl     = 0;//post_tx_cqh;     /* smsg_tx_cqh;  */
        pd->rdma_mode       = 0;
        pd->cqwrite_value   = PERSIST_SEQ;
        pd->amo_cmd         = 0;

#if CMK_WITH_STATS 
        pd->sync_flag_addr = 1000000 * CmiWallTimer(); //microsecond
#endif
        SetMemHndlZero(pd->local_mem_hndl);

        TRACE_COMM_CREATION(CpvAccess(projTraceStart), (void*)pd->local_addr);
         /* always buffer */
#if CMK_SMP || 1
#if REMOTE_EVENT
        bufferRdmaMsg(destNode, pd, (int)(size_t)(slot->destHandle));
#else
        bufferRdmaMsg(destNode, pd, -1);
#endif

#else                      /* non smp */

#if REMOTE_EVENT
        pd->cq_mode |= GNI_CQMODE_REMOTE_EVENT;
        int sts = GNI_EpSetEventData(ep_hndl_array[destNode], destNode, PERSIST_EVENT((int)(size_t)(slot->destHandle)));
        GNI_RC_CHECK("GNI_EpSetEventData", sts);
#endif
        status = registerMessage((void*)(pd->local_addr), pd->length, pd->cqwrite_value, &pd->local_mem_hndl);
        if (status == GNI_RC_SUCCESS) 
        {
#if CMK_WITH_STATS
            RDMA_TRY_SEND(pd->type)
#endif
            if(pd->type == GNI_POST_RDMA_PUT) 
                status = GNI_PostRdma(ep_hndl_array[destNode], pd);
            else
                status = GNI_PostFma(ep_hndl_array[destNode],  pd);
        }
        else
            status = GNI_RC_ERROR_RESOURCE;
        if(status == GNI_RC_ERROR_RESOURCE|| status == GNI_RC_ERROR_NOMEM )
        {
#if REMOTE_EVENT
            bufferRdmaMsg(destNode, pd, (int)(size_t)(slot->destHandle));
#else
            bufferRdmaMsg(destNode, pd, -1);
#endif
        }
        else {
            GNI_RC_CHECK("AFter posting", status);
#if  CMK_WITH_STATS
            pd->sync_flag_value = 1000000 * CmiWallTimer(); //microsecond
            RDMA_TRANS_INIT(pd->type, pd->sync_flag_addr/1000000.0)
#endif
        }
#endif
  }
  else {
#if 1
    if (slot->messageBuf != NULL) {
      CmiPrintf("Unexpected message in buffer on %d\n", CmiMyPe());
      CmiAbort("");
    }
    slot->messageBuf = m;
    slot->messageSize = size;
#else
    /* normal send */
    PersistentHandle  *phs_tmp = phs;
    int phsSize_tmp = phsSize;
    phs = NULL; phsSize = 0;
    CmiPrintf("[%d]Slot sending message directly\n", CmiMyPe());
    CmiSyncSendAndFree(slot->destPE, size, m);
    phs = phs_tmp; phsSize = phsSize_tmp;
#endif
  }
}

#if 0
void CmiSyncSendPersistent(int destPE, int size, char *msg, PersistentHandle h)
{
  CmiState cs = CmiGetState();
  char *dupmsg = (char *) CmiAlloc(size);
  memcpy(dupmsg, msg, size);

  /*  CmiPrintf("Setting root to %d\n", 0); */
  CMI_SET_BROADCAST_ROOT(dupmsg, 0);

  if (cs->pe==destPE) {
    CQdCreate(CpvAccess(cQdState), 1);
    CdsFifo_Enqueue(CpvAccess(CmiLocalQueue),dupmsg);
  }
  else
    LrtsSendPersistentMsg(h, destPE, size, dupmsg);
}
#endif

extern void CmiReference(void *blk);

#if 0

/* called in PumpMsgs */
int PumpPersistent()
{
  int status = 0;
  PersistentReceivesTable *slot = persistentReceivesTableHead;
  while (slot) {
    char *msg = slot->messagePtr[0];
    int size = *(slot->recvSizePtr[0]);
    if (size)
    {
      int *footer = (int*)(msg + size);
      if (footer[0] == size && footer[1] == 1) {
/*CmiPrintf("[%d] PumpPersistent messagePtr=%p size:%d\n", CmiMyPe(), slot->messagePtr, size);*/

#if 0
      void *dupmsg;
      dupmsg = CmiAlloc(size);
                                                                                
      _MEMCHECK(dupmsg);
      memcpy(dupmsg, msg, size);
      memset(msg, 0, size+2*sizeof(int));
      msg = dupmsg;
#else
      /* return messagePtr directly and user MUST make sure not to delete it. */
      /*CmiPrintf("[%d] %p size:%d rank:%d root:%d\n", CmiMyPe(), msg, size, CMI_DEST_RANK(msg), CMI_BROADCAST_ROOT(msg));*/

      CmiReference(msg);
      swapRecvSlotBuffers(slot);
#endif

      CmiPushPE(CMI_DEST_RANK(msg), msg);
#if CMK_BROADCAST_SPANNING_TREE
      if (CMI_BROADCAST_ROOT(msg))
          SendSpanningChildren(size, msg);
#endif
      /* clear footer after message used */
      *(slot->recvSizePtr[0]) = 0;
      footer[0] = footer[1] = 0;

#if 0
      /* not safe at all! */
      /* instead of clear before use, do it earlier */
      msg=slot->messagePtr[0];
      size = *(slot->recvSizePtr[0]);
      footer = (int*)(msg + size);
      *(slot->recvSizePtr[0]) = 0;
      footer[0] = footer[1] = 0;
#endif
      status = 1;
      }
    }
    slot = slot->next;
  }
  return status;
}

#endif

void *PerAlloc(int size)
{
//  return CmiAlloc(size);
  gni_return_t status;
  void *res = NULL;
  char *ptr;
  size = ALIGN64(size + sizeof(CmiChunkHeader));
  //printf("[%d] PerAlloc %p %p %d. \n", myrank, res, ptr, size);
  res = mempool_malloc(CpvAccess(mempool), ALIGNBUF+size-sizeof(mempool_header), 1);
  if (res) ptr = (char*)res - sizeof(mempool_header) + ALIGNBUF;
  SIZEFIELD(ptr)=size;
  REFFIELD(ptr)=1;
  MEMHFIELD(ptr) = GetMemHndl(ptr);
  return ptr;
}
                                                                                
void PerFree(char *msg)
{
#if CMK_SMP
  mempool_free_thread((char*)msg - ALIGNBUF + sizeof(mempool_header));
#else
  mempool_free(CpvAccess(mempool), (char*)msg - ALIGNBUF + sizeof(mempool_header));
#endif
}

/* machine dependent init call */
void persist_machine_init(void)
{
}

void initSendSlot(PersistentSendsTable *slot)
{
  int i;
  slot->destPE = -1;
  slot->sizeMax = 0;
  slot->destHandle = 0; 
#if 0
  for (i=0; i<PERSIST_BUFFERS_NUM; i++) {
    slot->destAddress[i] = NULL;
    slot->destSizeAddress[i] = NULL;
  }
#endif
  memset(&slot->destBuf, 0, sizeof(PersistentBuf)*PERSIST_BUFFERS_NUM);
  slot->messageBuf = 0;
  slot->messageSize = 0;
  slot->prev = slot->next = NULL;
}

void initRecvSlot(PersistentReceivesTable *slot)
{
  int i;
#if 0
  for (i=0; i<PERSIST_BUFFERS_NUM; i++) {
    slot->messagePtr[i] = NULL;
    slot->recvSizePtr[i] = NULL;
  }
#endif
  memset(&slot->destBuf, 0, sizeof(PersistentBuf)*PERSIST_BUFFERS_NUM);
  slot->sizeMax = 0;
  slot->index = -1;
  slot->prev = slot->next = NULL;
}

void setupRecvSlot(PersistentReceivesTable *slot, int maxBytes)
{
  int i;
  for (i=0; i<PERSIST_BUFFERS_NUM; i++) {
    char *buf = PerAlloc(maxBytes+sizeof(int)*2);
    _MEMCHECK(buf);
    memset(buf, 0, maxBytes+sizeof(int)*2);
    slot->destBuf[i].mem_hndl = MEMHFIELD(buf);
    slot->destBuf[i].destAddress = buf;
    /* note: assume first integer in elan converse header is the msg size */
    slot->destBuf[i].destSizeAddress = (unsigned int*)buf;
#if USE_LRTS_MEMPOOL
    // assume already registered from mempool
    // slot->destBuf[i].mem_hndl = GetMemHndl(buf);
#endif
    // FIXME:  assume always succeed
  }
  slot->sizeMax = maxBytes;
#if REMOTE_EVENT
  CmiLock(persistPool.lock);
  slot->index = IndexPool_getslot(&persistPool, slot, 2);
  CmiUnlock(persistPool.lock);
#endif
}

void clearRecvSlot(PersistentReceivesTable *slot)
{
#if REMOTE_EVENT
  CmiLock(persistPool.lock);
  IndexPool_freeslot(&persistPool, slot->index);
  CmiUnlock(persistPool.lock);
#endif
}

PersistentHandle getPersistentHandle(PersistentHandle h, int toindex)
{
#if REMOTE_EVENT
  if (toindex)
    return (PersistentHandle)(((PersistentReceivesTable*)h)->index);
  else {
    return (PersistentHandle)GetIndexAddress(persistPool, (int)(size_t)h);
  }
#else
  return h;
#endif
}
