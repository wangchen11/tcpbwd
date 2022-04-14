#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <signal.h>
#include <unistd.h>
#include <pthread.h>
#include <netdb.h>
#include <errno.h>

/*
tcpbwd is TCP backward.
用于反向桥接TCP连接。类似于花生壳的内网端口映射功能。

例如你有一台有公网IP的服务器HOST_A，和一台没有公网IP但能访问公网的PC_A。
但是你希望通过公网访问PC_A。 这时TCP backward就可以发挥用处了。
step1: 在HOST_A上运行 tcpbwd server <CTRL_PORT>        <PUBLISH_PORT>
setp2: 在PC_A上运行   tcpbwd client <HOST_A:CTRL_PORT> <PC_A:PORT>
这样就可以通过直接访问HOST_A:PUBLISH_PORT来间接的访问PC_A:PORT了

by 望尘11 
*/

#define MAX_CONNECT     500 //最大连接数
#define PROTOCOL_SIZE   128  //协议头长度为固定128字节
#define IO_TIMEOUT      (30*1000)  //读写超时时间，单位ms
#define MTU             1500 // 每次读取最大长度

#define CONTROLLOR_HI     "hi#i am tcpbwd controllor client#"  // CLIENT TO HOST
#define CONNECTION_HI     "hi#i am tcpbwd connection client#"  // CLIENT TO HOST, after this string is connection id.
#define HI_RESPONSE       "hi#i am tcpbwd server#"             // HOST TO CLIENT,
#define CONNECTION_PLEASE "please#new connection#"             // HOST TO CLIENT, after this string is connection id.
#define OK_RESPONSE       "ok#got it#"                         // OK response 
#define NG_RESPONSE       "no#i can't#"                        // NG response 
#define TEST_ALIVE_RESPONSE       "test alive##"                       // NG response 

static volatile int running = 1;
static int sigint_count = 0;

void shutdownAllOpeningFd() {
    char path[255];
    for (int i = 3; i <= 0xffff; i++) {
        sprintf(path, "/proc/self/fd/%d", i);
        if (access(path, F_OK) == 0) {
            printf("emergency shutdown fd:%d\n", i);
            shutdown(i, SHUT_RDWR);
        }
    }
}

void checkFdBeforeExit() {
    char path[255];
    for (int i = 3; i <= 65535; i++) {
        sprintf(path, "/proc/self/fd/%d", i);
        if (access(path, F_OK) == 0) {
            printf("on exit fd %d still exist. give up!\n", i);
        }
    }
}

void signalHandler(int signo) {
    switch (signo) {
        case SIGPIPE: {
            printf("SIGPIPE received. ignore!\n");
            break;
        }
        case SIGINT: {
            if (sigint_count>=3) {
                printf("\nSIGINT received 3 times! exit now!!!\n");
                checkFdBeforeExit();
                exit(0);
                break;
            }
            if (sigint_count==0) {
                printf("\nSIGINT received! shuting down now.\n");
                shutdownAllOpeningFd();
            }
            running = 0;
            sigint_count++;
            break;
        }
    }
}

static const char*usage = 
        "tcpbwd server <CTRL_PORT>        <PUBLISH_PORT>\n"\
        "tcpbwd client <HOST_A:CTRL_PORT> <TARGET_IP:TARGET_PORT>\n"\
        ;

static int startWith(const char* longStr, const char* shortStr) {
    int cmpLen = strlen(shortStr);
    return 0==strncmp(longStr, shortStr, cmpLen);
}

struct socket_bridge_fds {
    int fda;
    int fdb;
};

static int response(int fd, const char* head, const char* msg) {
    char protocolSend[PROTOCOL_SIZE + 2];
    memset(protocolSend, 0, PROTOCOL_SIZE);
    // TODO check msg length in range
    sprintf(protocolSend, "%s%s", head, msg);
    printf("## response ##%s\n", protocolSend);
    if (write(fd, protocolSend, PROTOCOL_SIZE) != PROTOCOL_SIZE) {
        return -1;
    }
    return 0;
}

static int responseOk(int fd, const char* msg) {
    return response(fd, OK_RESPONSE, msg);
}

static int responseNg(int fd, const char* msg) {
    return response(fd, NG_RESPONSE, msg);
}

