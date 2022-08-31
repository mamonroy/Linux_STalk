#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h> 
#include <arpa/inet.h>
#include <pthread.h>
#include <string.h>
#include "list.h"

#define MSG_MAX_LEN 1024
#define h_addr h_addr_list[0]

pthread_t send_id;
pthread_t inputKeyboard_id;
pthread_t outputKeyboard_id;
pthread_t recv_id;


typedef struct arg_struct {
    struct sockaddr_in myself;
    struct sockaddr_in remote;
    int socketDescriptor;
    unsigned int myPortNumber;
    unsigned int remoteIPAddress;
    unsigned int remotePortNumber;
} Network;

List* pList_tobeSent;
List* pList_tobeRecv;

static pthread_cond_t s_syncOkToSendCondVar = PTHREAD_COND_INITIALIZER;
static pthread_mutex_t s_syncOkToSendMutex = PTHREAD_MUTEX_INITIALIZER;

static pthread_cond_t s_syncOkToRecvCondVar = PTHREAD_COND_INITIALIZER;
static pthread_mutex_t s_syncOkToRecvMutex = PTHREAD_MUTEX_INITIALIZER;

static pthread_mutex_t guard_mutex_sendLst = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t guard_mutex_rcvLst = PTHREAD_MUTEX_INITIALIZER;

bool shouldQuit = false;

char *messageSend;
char *messageReceived;

/*The keyboard input thread, on receipt of input, adds the input to the list of
  messages that need to be sent to the remote s-talk client.  */
void* keyboardInput(void* args)
{
    while(1) {

        messageSend = (char*)malloc(MSG_MAX_LEN*sizeof(char));
        fgets(messageSend, MSG_MAX_LEN ,stdin);

        // Signal Send Msg that It's OK to trim the list or Send the Message)
        pthread_mutex_lock(&s_syncOkToSendMutex);
        {
            pthread_mutex_lock(&guard_mutex_sendLst);
            List_prepend(pList_tobeSent, messageSend);
            pthread_mutex_unlock(&guard_mutex_sendLst);
            pthread_cond_signal(&s_syncOkToSendCondVar);
        }
        pthread_mutex_unlock(&s_syncOkToSendMutex);

        if(strncmp(messageSend,"!",1) == 0) {
            shouldQuit = true;
            break;
        }
    }

    printf("\nConnection Terminated\n");
    pthread_exit(0);
}

/* The UDP output thread will take each message off this list and send it over the
   network to the remote client.  */
void* sendMsg(void* args)
{
    while(1) {

        // // If signalled accompanies with quit == true, get out of the loop
        if(shouldQuit && List_count(pList_tobeSent) == 0) {
            break;
        } 

        pthread_mutex_lock(&s_syncOkToSendMutex);
        while(List_count(pList_tobeSent) == 0 && !shouldQuit) { 
            pthread_cond_wait(&s_syncOkToSendCondVar, &s_syncOkToSendMutex);
        }

        struct arg_struct *arguments = (struct arg_struct *)args;
        int returnCode;

        pthread_mutex_lock(&guard_mutex_sendLst);
        char *messagelocalSend = List_trim(pList_tobeSent);
        // printf("%s\n",messagelocalSend);
        pthread_mutex_unlock(&guard_mutex_sendLst);

        // Send message to remote
        returnCode = sendto(arguments -> socketDescriptor, messagelocalSend , strlen(messagelocalSend), 0, (struct sockaddr *) &(arguments -> remote), sizeof(arguments -> remote));
            
        if(returnCode < 0) {
            printf("Error in Sending Message to the Remote!\n");
        }

        if(strncmp(messagelocalSend,"!",1) == 0) {
            shouldQuit = true;
        }

        free(messagelocalSend);
        pthread_mutex_unlock(&s_syncOkToSendMutex);
    }
    
    pthread_cancel(outputKeyboard_id);
    pthread_cancel(recv_id);

    pthread_exit(0);
}
/* The UDP input thread, on receipt of input from the remote s-talk client, will put
   the message onto the list of messages that need to be printed to the local screen.  */
void* receiveMsg(void* args) 
{

    while(1) {

        messageReceived = (char*)malloc(MSG_MAX_LEN*sizeof(char));

        struct arg_struct *arguments = (struct arg_struct *)args;
        unsigned int messageReceived_len;
        int returnCode;

        returnCode = recvfrom(arguments -> socketDescriptor, messageReceived, MSG_MAX_LEN, 0, (struct sockaddr *) &(arguments -> remote), &messageReceived_len);

        if(returnCode < 0) {
            printf("Error in Receiving Message to from the remote!\n");
        }

        // Signal Keyboard that It's OK to trim the list)
        pthread_mutex_lock(&s_syncOkToRecvMutex);
        {
            pthread_mutex_lock(&guard_mutex_rcvLst);
            List_prepend(pList_tobeRecv, messageReceived);
            pthread_mutex_unlock(&guard_mutex_rcvLst);
            pthread_cond_signal(&s_syncOkToRecvCondVar);
        }
        pthread_mutex_unlock(&s_syncOkToRecvMutex);

        if(strncmp(messageReceived,"!",1) == 0) {
            shouldQuit = true;
            pthread_mutex_lock(&s_syncOkToRecvMutex);
            {
                pthread_cond_signal(&s_syncOkToRecvCondVar);
            }
            pthread_mutex_unlock(&s_syncOkToRecvMutex);
            break;
        }
    }
    pthread_exit(0);
}

