/*
 *   CSC469 Fall 2010 A3
 *  
 *      File:      client_main.c 
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
#include <assert.h>
#include <unistd.h>
#include <time.h>

#include <netinet/in.h>
#include <netdb.h>

#include "client.h"
#include <errno.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/select.h>
#include <sys/time.h>

/*************** GLOBAL VARIABLES ******************/

static char *option_string = "h:t:u:n:";

/* For communication with chat server */
/* These variables provide some extra clues about what you will need to
 * implement.
 */
char server_host_name[MAX_HOST_NAME_LEN] = "";

/* For control messages */
u_int16_t server_tcp_port = 0;
int tcp_socket_fd = -1;
struct addrinfo* tcp_addrinfo = NULL;

/* For chat messages */
u_int16_t server_udp_port = 0;
int udp_socket_fd = -1;

/* Needed for REGISTER_REQUEST */
char member_name[MAX_MEMBER_NAME_LEN] = "";
u_int16_t client_udp_port = 0; 

/* Initialize with value returned in REGISTER_SUCC response */
u_int16_t member_id = 0;

// Keep track of what room we're in
char room[MAX_ROOM_NAME_LEN+1];
int inside_room = FALSE; /* whether we are in a room or not */

/* For communication with receiver process */
pid_t receiver_pid = -1;
char ctrl2rcvr_fname[MAX_FILE_NAME_LEN] = "";
int ctrl2rcvr_qid;

/* MAX_MSG_LEN is maximum size of a message, including header+body.
 * We define the maximum size of the msgdata field based on this.
 */
#define MAX_MSGDATA (MAX_MSG_LEN - sizeof(struct chat_msghdr))

// holds the last error message from a failed control message exchange
char last_error_msg[MAX_MSGDATA+1] = "";

// how many times a control message is retried
#define MAX_CTRL_RETRIES 3

// how many times a reconnection is tried
#define MAX_CONNECT_RETRIES 5
// delay between reconnection attempts in seconds
#define RECONNECT_DELAY 3

// seconds between sending out keep alives
#define KEEP_ALIVE_PERIOD 30

/************* FUNCTION DEFINITIONS ***********/

static void usage(char **argv) {

	printf("usage:\n");

#ifdef USE_LOCN_SERVER
	printf("%s -n <client member name>\n",argv[0]);
#else 
	printf("%s -h <server host name> -t <server tcp port> -u <server udp port> -n <client member name>\n",argv[0]);
#endif /* USE_LOCN_SERVER */

	exit(1);
}

static void error(char *const msg) {
	printf("ERROR: %s\n", msg);
	exit(1);
}


void shutdown_clean() {
	/* Function to clean up after ourselves on exit, freeing any 
	 * used resources 
	 */

	/* Add to this function to clean up any additional resources that you
	 * might allocate.
	 */

	msg_t msg;

	/* 1. Send message to receiver to quit */
	msg.mtype = RECV_TYPE;
	msg.body.status = CHAT_QUIT;
	msgsnd(ctrl2rcvr_qid, &msg, sizeof(struct body_s), 0);

	/* 2. Close open fd's */
	close(udp_socket_fd);

	/* 3. Wait for receiver to exit */
	waitpid(receiver_pid, 0, 0);

	/* 4. Destroy message channel */
	unlink(ctrl2rcvr_fname);
	if (msgctl(ctrl2rcvr_qid, IPC_RMID, NULL)) {
		perror("cleanup - msgctl removal failed");
	}

	exit(0);
}

