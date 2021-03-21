#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/socket.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <ctype.h>
#include <unistd.h>
#include <sys/stat.h> 
#include <pthread.h> 
extern "C"{
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <stdarg.h>
#include <inttypes.h>
#include "libswscale/swscale.h"

#include <sys/types.h>
#include <dirent.h>
}

using namespace std;

#define BUFSIZE 4097
//sprintf(buf, "Operation failed.\n");
//write(i, buf, strlen(buf));
/* maxfd may be a bug*/
#define ERR_EXIT(a) do { perror(a); exit(1); } while(0)
#define Buffer_size 1024

class Buffer{
public:
    AVPacket *pPackets[Buffer_size];
    unsigned int head;
    unsigned int tail;
    int isend;
    Buffer(){
        head = 0;
        tail = 0;
        isend = 0;
    }
    int isempty(){
        return (tail == head)? 1 : 0;
    }
    int isfull(){
        return (tail - head >= Buffer_size)? 1 : 0;
    }
    void push(AVPacket *pPacket){
        pPackets[tail % Buffer_size] = pPacket;
        tail++;
    }
    AVPacket *pop(){
        head++;
        return pPackets[(head - 1) % Buffer_size];
    }
};
typedef struct{
    char *filename;
    Buffer *ServerBuffer;
    int conn_fd;
} Arg;
typedef struct {
    char hostname[512];  // server's hostname
    unsigned short port;  // port to listen
    int listen_fd;  // fd to wait for a new connection
} server;

typedef struct {
    char host[512];  // client's host
    int conn_fd;  // fd to talk with client
    int cmd;
    long long int file_size;
    int fd;         //upload fd
    long long int current_offset;
    pthread_t pid;
    Arg *arg;
    int not_yet_send;
} request;

server svr;  // server
request* requestP = NULL;  // point to a list of requests
int maxfd;  // size of open file descriptor table, size of request list
int cnt_client_ID = 0;
const char* accept_read_header = "ACCEPT_FROM_READ";
const char* accept_write_header = "ACCEPT_FROM_WRITE";

static void init_server(unsigned short port);
// initailize a server, exit for error

static void init_request(request* reqP);
// initailize a request instance

static void free_request(request* reqP);
// free resources used by a request instance
static void *pushPacket(void *arg_void);
static void saveframe(unsigned char **data, int *wrap, int xsize, int ysize, char *filename);
// print out the steps and errors
static void logging(const char *fmt, ...);
static void avframeToCvmat(const AVFrame * frame);
void readInt(request *reqP, int *intP){
    read(reqP->conn_fd, intP, sizeof(int));
    return;
}

void readLongLongInt(request *reqP, long long int *longlongint){
    read(reqP->conn_fd, longlongint, sizeof(long long int));
    return;
}
void readString(request *reqP, char *strP, int len){
    char buf[512];
    read(reqP->conn_fd, buf, len);
    memcpy(strP, buf, len);
    strP[len] = '\0';
    return;
}
void readMode(request *reqP, mode_t *mode){
    read(reqP->conn_fd, mode, sizeof(mode_t));
    return;
}
void svrSendFile(request *reqP){
    char buf[BUFSIZE];
    if(reqP->current_offset + 4096 < reqP->file_size){
        read(reqP->fd, buf, 4096);
        write(reqP->conn_fd, buf, 4096);
        reqP->current_offset += 4096;
    }
    else{
        read(reqP->fd, buf, reqP->file_size - reqP->current_offset);
        write(reqP->conn_fd, buf, reqP->file_size - reqP->current_offset);
        reqP->cmd = -1;
        close(reqP->fd);
    }
}

void svrSendParameter(int conn_fd, AVCodecParameters *pCodecParameters){
    write(conn_fd, pCodecParameters, sizeof(AVCodecParameters));
    write(conn_fd, pCodecParameters->extradata, sizeof(uint8_t) * pCodecParameters->extradata_size);
}


void svrSendPacket(request *reqP){
    while(1){
        if(reqP->arg->ServerBuffer->isempty() == 1){           //race condition
            if(reqP->arg->ServerBuffer->isend == 1){
                if(reqP->not_yet_send == 1){
                    reqP->arg->ServerBuffer->isend = 0;
                    continue;
                }
                write(reqP->conn_fd, "0", 1);     //0 end 1 continue
                reqP->cmd = -1;
                delete reqP->arg->ServerBuffer;
                free(reqP->arg->filename);
                free(reqP->arg);
                pthread_join(reqP->pid, NULL);
                return;
            }
            else continue;
        }
        else{
            write(reqP->conn_fd, "1", 1);     //continue
            reqP->not_yet_send = 0;
            AVPacket *pPacket = reqP->arg->ServerBuffer->pop();
            write(reqP->conn_fd, pPacket, sizeof(AVPacket));
            write(reqP->conn_fd, pPacket->data, sizeof(uint8_t) * pPacket->size);
            write(reqP->conn_fd, pPacket->side_data, sizeof(AVPacketSideData) * pPacket->side_data_elems);
            for(int i = 0; i < pPacket->side_data_elems; ++i){
                write(reqP->conn_fd, (pPacket->side_data + i)->data, sizeof(uint8_t) * (pPacket->side_data + i)->size);
            }
            //av_packet_free(&pPacket);
            return;
        }
    }
}