/**
 * 接受一个客户端的某种协议
 * @param protocolRecv 长度至少为PROTOCOL_SIZE + 1, 
 * @param recvProtocolHead 期望的协议头
 * @return 0成功，其他:失败。
 */
static int recvForProtocol(int fd, char protocolRecv[], const char* recvProtocolHead) {
    printf("## expected ####%s\n", recvProtocolHead);
    while (1) {
        int len = read(fd, protocolRecv, PROTOCOL_SIZE);
        if (len>=0) {
            protocolRecv[len] = 0;
        }
        if((len==PROTOCOL_SIZE) && startWith(protocolRecv, TEST_ALIVE_RESPONSE)) {
            continue;
        }
        printf("## received ##%d##%s\n", len, protocolRecv);
        if((len==PROTOCOL_SIZE) && startWith(protocolRecv, recvProtocolHead)) {
            return 0;
        } else {
            return -1;
        }
    }
    return -1;
}

static int recvOk(int fd) {
    char protocolRecv[PROTOCOL_SIZE + 2];
    memset(protocolRecv, 0, sizeof(protocolRecv));
    int ret = recvForProtocol(fd, protocolRecv, OK_RESPONSE);
    if (ret != 0) {
        printf("received not ok response:%s\n", protocolRecv);
    }
    return ret;
}

/**
 * 监听一个端口并返回socket fd, 返回值＞0时需要用shutdown(socketfd, SHUT_RDWR);关闭
 */
static int listenOnPort(int port){
    int socketfd;
    struct sockaddr_in serverAddr;
    socketfd = socket(AF_INET,SOCK_STREAM,0);//创建套接字 
    if(socketfd==-1){
        perror("create socket failed");
        goto failed;
    }

    bzero(&serverAddr,sizeof(serverAddr));//相当于memset
    serverAddr.sin_family = AF_INET;//ipv4
    serverAddr.sin_addr.s_addr = htonl(INADDR_ANY);//设定监听的地址为任何地址都监听
    serverAddr.sin_port = htons(port);//设置端口号
    //套接字与端口和地址绑定
    if(-1==bind(socketfd,(struct sockaddr*)&serverAddr,sizeof(struct sockaddr))){
        perror("bind port failed");
        goto failed;
    }
    
    // 避免杀掉进程后端口仍被占用 
    // unsigned char resueaddr = 1;
    // setsockopt(socketfd, SOL_SOCKET, SO_REUSEADDR, (void *)&resueaddr, 1);
    
    //创建监听
    if(-1==listen(socketfd,MAX_CONNECT)){
        perror("listen failed");
        goto failed;
    }
    
    return socketfd;
    
failed:
    if(socketfd>0) {
        shutdown(socketfd, SHUT_RDWR);
    }
    return-1;
}

static void setTimeout(int fd, int timeoutMs) {
    struct timeval timeoutVal;
    timeoutVal.tv_sec  = timeoutMs/1000;
    timeoutVal.tv_usec = (timeoutMs%1000) * 1000;
    if(setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, (void*)& timeoutVal, sizeof(timeoutVal))<0) {
        perror("setsockopt SO_SNDTIMEO");
    }
    if(setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, (void*)& timeoutVal, sizeof(timeoutVal))<0) {
        perror("setsockopt SO_RCVTIMEO");
    }
}

/**
 * 等待一个客户端的某种协议连接
 * @pragma protocolRecv 长度至少为PROTOCOL_SIZE + 1, 
 * @pragma recvProtocolHead 期望的协议头
 * 返回值若非-1需要用close关闭。
 */
static int acceptForProtocol(int ctrlFd, char protocolRecv[], const char* recvProtocolHead) {
    struct sockaddr_in clientAddr;
    socklen_t socketLen = 0;
    int clientFd = accept(ctrlFd,(struct sockaddr*)&clientAddr,&socketLen);
    if (clientFd < 0) {
        return -1;
    }

    if(recvForProtocol(clientFd, protocolRecv, recvProtocolHead)==0) {
        responseOk(clientFd, "access accept. welecom.");
        printf("access accept. welecom.\n");
        return clientFd;
    }

    responseNg(clientFd, "access denial");
    printf("access denial\n");
    close(clientFd);
    return -1;
}

/**
 * 等待一个客户端控制器连接
 * 返回值若非-1需要用close关闭。
 */