int initialize_client_only_channel(int *qid)
{
	/* Create IPC message queue for communication with receiver process */

	int msg_fd;
	int msg_key;

	/* 1. Create file for message channels */

	snprintf(ctrl2rcvr_fname,MAX_FILE_NAME_LEN,"/tmp/ctrl2rcvr_channel.XXXXXX");
	msg_fd = mkstemp(ctrl2rcvr_fname);

	if (msg_fd  < 0) {
		perror("Could not create file for communication channel");
		return -1;
	}

	close(msg_fd);

	/* 2. Create message channel... if it already exists, delete it and try again */

	msg_key = ftok(ctrl2rcvr_fname, 42);

	if ( (*qid = msgget(msg_key, IPC_CREAT|IPC_EXCL|S_IREAD|S_IWRITE)) < 0) {
		if (errno == EEXIST) {
			if ( (*qid = msgget(msg_key, S_IREAD|S_IWRITE)) < 0) {
				perror("First try said queue existed. Second try can't get it");
				unlink(ctrl2rcvr_fname);
				return -1;
			}
			if (msgctl(*qid, IPC_RMID, NULL)) {
				perror("msgctl removal failed. Giving up");
				unlink(ctrl2rcvr_fname);
				return -1;
			} else {
				/* Removed... try opening again */
				if ( (*qid = msgget(msg_key, IPC_CREAT|IPC_EXCL|S_IREAD|S_IWRITE)) < 0) {
					perror("Removed queue, but create still fails. Giving up");
					unlink(ctrl2rcvr_fname);
					return -1;
				}
			}

		} else {
			perror("Could not create message queue for client control <--> receiver");
			unlink(ctrl2rcvr_fname);
			return -1;
		}
    
	}

	return 0;
}

int create_receiver()
{
	/* Create the receiver process using fork/exec and get the port number
	 * that it is receiving chat messages on.
	 */

	int retries = 20;
	int numtries = 0;

	/* 1. Set up message channel for use by control and receiver processes */

	if (initialize_client_only_channel(&ctrl2rcvr_qid) < 0) {
		return -1;
	}

	/* 2. fork/exec xterm */

	receiver_pid = fork();

	if (receiver_pid < 0) {
		fprintf(stderr,"Could not fork child for receiver\n");
		return -1;
	}

	if ( receiver_pid == 0) {
		/* this is the child. Exec receiver */
		char *argv[] = {"xterm",
				"-e",
				"./receiver",
				"-f",
				ctrl2rcvr_fname,
				0
		};

		execvp("xterm", argv);
		printf("Child: exec returned. that can't be good.\n");
		exit(1);
	} 

	/* This is the parent */

	/* 3. Read message queue and find out what port client receiver is using */

	while ( numtries < retries ) {
		int result;
		msg_t msg;
		result = msgrcv(ctrl2rcvr_qid, &msg, sizeof(struct body_s), CTRL_TYPE, IPC_NOWAIT);
		if (result == -1 && errno == ENOMSG) {
			sleep(1);
			numtries++;
		} else if (result > 0) {
			if (msg.body.status == RECV_READY) {
				printf("Start of receiver successful, port %u\n",ntohs(msg.body.value));
				client_udp_port = msg.body.value;
			} else {
				printf("start of receiver failed with code %u\n",msg.body.value);
				return -1;
			}
			break;
		} else {
			perror("msgrcv");
		}
    
	}

	if (numtries == retries) {
		/* give up.  wait for receiver to exit so we get an exit code at least */
		int exitcode;
		printf("Gave up waiting for msg.  Waiting for receiver to exit now\n");
		waitpid(receiver_pid, &exitcode, 0);
		printf("start of receiver failed, exited with code %d\n",exitcode);
	}

	return 0;
}