void svrReceiveFile(request *reqP){
    char buf[BUFSIZE];
    if(reqP->current_offset + 4096 < reqP->file_size){
        read(reqP->conn_fd, buf, 4096);
        write(reqP->fd, buf, 4096);
        reqP->current_offset += 4096;
    }
    else{
        read(reqP->conn_fd, buf, reqP->file_size - reqP->current_offset);
        write(reqP->fd, buf, reqP->file_size - reqP->current_offset);
        reqP->cmd = -1;
        close(reqP->fd);
    }
}
int fileIsExist(char folder[1000][256], int file_num, char filename[512]){
    for(int i = 0; i < file_num; ++i){
        if(strcmp(folder[i], filename) == 0)
            return 1;
    }
    return 0;
}

void ls(char folder[1000][256], int *file_num){
    DIR *dir;
    struct dirent *ptr;
    int i;
    dir = opendir("./");
    *file_num = 0;
    while((ptr = readdir(dir)) != NULL){
        if(ptr->d_name[0] != '.'){
            memcpy(folder[*file_num], ptr->d_name, strlen(ptr->d_name));
            folder[*file_num][strlen(ptr->d_name)] = '\0';
            (*file_num)++;
        }
    }
    closedir(dir);
}
int main(int argc, char** argv) {
    //mkdir
    mkdir("./server_folder", S_IRWXU);
    chdir("./server_folder");
    // Parse args.
    if (argc != 2) {
        fprintf(stderr, "usage: %s [port]\n", argv[0]);
        exit(1);
    }
    /*cmd format
     *ls->[int cmd]
     *put->[int cmd][int filename_len][char *filename][mode_t file_mode][long long int file_size][char content]
     *get->[int cmd][int filename_len][char *filename]
     *play->[int cmd][int filename_len][char *filename]
     */
    int filename_len;        //-1:no cmd 0:ls 1:put 2:get 3:play         
    char filename[512], buf[BUFSIZE];


    char folder[1000][256]; //server's folder
    int file_num = 0;

    mode_t file_mode;

    struct sockaddr_in cliaddr;  // used by accept()
    int clilen;

    int conn_fd;  // fd for a new connection with client

    // Initialize server
    init_server((unsigned short) atoi(argv[1]));

    // Loop for handling connections
    fd_set read_set;
    struct timeval timeout;
    timeout.tv_sec = 3;
    timeout.tv_usec = 0;
    FD_ZERO(&read_set);
    FD_SET(svr.listen_fd, &read_set);
    while (1) {
        fd_set read_set_copy;
        read_set_copy = read_set;
        
        switch (select(maxfd + 5, &read_set_copy, NULL, NULL, &timeout)){
            case -1:
                fprintf(stderr, "select error\n");
                break;
            case 0:
                break;
            default:
                for(int i = 0; i < maxfd; ++i){
                    if(FD_ISSET(i, &read_set_copy)){
                        if(i == svr.listen_fd){
                             // Check new connection
                            clilen = sizeof(cliaddr);
                            conn_fd = accept(svr.listen_fd, (struct sockaddr*)&cliaddr, (socklen_t*)&clilen);
                            if (conn_fd < 0) {
                                if (errno == EINTR || errno == EAGAIN) continue;  // try again
                                if (errno == ENFILE) {
                                    (void) fprintf(stderr, "out of file descriptor table ... (maxconn %d)\n", maxfd);
                                    continue;
                                }
                                ERR_EXIT("accept");
                            } 
                            else{
                                requestP[conn_fd].conn_fd = conn_fd;
                                strcpy(requestP[conn_fd].host, inet_ntoa(cliaddr.sin_addr));
                                FD_SET(conn_fd, &read_set);
                                cnt_client_ID++;
                                write(conn_fd, &cnt_client_ID, sizeof(int));
                            }                  
                        }
                        else{
                            if(requestP[i].cmd == -1){
                                readInt(&requestP[i], &requestP[i].cmd);
                                if(requestP[i].cmd == 0){
                                    ls(folder, &file_num);
                                    write(i, &file_num, sizeof(int));
                                    for(int j = 0; j < file_num; ++j){
                                        write(i, folder[j], strlen(folder[j]));
                                        write(i, "\n", 1);
                                    }
                                    requestP[i].cmd = -1;
                                }
                                else if(requestP[i].cmd == 1){
                                    readInt(&requestP[i], &filename_len);
                                    readString(&requestP[i], filename, filename_len);
                                    ls(folder, &file_num);
                                    memcpy(folder[file_num], filename, filename_len);
                                    file_num++;
                                    readMode(&requestP[i], &file_mode);
                                    readLongLongInt(&requestP[i], &requestP[i].file_size);
                                    requestP[i].fd = open(filename, O_WRONLY | O_TRUNC | O_CREAT, file_mode);
                                    requestP[i].current_offset = 0;
                                    svrReceiveFile(&requestP[i]); 

                                }
                                else if(requestP[i].cmd == 2){
                                    readInt(&requestP[i], &filename_len);
                                    readString(&requestP[i], filename, filename_len);
                                    ls(folder, &file_num);
                                    if(fileIsExist(folder, file_num, filename) != 1){
                                        write(i, "0", 1);         //0 doesn't exist. 1 exist
                                        requestP[i].cmd = -1;
                                        continue;
                                    }
                                    write(i, "1", 1);
                                    requestP[i].fd = open(filename, O_RDONLY);
                                    struct stat fileInfo;
                                    fstat(requestP[i].fd, &fileInfo);
                                    write(i, &fileInfo.st_mode, sizeof(mode_t));
                                    requestP[i].file_size = fileInfo.st_size;
                                    requestP[i].current_offset = 0;
                                    write(i, &requestP[i].file_size, sizeof(long long int));
                                    svrSendFile(&requestP[i]);
                                }
                                else if(requestP[i].cmd == 3){
                                    readInt(&requestP[i], &filename_len);
                                    readString(&requestP[i], filename, filename_len);
                                    requestP[i].fd = open(filename, O_RDONLY);  //can modify if time is enough
                                    if(requestP[i].fd < 0){
                                        write(i, "0", 1);
                                        requestP[i].cmd = -1;
                                        continue;
                                    }
                                    close(requestP[i].fd);
                                    write(i, "1", 1);
                                    requestP[i].arg = (Arg *)malloc(sizeof(Arg));
                                    requestP[i].arg->ServerBuffer = new Buffer();
                                    requestP[i].arg->filename = (char *)malloc(sizeof(char) * 512);
                                    memcpy(requestP[i].arg->filename, filename, strlen(filename) + 1);
                                    requestP[i].arg->conn_fd = i;
                                    requestP[i].not_yet_send = 1;
                                    pthread_create(&requestP[i].pid, NULL, pushPacket, (void *)requestP[i].arg);
                                    svrSendPacket(&requestP[i]);
                                }
                                else if(requestP[i].cmd == 4){
                                    close(requestP[i].conn_fd);
                                    FD_CLR(requestP[i].conn_fd, &read_set);
                                    requestP[i].cmd = -1;
                                    exit(0);
                                }
                            }
                            else{
                                if(requestP[i].cmd == 1){
                                    svrReceiveFile(&requestP[i]);
                                }
                                else if(requestP[i].cmd == 2){
                                    read(requestP[i].conn_fd, buf, 1);
                                    svrSendFile(&requestP[i]);
                                }
                                else if(requestP[i].cmd == 3){
                                    read(requestP[i].conn_fd, buf, 1);
                                    svrSendPacket(&requestP[i]);
                                    /*
                                    wait child    
                                    */
                                }
                            }
                        }
                    }
                }
                break;
        }
    }
    free(requestP);
    return 0;
}

