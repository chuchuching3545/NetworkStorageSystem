#include <iostream>
#include <sys/socket.h> 
#include <arpa/inet.h>
#include <sys/ioctl.h>
#include <net/if.h>
#include <unistd.h> 
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h> 
#include <fcntl.h>
extern "C"{
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <stdarg.h>
#include <inttypes.h>
#include "libswscale/swscale.h"
#include <pthread.h> 
}
#include "opencv2/opencv.hpp"
#define BUFF_SIZE 4096
#define Buffer_size 1024
using namespace cv;
using namespace std;
static char avframeToCvmat(AVFrame * frame);
static void logging(const char *fmt, ...);
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

void clientSendFile(int fd, int conn_fd){
    char buf[BUFF_SIZE];
    long long int n;
    while((n = read(fd, buf, sizeof(buf))) != 0){
        write(conn_fd, buf, n);
    }
}

void clientReceiveFile(int fd, int conn_fd, long long int file_size){
    char buf[BUFF_SIZE];
    long long int n, size = 0;

    while((n = read(conn_fd, buf, sizeof(buf))) != 0){
        write(fd, buf, n);
        size += n;
        if(size == file_size) break;
        else write(conn_fd, "1", 1);         //request 1
    }
}
typedef struct{
    int conn_fd;
    Buffer *ClientBuffer;
} Arg;


void clientReceiveParameter(int conn_fd, AVCodecParameters *pCodecParameters){
    read(conn_fd, pCodecParameters, sizeof(AVCodecParameters));
    pCodecParameters->extradata = (uint8_t *)malloc(sizeof(uint8_t) * pCodecParameters->extradata_size);
    read(conn_fd, pCodecParameters->extradata, sizeof(uint8_t) * pCodecParameters->extradata_size);
}

void *clientReceivePacket(void *arg_void){
    Arg *arg = (Arg *)arg_void;
    AVPacket *pPacket;
    char buf[5];
    while(read(arg->conn_fd, buf, 1)){
        if(buf[0] == '1'){
            pPacket = (AVPacket *)malloc(sizeof(AVPacket));
            read(arg->conn_fd, pPacket, sizeof(AVPacket));
            pPacket->data = (uint8_t *)malloc(sizeof(uint8_t) * pPacket->size);
            int size = read(arg->conn_fd, pPacket->data, sizeof(uint8_t) * pPacket->size);     //Big bug
            while(size < pPacket->size){
                size += read(arg->conn_fd, pPacket->data + size, sizeof(uint8_t) * (pPacket->size - size));
            }
            pPacket->side_data = (AVPacketSideData *)malloc(sizeof(AVPacketSideData) * pPacket->side_data_elems);
            read(arg->conn_fd, pPacket->side_data, sizeof(AVPacketSideData) * pPacket->side_data_elems);
            for(int i = 0; i < pPacket->side_data_elems; ++i){
                (pPacket->side_data + i)->data = (uint8_t *)malloc(sizeof(uint8_t) * (pPacket->side_data + i)->size);
                read(arg->conn_fd, (pPacket->side_data + i)->data, sizeof(uint8_t) * (pPacket->side_data + i)->size);
            }
            pPacket->buf = NULL;
            while(1){
                if(arg->ClientBuffer->isfull() == 1) continue;
                arg->ClientBuffer->push(pPacket);
                break;   
            }
            write(arg->conn_fd, "1", 1);
        }
        else{
            arg->ClientBuffer->isend = 1;
            break;
        }
    }
}
AVCodecContext *clientReceiveCodec(int conn_fd){
    avcodec_register_all();
    AVCodecID codec_id;
    read(conn_fd, &codec_id, sizeof(AVCodecID));
    AVCodec *pCodec = avcodec_find_decoder(codec_id);
    AVCodecParameters *pCodecParameters = (AVCodecParameters *)malloc(sizeof(AVCodecParameters));
    clientReceiveParameter(conn_fd, pCodecParameters);
    AVCodecContext *pCodecContext = avcodec_alloc_context3(pCodec);
    if(!pCodecContext){
        logging("failed to allocated memory for AVCodecContext");
        //return -1;
    }
    if(avcodec_parameters_to_context(pCodecContext, pCodecParameters) < 0){
        logging("failed to copy codec params to codec context");
        //return -1;
    }
    if(avcodec_open2(pCodecContext, pCodec, NULL) < 0){
        logging("failed to open codec through avcodec_open2");
        //return -1;
    }
    free(pCodecParameters->extradata);
    free(pCodecParameters);
    return pCodecContext;
}


