/******************************************************************************
** Copyright (c) 2013-2017 Intel Corporation All Rights Reserved
**
** Licensed under the Apache License, Version 2.0 (the "License"); you may not
** use this file except in compliance with the License.
**
** You may obtain a copy of the License at
**     http://www.apache.org/licenses/LICENSE-2.0
**
** Unless required by applicable law or agreed to in writing, software
** distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
** WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
**
** See the License for the specific language governing permissions and
** limitations under the License.
**
******************************************************************************/
#define ESIF_TRACE_ID ESIF_TRACEMODULE_WEBSERVER
#include <ctype.h>
#include "esif_ws_socket.h"
#include "esif_ws_http.h"
#include "esif_ws_server.h"
#include "esif_ccb_atomic.h"
#include "esif_ccb_lock.h"
#include "esif_uf_shell.h"

#ifdef ESIF_ATTR_OS_WINDOWS
//
// The Windows banned-API check header must be included after all other headers, or issues can be identified
// against Windows SDK/DDK included headers which we have no control over.
//
#define _SDL_BANNED_RECOMMENDED
#include "win\banned.h"
#endif

#define MESSAGE_SUCCESS 0
#define MESSAGE_ERROR 1

/* Max number of Client connections. This cannot exceed FD_SETSIZE-1 (Default: Windows=64, Linux=1024) */
#define MAX_CLIENTS		10

#if (MAX_CLIENTS > (FD_SETSIZE - 1))
# undef  MAX_CLIENTS
# define MAX_CLIENTS	(FD_SETSIZE - 1)
#endif
#define MAX_SOCKETS		(MAX_CLIENTS + 1)

#define	MIN_REST_OUT_PADDING	15	/* space for "%u:" */

/* for cleaning data that may be written to the socket */
#define ASCII_CHAR_LBOUND 32
#define ASCII_CHAR_UBOUND 127
#define ASCII_LINE_FEED 10
#define ASCII_CARRIAGE_RETURN 13

/* Notes on Buffering Incoming Client Messages in ClientRecord buffers:
 *
 * recv() may or may not return a complete websocket frame, depending on whether all
 * of the buffered network data is available yet or not. If the frame is only partially
 * available (or if it is larger than the http buffer), we will read in as much as is
 * available and attempt to process it. If it is determined to be an Incomplete Frame
 * while being processed then its contents are buffered in the current connection's Frame
 * Buffer [ClientRecord.frame_buffer]. Each subsequent recv will append to that buffer
 * (growing it if necessary to a max size) until the complete Frame (defined by payload-size
 * in the header) is received. A complete frame must be received before it can be processed.
 *
 * Even after a complete websocket frame is available (payload-size bytes received),
 * the message may be broken up into multiple frames. These must be processed since
 * it is up to the client whether or not to break a message up into multiple frames.
 * The first frame is identified when a non-control frame type (TEXT, BINARY) is received
 * with FIN=0. Subsequent frames are identified by a CONTINUATION frame type up to
 * and including the last frame for the message, which is identified by FIN=1. When
 * a complete frame is received with FIN=0, its contents are buffered in the current
 * connection's Fragment Buffer [ClientRecord.frag_buffer]. Each subsequent frame will
 * append to that buffer (growing it if necessary to a max size) until the last frame
 * for the message has been received (defined by FIN=1). All message fragments must
 * be received and combined before it can be processed.
 *
 * Fragmented Frames must appear in sequence and all fragments must be sent before any
 * new non-control messages (TEXT, BINARY) can be received, whether fragmented or not.
 * However, control frames (PING, PONG, CLOSE) may be received between message fragments.
 * This is supported by this implementation and control frames are handled as expected.
 *
 * Both the Frame Buffer and Fragment Buffer may or may not exist simultaneously or even
 * at all; after a buffer is combined with the last data received, the buffer is destroyed
 * (unless more recv's or fragments are necessary)
 */
#define MAX_WEBSOCKET_BUFFER	(8*1024*1024)	/* Max size of any single inbound websocket message (all combined fragments) */

/*
 *******************************************************************************
 ** EXTERN
 *******************************************************************************
 */
extern int g_shell_enabled;

/*
 *******************************************************************************
 ** PRIVATE
 *******************************************************************************
 */
#define WEBSOCKET_DEFAULT_IPADDR	"0.0.0.0"	// All Interfaces
#define WEBSOCKET_DEFAULT_PORT		"8888"		// Public Port
#define WEBSOCKET_RESTRICTED_IPADDR	"127.0.0.1"	// Localhost Only
#define WEBSOCKET_RESTRICTED_PORT	"888"		// System Port
#define WEBSOCKET_FRAME_SIZE_DEFAULT (sizeof(WsSocketFrame) + sizeof(EsifCapability))

static char *esif_ws_server_get_rest_response(size_t *msgLenPtr);
static char *esif_ws_server_get_rest_buffer(void);
static void esif_ws_server_initialize_clients(void);
static int esif_ws_broadcast_frame(const u8 *framePtr, size_t frameSize);

static int esif_ws_server_create_inet_addr(
	void *addrPtr,
	socklen_t *addrlenPtr,
	char *hostPtr,
	char *portPtr,
	char *protPtr
	);

void esif_ws_server_execute_rest_cmd(
	const char *dataPtr,
	const size_t dataSize
	);

static void esif_ws_client_initialize_client(ClientRecordPtr);
static eEsifError esif_ws_client_process_request(ClientRecordPtr clientPtr);

static int esif_ws_client_write_to_socket(
	ClientRecordPtr clientPtr,
	const char *bufferPtr,
	size_t bufferSize
	);