#include <fcntl.h>

static void init_request(request* reqP) {
    reqP->conn_fd = -1;
    reqP->cmd = -1;
}

static void free_request(request* reqP) {
    /*if (reqP->filename != NULL) {
        free(reqP->filename);
        reqP->filename = NULL;
    }*/
    init_request(reqP);
}

static void init_server(unsigned short port) {
    struct sockaddr_in servaddr;
    int tmp;

    gethostname(svr.hostname, sizeof(svr.hostname));
    svr.port = port;

    svr.listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (svr.listen_fd < 0) ERR_EXIT("socket");

    bzero(&servaddr, sizeof(servaddr));
    servaddr.sin_family = AF_INET;
    servaddr.sin_addr.s_addr = INADDR_ANY;
    servaddr.sin_port = htons(port);
    tmp = 1;
    if (setsockopt(svr.listen_fd, SOL_SOCKET, SO_REUSEADDR, (void*)&tmp, sizeof(tmp)) < 0) {
        ERR_EXIT("setsockopt");
    }
    if (bind(svr.listen_fd, (struct sockaddr*)&servaddr, sizeof(servaddr)) < 0) {
        ERR_EXIT("bind");
    }
    if (listen(svr.listen_fd, 1024) < 0) {
        ERR_EXIT("listen");
    }

    // Get file descripter table size and initize request table
    maxfd = getdtablesize();
    requestP = (request*) malloc(sizeof(request) * maxfd);
    if (requestP == NULL) {
        ERR_EXIT("out of memory allocating all requests");
    }
    for (int i = 0; i < maxfd; i++) {
        init_request(&requestP[i]);
    }
    requestP[svr.listen_fd].conn_fd = svr.listen_fd;
    strcpy(requestP[svr.listen_fd].host, svr.hostname);

    return;
}

