/*
 *   CSC469 Fall 2010 A3
 *  
 *      File:      client_recv.c 
 *      Author:    Angela Demke Brown
 *      Version:   1.0.0
 *      Date:      17/11/2010
 *   
 * Please report bugs/comments to demke@cs.toronto.edu
 *
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <netdb.h>

#include "client.h"

static char *option_string = "f:";

/* For communication with chat client control process */
int ctrl2rcvr_qid;
char ctrl2rcvr_fname[MAX_FILE_NAME_LEN];

/* For receiving messages from the server */
int sockfd;


void usage(char **argv) {
	printf("usage:\n");
	printf("%s -f <msg queue file name>\n",argv[0]);
	exit(1);
}


void open_client_channel(int *qid) {

	/* Get messsage channel */
	key_t key = ftok(ctrl2rcvr_fname, 42);

	if ((*qid = msgget(key, 0400)) < 0) {
		perror("open_channel - msgget failed");
		fprintf(stderr,"for message channel ./msg_channel\n");

		/* No way to tell parent about our troubles, unless/until it 
		 * waits for us.  Quit now.
		 */
		exit(1);
	}

	return;
}

void send_error(int qid, u_int16_t code)
{
	/* Send an error result over the message channel to client control process */
	msg_t msg;

	msg.mtype = CTRL_TYPE;
	msg.body.status = RECV_NOTREADY;
	msg.body.value = code;

	if (msgsnd(qid, &msg, sizeof(struct body_s), 0) < 0) {
		perror("send_error msgsnd");
	}
							 
}

void send_ok(int qid, u_int16_t port)
{
	/* Send "success" result over the message channel to client control process */
	msg_t msg;

	msg.mtype = CTRL_TYPE;
	msg.body.status = RECV_READY;
	msg.body.value = port;

	if (msgsnd(qid, &msg, sizeof(struct body_s), 0) < 0) {
		perror("send_ok msgsnd");
	} 

}

void init_receiver()
{

	/* 1. Make sure we can talk to parent (client control process) */
	printf("Trying to open client channel\n");

	open_client_channel(&ctrl2rcvr_qid);

	/**** YOUR CODE TO FILL IMPLEMENT STEPS 2 AND 3 ****/

	/* 2. Initialize UDP socket for receiving chat messages. */

	struct sockaddr_in servaddr;
	socklen_t slen = sizeof(servaddr); 
	//assuming we don't have a specific protocol in mind?
	if ((sockfd = socket(AF_INET, SOCK_DGRAM, 0)) < 0){
		send_error(ctrl2rcvr_qid, SOCKET_FAILED);
		exit(1);
	}
	
	bzero(&servaddr, sizeof(servaddr));
	servaddr.sin_family = AF_INET;
	servaddr.sin_addr.s_addr = htonl(INADDR_ANY);
	servaddr.sin_port = 0;
	
	/* 3. Tell parent the port number if successful, or failure code if not. 
	 *    Use the send_error and send_ok functions
	 */
	
	if (bind(sockfd, (struct sockaddr *)&servaddr, slen) < 0){
		send_error(ctrl2rcvr_qid, BIND_FAILED);
		exit(1);
	}
	 
	if (getsockname(sockfd, (struct sockaddr *)&servaddr, &slen) < 0){

	  send_error(ctrl2rcvr_qid, NAME_FAILED);
	  exit(1);

	}

	send_ok(ctrl2rcvr_qid, servaddr.sin_port);

}




/* Function to deal with a single message from the chat server */

void handle_received_msg(char *buf)
{

	/**** YOUR CODE HERE ****/
  	//process message from server and output to user
	struct chat_msghdr *recvd = (struct chat_msghdr*)buf;
	printf("%s:\n", recvd->sender.member_name);
	char msgbd[recvd->msg_len+ 1];
	strncpy(msgbd, (char *)recvd->msgdata, recvd->msg_len + 1);
	//null terminate for safety
	msgbd[recvd->msg_len] = '\0';
	printf("%s\n", msgbd);
	fflush(stdout);
}



/* Main function to receive and deal with messages from chat server
 * and client control process.  
 *
 * You may wish to refer to server_main.c for an example of the main 
 * server loop that receives messages, but remember that the client 
 * receiver will be receiving (1) connection-less UDP messages from the 
 * chat server and (2) IPC messages on the from the client control process
 * which cannot be handled with the same select()/FD_ISSET strategy used 
 * for file or socket fd's.
 */
void receive_msgs()
{
	char *buf = (char *)malloc(MAX_MSG_LEN);
  
	if (buf == 0) {
		printf("Could not malloc memory for message buffer\n");
		exit(1);
	}

	/**** YOUR CODE HERE ****/
	//for the message queue
	msg_t msg;
	
	//for the select()
	fd_set readset, readset_orig;
	struct timeval wtime;

	//set up readset and add the socket descriptor to it
	FD_ZERO(&readset);
	FD_SET(sockfd, &readset);
	readset_orig = readset;

	while(TRUE) {

		/**** YOUR CODE HERE ****/
		//check if server has sent a message over
		//set how long to wait for server messages
		wtime.tv_sec = 5; 
		wtime.tv_usec = 0;
		readset = readset_orig;
		if (select(sockfd + 1, &readset, NULL, NULL, &wtime)< 0){
			perror("select");
			free(buf);
			exit(1);
		}

		if(FD_ISSET(sockfd, &readset)){
			memset(buf, 0, MAX_MSG_LEN);
			recv(sockfd, buf, MAX_MSG_LEN, 0);
			handle_received_msg(buf);

		}
		

		//select timed out, so check for messages from control message queue
		//msgflag is 1 to ensure no waiting? (check if correct)
		msgrcv(ctrl2rcvr_qid, &msg, sizeof(msg_t), RECV_TYPE, IPC_NOWAIT);
		if (msg.body.status == CHAT_QUIT){
			// quit n stuff
			if (close(sockfd) < 0){
				perror("close"); //ALTERNATIVELY: send_error to control???
				exit(1);
			}

			break;

	  	}

	}

	/* Cleanup */
	free(buf);
	return;
}


int main(int argc, char **argv) {
	char option;

	printf("RECEIVER alive: parsing options! (argc = %d\n",argc);

	while((option = getopt(argc, argv, option_string)) != -1) {
		switch(option) {
		case 'f':
			strncpy(ctrl2rcvr_fname, optarg, MAX_FILE_NAME_LEN);
			break;
		default:
			printf("invalid option %c\n",option);
			usage(argv);
			break;
		}
	}

	if(strlen(ctrl2rcvr_fname) == 0) {
		usage(argv);
	}

	printf("Receiver options ok... initializing\n");

	init_receiver();

	receive_msgs();

	return 0;
}
