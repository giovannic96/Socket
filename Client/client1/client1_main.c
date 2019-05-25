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

#define BUF_SIZE 255 //NAME_MAX 
#define TIMEOUT_SEC 15	

#define OK_MSG "+OK\r\n"
#define ERR_MSG "-ERR"
#define GET_MSG "GET "
#define END_MSG "\r\n"
#define MAX_READLINE 6
#define MIN_ARGS 4

const char* prog_name = "MyClientTCP";

__blksize_t getBlockSize(const char *filename) {
    struct stat st;      
    if(stat(filename, &st) == 0)
        return st.st_blksize;
    else
        return -1;
}

void initServer(struct sockaddr_in *saddr, uint16_t port, const char *ipAddr) {
    saddr->sin_family = AF_INET;
    saddr->sin_port = port;
    Inet_aton(ipAddr, &saddr->sin_addr);
}

int main(int argc, char* argv[]) {

	if(argc < MIN_ARGS)
		err_quit("Usage: %s <address> <port> <filename1> <filename2> ...\n", prog_name);

	struct sockaddr_in saddr; 
	uint16_t tport_n = htons(atoi(argv[2])); /*NB: in network-byte-order*/
	int s;
	ssize_t bytes_rcvd, bytes_snd;
	char msg_rcvd[BUF_SIZE];
	uint32_t fileSize, lastUpdate;
	FILE *file_rcvd;
	size_t nleft;

	initServer(&saddr, tport_n, argv[1]);

	s = Socket(PF_INET, SOCK_STREAM, IPPROTO_TCP);

	if (s < 0)
		err_quit("Creazione socket fallita\n");
	
	Connect(s, (struct sockaddr*)&saddr, sizeof(saddr));
	
	for(int i = MIN_ARGS-1; i < argc; i++) { 
		
		/* SEND */

		/* Send GET message */
		bytes_snd = sendn(s, GET_MSG, 4, MSG_NOSIGNAL);
		if(bytes_snd == -1) 
			break;

		/* Send FILENAME message */
		if(strstr(argv[i], END_MSG) != NULL) //because last argv will contain also \r\n --> remove them!
			argv[i] = strtok(argv[i], END_MSG);
		char str[strlen(argv[i])+2]; //because it will contain also \r\n
		strcpy(str, argv[i]);
		strcat(str, END_MSG);
		bytes_snd = sendn(s, str, strlen(str), MSG_NOSIGNAL);
		if(bytes_snd == -1) 
			break;

		/* RECEIVE */

		/* Receive OK message */
		memset(msg_rcvd, 0, BUF_SIZE);
		bytes_rcvd = protocol_readline_unbuffered(s, msg_rcvd, MAX_READLINE, TIMEOUT_SEC);
		if(bytes_rcvd == -2) {
			err_msg("\nNessuna risposta dal server dopo %d secondi\n", TIMEOUT_SEC);
			break;
		}
		if (bytes_rcvd == 0) 
			break;
		if(bytes_rcvd == MAX_READLINE && strcmp(strtok(msg_rcvd, END_MSG), ERR_MSG) == 0) {
			err_msg("Ricevuto -ERR");
			break;
		}
		if(bytes_rcvd != 5 || strcmp(msg_rcvd, OK_MSG) != 0 || bytes_rcvd < 0) {
			err_msg("Ricevuto un messaggio non corretto", msg_rcvd);
			break;
		} 

		/* Receive FILE SIZE */
		bytes_rcvd = protocol_readn(s, &fileSize, 4, TIMEOUT_SEC);
		if(bytes_rcvd == -2) {
			err_msg("\nNessuna risposta dal server dopo %d secondi\n", TIMEOUT_SEC);
			break;
		}
		if(bytes_rcvd == 0) 
			break;
		if(bytes_rcvd == MAX_READLINE && strcmp(strtok(msg_rcvd, END_MSG), ERR_MSG) ==0) {
			err_msg("Ricevuto -ERR");
			break;
		}
		if (bytes_rcvd < 0) {
			err_msg("Ricevuto un messaggio non corretto", fileSize);
			break;
		}		
		if(bytes_rcvd != 4) 
			break;	
		fileSize = ntohl(fileSize);

		/* Receive FILE CONTENT */
		file_rcvd = fopen(argv[i], "w");

		if (file_rcvd == NULL) {
			err_msg("Impossibile aprire il file\n");
			break;
		}

		nleft = fileSize;	
		size_t amountToRead;
		size_t written;
		int blockSize = (int)getBlockSize(argv[i]);
		if(blockSize == -1)
			break;
		char buf[blockSize]; 
		int flag = 0;

		/* Read and write bytes */
		while (nleft > 0) {

			if(nleft >= sizeof(buf))
				amountToRead = sizeof(buf);
			else amountToRead = nleft;

			bytes_rcvd = protocol_readn(s, buf, amountToRead, TIMEOUT_SEC);
			if(bytes_rcvd == -2) {
				err_msg("\nNessuna risposta dal server dopo %d secondi\n", TIMEOUT_SEC);
				flag = 90;
				break; 
			}
			if(bytes_rcvd == 0) 
				break;
			if(bytes_rcvd < 0) {
				err_msg("Errore nella ricezione del file");
				break;
			}
			nleft -= bytes_rcvd;
			written = fwrite(buf, 1, bytes_rcvd, file_rcvd);
			if(written != bytes_rcvd) {
				flag = 90;
				break;
			}
		}
		fclose(file_rcvd);
		if(flag == 90) {
			flag = 0;
			break;
		}

		/* Receive TIMESTAMP OF LAST UPDATE */
		bytes_rcvd = protocol_readn(s, &lastUpdate, 4, TIMEOUT_SEC);
		if(bytes_rcvd == -2) {
			err_msg("\nNessuna risposta dal server dopo %d secondi\n", TIMEOUT_SEC);
			break;
		}
		if(bytes_rcvd == 0) 
			break;
		if(bytes_rcvd < 0) {
			err_msg("Ricevuto un messaggio non corretto", lastUpdate);
			break;
		}			
		if(bytes_rcvd != 4) 
			break;
		
		lastUpdate = ntohl(lastUpdate);
		printf("Received file %s\n", argv[i]);
		printf("Received file size %u\n", fileSize);
		printf("Received file timestamp %d\n", lastUpdate);
	}
	Close(s);
}