static int decode_packet(AVPacket *pPacket, AVCodecContext *pCodecContext, AVFrame *pFrame){
    // Supply raw packet data as input to a decoder
    int response = avcodec_send_packet(pCodecContext, pPacket);

    if(response < 0){
        //logging("Error while sending a packet to the decoder: %s", av_err2str(response));
        return response;
    }

    while(response >= 0){
        // Return decoded output data (into a frame) from a decoder
        response = avcodec_receive_frame(pCodecContext, pFrame);
        if(response == AVERROR(EAGAIN) || response == AVERROR_EOF)
            break;
        else if(response < 0){
        //logging("Error while receiving a frame from the decoder: %s", av_err2str(response));
        return response;
        }

        if(response >= 0){
            /*logging(
                "Frame %d (type=%c, size=%d bytes) pts %d key_frame %d [DTS %d]",
                pCodecContext->frame_number,
                av_get_picture_type_char(pFrame->pict_type),
                pFrame->pkt_size,
                pFrame->pts,
                pFrame->key_frame,
                pFrame->coded_picture_number);*/
            if(avframeToCvmat(pFrame) == 27) return -1;  
        }
    }
    return 0;
}
void FreePacket(AVPacket *pPacket){
    for(int i = 0; i < pPacket->side_data_elems; ++i){
        free((pPacket->side_data + i)->data);
    }
    free(pPacket->side_data);
    free(pPacket->data);
    free(pPacket);
}
int main(int argc , char *argv[]){
    char str_addr[10], str_port[10];
    int index = 0, index2 = 0;
    while(argv[1][index] != ':'){
        str_addr[index] = argv[1][index];
        index++; 
    }
    str_addr[index] = '\0';
    index++;
    while(argv[1][index] != '\0'){
        str_port[index2] = argv[1][index];
        index2++;
        index++;
    }
    str_port[index2] = '\0';

    int localSocket;
    localSocket = socket(AF_INET , SOCK_STREAM , 0);

    
    int conn_fd, recved;
    conn_fd = socket(AF_INET , SOCK_STREAM , 0);

    if (conn_fd == -1){
        printf("Fail to create a socket.\n");
        return 0;
    }

    struct sockaddr_in info;
    bzero(&info,sizeof(info));

    info.sin_family = PF_INET;
    info.sin_addr.s_addr = inet_addr(str_addr);
    unsigned short port = (unsigned short) atoi(str_port);
    info.sin_port = htons(port);


    int err = connect(conn_fd,(struct sockaddr *)&info,sizeof(info));
    if(err==-1){
        printf("Connection error\n");
        return 0;
    }

    FILE *conn_fp = fdopen(conn_fd, "r+");
    char buf[BUFF_SIZE], dir_name[50];
    char command[256];
    int file_num, cmd, fd, filename_len, client_ID;
    long long int file_size;
    mode_t file_mode;
    read(conn_fd, &client_ID, sizeof(int));
    printf("ClientID is %d\n", client_ID);  

    //mkdir
    sprintf(dir_name, "./client_%d", client_ID);
    mkdir(dir_name, S_IRWXU);
    chdir(dir_name); 
                                   
    while(fgets(command, 256, stdin) != NULL){
        if(strncmp(command, "ls", 2) == 0){
            if(command[2] == '\n'){
                cmd = 0;
                write(conn_fd, &cmd, sizeof(int));
                read(conn_fd, &file_num, sizeof(int));
                for(int i = 0; i < file_num; ++i){
                    fgets(buf, BUFF_SIZE, conn_fp);
                    printf("%s", buf);
                }
            }
            else if(command[2] == ' ')
                printf("Command format error.\n");
            else
                printf("Command not found.\n");
        }
        else if(strncmp(command, "put ", 4) == 0){
            if(strchr(command + 4, ' ') != NULL)
                printf("Command format error.\n");
            else{
                command[strlen(command) - 1] = '\0';
                fd = open(command + 4, O_RDONLY);
                if(fd < 0){
                    printf("The %s doesn’t exist.\n", command + 4);
                    continue;
                }
                cmd = 1;
                write(conn_fd, &cmd, sizeof(int));
                filename_len = strlen(command + 4);
                write(conn_fd, &filename_len, sizeof(int));
                write(conn_fd, command + 4, filename_len);
                struct stat fileInfo;
                fstat(fd, &fileInfo);
                file_size = fileInfo.st_size;
                write(conn_fd, &fileInfo.st_mode, sizeof(mode_t));
                write(conn_fd, &file_size, sizeof(long long int));
                clientSendFile(fd, conn_fd);
                close(fd);
            }
        }
        else if(strncmp(command, "get ", 4) == 0){
            if(strchr(command + 4, ' ') != NULL){
                printf("Command format error.\n");
            }
            else{
                command[strlen(command) - 1] = '\0';
                cmd = 2;
                write(conn_fd, &cmd, sizeof(int));
                filename_len = strlen(command + 4);
                write(conn_fd, &filename_len, sizeof(int));
                write(conn_fd, command + 4, filename_len);
                read(conn_fd, buf, 1);
                if(buf[0] == '0'){
                    printf("The %s doesn’t exist.\n", command + 4);
                    continue;
                }
                read(conn_fd, &file_mode, sizeof(mode_t));
                fd = open(command + 4, O_WRONLY | O_CREAT | O_TRUNC, file_mode);
                read(conn_fd, &file_size, sizeof(long long int));
                clientReceiveFile(fd, conn_fd, file_size);
                close(fd);
            }
        }
        else if(strncmp(command, "play ", 5) == 0){
            if(strchr(command + 5, ' ') != NULL){
                printf("Command format error.\n");
            }
            else{
                command[strlen(command) - 1] = '\0';
                filename_len = strlen(command + 5);
                if(filename_len >= 4 && strncmp(command + strlen(command) - 4, ".mpg", 4) == 0){
                    cmd = 3;
                    write(conn_fd, &cmd, sizeof(int));
                    write(conn_fd, &filename_len, sizeof(int));
                    write(conn_fd, command + 5, filename_len);
                    read(conn_fd, buf, 1);
                    if(buf[0] == '0'){
                        printf("The %s doesn’t exist.\n", command + 5);
                        continue;
                    }
                    Arg *arg = (Arg *)malloc(sizeof(Arg));
                    arg->conn_fd = conn_fd;
                    arg->ClientBuffer = new Buffer();
                    AVCodecContext *pCodecContext = clientReceiveCodec(conn_fd);
                    pthread_t pid;
                    pthread_create(&pid, NULL, clientReceivePacket,(void*)arg);
                    AVFrame *pFrame = av_frame_alloc();
                    AVPacket *pPacket;
                    while(1){
                        if(arg->ClientBuffer->isempty() == 1){
                            if(arg->ClientBuffer->isend == 1){
                                destroyAllWindows();
                                break;
                            }
                            else continue;
                        }
                        else{
                            pPacket = arg->ClientBuffer->pop();
                            if(decode_packet(pPacket, pCodecContext, pFrame) == -1) break;
                            FreePacket(pPacket);
                        }
                    }
                    pthread_join(pid, NULL);
                    delete arg->ClientBuffer;
                    free(arg);
                    //av_frame_free(&pFrame);
                    //avcodec_free_context(&pCodecContext);
                }
                else{
                    printf("The %s is not a mpg file.\n", command + 5);
                    continue;
                }
            }
        }
        else if(strncmp(command, "end", 3) == 0){
            cmd = 4;
            write(conn_fd, &cmd, sizeof(int));
        }
        else{
            printf("Command not found.\n");
        }
    }
    

    close(localSocket);
    return 0;
}
static char avframeToCvmat(AVFrame * frame)  {  
    int width = frame->width;  
    int height = frame->height;  
    cv::Mat image(height, width, CV_8UC3);  
    int cvLinesizes[1];  
    cvLinesizes[0] = image.step1();  
    SwsContext* conversion = sws_getContext(width, height, (AVPixelFormat) frame->format, width, height, AV_PIX_FMT_BGR24, SWS_FAST_BILINEAR, NULL, NULL, NULL);  
    sws_scale(conversion, frame->data, frame->linesize, 0, height, &image.data, cvLinesizes);  
    sws_freeContext(conversion);  
    imshow("Vedio", image);
    char c = (char)waitKey(33.3333);
    return c;
    //free(frame);
}  
static void logging(const char *fmt, ...){
    va_list args;
    fprintf(stderr, "LOG: ");
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    fprintf(stderr, "\n");
}