// this finds the tcp and udp ports and sets it up
// this can be called many times if it fails
// returns 0 on success, -1 on error
int find_server() {
	
	// if we're trying to find a server, it must mean that we are no longer in a room
	inside_room = FALSE;
	
	int status;
	struct addrinfo hints;
	struct addrinfo *res;  // will point to the results
	
#ifdef USE_LOCN_SERVER
	
	/* 0. Get server host name, port numbers from location server.
	 *    See retrieve_chatserver_info() in client_util.c
	 */
	
	status = retrieve_chatserver_info(server_host_name, &server_tcp_port, &server_udp_port);
	if (status != 0) {
		return -1;
	}
	
#endif
	
	// as of here, we should have access to:
	// server_host_name,server_tcp_port,server_udp_port
	
	/* 1. initialization to allow TCP-based control messages to chat server */
	
	memset(&hints, 0, sizeof(hints)); // make sure the struct is empty
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_STREAM;
	
	// get a string representation of the port number
	char server_tcp_port_str[20];
	sprintf(server_tcp_port_str, "%d", server_tcp_port);
	
	status = getaddrinfo(server_host_name, server_tcp_port_str, &hints, &res);
	if (status != 0) {
		return -1;
	}
	
	// keep it around because we need to connect for every control message
	// free up the old one if necessary
	if (tcp_addrinfo != NULL) {
		freeaddrinfo(tcp_addrinfo);
	}
	tcp_addrinfo = res;
	
	/* 2. initialization to allow UDP-based chat messages to chat server */
	memset(&hints, 0, sizeof(hints)); // make sure the struct is empty
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_DGRAM;
	
	// get a string representation of the port number
	char server_udp_port_str[20];
	sprintf(server_udp_port_str, "%d", server_udp_port);
	
	status = getaddrinfo(server_host_name, server_udp_port_str, &hints, &res);
	if (status != 0) {
		return -1;
	}
	
	// make a udp socket
	// clean up an old one if necessary
	if (udp_socket_fd >= 0) {
		close(udp_socket_fd);
		udp_socket_fd = -1;
	}
	
	udp_socket_fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
	if (udp_socket_fd < 0) {
		freeaddrinfo(res);
		return -1;
	}
	
	// connect to the server (so we can use send with udp socket)
	status = connect(udp_socket_fd, res->ai_addr, res->ai_addrlen);
	if (status != 0) {
		freeaddrinfo(res);
		close(udp_socket_fd);
		udp_socket_fd = -1;
		return -1;
	}
	
	// done with the addrinfo
	freeaddrinfo(res);
	
	// success!
	return 0;
}

// try several times to reconnect to the server and register
// 0 on success, shuts down on failure
// we shut down on failure because there isn't much more we can do
// assume receiver process is up, so shutdown_clean will work properly
// given a flag for whether to register or not
int try_reconnect() {
	int was_inside_room = inside_room;
	inside_room = FALSE;
	
	printf("\nServer connection lost\n");
	fflush(stdout);
	
	int status;
	int i;
	
	int connected = FALSE;
	
	for (i = 0; i < MAX_CONNECT_RETRIES; ++i) {
		printf("Attempting to reconnect to the server (try %d of %d)\n", i+1, MAX_CONNECT_RETRIES);
		fflush(stdout);
		status = find_server();
		if (status == 0) {
			// re-register if specified
			int handle_register_req();
			status = handle_register_req();
			if (status != 0) {
				printf("Error encountered: %s\n", last_error_msg);
				fflush(stdout);
			} else {
				connected = TRUE;
				break;
			}
		}
		sleep(RECONNECT_DELAY);
	}
	
	if (!connected) {
		printf("Error encountered: cannot connect to server\n");
		fflush(stdout);
		shutdown_clean();
		return -1;
	}
	
	printf("Reconnected>  ");
	fflush(stdout);
	
	// now that we're reconnected, try to rejoin the room we were in
	// we can call this because we at most recurse down one level
	// since inside_room is reset to FALSE in find_server
	int handle_create_room_req(char *room_name);
	int handle_switch_room_req(char *room_name);
	
	
	if (was_inside_room) {
		char old_room[MAX_ROOM_NAME_LEN+1];
		old_room[MAX_ROOM_NAME_LEN] = '\0';
		strncpy(old_room, room, MAX_ROOM_NAME_LEN);
		
		// first try to rejoin
		status = handle_switch_room_req(old_room);
		if (status == 0) {
			// everything good
			printf("Rejoined room (%s)>  ", room);
			fflush(stdout);
		} else {
			// otherwise try to create it again
			status = handle_create_room_req(old_room);
			if (status == 0) {
				// good, so try to join again
				status = handle_switch_room_req(old_room);
				if (status == 0) {
					printf("Joined recreated room (%s)>  ", room);
					fflush(stdout);
				} else {
					printf("Couldn't joined recreated room (%s)>  ", room);
					fflush(stdout);
				}
			} else {
				printf("Couldn't rejoin previous room>  ");
				fflush(stdout);
			}
		}
	}
	
	return 0;
}