static int acceptForControllor(int ctrlFd) {
    char protocolRecv[PROTOCOL_SIZE + 2];
    return acceptForProtocol(ctrlFd, protocolRecv, CONTROLLOR_HI);
}

/**
 * 等待一个客户端回话连接
 * 返回值若非-1需要用close关闭。
 */
static int acceptForConnction(int toClientCtrlFd, int id) {
    char protocolRecv[PROTOCOL_SIZE + 2];

    int fd = acceptForProtocol(toClientCtrlFd, protocolRecv, CONNECTION_HI);
    if (fd >= 0) {
        int recvId = 0;
        if(1==sscanf(protocolRecv, CONNECTION_HI"%d", &recvId)) {
            if (id == recvId) {
                printf("accept connection id:%d fd:%d\n", id, fd);
                return fd;
            } else {
                fprintf(stderr, "accept request id:%d but recv id:%d", id, recvId);
            }
        } else {
            fprintf(stderr, "can not parse protocol:%s", protocolRecv);
        }
        printf("accept for connection. access denial\n");
        close(fd);
    }
    return -1;
}

static void* bridgeForwordThreadLoop(void* data) {
    struct socket_bridge_fds fds = *(struct socket_bridge_fds*)data;
    free(data);
    printf("bridgeForwordThreadLoop:%d -> %d start\n", fds.fda, fds.fdb);

    // setTimeout(fds.fda, IO_TIMEOUT);
    // setTimeout(fds.fdb, IO_TIMEOUT);

    char buffer[MTU + 1];
    while (running) {
        int readLen = read(fds.fda, buffer, MTU);
        if (readLen == 0) {
            printf("bridgeForwordThreadLoop:fd:%d was close by remote.\n", fds.fda);
            break;
        }
        if (readLen < 0) {
            perror("read error");
            printf("bridgeForwordThreadLoop:%d -> %d. read failed, readLen = %d\n", fds.fda, fds.fdb, readLen);
            break;
        }
        int writeLen = write(fds.fdb, buffer, readLen);
        if (writeLen == 0) {
            printf("bridgeForwordThreadLoop:fd:%d was close by remote.\n", fds.fdb);
            break;
        }
        if (readLen != writeLen) {
            printf("bridgeForwordThreadLoop:%d -> %d. write failed, readLen(%d) != writeLen(%d)\n", fds.fda, fds.fdb, readLen, writeLen);
            break;
        }
    }
    
    /**
     * fda will never use. close it.
     * fdb using in anouther thread. shutdown it. then anouther thread will read 0.
     */
     
    int ret = close(fds.fda);
    printf("bridgeForwordThreadLoop close return %d  fd:%d\n",ret , fds.fda);
    shutdown(fds.fdb, SHUT_RDWR);
    printf("bridgeForwordThreadLoop:%d -> %d end\n", fds.fda, fds.fdb);
    return NULL;
}

static int startBridgeThread(int fda, int fdb) {
    printf("start bridge %d and %d\n", fda, fdb);
    pthread_t forwardThread;
    pthread_t backwordThread;

    struct socket_bridge_fds* forwardFds  = (struct socket_bridge_fds*)malloc(sizeof(struct socket_bridge_fds));
    struct socket_bridge_fds* backwordFds = (struct socket_bridge_fds*)malloc(sizeof(struct socket_bridge_fds));

    if (forwardFds==NULL || backwordFds==NULL) {
        perror("new socket_bridge_fds failed: out of memory.");
        return -1;
    }
    // 正向读写代理
    forwardFds->fda = fda;
    forwardFds->fdb = fdb;

    // 反向读写代理
    backwordFds->fda = fdb;
    backwordFds->fdb = fda;
    

    int ret = 0;
    ret = pthread_create(&forwardThread, NULL, bridgeForwordThreadLoop,  forwardFds);
    if (ret != 0) {
        perror("create forward bridge thread failed");
        return -1;
    }
    
    ret = pthread_create(&backwordThread, NULL, bridgeForwordThreadLoop, backwordFds);
    if (ret != 0) {
        perror("create backword bridge thread failed");
        return -1;
    }
    return 0;
}

