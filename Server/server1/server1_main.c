#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <string.h>
#include "../sockwrap.h"
#include "../errlib.h"

#define BUF_SIZE 255
#define LISTEN_QUEUE 15
#define TIMEOUT_SEC 15
#define PATH_MAX 4096

#define OK_MSG "+OK\r\n"
#define ERR_MSG "-ERR\r\n"
#define GET_MSG "GET "
#define END_MSG "\r\n"

const char* prog_name = "MyServerTCP";

__blksize_t getBlockSize(const char *filename) {
    struct stat st;      
    if(stat(filename, &st) == 0)
        return st.st_blksize;
    else
        return -1;
}

long int getFileSize(const char *filename) {
    struct stat st;      
    if(stat(filename, &st) == 0)
        return st.st_size;
    else
        return -1;
}

time_t getLastUpdate(const char *filename) {
    struct stat st;      
    if(stat(filename, &st) == 0)
        return st.st_mtime;
    else
        return -1;
}

void sendError(int sock) {
	sendn(sock, ERR_MSG, 6, MSG_NOSIGNAL);
}

void initServer(struct sockaddr_in *saddr, uint16_t port) {
    saddr->sin_family = AF_INET;
    saddr->sin_port = port;
	saddr->sin_addr.s_addr = INADDR_ANY;
}

int main(int argc, char* argv[]) {

	/* check arguments */
	if (argc!=2)
		err_quit("Usage: %s <port>", prog_name);
	if(strspn(argv[1], "0123456789") != strlen(argv[1]) || strlen(argv[1]) < 4 || strlen(argv[1]) > 5)  
		err_quit("Please insert a correct port number");
	int port = atoi(argv[1]);
	if(port < 1024 || port > 65535)
		err_quit("Please insert a correct port number");

	struct sockaddr_in servaddr, cliaddr;
	uint16_t tport_n = htons(port);
	socklen_t cliaddrlen = sizeof(cliaddr);
	int s_pbc, s_pvt;
	char msg_rcvd[BUF_SIZE];
	ssize_t bytes_rcvd, bytes_snd;
	char filepath[PATH_MAX];
	long int size;
	time_t timestamp;
	__blksize_t blockSize;
	uint32_t fileSize, lastUpdate;
	size_t bytes_read;
	int option = 1;

	initServer(&servaddr, tport_n);
	s_pbc = Socket(AF_INET, SOCK_STREAM, 0);
	setsockopt(s_pbc, SOL_SOCKET, SO_REUSEADDR, &option, sizeof(option)); //to prevent "address already in use" error
	Bind(s_pbc, (struct sockaddr*)&servaddr, sizeof(servaddr));
	Listen(s_pbc, LISTEN_QUEUE);

	while(1) {		
		/* Flush the buffers */
		memset(msg_rcvd, 0, BUF_SIZE);

		s_pvt = accept(s_pbc, (struct sockaddr*)&cliaddr, &cliaddrlen);
		if(s_pvt < 0) {
			err_msg("Accept() fallita\n");
			break;
		} 

		while(1) {
			
			/* Receive GET message */
			memset(msg_rcvd, 0, BUF_SIZE);
			bytes_rcvd = protocol_readn(s_pvt, msg_rcvd, 4, TIMEOUT_SEC); // 4 for "GET "
			if(bytes_rcvd == -2) {
				err_msg("\nNessuna risposta dal client dopo %d secondi\n", TIMEOUT_SEC);
				break;
			}	
			if (bytes_rcvd == 0)
				break;
			if(bytes_rcvd != 4 || strcmp(msg_rcvd, GET_MSG) != 0 || bytes_rcvd < 0) {
				sendError(s_pvt);
				break;
			} 

			/* Receive FILENAME message */
			bytes_rcvd = protocol_readline_unbuffered(s_pvt, msg_rcvd, BUF_SIZE, TIMEOUT_SEC);
			if(bytes_rcvd == -2) {
				err_msg("\nNessuna risposta dal client dopo %d secondi\n", TIMEOUT_SEC);
				break;
			}
			if(bytes_rcvd == 0)
				break;
			if(bytes_rcvd < 0) {
				sendError(s_pvt);
				break;
			}
			if(strtok(msg_rcvd, END_MSG) == NULL) { //get string before '\r\n' (NULL if '\r\n' is not present)
				sendError(s_pvt);
				break;
			}
		
			/* Get filepath */
			strcpy(filepath, msg_rcvd);

			/* Check existence/permissions of file */
			if(access(filepath, F_OK|R_OK) != -1) {

				/* Detect file size (in bytes) */
				size = getFileSize(filepath);
				if(size > __UINT32_MAX__) { 
					sendError(s_pvt);
					break;
				} else if (size == -1) {
					sendError(s_pvt);
					break;
				} else
					fileSize = (uint32_t)htonl(size);
				
				/* Detect time of last update */
				timestamp = getLastUpdate(filepath);
				if(timestamp == -1) {
					sendError(s_pvt);
					break;
				} 
				lastUpdate = (uint32_t)htonl(timestamp);

				/* Read file content */
				FILE *fd;
				fd = fopen(filepath,"rb");  // in binary mode
				if(fd == NULL) {
					sendError(s_pvt);
					break;
				} 
				else {
					/* Get optimal block size */
					blockSize = (int)getBlockSize(filepath);
					if(blockSize == -1) {
						sendError(s_pvt);
						break;
					} 

					/* Send MSG_OK */
					bytes_snd = sendn(s_pvt, OK_MSG, 5, MSG_NOSIGNAL);
					if(bytes_snd == -1) 
						break;

					/* Send FILESIZE */
					bytes_snd = sendn(s_pvt, &fileSize, 4, MSG_NOSIGNAL);
					if(bytes_snd == -1) 
						break;	
						
					/* Send FILE */
					char buff[blockSize];
					int flag = 0; //alternative to "goto"
					while ((bytes_read = fread(buff, 1, sizeof(buff), fd)) > 0) {
						bytes_snd = sendn(s_pvt, buff, bytes_read, MSG_NOSIGNAL);
						if(bytes_snd == -1) {
							flag = 90;
							break;
						}
					}
					fclose(fd);
					if(flag == 90) {
						flag = 0;
						break;
					}

					/* Send TIMESTAMP */
					bytes_snd = sendn(s_pvt, &lastUpdate, 4, MSG_NOSIGNAL);	
					if(bytes_snd == -1) 
						break;	
				}
			} else {
				sendError(s_pvt);
				break;
			}
		}
		Close(s_pvt);
	}
	Close(s_pbc);
	Close(s_pvt);
	return 0;
}