static eEsifError esif_ws_client_open_client(
	ClientRecordPtr clientPtr,
	char *bufferPtr,
	size_t bufferSize,
	size_t messageLength
	);

static eEsifError esif_ws_client_process_active_client(
	ClientRecordPtr clientPtr,
	char *bufferPtr,
	size_t bufferSize,
	size_t messageLength
	);

static Bool charIsLineFeed(
	int asciiChar
);

static void strip_extended_ascii(
	char * str
);

static void esif_ws_protocol_initialize(ProtocolPtr protPtr);

static esif_ccb_mutex_t g_ws_lock;      /* lock ws global variables. Move all these to a struct */
static ClientRecordPtr g_clients = NULL; /* dynamically allocated array of MAX_CLIENTS */
static char *g_ws_http_buffer = NULL; /* dynamically allocated buffer of size OUT_BUF_LEN */
static u32  g_ws_http_buffer_len = 0; /* current allocated size of g_ws_http_buffer */
static char *g_rest_out = NULL;

static atomic_t g_ws_quit = 0;
atomic_t g_ws_threads = 0;
static u8 *g_ws_broadcast_frame = NULL; /* Global buffer used for Websocket broadcasts. Grows dynamically */
static size_t g_ws_broadcast_frame_len = 0;

/*
 *******************************************************************************
 ** PUBLIC
 *******************************************************************************
 */
#define MAX_IPADDR 20
char g_ws_ipaddr[MAX_IPADDR]= WEBSOCKET_DEFAULT_IPADDR;
char g_ws_port[MAX_IPADDR] =  WEBSOCKET_DEFAULT_PORT;
Bool g_ws_restricted = ESIF_FALSE;

esif_ccb_socket_t g_listen = INVALID_SOCKET;

int esif_ws_init(void)
{
	int index=0;
	int retVal=0;
	char *ipaddr = (char*)g_ws_ipaddr;
	char *portPtr = g_ws_port;

	struct sockaddr_in addrSrvr = {0};
	struct sockaddr_in addrClient = {0};
	socklen_t len_inet = 0;

	esif_ccb_socket_t client_socket = INVALID_SOCKET;

	int option = 1;
	eEsifError req_results = ESIF_OK;
	ClientRecordPtr clientPtr = NULL;

	int selRetVal = 0;
	int maxfd = 0;
	int setsize = 0;

	struct timeval tv={0}; 	/* Timeout value */
	fd_set workingSet = {0};

	atomic_inc(&g_ws_threads);
	atomic_set(&g_ws_quit, 0);

	esif_ccb_mutex_init(&g_ws_lock);
	esif_ccb_mutex_lock(&g_ws_lock);


	CMD_OUT("starting %sweb server [IP %s port %s]...\n", (g_ws_restricted ? "restricted " : ""), ipaddr, portPtr);

	esif_ccb_socket_init();

	// Allocate pool of Client Records and HTTP input buffer
	esif_ws_server_initialize_clients();
	esif_ws_buffer_resize(WS_BUFFER_LENGTH);
	if (NULL == g_clients || NULL == g_ws_http_buffer) {
		ESIF_TRACE_DEBUG("Out of memory");
		goto exit;
	}

	len_inet = sizeof(addrSrvr);

	retVal   = esif_ws_server_create_inet_addr(&addrSrvr, &len_inet, ipaddr, portPtr, (char*)"top");
	if (retVal < 0) {
		ESIF_TRACE_DEBUG("Invalid server address/port number");
		goto exit;
	}

	g_listen = socket(PF_INET, SOCK_STREAM, 0);
	if (g_listen == SOCKET_ERROR) {
		ESIF_TRACE_DEBUG("open socket error, error #%d", errno);
		goto exit;
	}

	retVal = setsockopt(g_listen, SOL_SOCKET, SO_REUSEADDR, (char*)&option, sizeof(option));
	if (retVal < 0) {
		ESIF_TRACE_DEBUG("setsockopt failed, error #%d", errno);
		goto exit;
	}

	retVal = bind(g_listen, (struct sockaddr*)&addrSrvr, len_inet);
	if (retVal < 0) {
		ESIF_TRACE_DEBUG("bind system call failed, error #%d", errno);
		goto exit;
	}

	retVal = listen(g_listen, MAX_CLIENTS);
	if (retVal < 0) {
		ESIF_TRACE_DEBUG("listen system call failed, error #%d", errno);
		goto exit;
	}

	/* Accept client requests and new connections until told to quit */
	while (!atomic_read(&g_ws_quit)) {

		/* Build file descriptor set of active sockets */
		maxfd = 0;
		setsize = 0;

		/* Clear the FD set we will check after each iteration */
		FD_ZERO(&workingSet);

		/* Add our listner to the FD set to check */
		FD_SET((u_int)g_listen, &workingSet);
		maxfd = (int)g_listen + 1;
		setsize++;

		/* Add our current clients to the FD set to check */
		for (index = 0; index < MAX_CLIENTS && setsize < MAX_SOCKETS; index++) {
			if (g_clients[index].socket != INVALID_SOCKET) {
				FD_SET((u_int)g_clients[index].socket, &workingSet);
				maxfd = esif_ccb_max(maxfd, (int)g_clients[index].socket + 1);
				setsize++;
			}
		}

		/* If we have nothing functional in the FD set to check; break */
		if (maxfd == 0) {
			break;
		}

		/*
		 *  timeout of N + 0.05 secs
		 */
		tv.tv_sec  = 2;
		tv.tv_usec = 50000;

		/* Wait for activity on listener or client sockets for up to maximum timeout period */
		esif_ccb_mutex_unlock(&g_ws_lock);
		selRetVal  = select(maxfd, &workingSet, NULL, NULL, &tv);
		esif_ccb_mutex_lock(&g_ws_lock);

		if (selRetVal == SOCKET_ERROR) {
			break;
		} else if (!selRetVal) {
			continue;
		}

		/* Accept any new connections on the listening socket */
		if (FD_ISSET(g_listen, &workingSet)) {
			int sockets = 1;
			len_inet = sizeof addrClient;

			client_socket = (int)accept(g_listen, (struct sockaddr*)&addrClient, &len_inet);

			if (client_socket == SOCKET_ERROR) {
				ESIF_TRACE_DEBUG("accept(2)");
				goto exit;
			}

			/* Find the first empty client in our list */
			for (index = 0; index < MAX_CLIENTS && sockets < MAX_SOCKETS; index++) {
				if (g_clients[index].socket == INVALID_SOCKET) {
					esif_ws_client_initialize_client(&g_clients[index]);
					g_clients[index].socket = client_socket;
					client_socket = INVALID_SOCKET;
					break;
				}
				sockets++;
			}

			/* If all clients are in use, close the new client */
			if (index >= MAX_CLIENTS || sockets >= MAX_SOCKETS) {
				ESIF_TRACE_DEBUG("Connection Limit Exceeded (%d)", MAX_CLIENTS);
				esif_ccb_socket_close(client_socket);
				client_socket = INVALID_SOCKET;
				continue;
			}
			if (client_socket != INVALID_SOCKET) {
				esif_ccb_socket_close(client_socket);
				client_socket = INVALID_SOCKET;
			}
		}

		/* Go through our client list and check if the FD set indicates any have activity */
		for (index = 0; index < MAX_CLIENTS; index++) {
			client_socket = g_clients[index].socket;
			if (client_socket == INVALID_SOCKET || client_socket == g_listen) {
				continue;
			}

			/* Process client if it is in the set of active file descriptors */
			if (FD_ISSET(client_socket, &workingSet)) {
				ESIF_TRACE_DEBUG("Client %d connected\n", client_socket);

				/******************** Process the client request ********************/
				clientPtr = &g_clients[index];
				req_results = esif_ws_client_process_request(clientPtr);

				if (req_results == ESIF_E_WS_DISC) {
					ESIF_TRACE_DEBUG("Client %d disconnected\n", client_socket);
					esif_ws_client_initialize_client(clientPtr); /* reset */
				}
				else if (req_results == ESIF_E_NO_MEMORY) {
					ESIF_TRACE_DEBUG("Out of memory\n");
					esif_ws_client_initialize_client(clientPtr); /* reset */
				}

				/* Clear everything after use */
				esif_ws_protocol_initialize(&clientPtr->prot);
			}
		}
	}

exit:
	/* cleanup */
	if (g_listen != INVALID_SOCKET) {
		esif_ccb_socket_close(g_listen);
		g_listen = INVALID_SOCKET;
	}
	if (g_clients) {
		for (index = 0; index < MAX_CLIENTS; index++) {
			esif_ws_client_close_client(&g_clients[index]);
		}
		esif_ccb_free(g_clients);
		g_clients = NULL;
	}
	esif_ccb_free(g_rest_out);
	esif_ccb_free(g_ws_http_buffer);
	esif_ccb_free(g_ws_broadcast_frame);
	g_rest_out = NULL;
	g_ws_http_buffer = NULL;
	g_ws_http_buffer_len = 0;
	g_ws_broadcast_frame = NULL;
	g_ws_broadcast_frame_len = 0;
	esif_ccb_socket_exit();
	atomic_dec(&g_ws_threads);
	esif_ccb_mutex_unlock(&g_ws_lock);
	esif_ccb_mutex_uninit(&g_ws_lock);
	return 0;
}

