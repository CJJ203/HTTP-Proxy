#include <arpa/inet.h>
#include <errno.h>
#include <libgen.h>
#include <netdb.h>
#include <resolv.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include <sys/wait.h>
#include <netinet/in.h> 
#include <string.h>

#define BUF_SIZE 8192

#define DEFAULT_LOCAL_PORT    8080  
#define DEFAULT_REMOTE_PORT   8081 

#define READ  0
#define WRITE 1

#define MAX_HEADER_SIZE 8192

#define LOG(fmt...)  do {fprintf(stderr, ##fmt);} while(0)


char remote_host[128]; 
int remote_port; 
int local_port;

int server_sock; 
int client_sock;
int remote_sock;

char * header_buffer ;


// Proxy回环等待客户端的连接请求
void server_loop();

// Proxy处理客户端发起的连接请求
void handle_client(int client_sock, struct sockaddr_in client_addr);

// Proxy通过destination_sock转发HTTP头部
void forward_header(int destination_sock);

// Proxy从source_sock接收数据，并把接收到的数据转发到destination_sock上
void forward_data(int source_sock, int destination_sock);

// 重写HTTP头部
void rewrite_header();

// 创建proxy与服务器的连接
int create_connection();
int create_server_socket(int port);

// Read HTTP Header
int read_header(int fd, void * buffer);

// 读取套接字中的数据流
int readLine(int fd, void *buffer, int n);


int readLine(int fd, void *buffer, int n)
{
    int numRead;                    
    int totRead;                     
    char *buf;
    char ch;

    if (n <= 0 || buffer == NULL) {
        errno = EINVAL;
        return -1;
    }

    buf = buffer;                       

    totRead = 0;
    for (;;) {
        numRead = recv(fd, &ch, 1, 0);

        if (numRead == -1) {
            if (errno == EINTR)         
                continue;
            else
                return -1;              /* 未知错误 */

        } else if (numRead == 0) {      /* EOF */
            if (totRead == 0)           /* No bytes read; return 0 */
                return 0;
            else                        /* Some bytes read; add '\0' */
                break;

        } else {     
                                      
            if (totRead < n - 1) {      /* Discard > (n - 1) bytes */
                totRead++;
                *buf++ = ch;
            }

            if (ch == '\n')
                break;
        }
    }

    *buf = '\0';
    return totRead;
}


// Read HTTP Header
int read_header(int fd, void *buffer)
{
    memset(header_buffer,0,MAX_HEADER_SIZE);
    char line_buffer[2048];
    char * base_ptr = header_buffer;

    for(;;)
    {
        memset(line_buffer,0,2048);

        int total_read = readLine(fd, line_buffer, 2048);
        if(total_read <= 0)
        {
            return -1;
        }

        //防止header缓冲区越界
        if(base_ptr + total_read - header_buffer <= MAX_HEADER_SIZE)
        {
           strncpy(base_ptr,line_buffer,total_read); 
           base_ptr += total_read;
        } else 
        {
            return -1;
        }

        //读到了空行，http头结束
        if(strcmp(line_buffer,"\r\n") == 0 || strcmp(line_buffer,"\n") == 0)
        {
            break;
        }

    }
    return 0;

}


// 解析路径
void extract_server_path(const char *header,char *output)
{
    char *p = strstr(header,"GET /");
    if(p) {
        char *p1 = strchr(p+4,' ');
        strncpy(output,p+4,(int)(p1  - p - 4) );
    }
    
}


// 解析主机名称及端口号
int extract_host(const char *header)
{
    char *p = strstr(header,"Host:");
    if(!p) 
    {
        return -1;
    }
    char *p1 = strchr(p,'\n');
    if(!p1) 
    {
        return -1; 
    }

    char *p2 = strchr(p + 5, ':'); /* 5是指'Host:'的长度 */

    if(p2 && p2 < p1) 
    {
        
        int p_len = (int)(p1 - p2 -1);
        char s_port[p_len];
        strncpy(s_port, p2+1, p_len);
        s_port[p_len] = '\0';
        remote_port = atoi(s_port);

        int h_len = (int)(p2 - p -5 -1 );
        strncpy(remote_host, p + 5 + 1  ,h_len); //Host:
        remote_host[h_len] = '\0';
    } 
    else 
    {   
        int h_len = (int)(p1 - p - 5 -1 -1); 
        strncpy(remote_host, p + 5 + 1, h_len);
        remote_host[h_len] = '\0';
        remote_port = 80;
    }
    return 0;
}