//===========================================================================================================


static void *pushPacket(void *arg_void){
    Arg *arg = (Arg *)arg_void;
    logging("initializing all the containers, codecs and protocols.");
    av_register_all();

    // Preparing AVFormatContext
    // AVFormatContext holds the header information from video
    AVFormatContext *pFormatContext = avformat_alloc_context();
    if (!pFormatContext){
        logging("ERROR could not allocate memory for Format Context");
        //return -1;
    }
    logging("opening the input file (%s) and loading format (container) header", arg->filename);

    // Open the file and read its header
    if (avformat_open_input(&pFormatContext, arg->filename, NULL, NULL) != 0){
        logging("ERROR could not open the file");
        //return -1;
    }

    // Check stream info is properly
    if (avformat_find_stream_info(pFormatContext, NULL) < 0){
        logging("ERROR could not get the stream info");
        //return -1;
    }

    // It's the codec, the component that knows how to encode and decode the stream
    AVCodec *pCodec = NULL;
    AVCodecParameters *pCodecParameters = NULL;

    // Loop though all the streams and print its main information
    // We want to fine video stream Codec and Parameters!!
    for (int i = 0; i < pFormatContext->nb_streams; i++){
        AVCodecParameters *pLocalCodecParameters = NULL;

        // Find Parameters
        pLocalCodecParameters = pFormatContext->streams[i]->codecpar;

        // Finds Codec from a codec ID
        logging("finding the proper decoder (CODEC)");
        AVCodec *pLocalCodec = NULL;
        pLocalCodec = avcodec_find_decoder(pLocalCodecParameters->codec_id);
        if (pLocalCodec == NULL){
            logging("ERROR unsupported codec!");
            arg->ServerBuffer->isend = 1;
            return NULL;
        }

        // When the stream is a audio
        if (pLocalCodecParameters->codec_type == AVMEDIA_TYPE_AUDIO){
        // You may not care about audio stream
        }
        // When the stream is a video
        else if (pLocalCodecParameters->codec_type == AVMEDIA_TYPE_VIDEO){
            // We fine video stream Codec and Parameters!!
            printf("Send codec_id : %d\n", pLocalCodecParameters->codec_id);
            write(arg->conn_fd, &pLocalCodecParameters->codec_id, sizeof(AVCodecID));
            pCodec = pLocalCodec;
            pCodecParameters = pLocalCodecParameters;
            //svrSendParameter(arg->conn_fd, pCodecParameters);
            write(arg->conn_fd, pCodecParameters, sizeof(AVCodecParameters));
            write(arg->conn_fd, pCodecParameters->extradata, sizeof(uint8_t) * pCodecParameters->extradata_size);
            logging("Video Codec: resolution %d x %d", pLocalCodecParameters->width, pLocalCodecParameters->height);
        }
        // Print its name, id and bitrate
        logging("\tCodec %s ID %d bit_rate %lld", pLocalCodec->name, pLocalCodec->id, pLocalCodecParameters->bit_rate);
    }

    // In Server, prepare AVPacket (THE SECOND COMPONENT you should use Socket to transmit)
    AVPacket *pPacket = av_packet_alloc();
    if (!pPacket){
        logging("failed to allocated memory for AVPacket");
        //return -1;
    }

    
    int response = 0;
    
    // Fill the AVPacket (pPacket) with data from the Stream
    while (av_read_frame(pFormatContext, pPacket) >= 0){
        while(1){
            if(arg->ServerBuffer->isfull() == 1) continue;
            AVPacket *copy = (AVPacket *)malloc(sizeof(AVPacket));
            memcpy(copy, pPacket, sizeof(AVPacket));
            arg->ServerBuffer->push(copy);
            break;
        }
        //av_packet_unref(pPacket); 
    }

    logging("releasing all the resources");
    //set isend = 1 to tell parent should wait child and send zero message to client
    arg->ServerBuffer->isend = 1;

    //avformat_close_input(&pFormatContext);
    //av_packet_free(&pPacket);
    return 0;
}

static void logging(const char *fmt, ...){
    va_list args;
    fprintf(stderr, "LOG: ");
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    fprintf(stderr, "\n");
}
