/* stop web server and wait for worker threads to exit */
void esif_ws_exit(esif_thread_t *threadPtr)
{
	CMD_OUT("stopping web server...\n");
	atomic_set(&g_ws_quit, 1);
	esif_ccb_thread_join(threadPtr);  /* join to close child thread, clean up handle */
	// Wait for worker thread to finish
	while (atomic_read(&g_ws_threads) > 0) {
		esif_ccb_sleep(1);
	}
	atomic_set(&g_ws_quit, 0);
	CMD_OUT("web server stopped\n");
}

void esif_ws_server_set_ipaddr_port(const char *ipaddr, u32 portPtr, Bool restricted)
{
	if (ipaddr == NULL) {
		ipaddr = (restricted ? WEBSOCKET_RESTRICTED_IPADDR : WEBSOCKET_DEFAULT_IPADDR);
	}
	if (portPtr == 0) {
		portPtr = atoi(restricted ? WEBSOCKET_RESTRICTED_PORT : WEBSOCKET_DEFAULT_PORT);
	}
	esif_ccb_strcpy(g_ws_ipaddr, ipaddr, sizeof(g_ws_ipaddr));
	esif_ccb_sprintf(sizeof(g_ws_port), g_ws_port, "%d", portPtr);
	g_ws_restricted = restricted;
}


char *esif_ws_server_get_rest_response(size_t *msgLenPtr)
{
	char *msgPtr = g_rest_out;
	size_t msgLen = 0;

	ESIF_TRACE_DEBUG("Message Received: %s \n", msgPtr);

	if (msgPtr == NULL) {
		goto exit;
	}

	msgLen = esif_ccb_strlen((char *)msgPtr, g_ws_http_buffer_len);
	if (msgLen == 0) {
		goto exit;
	}

	*msgLenPtr = msgLen;
exit:
	return msgPtr;
}