// attempts to reconnect to the server if the given condition is false
// returns if successful, and shuts down if not
// returns whether a reconnect was necessary
// used for any connection related calls
int check(int cond) {
	if (!cond) {
		try_reconnect();
	}
	return !cond;
}

/*********************************************************************/

/* We define one handle_XXX_req() function for each type of 
 * control message request from the chat client to the chat server.
 * These functions should return 0 on success, and a negative number 
 * on error. On error, they should set an appropriate error message to
 * last_error_msg.
 */

// helper functions for command messages

// always call this to prepare for handling a control message
// returns NULL on error
struct control_msghdr *prepare_handle() {
	// make a tcp socket
	tcp_socket_fd = socket(tcp_addrinfo->ai_family, tcp_addrinfo->ai_socktype, tcp_addrinfo->ai_protocol);
	if (tcp_socket_fd < 0) {
		return NULL;
	}
	
	// connect to the server
	int status = connect(tcp_socket_fd, tcp_addrinfo->ai_addr, tcp_addrinfo->ai_addrlen);
	if (status != 0) {
		close(tcp_socket_fd);
		tcp_socket_fd = -1;
		return NULL;
	}
	
	char *buf = malloc(MAX_MSG_LEN);
	memset(buf, 0, MAX_MSG_LEN);
	return (struct control_msghdr*)buf;
}

// always call this to finish handling a control message
void finish_handle(struct control_msghdr *ptr) {
	// finish the exchange and close the connection
	if (ptr != NULL) {
		free(ptr);
	}
	if (tcp_socket_fd >= 0) {
		close(tcp_socket_fd);
		tcp_socket_fd = -1;
	}
}

// Assume we have server_host_name,server_tcp_port,member_name
// this doesn't try to reconnect on error
int handle_register_req()
{
	// try to send the message
	struct control_msghdr *cmh = prepare_handle();
	if (cmh == NULL) {
		finish_handle(cmh);
		strcpy(last_error_msg, "failed to connect\n");
		return -1;
	}
	
	// package message to send to server
	struct register_msgdata *rdata = (struct register_msgdata*)cmh->msgdata;
	
	cmh->msg_type = REGISTER_REQUEST;
	rdata->udp_port = client_udp_port; 
	
	strncpy((char*)rdata->member_name, member_name, MAX_MSGDATA-1);
	
	cmh->msg_len = sizeof(struct control_msghdr) + sizeof(struct register_msgdata) + strlen(member_name)+1;
	
	int sent_length = send(tcp_socket_fd, cmh, cmh->msg_len, 0);
	if (sent_length != cmh->msg_len) {
		strcpy(last_error_msg, "failed to connect\n");
		return -1;
	}
	
	// receive response
	int received_length = recv(tcp_socket_fd, cmh, MAX_MSG_LEN, 0);
	if (received_length < sizeof(struct control_msghdr)) {
		strcpy(last_error_msg, "failed to connect\n");
		return -1;
	}

	int ret = -1;
	
	switch(cmh->msg_type) {
		case REGISTER_SUCC:
			// get the member id
			member_id = cmh->member_id;
			ret = 0;
			break;
		case REGISTER_FAIL:
			// store the error message
			strncpy(last_error_msg, (char*)cmh->msgdata, MAX_MSGDATA);
			ret = -1;
			break;
		default:
			// unexpected control message
			error("Unexpected control message response");
			ret = -1;
			break;
	}
	
	finish_handle(cmh);
	
	return ret;
}

