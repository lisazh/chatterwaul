/*
 *   CSC469 Fall 2010 A3
 *  
 *      File:      client.h 
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
#include <assert.h>

#include <netinet/in.h>
#include <netdb.h>

#include "client.h"

static void build_req(char *buf)
{
	/* Write an HTTP GET request for the chatserver.txt file into buf */

	int nextpos;

	sprintf(buf,"GET /~csc469h/winter/chatserver.txt HTTP/1.0\r\n");

	nextpos = strlen(buf);
	sprintf(&buf[nextpos],"\r\n");
}

static char *skip_http_headers(char *buf)
{
	/* Given a pointer to a buffer which contains an HTTP reply,
	 * skip lines until we find a blank, and then return a pointer
	 * to the start of the next line, which is the reply body.
	 * 
	 * DO NOT call this function if buf does not contain an HTTP
	 * reply message.  The termination condition on the while loop 
	 * is ill-defined for arbitrary character arrays, and may lead 
	 * to bad things(TM). 
	 *
	 * Feel free to improve on this.
	 */

	char *curpos;
	int n;
	char line[256];

	curpos = buf;

	while ( sscanf(curpos,"%256[^\n]%n",line,&n) > 0) {
		if (strlen(line) == 1) { /* Just the \r was consumed */
			/* Found our blank */
			curpos += n+1; /* skip line and \n at end */
			break;
		}
		curpos += n+1;
	}

	return curpos;
}


// find next network newline
int find_nn(char *s, int n) {
	int i;
	for (i = 0; i < n-1; ++i) {
		if (s[i] == '\r' && s[i+1] == '\n') {
			return i;
		}
	}
	return -1;
}

int retrieve_chatserver_info(char *chatserver_name, u_int16_t *tcp_port, u_int16_t *udp_port)
{
	int locn_socket_fd;
	char *buf;
	int buflen;
	int code;
	int  n;

	/* Initialize locnserver_addr. 
	 * We use a text file at a web server for location info
	 * so this is just contacting the CDF web server 
	 */

	/* 
	 * 1. Set up TCP connection to web server "www.cdf.toronto.edu", 
	 *    port 80 
	 */
	/* The code you write should initialize locn_socket_fd so that
	 * it is valid for the write() in the next step.
	 */
	/**** YOUR CODE HERE ****/
	
	int status;
	struct addrinfo hints;
	struct addrinfo *res;  // will point to the results
	
	memset(&hints, 0, sizeof(hints)); // make sure the struct is empty
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_STREAM;
	
	status = getaddrinfo("www.cdf.toronto.edu", "http", &hints, &res);
	if (status != 0) {
		return -1;
	}
	
	// make a tcp socket
	locn_socket_fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
	if (locn_socket_fd < 0) {
		freeaddrinfo(res);
		return -1;
	}
	
	// connect to the server
	status = connect(locn_socket_fd, res->ai_addr, res->ai_addrlen);
	if (status != 0) {
		freeaddrinfo(res);
		close(locn_socket_fd);
		return -1;
	}
	
	/* 2. write HTTP GET request to socket */

	buf = (char *)malloc(MAX_MSG_LEN);
	bzero(buf, MAX_MSG_LEN);
	build_req(buf);
	buflen = strlen(buf);

	write(locn_socket_fd, buf, buflen);

	/* 3. Read reply from web server */

	read(locn_socket_fd, buf, MAX_MSG_LEN);

	/* 
	 * 4. Check if request succeeded.  If so, skip headers and initialize
	 *    server parameters with body of message.  If not, print the 
	 *    STATUS-CODE and STATUS-TEXT and return -1.
	 */

	/* Ignore version, read STATUS-CODE into variable 'code' , and record
	 * the number of characters scanned from buf into variable 'n'
	 */
	sscanf(buf, "%*s %d%n", &code, &n);


	/**** YOUR CODE HERE ****/
	if (code >= 200 && code < 300) {
		// success!
		int eol = find_nn(buf, MAX_MSG_LEN);
		assert(eol >= 0);
		
		printf("HTTP %d", code);
		int i;
		for (i = n; i < eol; ++i) {
			putchar(buf[i]);
		}
		printf("\n");
		
		// get the server info
		char *body = skip_http_headers(buf);
		sscanf(body, "%s %hu %hu", chatserver_name, tcp_port, udp_port);
		
	} else {
		// not success
		// so print out status and message
		int eol = find_nn(buf, MAX_MSG_LEN);
		assert(eol >= 0);
		
		printf("HTTP error %d", code);
		int i;
		for (i = n; i < eol; ++i) {
			putchar(buf[i]);
		}
		printf("\n");
		
		free(buf);
		freeaddrinfo(res);
		close(locn_socket_fd);
		return -1;
	}

	/* 5. Clean up after ourselves and return. */
	free(buf);
	
	freeaddrinfo(res);
	close(locn_socket_fd);
	
	return 0;

}