/**
 * @brief 代理一次
 * 
 * @param toClientCtrlFd 
 * @param publishFd 
 * @return int 大于等于0正常. 小于0则客户端可能已断开需要断开连接。
 */
static int proxyOnce(int ctrlFd,int toClientCtrlFd, int publishFd) {
    char protocol[PROTOCOL_SIZE + 2];
    static int id = 0;
    struct sockaddr_in clientAddr;
    socklen_t socketLen = 0;

    printf("proxyOnce %d\n", publishFd);
    int usefulFd = -1;
    {
        fd_set selectFdSet;
        struct timeval selectTimeOut;
        int ms = 4*1000;
        selectTimeOut.tv_sec  = ms/1000;
        selectTimeOut.tv_usec = ms%1000 * 1000;
        
        FD_ZERO(&selectFdSet);
        FD_SET(publishFd, &selectFdSet);
        int selectRet = select(publishFd + 1, &selectFdSet, NULL, NULL, &selectTimeOut);
        if (selectRet > 0) {
            usefulFd = accept(publishFd, (struct sockaddr*)&clientAddr, &socketLen);
            printf("proxyOnce accepted %d\n", usefulFd);
        } else if (selectRet == 0) {
            // printf("proxyOnce select time out\n");
            return 0;
        } else { // selectRet<0
            perror("proxyOnce select error");
            return -1;
        }
    }

    if (usefulFd < 0) {
        perror("proxy once accept failed");
        return -1;
    }

    id++;
    id = id % 0xffffff;
    sprintf(protocol, CONNECTION_PLEASE"%d", id);
    printf("## send ##%s\n", protocol);
    if (PROTOCOL_SIZE != write(toClientCtrlFd, protocol, PROTOCOL_SIZE)) {
        perror("proxy once write failed. connection was broken.");
        close(usefulFd);
        return -1;
    }

    printf("waiting for connection %d response ok.\n", id);
    if (recvOk(toClientCtrlFd)!=0) {
        fprintf(stderr, "proxy once can not recv ok.\n");
        close(usefulFd);
        return -1;
    }
    printf("connection %d connected.\n", id);

    int connectionFd = acceptForConnction(ctrlFd, id);
    if (connectionFd < 0) {
        fprintf(stderr, "accept for connection failed.\n");
        close(usefulFd);
        return -1;
    }
    
    if(startBridgeThread(usefulFd, connectionFd) != 0) {
        fprintf(stderr, "start bridge thread failed.\n");
        // usefulFd and connectionFd will close in startBridgeThread or bridge thread. do not close here.
        return -1;
    }
    printf("proxyOnce end\n");
    return 0;
}

static int proxyByClient(int ctrlFd, int toClientCtrlFd, int publishFd) {
    while (running) {
        int ret = proxyOnce(ctrlFd, toClientCtrlFd, publishFd);
        if(ret < 0) {
            return ret;
        }
        char buffer[PROTOCOL_SIZE + 2] = TEST_ALIVE_RESPONSE;
        setTimeout(toClientCtrlFd, 0);
        // test client alive.
        if (write(toClientCtrlFd, buffer, PROTOCOL_SIZE) < 0) {
            // connection is not alive.
            printf("proxyByClient toClientCtrlFd %d was closed\n", toClientCtrlFd);
            perror("proxyByClient recv error");
            return -1;
        }
    }
    return 0;
}

static int runServer(int ctrlPort, int publishPort) {
    if(ctrlPort == publishPort) {
        perror("ctrlPort can not equals with publishPort.\n");
        return -1;
    }
    int ret = 0;
    int ctrlFd    = -1;
    int publishFd = -1;

    ctrlFd = listenOnPort(ctrlPort);
    if (ctrlFd<0) {
        fprintf(stderr, "linsten on port %d failed.\n", ctrlPort);
        ret = -1;
        goto finally;
    }
    
    publishFd = listenOnPort(publishPort);
    if (publishFd<0) {
        fprintf(stderr, "linsten on port %d failed.\n", publishPort);
        ret = -1;
        goto finally;
    }
    
    printf("listen on ctrl port %d and publish port %d success. waiting for connection.\n", ctrlPort, publishPort);

    while (running) {
        int toClientCtrlFd = acceptForControllor(ctrlFd);
        if (toClientCtrlFd<0) {
            perror("accept for controllor failed\n");
            ret = -1;
            goto finally;
        }
        proxyByClient(ctrlFd, toClientCtrlFd, publishFd);
        if(toClientCtrlFd>=0) {
            close(toClientCtrlFd);
        }
        usleep(1);
    }
    
finally:
    if(ctrlFd>=0) {
        close(ctrlFd);
    }
    if(publishFd>=0) {
        close(publishFd);
    }
    return ret;
}