void esif_ws_server_execute_rest_cmd(
	const char *dataPtr,
	const size_t dataSize
	)
{
	char *command_buf = NULL;

	if (atomic_read(&g_ws_quit))
		return;

	command_buf = strchr(dataPtr, ':');
	if (NULL == command_buf) {
		esif_ccb_free(g_rest_out);
		g_rest_out = esif_ccb_strdup("0:ERROR");
	}
	else {
		u32 msg_id = atoi(dataPtr);
		*command_buf = 0;
		command_buf++;

		// Ad-Hoc UI Shell commands begin with "0:", so verify ESIF shell is enabled and command is valid
		if (msg_id == 0 || g_ws_restricted) {
			char *response = NULL;
			if (msg_id == 0 && !g_shell_enabled) {
				response = "Shell Disabled";
			}
			else {
				static char *whitelist[] = { "status", "participants", NULL };
				static char *blacklist[] = { "shell", "web", "exit", "quit", NULL };
				Bool blocked = ESIF_FALSE;
				int j = 0;
				const char *skip_cmds[] = { "format text && ", "format xml && ", NULL };
				char *shell_cmd = command_buf;

				// Skip any commands in the skip_cmds list in before checking the shell command against the blacklist/whitelist
				while (skip_cmds[j]) {
					size_t cmd_len = esif_ccb_strlen(skip_cmds[j], MAX_PATH);
					if (esif_ccb_strnicmp(shell_cmd, skip_cmds[j], cmd_len) == 0) {
						shell_cmd += cmd_len;
						j = 0;
						continue;
					}
					j++;
				}

				// Verify the shell command against Whitelist and Blacklist
				if (g_ws_restricted) {
					for (j = 0; whitelist[j] != NULL; j++) {
						if (esif_ccb_strnicmp(shell_cmd, whitelist[j], esif_ccb_strlen(whitelist[j], MAX_PATH)) == 0) {
							break;
						}
					}
					if (whitelist[j] == NULL) {
						blocked = ESIF_TRUE;
					}
				}
				else {
					for (j = 0; blacklist[j] != NULL; j++) {
						if (esif_ccb_strnicmp(shell_cmd, blacklist[j], esif_ccb_strlen(blacklist[j], MAX_PATH)) == 0) {
							blocked = ESIF_TRUE;
							break;
						}
					}
				}
				if (blocked) {
					response = "Unsupported Command";
				}
			}
			// Exit if shell or command unavailable
			if (response) {
				char buffer[MAX_PATH] = { 0 };
				esif_ccb_sprintf(sizeof(buffer), buffer, "%d:%s", msg_id, response);
				esif_ccb_free(g_rest_out);
				g_rest_out = esif_ccb_strdup(buffer);
				goto exit;
			}
		}

		// Lock Shell so we can capture output before another thread executes another command
		esif_uf_shell_lock();
		if (!atomic_read(&g_ws_quit)) {
			EsifString cmd_results = esif_shell_exec_command(command_buf, dataSize, ESIF_TRUE, ESIF_FALSE);
			if (NULL != cmd_results) {
				strip_extended_ascii(cmd_results);
				size_t out_len = esif_ccb_strlen(cmd_results, OUT_BUF_LEN) + MIN_REST_OUT_PADDING;
				esif_ccb_free(g_rest_out);
				g_rest_out = (EsifString) esif_ccb_malloc(out_len);
				if (g_rest_out && out_len >= MIN_REST_OUT_PADDING) {
					esif_ccb_sprintf(out_len, g_rest_out, "%u:%s", msg_id, cmd_results);
				}
				esif_ws_buffer_resize(WS_BUFFER_LENGTH);
			}
			else {
				esif_ccb_free(g_rest_out);
				g_rest_out = esif_ccb_strdup("0:");
			}
		}
		esif_uf_shell_unlock();
	}

exit:
	return;
}


static int esif_ws_server_create_inet_addr(
	void *addrPtr,
	socklen_t *addrlenPtr,
	char *hostPtr,
	char *portPtr,
	char *protPtr
	)
{
	struct sockaddr_in *sockaddr_inPtr = (struct sockaddr_in*)addrPtr;
	struct servent *serventPtr = NULL;

	char *endStr = NULL;
	long longVal;

	if (!hostPtr) {
		hostPtr = (char*)"*";
	}

	if (!portPtr) {
		portPtr = (char*)"*";
	}

	if (!protPtr) {
		protPtr = (char*)"tcp";
	}


	esif_ccb_memset(sockaddr_inPtr, 0, *addrlenPtr);
	sockaddr_inPtr->sin_family = AF_INET;
	sockaddr_inPtr->sin_port   = 0;
	sockaddr_inPtr->sin_addr.s_addr = INADDR_ANY;


	if (esif_ccb_strcmp(hostPtr, "*") == 0) {
		;
	} else {
		sockaddr_inPtr->sin_addr.s_addr = inet_addr(hostPtr);

		if (sockaddr_inPtr->sin_addr.s_addr == INADDR_NONE) {
			return -1;
		}
	}

	if (!esif_ccb_strcmp(portPtr, "*")) {
		;
	} else if (isdigit(*portPtr)) {
		longVal = strtol(portPtr, &endStr, 10);
		if (endStr != NULL && *endStr) {
			return -2;
		}

		if (longVal < 0L || longVal >= 32768) {
			return -2;
		}

		sockaddr_inPtr->sin_port = esif_ccb_htons((short)longVal);
	} else {
		serventPtr = getservbyname(portPtr, protPtr);
		if (!serventPtr) {
			return -2;
		}

		sockaddr_inPtr->sin_port = (short)serventPtr->s_port;
	}

	*addrlenPtr = sizeof *sockaddr_inPtr;

	return 0;
}