int handle_room_list_req()
{
	// send the message
	int i;
	for (i = 0; i < MAX_CTRL_RETRIES; ++i) {
		struct control_msghdr *cmh = prepare_handle();
		if (check(cmh != NULL)) {
			finish_handle(cmh);
			continue;
		}
		
		cmh->msg_type = ROOM_LIST_REQUEST;
		cmh->member_id = member_id;
		cmh->msg_len = sizeof(struct control_msghdr);
		
		int sent_length = send(tcp_socket_fd, cmh, cmh->msg_len, 0);
		if (check(sent_length == cmh->msg_len)) {
			finish_handle(cmh);
			continue;
		}
		
		// receive response
		int received_length = recv(tcp_socket_fd, cmh, MAX_MSG_LEN, 0);
		if (check(received_length >= sizeof(struct control_msghdr))) {
			finish_handle(cmh);
			continue;
		}
		
		int ret = -1;
		
		switch(cmh->msg_type) {
			case ROOM_LIST_SUCC:
				// print out the rooms
				printf("%s\n", (char*)cmh->msgdata);
				ret = 0;
				break;
			case ROOM_LIST_FAIL:
				// store the error message
				strncpy(last_error_msg, (char*)cmh->msgdata, MAX_MSGDATA);
				ret = -1;
				break;
			default:
				// unexpected control message
				error("Unexpected control message response");
				ret = -1;
				break;
		}
		
		finish_handle(cmh);
		
		return ret;
	}
	
	strcpy(last_error_msg, "control message failed\n");
	return -1;
}

int handle_member_list_req(char *room_name)
{
	// send the message
	int i;
	for (i = 0; i < MAX_CTRL_RETRIES; ++i) {
		struct control_msghdr *cmh = prepare_handle();
		if (check(cmh != NULL)) {
			finish_handle(cmh);
			continue;
		}
		
		cmh->msg_type = MEMBER_LIST_REQUEST;
		cmh->member_id = member_id;
		
		strncpy((char*)cmh->msgdata, room_name, MAX_MSGDATA-1);
		
		cmh->msg_len = sizeof(struct control_msghdr) + strlen(room_name)+1;
		
		int sent_length = send(tcp_socket_fd, cmh, cmh->msg_len, 0);
		if (check(sent_length == cmh->msg_len)) {
			finish_handle(cmh);
			continue;
		}
		
		// receive response
		int received_length = recv(tcp_socket_fd, cmh, MAX_MSG_LEN, 0);
		if (check(received_length >= sizeof(struct control_msghdr))) {
			finish_handle(cmh);
			continue;
		}
		
		int ret = -1;
		
		switch(cmh->msg_type) {
			case MEMBER_LIST_SUCC:
				// print out the members
				printf("%s\n", (char*)cmh->msgdata);
				ret = 0;
				break;
			case MEMBER_LIST_FAIL:
				// store the error message
				strncpy(last_error_msg, (char*)cmh->msgdata, MAX_MSGDATA);
				ret = -1;
				break;
			default:
				// unexpected control message
				error("Unexpected control message response");
				ret = -1;
				break;
		}
		
		finish_handle(cmh);
		
		return ret;
	}
	
	strcpy(last_error_msg, "control message failed\n");
	return -1;
}