/* The screen output thread will take each message off this list and output it to the screen.  */
/* The screen output thread will take each message off this list and output it to the screen.  */
void* keyboardOutput(void* args)
{
    while(1) {

        // If signalled accompanies with quit == true, get out of the loop
        if(shouldQuit && List_count(pList_tobeRecv) == 0) {
            break;
        } 

        pthread_mutex_lock(&s_syncOkToRecvMutex);
        while(List_count(pList_tobeRecv) == 0 && !shouldQuit) { 
            pthread_cond_wait(&s_syncOkToRecvCondVar, &s_syncOkToRecvMutex);
        }

        pthread_mutex_lock(&guard_mutex_rcvLst);
        char *localmessageReceived = List_trim(pList_tobeRecv);
        if(strncmp(localmessageReceived,"!",1) == 0) {
            shouldQuit = true;
            break;
        }
        fputs(localmessageReceived,stdout);
        pthread_mutex_unlock(&guard_mutex_rcvLst);

        pthread_mutex_unlock(&s_syncOkToRecvMutex);
    }

    printf("\nConnection Terminated\n");

    pthread_cancel(inputKeyboard_id);
    pthread_cancel(send_id);
    pthread_exit(0);
}

int main(int argCount, char** argv) 
{

    // Exit if argument provided does not have the port number and ip address
    if(argCount < 4) {
        printf("Usage: %s [My Port Number][Server IP Address] [Server Port]\n", argv[0]);
        exit(1);
    }

    pList_tobeSent = List_create();
    pList_tobeRecv = List_create();

    unsigned int myPortNumber = atoi(argv[1]);
    unsigned int remoteIPAddress = inet_addr(argv[2]);
    unsigned int remotePortNumber = atoi(argv[3]);

    // Print details first
    printf("#########################################################\n");
    printf("Running on Port: %d and Connected to endpoint: %s:%d\n", myPortNumber, argv[2],remotePortNumber);
    printf("#########################################################\n");
    // Create the socket for UDP
    int socketDescriptor = socket(AF_INET, SOCK_DGRAM, 0);

    // Initialize myself and remote
    struct sockaddr_in sender, receiver;
    memset(&sender, 0, sizeof(sender)); 
    memset(&receiver, 0, sizeof(receiver)); 

    // Sender
    struct hostent *returned_host_remote= gethostbyname(argv[2]);
    sender.sin_family = AF_INET;                                                        // IPv4 
    sender.sin_addr = *(struct in_addr *) returned_host_remote -> h_addr;               // Send into Remote IP Address Only 
    sender.sin_port = htons(remotePortNumber);                                          // Send into Remote Port Number Only

    // Receiver
    receiver.sin_family = AF_INET;
    // receiver.sin_addr.s_addr = INADDR_ANY;                                            // Connect to the remote IP Address
    receiver.sin_port = htons(myPortNumber);                                             // Receiver's Port Number

    // Bind the socket with my address 
    bind(socketDescriptor, (const struct sockaddr *)&receiver, sizeof(receiver));

    // pthread_attributes initialize
    pthread_attr_t attr;
    pthread_attr_init(&attr);

    // Encapsulate information to struct
    Network netInfo;
    netInfo.myself = receiver;
    netInfo.remote = sender;
    netInfo.socketDescriptor = socketDescriptor;
    netInfo.myPortNumber = myPortNumber;
    netInfo.remoteIPAddress = remoteIPAddress;
    netInfo.remotePortNumber = remotePortNumber;

    // Output keyboard thread
    pthread_create(&outputKeyboard_id, &attr, keyboardOutput, NULL);

    // Receive message from remote thread
    pthread_create(&recv_id, &attr, receiveMsg, &netInfo);

    // Send message to remote thread
    pthread_create(&send_id, &attr, sendMsg, &netInfo);

    // Input keyboard thread
    pthread_create(&inputKeyboard_id, &attr, keyboardInput, NULL);

    printf("Now Working....Please Type Messages\n\n");

    // Wait until thread is done its work
    pthread_join(send_id, NULL);
    pthread_join(inputKeyboard_id, NULL);
    pthread_join(outputKeyboard_id, NULL);
    pthread_join(recv_id, NULL);

    close(socketDescriptor);

    return 0;
}