/////////////////////////////////////////////////////////////////////
// client before
////////////////////////////////////////////////////////////////////

/**
 * @brief 通过某个协议连接， 
 * 
 * @param sendProtocolHead 为空 或 至少有PROTOCOL_SIZE+1字节可读
 * @return int 返回值大于0 时需要用close关闭
 */
static int connectByProtocol(struct sockaddr_in addr, const char* sendProtocolHead) {
    int socketfd = socket(addr.sin_family, SOCK_STREAM, 0); //创建套接字 ,AF_NET:ipv4，SOCK_STREAM:TCP协议 

    if(socketfd == -1) {
        perror("create socket failed");
        goto failed;
    }
    
    if(-1 == connect(socketfd,(struct sockaddr*)&addr,sizeof(struct sockaddr))) {
        perror("connect server failed");
        goto failed;
    }
    
    if (sendProtocolHead==NULL) {
        // no protocol, return raw connection.
        return socketfd;
    }
    

    printf("## send ##%s\n", sendProtocolHead);
    if(write(socketfd, sendProtocolHead, PROTOCOL_SIZE) != PROTOCOL_SIZE) {
        perror("send protocol head failed");
        goto failed;
    }

    printf("connect by protocol waiting for recvOk\n");
    if(recvOk(socketfd) != 0) {
        fprintf(stderr, "'%s' was send,but can not recv ok response.\n", sendProtocolHead);
        goto failed;
    }

    printf("connected.\n");
    return socketfd;
failed:
    if (socketfd>0) {
        close(socketfd);
    }
    return -1;
}

/**
 * @brief 通过某个协议连接， 
 * @return int 返回值大于0 时需要用close关闭
 */
static int connectToControllor(struct sockaddr_in addr) {
    char protocolSend[PROTOCOL_SIZE + 2];
    memset(protocolSend, 0, sizeof(protocolSend));
    sprintf(protocolSend, CONTROLLOR_HI);
    return connectByProtocol(addr, protocolSend);
}

static int connectToConnection(struct sockaddr_in addr, int id) {
    char protocolSend[PROTOCOL_SIZE + 2];
    memset(protocolSend, 0, sizeof(protocolSend));
    sprintf(protocolSend, CONNECTION_HI"%d", id);
    return connectByProtocol(addr, protocolSend);
}

/**
 * @brief 接受一个新的连接请求
 * 
 * @param controllorFd 
 * @return int 请求id。 大于等于0时可连接，
 */
static int recvNewConnection(int controllorFd) {
    char protocolRecv[PROTOCOL_SIZE + 2];
    while (running) {
        int readLen = read(controllorFd, protocolRecv, PROTOCOL_SIZE);
        if (readLen==0) {
            printf("connect close by remote when read new connection.\n");
            return -1;
        }
        if (readLen<0) {
            perror("cannot read new connection");
            return -1;
        }
        protocolRecv[readLen] = 0;
        if (readLen==PROTOCOL_SIZE) {
            if (startWith(protocolRecv, TEST_ALIVE_RESPONSE)) {
                continue;
            }

            if (startWith(protocolRecv, CONNECTION_PLEASE)) {
                printf("new connection please:%s\n", protocolRecv);
                int id = 0;
                if (sscanf(protocolRecv, CONNECTION_PLEASE"%d", &id)==1) {
                    printf("got new connection:%d\n", id);
                    responseOk(controllorFd, "got new connection");
                    return id;
                }
            }
            responseNg(controllorFd, "protocol parse failed");
            fprintf(stderr, "new connection protocol parse failed:%s\n", protocolRecv);
        } else {
            responseNg(controllorFd, "no full package recived");
            fprintf(stderr, "no full package recived, readLen:%d\n", readLen);
        }
    }
    return -1;
}

static int connectToTarget(struct sockaddr_in addr) {
    return connectByProtocol(addr, NULL);
}