int handle_switch_room_req(char *room_name)
{
	// send the message
	int i;
	for (i = 0; i < MAX_CTRL_RETRIES; ++i) {
		struct control_msghdr *cmh = prepare_handle();
		if (check(cmh != NULL)) {
			finish_handle(cmh);
			continue;
		}
		
		cmh->msg_type = SWITCH_ROOM_REQUEST;
		cmh->member_id = member_id;
		
		strncpy((char*)cmh->msgdata, room_name, MAX_MSGDATA-1);
		
		cmh->msg_len = sizeof(struct control_msghdr) + strlen(room_name)+1;
		
		int sent_length = send(tcp_socket_fd, cmh, cmh->msg_len, 0);
		if (check(sent_length == cmh->msg_len)) {
			finish_handle(cmh);
			continue;
		}
		
		// receive response
		int received_length = recv(tcp_socket_fd, cmh, MAX_MSG_LEN, 0);
		if (check(received_length >= sizeof(struct control_msghdr))) {
			finish_handle(cmh);
			continue;
		}
		
		int ret = -1;
		
		switch(cmh->msg_type) {
			case SWITCH_ROOM_SUCC:
				// print out the members
				printf("Successfully switched to room %s\n", room_name);
				room[MAX_ROOM_NAME_LEN] = '\0';
				strncpy(room, room_name, MAX_ROOM_NAME_LEN);
				inside_room = TRUE;
				ret = 0;
				break;
			case SWITCH_ROOM_FAIL:
				// store the error message
				strncpy(last_error_msg, (char*)cmh->msgdata, MAX_MSGDATA);
				ret = -1;
				break;
			default:
				// unexpected control message
				error("Unexpected control message response");
				ret = -1;
				break;
		}
		
		finish_handle(cmh);
		
		return ret;
	}
	
	strcpy(last_error_msg, "control message failed\n");
	return -1;
}

int handle_create_room_req(char *room_name)
{
	// send the message
	int i;
	for (i = 0; i < MAX_CTRL_RETRIES; ++i) {
		struct control_msghdr *cmh = prepare_handle();
		if (check(cmh != NULL)) {
			finish_handle(cmh);
			continue;
		}
		
		cmh->msg_type = CREATE_ROOM_REQUEST;
		cmh->member_id = member_id;
		
		strncpy((char*)cmh->msgdata, room_name, MAX_MSGDATA-1);
		
		cmh->msg_len = sizeof(struct control_msghdr) + strlen(room_name)+1;
		
		int sent_length = send(tcp_socket_fd, cmh, cmh->msg_len, 0);
		if (check(sent_length == cmh->msg_len)) {
			finish_handle(cmh);
			continue;
		}
		
		// receive response
		int received_length = recv(tcp_socket_fd, cmh, MAX_MSG_LEN, 0);
		if (check(received_length >= sizeof(struct control_msghdr))) {
			finish_handle(cmh);
			continue;
		}
		
		int ret = -1;
		
		switch(cmh->msg_type) {
			case CREATE_ROOM_SUCC:
				// print out the members
				printf("Successfully created room %s\n", room_name);
				ret = 0;
				break;
			case CREATE_ROOM_FAIL:
				// store the error message
				strncpy(last_error_msg, (char*)cmh->msgdata, MAX_MSGDATA);
				ret = -1;
				break;
			default:
				// unexpected control message
				error("Unexpected control message response");
				ret = -1;
				break;
		}
		
		finish_handle(cmh);
		
		return ret;
	}
	
	strcpy(last_error_msg, "control message failed\n");
	return -1;
}

int handle_quit_req()
{
	// send the message
	// if this fails anywhere, then just quit
	struct control_msghdr *cmh = prepare_handle();
	if (cmh == NULL) {
		shutdown_clean();
		return -1;
	}
	
	cmh->msg_type = QUIT_REQUEST;
	cmh->member_id = member_id;
	
	cmh->msg_len = sizeof(struct control_msghdr);
	
	send(tcp_socket_fd, cmh, cmh->msg_len, 0);
	
	// no response necessary
	
	finish_handle(cmh);
	
	// now we shutdown
	shutdown_clean();
	
	return 0;
}

