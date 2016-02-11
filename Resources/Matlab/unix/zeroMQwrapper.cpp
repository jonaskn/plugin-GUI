/*
% Copyright (c) 2008 Shay Ohayon, California Institute of Technology.
% This file is a part of a free software. you can redistribute it and/or modify
% it under the terms of the GNU General Public License as published by
% the Free Software Foundation (see GPL.txt)
*/
#include <stdio.h>
#include "mex.h"
#include "zmq.h"
#include "zmq_utils.h"
#include <string>
#include <cstring>
#include <queue>
#include <list>
#include <map>
#include <pthread.h>
#include <unistd.h>
#include <mutex> 
#include <chrono>


typedef struct {
    bool thread_running;
    int threadIdleTime;
} ThreadData;

typedef struct {
    const char *url;
    void * zmqSocket;
    double timeOut;
} socketData;

typedef struct{
    std::string url;
    std::string order;
    std::string response;
    double timeOrderAdded; //e.g. from PsychGetAdjustedPrecisionTimerSeconds(returnValue);
    double timeOrderSent;
    double timeResponseReceived;
} dialogue;
const char* dialogueFieldnames[5] = { "order", "response","timeOrderAdded", "timeOrderSent", "timeResponseReceived"};

dialogue SendAndReceive( std::string url, std::string order );

#define MAX(x,y)(x>y)?(x):(y)
#define MIN(x,y)(x<y)?(x):(y)
#define ms1 1000
#define ms10 10000
#define ms100 100000
#define ms1000 1000000


typedef std::map<std::string,socketData> mp;
mp socketMap;
std::mutex socketMap_mutex;

std::queue<dialogue> InDialogues;
std::mutex InDialogues_mutex;

std::queue<dialogue> OutDialogues;
std::mutex OutDialogues_mutex;

typedef std::list<ThreadData*> lst;
lst threadList;
std::mutex threadList_mutex;

double lastDialogueAdded;
double lastDialogueFetched;

std::mutex sendReceiveBusy;

bool initialized = false;
void* context;

std::chrono::high_resolution_clock timer;

void CloseContext(void)
{
	if (initialized) {
        threadList_mutex.lock();
        for (lst::iterator i= threadList.begin(); i != threadList.end(); i++) {
            (*i)->thread_running=false;
        }
        threadList_mutex.unlock();
        
        bool threadListEmpty=false;
        while(!threadListEmpty){
            threadList_mutex.lock();
            if(threadList.empty()){
                threadListEmpty=true;
            }
            threadList_mutex.unlock();
            
            if(!threadListEmpty){
                usleep(ms10);
            }
        }
            
		for (mp::iterator i= socketMap.begin(); i != socketMap.end(); i++) {
            int toms=0;
            zmq_setsockopt(i->second.zmqSocket,ZMQ_LINGER,&toms, sizeof(toms));
			zmq_close (i->second.zmqSocket);
		}
		zmq_ctx_destroy (context);
		initialized = false;
        socketMap.clear();
	}
}

void initialize_zmq()
{
	mexAtExit(CloseContext);
	context = zmq_ctx_new();
    lastDialogueAdded=0;
    lastDialogueFetched=0;
	initialized = true;
}

void SendAndReceive( dialogue* d )
{
    //check if we have an open socket for that url:
    std::string url = d->url;
    std::string order = d->order;
    
    socketMap_mutex.lock();
    mp::iterator it=socketMap.find(url);
    socketData sock;
    if (it == socketMap.end()){
        sock.url = url.c_str();
        sock.zmqSocket = zmq_socket (context, ZMQ_REQ);
        sock.timeOut=0.5;
        int toms=sock.timeOut*1000;
        zmq_setsockopt(sock.zmqSocket,ZMQ_LINGER,&toms, sizeof(toms));
        zmq_connect (sock.zmqSocket, sock.url); 
        socketMap.insert(std::pair<std::string,socketData>(url, sock));
    }else{
        sock = it->second;
    }
    socketMap_mutex.unlock();
			
    sendReceiveBusy.lock();
    
	zmq_msg_t request;
	zmq_msg_init_size (&request, d->order.length());
	memcpy (zmq_msg_data (&request), d->order.c_str() , d->order.length());
	  
    zmq_msg_send (&request, sock.zmqSocket, 0);
    
    auto time = std::chrono::high_resolution_clock::now().time_since_epoch();
    d->timeOrderSent = (std::chrono::duration_cast<std::chrono::microseconds>(time)).count();
	zmq_msg_close (&request);

    zmq_msg_t reply;
	zmq_msg_init (&reply);
    bool failed=false;
    auto timeR=std::chrono::high_resolution_clock::now().time_since_epoch();
    while (zmq_msg_recv(&reply, sock.zmqSocket, ZMQ_DONTWAIT)==-1)
    {
        timeR = std::chrono::high_resolution_clock::now().time_since_epoch();
        bool isTimeout=(std::chrono::duration_cast<std::chrono::microseconds>(timeR-time)).count() > sock.timeOut*1000000;

        if(isTimeout || errno!=EAGAIN){

            failed=true;
            break;
        }
    }
    
    sendReceiveBusy.unlock();
    
    if(failed){
        d->timeResponseReceived=-1;
        d->response="failed waiting for a reply";
    }else{
        timeR = std::chrono::high_resolution_clock::now().time_since_epoch();
        d->timeResponseReceived = (std::chrono::duration_cast<std::chrono::microseconds>(timeR)).count();

        size_t size = zmq_msg_size (&reply);
        char *s = (char *) malloc (size + 1);
        memcpy (s, zmq_msg_data (&reply), size);

        d->response=std::string(s,size);
    }

    zmq_msg_close (&reply);
	return;
}