// 处理客户端的连接 
void handle_client(int client_sock, struct sockaddr_in client_addr)
{
    char *client_ip;
    int client_port;

    client_ip = inet_ntoa(client_addr.sin_addr);
    client_port = client_addr.sin_port;

    LOG("Request arrived from client: [%s:%d]\n", client_ip, client_port);

    if(strlen(remote_host) == 0) // 未指定远端主机名称从 http 请求 HOST 字段中获取 
    {        
        if(read_header(client_sock, header_buffer) < 0)
        {
            LOG("Read Http header failed : Request from client: [%s:%d]\n", client_ip, client_port);
            return;
        } 
        else 
        {
            if(extract_host(header_buffer) < 0) 
            {
                LOG("Cannot extract host field : Request from client: [%s:%d]\n", client_ip, client_port);
                return;
            }
        }
    }

    if ((remote_sock = create_connection()) < 0) {
        LOG("Proxy cannot connect to host [%s:%d]\n",remote_host,remote_port);
        return;
    }

    LOG("Connected to remote host: [%s:%d]\n", remote_host, remote_port);

    if (fork() == 0) { // 创建子进程用于从客户端转发数据到远端socket接口

        if (strlen(header_buffer) > 0) 
        {
            forward_header(remote_sock); //转发HTTP Header
        } 
        
        LOG("Transfer data [%s:%d]-->[%s:%d]\n", client_ip, client_port, remote_host, remote_port);
        forward_data(client_sock, remote_sock);
        exit(0);
    }

    if (fork() == 0) { // 创建子进程用于转发从远端socket接口过来的数据到客户端

        LOG("Transfer data [%s:%d]-->[%s:%d]\n", remote_host, remote_port, client_ip, client_port);
        forward_data(remote_sock, client_sock);
        exit(0);
    }

    LOG("Close sock:  Proxy <===> [%s:%d] \n", remote_host, remote_port);
    close(remote_sock);

    LOG("Close sock:  Proxy <===> [%s:%d] \n", client_ip, client_port);
    close(client_sock);
}


// 转发HTTP Header
void forward_header(int destination_sock)
{
    rewrite_header();
   
    int len = strlen(header_buffer);
    send(destination_sock, header_buffer, len, 0) ;
}


// 代理中的完整URL转发前需改成path的形式 
void rewrite_header()
{
    char * p = strstr(header_buffer, "http://");
    char * p0 = strchr(p, '\0');
    char * p5 = strstr(header_buffer, "HTTP/"); // "HTTP/" 是协议标识 如 "HTTP/1.1" 
    int len = strlen(header_buffer);
    if(p)
    {
        char * p1 = strchr(p + 7,'/');
        if(p1 && (p5 > p1)) 
        {
            //转换url到 path
            memcpy(p,p1,(int)(p0 -p1));
            int l = len - (p1 - p) ;
            header_buffer[l] = '\0';

        } else 
        {
            char * p2 = strchr(p,' ');  //GET http://3g.sina.com.cn HTTP/1.1

            memcpy(p + 1, p2, (int)(p0-p2));
            *p = '/';  //url 没有路径使用根
            int l  = len - (p2  - p ) + 1;
            header_buffer[l] = '\0';

        }
    }
}


void forward_data(int source_sock, int destination_sock) {
    char buffer[BUF_SIZE];
    int n;

    while ((n = recv(source_sock, buffer, BUF_SIZE, 0)) > 0) 
    { 
        send(destination_sock, buffer, n, 0); 
    }
}



int create_connection() {
    struct sockaddr_in server_addr;
    struct hostent *server;
    int sock;

    if ((sock = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        return -1;
    }

    if ((server = gethostbyname(remote_host)) == NULL) {
        errno = EFAULT;
        return -1;
    }

    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    memcpy(&server_addr.sin_addr.s_addr, server->h_addr, server->h_length);
    server_addr.sin_port = htons(remote_port);

    LOG("Connect to remote host: [%s:%d]\n",remote_host,remote_port);
    if (connect(sock, (struct sockaddr *) &server_addr, sizeof(server_addr)) < 0) {
        return -1;
    }

    return sock;
}


int create_server_socket(int port) {
    int server_sock, optval;
    struct sockaddr_in server_addr;

    if ((server_sock = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        return -1;
    }

    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);
    server_addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(server_sock, (struct sockaddr*)&server_addr, sizeof(server_addr)) != 0) {
        return -1;
    }

    if (listen(server_sock, 20) < 0) {
        return -1;
    }

    return server_sock;
}


void server_loop() {
    struct sockaddr_in client_addr;
    socklen_t addrlen = sizeof(client_addr);

    while (1) {
        client_sock = accept(server_sock, (struct sockaddr*)&client_addr, &addrlen);

        char *client_ip;
        int client_port;

        client_ip = inet_ntoa(client_addr.sin_addr);
        client_port = client_addr.sin_port;

        LOG("Accepted connect from client: [%s:%d]\n", client_ip, client_port);
        
        if (fork() == 0) { // 创建子进程处理客户端连接请求
            close(server_sock);
            handle_client(client_sock, client_addr);
            exit(0);
        }
        close(client_sock);
    }

}


void start_server()
{
    //初始化全局变量
    header_buffer = (char *) malloc(MAX_HEADER_SIZE);

    if ((server_sock = create_server_socket(local_port)) < 0) 
    { 
        LOG("Cannot run server on port %d\n",local_port);
        exit(-1);
    }
   
    server_loop();
}


void usage(void)
{
    printf("Usage:\n");
    printf("\t-p <port number> : Specifyed local listen port \n");
    exit(0);
}


int main(int argc, char *argv[]) 
{
    local_port = DEFAULT_LOCAL_PORT;
	
	int opt;
	char optstrs[] = ":p";

	while((opt = getopt(argc, argv, optstrs)) != -1)
	{
		switch(opt)
		{
			case 'p':
				local_port = atoi(optarg);
				break;
			case ':':
				printf("\nMissing argument after: -%c\n", optopt);
				usage();
			case '?':
				printf("\nInvalid argument: %c\n", optopt);
			default:
				usage();
		}
    }

    LOG("Proxy Start on Port : %d\n", local_port);
    start_server();
    return 0;
}