// doesn't try to reconnect
int send_member_keep_alive() {
	// send the message
	struct control_msghdr *cmh = prepare_handle();
	if (cmh == NULL) {
		finish_handle(cmh);
		return -1;
	}
	
	cmh->msg_type = MEMBER_KEEP_ALIVE;
	cmh->member_id = member_id;
	
	cmh->msg_len = sizeof(struct control_msghdr);
	
	int sent_length = send(tcp_socket_fd, cmh, cmh->msg_len, 0);
	if (sent_length != cmh->msg_len) {
		finish_handle(cmh);
		return -1;
	}
	
	// no response necessary
	
	finish_handle(cmh);
	
	return 0;
}

int init_client()
{
	/* Initialize client so that it is ready to start exchanging messages
	 * with the chat server.
	 *
	 * YOUR CODE HERE
	 */
	
	// as of here, we should have access to:
	// server_host_name,server_tcp_port,server_udp_port,member_name
	
	/* 1. initialization to allow TCP-based control messages to chat server */
	/* 2. initialization to allow UDP-based chat messages to chat server */
	int status = find_server();
	if (status != 0) {
		// just quit and have the user try again
		printf("ERROR: cannot find server\n");
		exit(1);
	}
	
	/* 3. spawn receiver process - see create_receiver() in this file. */
	
	create_receiver();
	
	/* 4. register with chat server */
	
	status = handle_register_req();
	check(status == 0);
	
	return 0;
}



void handle_chatmsg_input(char *inputdata)
{
	/* inputdata is a pointer to the message that the user typed in.
	 * This function should package it into the msgdata field of a chat_msghdr
	 * struct and send the chat message to the chat server.
	 */

	char *buf = (char *)malloc(MAX_MSG_LEN);
  
	if (buf == 0) {
		printf("Could not malloc memory for message buffer\n");
		shutdown_clean();
		exit(1);
	}
	
	bzero(buf, MAX_MSG_LEN);
	
	
	/**** YOUR CODE HERE ****/
	// assume inputdata is restricted in size and null-terminated
	
	// assemble the chat message
	struct chat_msghdr *chmh = (struct chat_msghdr*)buf;
	chmh->sender.member_id = member_id;
	strncpy((char*)chmh->msgdata, inputdata, MAX_MSGDATA-1);
	chmh->msg_len = sizeof(struct chat_msghdr) + strlen(inputdata)+1;
	
	int sent_length = send(udp_socket_fd, chmh, chmh->msg_len, 0);
	// even though this is udp, check anyway
	check(sent_length == chmh->msg_len);
	
	free(buf);
	
	return;
}


/* This should be called with the leading "!" stripped off the original
 * input line.
 * 
 * You can change this function in any way you like.
 *
 */
void handle_command_input(char *line)
{
	char cmd = line[0]; /* single character identifying which command */
	int len = 0;
	int goodlen = 0;
	int result;

	line++; /* skip cmd char */

	/* 1. Simple format check */

	switch(cmd) {

	case 'r':
	case 'q':
		if (strlen(line) != 0) {
			printf("Error in command format: !%c should not be followed by anything.\n",cmd);
			return;
		}
		break;

	case 'c':
	case 'm':
	case 's':
		{
			int allowed_len = MAX_ROOM_NAME_LEN;

			if (line[0] != ' ') {
				printf("Error in command format: !%c should be followed by a space and a room name.\n",cmd);
				return;
			}
			line++; /* skip space before room name */

			len = strlen(line);
			goodlen = strcspn(line, " \t\n"); /* Any more whitespace in line? */
			if (len != goodlen) {
				printf("Error in command format: line contains extra whitespace (space, tab or carriage return)\n");
				return;
			}
			if (len > allowed_len) {
				printf("Error in command format: name must not exceed %d characters.\n",allowed_len);
				return;
			}
		}
		break;

	default:
		printf("Error: unrecognized command !%c\n",cmd);
		return;
		break;
	}

	/* 2. Passed format checks.  Handle the command */

	switch(cmd) {

	case 'r':
		result = handle_room_list_req();
		break;

	case 'c':
		result = handle_create_room_req(line);
		break;

	case 'm':
		result = handle_member_list_req(line);
		break;

	case 's':
		result = handle_switch_room_req(line);
		break;

	case 'q':
		result = handle_quit_req(); // does not return. Exits.
		break;

	default:
		printf("Error !%c is not a recognized command.\n",cmd);
		break;
	}

	/* Currently, we ignore the result of command handling.
	 * You may want to change that.
	 */
	if (result != 0) {
		printf("Error encountered: %s\n", last_error_msg);
	}
	

	return;
}