static int esif_ws_client_write_to_socket(
	ClientRecordPtr clientPtr,
	const char *bufferPtr,
	size_t bufferSize
	)
{
	ssize_t ret=0;

	ret = send(clientPtr->socket, (char*)bufferPtr, (int)bufferSize, ESIF_WS_SEND_FLAGS);
	if (ret == -1 || ret != (ssize_t)bufferSize) {
		esif_ccb_socket_close(clientPtr->socket);
		clientPtr->socket = INVALID_SOCKET;
		ESIF_TRACE_DEBUG("Error writing to socket: %s", (ret == -1 ? "Error in sending packets\n" : "Incomplete data\n"));
		return EXIT_FAILURE;
	}
	return EXIT_SUCCESS;
}

static int esif_ws_broadcast_frame(
	const u8 *framePtr,
	size_t frameSize
	)
{
	ssize_t ret = 0;
	int rc = EXIT_SUCCESS;
	int index = 0;

	if (NULL == g_clients) {
		rc = EXIT_FAILURE;
		goto exit;
	}

	for (index = 0; index < MAX_CLIENTS; index++) {
		ClientRecordPtr clientPtr = &g_clients[index];

		if (clientPtr->socket == INVALID_SOCKET || clientPtr->socket == g_listen || clientPtr->state != STATE_NORMAL) {
			continue;
		}

		ret = send(clientPtr->socket, (char*)framePtr, (int)frameSize, ESIF_WS_SEND_FLAGS);

		if (ret == -1 || ret != (ssize_t)frameSize) {
			esif_ccb_socket_close(clientPtr->socket);
			clientPtr->socket = INVALID_SOCKET;
			ESIF_TRACE_DEBUG("Error writing to socket: %s", (ret == -1 ? "Error in sending packets\n" : "Incomplete data\n"));
			rc = EXIT_FAILURE;
		}
	}

exit:
	return rc;
}


/*
 * This function processes requests for either websocket connections or
 * http connections.
 */
static eEsifError esif_ws_client_process_request(ClientRecordPtr clientPtr)
{
	eEsifError result = ESIF_OK;
	ssize_t messageLength  = 0;
	char *frameBuffer = NULL;
	char *messageBuffer = g_ws_http_buffer;
	size_t messageBufferLen = g_ws_http_buffer_len;

	esif_ccb_memset(messageBuffer, 0, messageBufferLen);

	/* Read the next partial or complete message fragment from the client socket.
	 * See "Notes on Buffering Incoming Client Messages" above for implementation details
	 */
	messageLength = recv(clientPtr->socket, messageBuffer, (int)messageBufferLen, 0);
	if (messageLength == 0 || messageLength == SOCKET_ERROR) {
		ESIF_TRACE_DEBUG("no messages received from the socket\n");
		result =  ESIF_E_WS_DISC;
		goto exit;
	}
	ESIF_TRACE_DEBUG("%d bytes received\n", (int)messageLength);

	/* Combine this partial frame with the current connection's Frame Buffer, if any*/
	if (clientPtr->frame_buffer != NULL && clientPtr->frame_buf_len > 0) {
		size_t total_buffer_len = clientPtr->frame_buf_len + messageLength;
		if (total_buffer_len <= MAX_WEBSOCKET_BUFFER) {
			frameBuffer = esif_ccb_realloc(clientPtr->frame_buffer, total_buffer_len);
		}
		if (frameBuffer == NULL) {
			result = ESIF_E_NO_MEMORY;
			goto exit;
		}
		ESIF_TRACE_DEBUG("WS Frame Unbuffering: buflen=%zd msglen=%zd total=%zd http=%d\n", clientPtr->frame_buf_len, messageLength, total_buffer_len, g_ws_http_buffer_len);
		esif_ccb_memcpy(frameBuffer + clientPtr->frame_buf_len, messageBuffer, messageLength);
		messageBuffer = frameBuffer;
		messageBufferLen = total_buffer_len;
		messageLength = total_buffer_len;
		clientPtr->frame_buffer = NULL;
		clientPtr->frame_buf_len = 0;
	}

	switch (clientPtr->state) {
	case STATE_OPENING:
		result = esif_ws_client_open_client(clientPtr, messageBuffer, messageBufferLen, (size_t)messageLength);
		break;
	case STATE_NORMAL:
		result = esif_ws_client_process_active_client(clientPtr, messageBuffer, messageBufferLen, (size_t)messageLength);
		break;
	default:
		result = ESIF_E_WS_DISC;
		break;
	}

exit:
	esif_ccb_free(frameBuffer);
	return result;
}


/*
 * This function processes the socket when it is in the "opening" state
 */