void* MyThreadFunction( void* lpParam ){
    //work on all queued dialogues
    ThreadData *pData = (ThreadData*) lpParam ;
    pData->thread_running = true;
    
    while(pData->thread_running){
        InDialogues_mutex.lock();
        if(!InDialogues.empty()){
            dialogue d = InDialogues.front();
            InDialogues.pop();
            InDialogues_mutex.unlock(); 

            SendAndReceive( &d );

            OutDialogues_mutex.lock(); 
            OutDialogues.push(d);
            OutDialogues_mutex.unlock(); 
        }else{
            InDialogues_mutex.unlock(); 
            usleep(pData->threadIdleTime);
        } 
    }
    
    pData->thread_running = false;
    threadList_mutex.lock();
    threadList.remove(pData);
    threadList_mutex.unlock();
    
    return 0;
}

ThreadData* create_client_thread()
{
	ThreadData* pData = new ThreadData;
	pData->threadIdleTime=ms10;

	pthread_t thread;
	
	int retValue = pthread_create(&thread,
							      NULL,
							      MyThreadFunction,
							      (void*) pData);

	return pData;
 
}

void mexFunction( int nlhs, mxArray* plhs[], 
				 int nrhs, const mxArray* prhs[] ) 
{

    if (!initialized) initialize_zmq();
    if (nrhs < 1) {
        return;
    }
    
	char* Command = mxArrayToString(prhs[0]);

	if   (strcmp(Command, "StartConnectThread") == 0)  {
        //return the url for backward compatability
        if(nlhs>0 && nrhs>1){
            int url_length = int(mxGetNumberOfElements(prhs[1])) + 1;
			char* url = mxArrayToString(prhs[1]);
            plhs[0] = mxCreateString(url);
        }
    }

    if (strcmp(Command, "Send") == 0) {
        if (nrhs < 3) {
            return;
        }
        bool blocking=false;
        if (nrhs >3) {
            double Tmp = mxGetScalar(prhs[3]);
            blocking = Tmp>0;
        }
        
        dialogue d;
        d.url = std::string(mxArrayToString(prhs[1]));
        d.order = std::string(mxArrayToString(prhs[2]));
        auto time = std::chrono::high_resolution_clock::now().time_since_epoch();
        d.timeOrderAdded = (std::chrono::duration_cast<std::chrono::microseconds>(time)).count();

        if(blocking){
            SendAndReceive(&d);
            
            if (d.timeResponseReceived==-1) {
                // Thread was killed and now we try to send things again?
                mexPrintf("ZMQ: Failed to get a response.\n");
            }

            // in blocking mode first argument is the response, second is the full dialogue
            if (nlhs>0){//just return the response
                mxArray * mxOutResponse;
                mxOutResponse = mxCreateString( d.response.c_str() );
                plhs[0]=mxOutResponse;
            }
            if (nlhs>1){//also return the dialogue struct
                mxArray * mxOutStruct = mxCreateStructMatrix(1,1,5,dialogueFieldnames);
                mxSetField(mxOutStruct, 0, "order",  mxCreateString( d.order.c_str() ) );
                mxSetField(mxOutStruct, 0, "response",  mxCreateString( d.response.c_str() ) );

                mxSetField(mxOutStruct,0, "timeOrderAdded", mxCreateDoubleMatrix(1,1, mxREAL));
                memcpy(mxGetData(mxGetField(mxOutStruct,0, "timeOrderAdded")) ,&(d.timeOrderSent),sizeof(double));

                mxSetField(mxOutStruct,0, "timeOrderSent", mxCreateDoubleMatrix(1,1, mxREAL));
                memcpy(mxGetData(mxGetField(mxOutStruct,0, "timeOrderSent")) ,&(d.timeOrderSent),sizeof(double));

                mxSetField(mxOutStruct,0, "timeResponseReceived", mxCreateDoubleMatrix(1,1, mxREAL));
                memcpy(mxGetData(mxGetField(mxOutStruct,0, "timeResponseReceived")) ,&(d.timeResponseReceived),sizeof(double));
                plhs[1]=mxOutStruct;
            }
        }else{ //not blocking
            lastDialogueAdded = d.timeOrderAdded;
            InDialogues_mutex.lock(); 
            InDialogues.push(d);
            InDialogues_mutex.unlock(); 

            //start a thread if non is open
            threadList_mutex.lock();
            if(threadList.empty()){
                ThreadData* sd = create_client_thread();
                threadList.push_back(sd);
            }
            threadList_mutex.unlock();
            
            //if a n output is requested return the time the order was added to the queue, for later matching in the 'getResponses' output
            if (nlhs>0)
            {
                mxArray * mxOutStruct;
                mxOutStruct = mxCreateDoubleMatrix(1,1, mxREAL);
                memcpy(mxOutStruct ,&(d.timeOrderAdded),sizeof(double));
                plhs[0]=mxOutStruct;
            }
        
        }//if blocking
    }
			
    if (strcmp(Command, "CloseAll") == 0)
    {
        CloseContext();
    }
    
            
	if (strcmp(Command, "CloseThread") == 0){
        threadList_mutex.lock();
        for (lst::iterator i= threadList.begin(); i != threadList.end(); i++) {
            (*i)->thread_running=false;
        }
        threadList_mutex.unlock();
	}
    
   if (strcmp(Command, "GetResponses") == 0)
   {
       //do we want to waint untill the current Sending Queue is emptied and all replies are in?
       bool wairForEmptyQueue;
       if(nrhs<3)
       {
           wairForEmptyQueue = false;
       }else
       {
           double Tmp = mxGetScalar(prhs[2]);
           wairForEmptyQueue = Tmp>0;
       }
         
       double lastAddedTime = lastDialogueAdded;
       double lastFetchedTime = lastDialogueFetched;
       if(lastFetchedTime ==lastAddedTime){ //nothing to fetch
          wairForEmptyQueue = false;
       }
       //if so, do it now
       while(wairForEmptyQueue)
       {
          OutDialogues_mutex.lock(); //we might still be one respone behind...         
          if(!OutDialogues.empty() && OutDialogues.back().timeOrderAdded>=lastAddedTime)
          {
               wairForEmptyQueue=false;
          }
          OutDialogues_mutex.unlock();     
          usleep(ms1);
       }
       
       //now we build a struct with all replies
       OutDialogues_mutex.lock();
       lastDialogueFetched=OutDialogues.back().timeOrderAdded;
       
       mxArray * mxOutStruct = mxCreateStructMatrix(1,OutDialogues.size(),5,dialogueFieldnames);		
       
       int j=0;
       while(!OutDialogues.empty())
       {
           dialogue d = OutDialogues.front();
           OutDialogues.pop();
           
           mxSetField(mxOutStruct, j, "order",  mxCreateString( d.order.c_str() ) );
           mxSetField(mxOutStruct, j, "response",  mxCreateString( d.response.c_str() ) );
           
           mxSetField(mxOutStruct,j, "timeOrderAdded", mxCreateDoubleMatrix(1,1, mxREAL));
       	memcpy(mxGetData(mxGetField(mxOutStruct,j, "timeOrderAdded")) ,&(d.timeOrderAdded),sizeof(double));
           
           mxSetField(mxOutStruct,j, "timeOrderSent", mxCreateDoubleMatrix(1,1, mxREAL));
       	memcpy(mxGetData(mxGetField(mxOutStruct,j, "timeOrderSent")) ,&(d.timeOrderSent),sizeof(double));
           
           mxSetField(mxOutStruct,j, "timeResponseReceived", mxCreateDoubleMatrix(1,1, mxREAL));
       	memcpy(mxGetData(mxGetField(mxOutStruct,j, "timeResponseReceived")) ,&(d.timeResponseReceived),sizeof(double));
           j++;
       }
       OutDialogues_mutex.unlock();
       
       plhs[0] = mxOutStruct;
	}


}