void get_user_input()
{
	// initialize data needed for a select call
	fd_set readfds;
	struct timespec waittime;
	waittime.tv_sec = KEEP_ALIVE_PERIOD;
	waittime.tv_nsec = 0;
	time_t start = time(NULL);
	
	int status;
	
	char *buf = (char *)malloc(MAX_MSGDATA);
	//char *result_str;
	
	while(TRUE) {
		if (inside_room) {
			printf("\n(%s)[%s]>  ", room, member_name);
		} else {
			printf("\n[%s]>  ",member_name);
		}
		fflush(stdout);
		
		// get ready for a select
		
		while(TRUE) {
			FD_ZERO(&readfds);
			FD_SET(0, &readfds);
			
			// pselect doesn't modify waittime
			status = pselect(1, &readfds, NULL, NULL, &waittime, NULL);
			
			// handle stuff
			if (status < 0) {
				perror("select()");
				shutdown_clean();
			}
			
			// need to send member keep alive
			time_t elapsed = time(NULL) - start;
			if (status == 0 || elapsed >= KEEP_ALIVE_PERIOD) {
				status = send_member_keep_alive();
				check(status == 0);
				
				start = time(NULL);
				elapsed = 0;
			} else {
				assert(elapsed >= 0 && elapsed < KEEP_ALIVE_PERIOD);
			}
			// keep waiting for however many seconds are leftover
			waittime.tv_sec = KEEP_ALIVE_PERIOD - elapsed;
			waittime.tv_nsec = 0;
			
			if (status > 0) {
				// need to go handle the command
				break;
			}
		}
		
		// now we know we can read from stdin without blocking
		bzero(buf, MAX_MSGDATA);
		
		ssize_t read_size = read(0, buf, MAX_MSGDATA-1);
		// result_str = fgets(buf,MAX_MSGDATA,stdin);
		if (read_size <= 0) {
			printf("Error or EOF while reading user input.  Guess we're done.\n");
			break;
		}

		/* Check if control message or chat message */
		
		/* buf probably ends with newline.  If so, get rid of it. */
		int len = strlen(buf);
		if (buf[len-1] == '\n') {
			buf[len-1] = '\0';
		}
		
		if (buf[0] == '!') {
			handle_command_input(&buf[1]);
		} else {
			handle_chatmsg_input(buf);
		}
	}
	free(buf);
}


int main(int argc, char **argv)
{
	char option;
 
	while((option = getopt(argc, argv, option_string)) != -1) {
		switch(option) {
		case 'h':
			strncpy(server_host_name, optarg, MAX_HOST_NAME_LEN);
			break;
		case 't':
			server_tcp_port = atoi(optarg);
			break;
		case 'u':
			server_udp_port = atoi(optarg);
			break;
		case 'n':
			strncpy(member_name, optarg, MAX_MEMBER_NAME_LEN);
			break;
		default:
			printf("invalid option %c\n",option);
			usage(argv);
			break;
		}
	}

#ifdef USE_LOCN_SERVER

	printf("Using location server to retrieve chatserver information\n");

	if (strlen(member_name) == 0) {
		usage(argv);
	}

#else

	if(server_tcp_port == 0 || server_udp_port == 0 ||
	   strlen(server_host_name) == 0 || strlen(member_name) == 0) {
		usage(argv);
	}

#endif /* USE_LOCN_SERVER */

	init_client();

	get_user_input();

	return 0;
}