static eEsifError esif_ws_client_open_client(
	ClientRecordPtr clientPtr,
	char *bufferPtr,
	size_t bufferSize,
	size_t messageLength
	)
{
	eEsifError result = ESIF_OK;
	FrameType frameType;
	size_t frameSize = 0;

	ESIF_ASSERT(clientPtr->state == STATE_OPENING);
	ESIF_ASSERT(messageLength > 0);

	ESIF_TRACE_DEBUG("Socket in its opening state\n");
	/*Determine the initial frame type:  http frame type or websocket frame type */
	frameType = esif_ws_socket_get_initial_frame_type(bufferPtr, messageLength, &clientPtr->prot);

	if ((INCOMPLETE_FRAME == frameType) ||  (ERROR_FRAME == frameType)) {
		if (INCOMPLETE_FRAME == frameType) {
			ESIF_TRACE_DEBUG("Incomplete frame received\n");
		} else {
			ESIF_TRACE_DEBUG("Improper format for frame\n");
		}

		/*
		 * If the socket frame type is in error or is incomplete and happens to
		 * be in its opening state, send a message to the client that the request is bad
		 */
		frameSize = esif_ccb_sprintf(bufferSize,
			(char*)bufferPtr,
			"HTTP/1.1 400 Bad Request" CRLF
			"Content-Type: text/html" CRLF
			CRLF
			"<html>"
			"<head></head>"
			"  <body>"
			"    ERROR: HTTP/1.1 400 Bad Request"
			"  </body>"
			"</html>");

		esif_ws_client_write_to_socket(clientPtr, bufferPtr, frameSize);
		result =  ESIF_E_WS_DISC;
		goto exit;
	}

	if (OPENING_FRAME == frameType) {
		if (esif_ws_socket_build_protocol_change_response(&clientPtr->prot, bufferPtr, bufferSize, &frameSize) != 0)	{
			ESIF_TRACE_DEBUG("Unable to build response header\n");
			result =   ESIF_E_WS_DISC;
			goto exit;
		}

		if (esif_ws_client_write_to_socket(clientPtr, bufferPtr, frameSize) == EXIT_FAILURE) {
			result =   ESIF_E_WS_DISC;
			goto exit;
		}

		/**************************** This is a now a websocket connection ****************************/
		clientPtr->state = STATE_NORMAL;
	}

	if (HTTP_FRAME == frameType) {
		result = esif_ws_http_process_reqs(clientPtr, g_ws_http_buffer, g_ws_http_buffer_len, messageLength);
	}
exit:
	return result;
}

static Bool charIsLineFeed(int asciiChar)
{
	return (asciiChar == ASCII_LINE_FEED || asciiChar == ASCII_CARRIAGE_RETURN);
}

static void strip_extended_ascii(char *bufferToClean)
{
	unsigned char *sourceStringPtr = (unsigned char *)(void*)bufferToClean;
	unsigned char *resultStringPtr = sourceStringPtr;

	ESIF_ASSERT(bufferToClean != NULL);

	while (*sourceStringPtr != '\0') {
		if (((int)*sourceStringPtr >= ASCII_CHAR_LBOUND && (int)*sourceStringPtr <= ASCII_CHAR_UBOUND) || charIsLineFeed((int)*sourceStringPtr)) {
			*(resultStringPtr++) = *sourceStringPtr;
		}
		sourceStringPtr++;
	}
	*resultStringPtr = '\0';
}

/*
 * This function processes requests for clients already opened.
 */