static int runClient(const char* ctrlHost, int ctrlPort, const char* targetHost, int targetPort) {
    struct sockaddr_in ctrlSockAddr;
    struct sockaddr_in targetSockAddr;
    int ret = 0;
    printf("run client %s:%d -> %s:%d \n", ctrlHost, ctrlPort, targetHost, targetPort);

    struct hostent* ctrlHostentPtr = gethostbyname(ctrlHost);
    if (ctrlHostentPtr == NULL) {
        fprintf(stderr, "try get host %s\n", ctrlHost);
        perror("get host by name failed");
        return -1;
    }
    memcpy(&ctrlSockAddr.sin_addr.s_addr, ctrlHostentPtr->h_addr, sizeof(ctrlSockAddr.sin_addr.s_addr));
    ctrlSockAddr.sin_family = ctrlHostentPtr->h_addrtype;
    ctrlSockAddr.sin_port   = htons(ctrlPort);


    struct hostent* targetHostentPtr = gethostbyname(targetHost);
    if (targetHostentPtr == NULL) {
        fprintf(stderr, "try get host %s\n", targetHost);
        perror("get host by name failed");
        return -1;
    }
    memcpy(&targetSockAddr.sin_addr.s_addr, targetHostentPtr->h_addr, sizeof(ctrlSockAddr.sin_addr.s_addr));
    targetSockAddr.sin_family = targetHostentPtr->h_addrtype;
    targetSockAddr.sin_port   = htons(targetPort);

    printf("connecting to controllor %s:%d\n", ctrlHost, ctrlPort);
    int controllorFd = connectToControllor(ctrlSockAddr);
    if (controllorFd<0) {
        fprintf(stderr, "connect to controllor failed: %s:%d\n", ctrlHost, ctrlPort);
        ret = -1;
        goto finally;
    }
    printf("connect to %s:%d success.\n", ctrlHost, ctrlPort);
    
    while (running) {
        int newId = recvNewConnection(controllorFd);
        if (newId<0) {
            fprintf(stderr, "recv new connection id failed.\n");
            ret = -1;
            goto finally;
        }

        int connectionFd = connectToConnection(ctrlSockAddr, newId);
        if (connectionFd<0) {
            fprintf(stderr, "connect to connection failed: %s:%d\n", ctrlHost, ctrlPort);
            ret = -1;
            goto finally;
        }
        

        int targetFd     = connectToTarget(targetSockAddr);
        if (targetFd<0) {
            fprintf(stderr, "connect to target failed: %s:%d\n", targetHost, targetPort);
            close(connectionFd);
            // try again later
            usleep(10000);
        }
        startBridgeThread(connectionFd, targetFd);
    }

finally:
    if (controllorFd>0) {
        close(controllorFd);
    }
    return ret;
}

static int runClientWithLoop(const char* ctrlHost, int ctrlPort, const char* targetHost, int targetPort) {
    while (running) {
        runClient(ctrlHost, ctrlPort, targetHost, targetPort);
        if (running) {
            sleep(10);
            printf("ctrl disconnected, reconnect after 10 sec\n");
        }
    }
    return 0;
}

int main(int argc,const char**argv) {
    if (argc >= 2) {
        if (strcmp(argv[1], "server")==0) {
            if (argc == 4) {
                int portA = 0;
                int portB = 0;
                if ((sscanf(argv[2], "%d", &portA)==1) && (sscanf(argv[3], "%d", &portB)==1)) {
                    signal(SIGPIPE, signalHandler);
                    signal(SIGINT,  signalHandler);
                    runServer(portA, portB);
                    if (!running) {
                        sleep(2);
                    }
                    return 0;
                }
            }
        } else if (strcmp(argv[1], "client")==0) {
            if (argc == 4) {
                char hostA[255];
                char hostB[255];
                int  portA = 0;
                int  portB = 0;
                if ((sscanf(argv[2], "%[^:]:%d", hostA, &portA)==2) && (sscanf(argv[3], "%[^:]:%d", hostB, &portB)==2)) {
                    signal(SIGINT,  signalHandler);
                    runClientWithLoop(hostA, portA, hostB, portB);
                    if (!running) {
                        sleep(2);
                    }
                    return 0;
                }
            }
        }
    }
    puts(usage);
    return -1;
}