static eEsifError esif_ws_client_process_active_client(
	ClientRecordPtr clientPtr,
	char *bufferPtr,
	size_t bufferSize,
	size_t messageLength
	)
{
	eEsifError result = ESIF_OK;
	FrameType frameType;
	size_t frameSize       = 0;
	char *data 		   = NULL;
	size_t dataSize        = 0;
	char *restRespPtr = NULL;
	size_t restRespSize = 0;
	size_t bytesRemaining  = 0;
	char *bufferRemaining = NULL;
	char *textStrPtr = NULL;

	do {
		/* If more frames remaining, copy them into buffer and reparse */
		if (bytesRemaining != 0) {
			esif_ccb_memcpy(bufferPtr, bufferRemaining, bytesRemaining);
			messageLength = bytesRemaining;
			bytesRemaining = 0;
		}

		WsSocketFramePtr framePtr = (WsSocketFramePtr)bufferPtr;
		frameType = esif_ws_socket_get_subsequent_frame_type(framePtr, messageLength, &data, &dataSize, &bytesRemaining);
		ESIF_TRACE_DEBUG("FrameType: %d\n", frameType);

		/* Append this partial frame to the current connection's Frame Buffer, if any */
		if (frameType == INCOMPLETE_FRAME) {
			size_t oldSize = clientPtr->frame_buf_len;
			size_t newSize = oldSize + messageLength;
			u8 *newBuffer = NULL;
			if (newSize <= MAX_WEBSOCKET_BUFFER) {
				newBuffer = esif_ccb_realloc(clientPtr->frame_buffer, newSize);
			}
			if (newBuffer == NULL) {
				result = ESIF_E_NO_MEMORY;
				goto exit;
			}
			ESIF_TRACE_DEBUG("WS Frame Buffering: buflen=%zd msglen=%zd total=%zd\n", oldSize, messageLength, newSize);
			esif_ccb_memcpy(newBuffer + oldSize, bufferPtr, messageLength);
			clientPtr->frame_buffer = newBuffer;
			clientPtr->frame_buf_len = newSize;
			goto exit;
		}

		/* Append this message fragment to the current connection's Fragment Buffer, if any */
		if (frameType == FRAGMENT_FRAME) {
			size_t oldSize = clientPtr->frag_buf_len;
			size_t newSize = oldSize + dataSize;
			u8 *newBuffer = NULL;
			if (newSize <= MAX_WEBSOCKET_BUFFER) {
				newBuffer = esif_ccb_realloc(clientPtr->frag_buffer, newSize);
			}
			if (newBuffer == NULL) {
				result = ESIF_E_NO_MEMORY;
				goto exit;
			}
			ESIF_TRACE_DEBUG("WS Fragment Buffering: buflen=%zd msglen=%zd total=%zd\n", oldSize, dataSize, newSize);
			esif_ccb_memcpy(newBuffer + oldSize, data, dataSize);
			clientPtr->frag_buffer = newBuffer;
			clientPtr->frag_buf_len = newSize;

			/* Multi-Fragment messages put real opcode in 1st Fragment only; All others are Continuation Frames, including FIN */
			if (framePtr->header.s.opcode != CONTINUATION_FRAME) {
				clientPtr->frag_type = framePtr->header.s.opcode;
			}
		}
		
		/* Use First Fragment's Frame Type if FIN is set on (final) CONTINUATION frame */
		if (frameType == CONTINUATION_FRAME && framePtr->header.s.fin == 1) {
			frameType = clientPtr->frag_type;
		}

		/* Save remaining frames for reparsing if more than one frame received */
		if (bytesRemaining > 0) {
			if (bufferRemaining == NULL) {
				bufferRemaining = (char *)esif_ccb_malloc(bytesRemaining);
				if (NULL == bufferRemaining) {
					result = ESIF_E_NO_MEMORY;
					goto exit;
				}
			}
			esif_ccb_memcpy(bufferRemaining, data + dataSize, bytesRemaining);
		}
		else {
			esif_ccb_free(bufferRemaining);
			bufferRemaining = NULL;
		}

		/* Close Connection on Error */
		if (ERROR_FRAME == frameType) {
			ESIF_TRACE_DEBUG("Invalid Frame; Closing socket: Type=%02hX FIN=%hd Len=%hd Mask=%hd\n", framePtr->header.s.opcode, framePtr->header.s.fin, framePtr->header.s.payLen, framePtr->header.s.maskFlag);

			/*
			 * If the socket is not in its opening state while its frame type is in error or is incomplete
			 * setup to store the payload to send to the client
			 */
			esif_ws_socket_build_payload(NULL, 0, (WsSocketFramePtr)bufferPtr, bufferSize, &frameSize, CLOSING_FRAME);
			esif_ws_client_write_to_socket(clientPtr, bufferPtr, frameSize);

			/*
			 * Force the socket state into its closing state
			 */
			result =   ESIF_E_WS_DISC;
			goto exit;

		}

		/* Close Connection on Closing Frame */
		if (CLOSING_FRAME == frameType) {
			ESIF_TRACE_DEBUG("Close frame received; closing socket\n");
			esif_ws_socket_build_payload(NULL, 0, (WsSocketFramePtr)bufferPtr, bufferSize, &frameSize, CLOSING_FRAME);
			esif_ws_client_write_to_socket(clientPtr, bufferPtr, frameSize);

			result =   ESIF_E_WS_DISC;
			goto exit;
		}

		/* Binary Frames currently unsupported - Discard and Ignore */
		if (BINARY_FRAME == frameType) {
			esif_ccb_free(clientPtr->frag_buffer);
			clientPtr->frag_buffer = NULL;
			clientPtr->frag_buf_len = 0;
			clientPtr->frag_type = EMPTY_FRAME;
		}

		/* Process Text Frames and send to REST API */
		if (TEXT_FRAME == frameType) {

			/* Use a copy of the frame text to send to the rest API, including any prior fragments */
			size_t prior_fragments_len = clientPtr->frag_buf_len;
			size_t total_message_len = prior_fragments_len + dataSize;
			if (total_message_len < MAX_WEBSOCKET_BUFFER) {
				textStrPtr = (char*)esif_ccb_malloc(total_message_len + 1);
			}
			if (NULL == textStrPtr) {
				result = ESIF_E_NO_MEMORY;
				goto exit;
			}
			/* Combine the final fragment with the current connection's Fragment Buffer, if any */
			if (prior_fragments_len > 0 && clientPtr->frag_buffer != NULL) {
				ESIF_TRACE_DEBUG("WS Fragment Unbuffering: buflen=%zd msglen=%zd total=%zd\n", prior_fragments_len, dataSize, total_message_len);
				esif_ccb_memcpy(textStrPtr, clientPtr->frag_buffer, prior_fragments_len);
				esif_ccb_free(clientPtr->frag_buffer);
				clientPtr->frag_buffer = NULL;
				clientPtr->frag_buf_len = 0;
				clientPtr->frag_type = EMPTY_FRAME;
			}
			esif_ccb_memcpy(textStrPtr + prior_fragments_len, data, dataSize);
			textStrPtr[total_message_len] = 0;

			esif_ws_server_execute_rest_cmd((const char*)textStrPtr,
				esif_ccb_strlen((const char*)textStrPtr, total_message_len + 1));

			// Reset Output Buffer since REST cmd may have grown it
			if (bufferSize != g_ws_http_buffer_len) {
				bufferPtr = g_ws_http_buffer;
				bufferSize = g_ws_http_buffer_len;
			}

			/* Get message to send*/
			restRespPtr = esif_ws_server_get_rest_response(&restRespSize);
			
			if (restRespPtr != NULL) {
				esif_ws_socket_build_payload(restRespPtr, restRespSize, (WsSocketFramePtr)bufferPtr, bufferSize, &frameSize, TEXT_FRAME);

				if (esif_ws_client_write_to_socket(clientPtr, bufferPtr, frameSize) == EXIT_FAILURE) {
					result =  ESIF_E_WS_DISC;
					goto exit;
				}
			}

			esif_ccb_free(textStrPtr);
			textStrPtr = NULL;
		}

		/* Respond to PING Frames with PONG Frame containing Ping's Data */
		if (PING_FRAME == frameType) {
			esif_ws_socket_build_payload(data, dataSize, (WsSocketFramePtr)bufferPtr, bufferSize, &frameSize, PONG_FRAME);
			esif_ws_client_write_to_socket(clientPtr, bufferPtr, frameSize);
		}

		/* Handle unsolicited PONG (keepalive) messages from Internet Explorer 10 (Not required per RFC 6455 but allowed) */
		if (PONG_FRAME == frameType) {
			esif_ws_socket_build_payload("", 0, (WsSocketFramePtr)bufferPtr, bufferSize, &frameSize, TEXT_FRAME);
			esif_ws_client_write_to_socket(clientPtr, bufferPtr, frameSize);
		}

		/* Ignore CONTINUATION Frames; They will be processed when a FIN is received for the last Frame */

	} while (bytesRemaining != 0);
exit:
	esif_ccb_free(textStrPtr);
	esif_ccb_free(bufferRemaining);
	textStrPtr = NULL;
	bufferRemaining = NULL;

	return result;
}


void esif_ws_server_initialize_clients(void)
{
	int index=0;
	g_clients = (ClientRecordPtr)esif_ccb_malloc(MAX_CLIENTS * sizeof(*g_clients));
	for (index = 0; (g_clients && index < MAX_CLIENTS); ++index) {
		g_clients[index].socket = INVALID_SOCKET;
		esif_ws_client_initialize_client(&g_clients[index]);
	}
}

void esif_ws_client_initialize_client(ClientRecordPtr clientPtr)
{
	if (NULL == clientPtr) {
		return;
	}

	clientPtr->state     = STATE_OPENING;
	esif_ws_protocol_initialize(&clientPtr->prot);
	if (clientPtr->socket != INVALID_SOCKET) {
		esif_ccb_socket_close(clientPtr->socket);
	}
	clientPtr->socket = INVALID_SOCKET;

	esif_ccb_free(clientPtr->frame_buffer);
	clientPtr->frame_buffer = NULL;
	clientPtr->frame_buf_len = 0;

	esif_ccb_free(clientPtr->frag_buffer);
	clientPtr->frag_buffer = NULL;
	clientPtr->frag_buf_len = 0;
	clientPtr->frag_type = EMPTY_FRAME;
}


void esif_ws_client_close_client(ClientRecordPtr clientPtr)
{
	if (NULL == clientPtr) {
		return;
	}

	esif_ws_protocol_initialize(&clientPtr->prot);
	clientPtr->state = STATE_OPENING;

	if (clientPtr->socket != INVALID_SOCKET) {
		esif_ccb_socket_close(clientPtr->socket);
		clientPtr->socket = INVALID_SOCKET;
	}
	esif_ccb_free(clientPtr->frame_buffer);
	clientPtr->frame_buffer = NULL;
	clientPtr->frame_buf_len = 0;

	esif_ccb_free(clientPtr->frag_buffer);
	clientPtr->frag_buffer = NULL;
	clientPtr->frag_buf_len = 0;
	clientPtr->frag_type = EMPTY_FRAME;
}


void esif_ws_protocol_initialize(ProtocolPtr protPtr)
{
	esif_ccb_free(protPtr->hostField);
	esif_ccb_free(protPtr->originField);
	esif_ccb_free(protPtr->webpage);
	esif_ccb_free(protPtr->keyField);
	esif_ccb_free(protPtr->web_socket_field);
	protPtr->hostField   = NULL;
	protPtr->originField = NULL;
	protPtr->webpage     = NULL;
	protPtr->keyField    = NULL;
	protPtr->web_socket_field = NULL;
	protPtr->frameType = EMPTY_FRAME;
}

u32 esif_ws_buffer_resize(u32 size)
{
	if (size > g_ws_http_buffer_len) {
		char *new_buffer = esif_ccb_realloc(g_ws_http_buffer, size);
		if (new_buffer != NULL) {
			g_ws_http_buffer = new_buffer;
			g_ws_http_buffer_len = size;
		}
	}
	return g_ws_http_buffer_len;
}

eEsifError esif_ws_broadcast_data_buffer(const u8 *bufferPtr, size_t bufferSize)
{
	eEsifError rc = ESIF_OK;
	size_t frameSize = 0; /* max frame size. will be updated when payload built */
	
	if (NULL == bufferPtr) {
		return ESIF_E_PARAMETER_IS_NULL;
	}

	if ((atomic_read(&g_ws_threads) < 1) ||
		(atomic_read(&g_ws_quit) == 1)) { /* Either web server is not running, or is shutting down */
		return ESIF_E_IFACE_DISABLED;
	}

	if (g_ws_restricted) { /* Support event broadcasting only in non-restricted mode */
		return ESIF_E_IFACE_DISABLED;
	}

	/* Lock WebServer so we can access clients & buffers since we are on a different thread */
	esif_ccb_mutex_lock(&g_ws_lock);

	/* Compute maximum frame size and grow global broadcast frame buffer if necessary */
	frameSize = bufferSize + sizeof(WsSocketFrame);
	if (frameSize > g_ws_broadcast_frame_len) {
		u8 *new_buffer = NULL;
		frameSize = esif_ccb_max(frameSize, WEBSOCKET_FRAME_SIZE_DEFAULT);
		new_buffer = (u8 *)esif_ccb_realloc(g_ws_broadcast_frame, frameSize);
		if (new_buffer == NULL) {
			rc = ESIF_E_NO_MEMORY;
			goto exit;
		}
		g_ws_broadcast_frame = new_buffer;
		g_ws_broadcast_frame_len = frameSize;
	}

	esif_ws_socket_build_payload((char *) bufferPtr,
		bufferSize,
		(WsSocketFramePtr)g_ws_broadcast_frame,
		g_ws_broadcast_frame_len,
		&frameSize,
		BINARY_FRAME
	);

	if (frameSize == 0 || esif_ws_broadcast_frame(g_ws_broadcast_frame, frameSize) != EXIT_SUCCESS) {
		ESIF_TRACE_ERROR("Failed to broadcast DPTF event to websocket clients");
		rc = ESIF_E_UNSPECIFIED;
	}

exit:
	esif_ccb_mutex_unlock(&g_ws_lock);
	return rc;
}
