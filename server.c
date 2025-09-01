#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <stdlib.h>
#include <unistd.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <sys/stat.h>
#include <time.h>

#define MAXLINE 200			//每个消息的最大长度
#define MAXNAME 20			//每个用户的最大用户名长度
#define SERV_PORT 8000		//服务器端口号
#define MAXCON (100 + 1) 	//最大连接数为100，最后一个用于临时连接
#define MAXFILE 10240		//最大文件缓存大小
#define FINISHFLAG "|_|_|"	//文件上传完成表示

#define HEARTBEAT_INTERVAL 30 // 心跳间隔时间，单位为秒
#define HEARTBEAT_TIMEOUT 60 // 心跳超时时间，单位为秒

struct sockaddr_in servaddr,chiladdr[MAXCON];
socklen_t cliaddr_len;
int listenfd,connfd[MAXCON],n;

char buf[MAXCON][MAXLINE+50];    //每个客户端的消息缓存区，用于接收并存储来自客户端的消息。
char spemsg[MAXCON][MAXLINE+50]; //发送给特定用户的消息缓存区。
char filebuf[MAXCON][MAXFILE+50];//用于存储文件内容的缓存区。
	
char str[INET_ADDRSTRLEN];			//存储客户端的IP地址字符串。
char names[MAXCON][MAXNAME];		//存储每个连接用户的名称。

int used[MAXCON];                //记录每个连接用户端是否使用。0代表未使用，1代表正在使用
int downloading[MAXCON];         //记录每个用户是否正在下载文件。0代表未使用，1代表正在使用

time_t last_heartbeat[MAXCON]; // 记录每个连接的最后心跳时间


// 函数声明提前
void *TRD(void *arg);	//处理客户端的消息
int Process(int ID);	//对接收的信息进行处理

void sendonemsg(int sockfd,char* msg);
void sendmsgtoall(int ID);

void *heartbeat_thread(void* arg);	// 心跳线程函数，用于检测客户端连接状态
void cleanup_client(int ID);		// 清理客户端资源


int main(){
	//服务器初始化
	listenfd=socket(AF_INET,SOCK_STREAM,0);
	
	bzero(&servaddr,sizeof(servaddr));
	servaddr.sin_family=AF_INET;
	servaddr.sin_addr.s_addr=htonl(INADDR_ANY);
	servaddr.sin_port=htons(SERV_PORT);
	
	bind(listenfd,(struct sockaddr*)&servaddr,sizeof(servaddr));
	
	listen(listenfd,20);
	
	memset(names,0,sizeof(names));				//初始化每个连接用户的名称
	memset(used,0,sizeof(used));				//初始化每个连接用户端
	memset(downloading,0,sizeof(downloading));	//初始化每个用户是否正在下载文件
	
	//预先开启最大用户连接数个线程
	pthread_t tids[MAXCON];
	int index[MAXCON];			//用来存储每个线程的索引值
	for(int i=0;i<MAXCON;i++){	//为每个线程创建一个索引值
		index[i]=i;
	}
	for(int i=0;i<MAXCON;i++){
		int ret=pthread_create(&tids[i],NULL,TRD,&index[i]);
		if(ret!=0){
			printf("thread create failed");
			return 0;
		}
	}
	
	//服务器在监听端口时，不断地等待客户端连接。一旦有客户端连接到达，
	//它就会为该客户端分配一个ID并接收连接请求。
	printf("Accepting connections ...\n");

	while (1) {
		// 读取下一个为占用的ID，作为下一个连接的用户ID
		int nowID = 0;
		for(nowID = 0; nowID < MAXCON-1; nowID++)
			if(!used[nowID]) break;
		cliaddr_len = sizeof(chiladdr[nowID]);
		// 等待连接
		connfd[nowID] = accept(listenfd, (struct sockaddr *)&chiladdr[nowID], &cliaddr_len);
		
		//检查是否是临时连接
		if(nowID > MAXCON-1){
			// 再次进行判断是否连接数满，因为第一次判断后可能有用户退出
			for(nowID = 0; nowID < MAXCON-1; nowID++)
				if(!used[nowID]) break;
			if(nowID > MAXCON-1){
				// 服务器连接数满
				memset(spemsg[nowID], 0, sizeof(spemsg[nowID]));
				strcpy(spemsg[nowID], "Error(1): 聊天室人已满。");
				sendonemsg(connfd[nowID], spemsg[nowID]);
				close(connfd[nowID]);
				continue;
			}else{
				// 再次判断后未满，设置对应的ID
				connfd[nowID] = connfd[MAXCON-1];
				chiladdr[nowID].sin_family = chiladdr[MAXCON-1].sin_family;
				chiladdr[nowID].sin_port = chiladdr[MAXCON-1].sin_port;
				chiladdr[nowID].sin_addr.s_addr = chiladdr[MAXCON-1].sin_addr.s_addr;
			}
		}
		used[nowID] = 1; // 该ID有人占用
	}
	return 0;
}


// 清理客户端资源函数
// 该函数用于清理客户端连接的资源，包括关闭连接、清除用户信息
void cleanup_client(int ID){
	if(used[ID]>0){
		printf("Cleaning up client %d\n", ID);

		//通知其他用户，该用户断开
		if(strlen(names[ID])>0){
			memset(buf[ID], 0, sizeof(buf[ID]));
			sprintf(buf[ID], "%s(%s:%d)离开了聊天室", names[ID],
					inet_ntop(AF_INET, &chiladdr[ID].sin_addr, str, sizeof(str)),
					ntohs(chiladdr[ID].sin_port));
			memset(spemsg[ID], 0, sizeof(spemsg[ID]));
			sendmsgtoall(ID);
		}

		close(connfd[ID]);
		used[ID]=0;
		downloading[ID]=0;
		memset(names[ID],0,sizeof(names[ID]));
		last_heartbeat[ID]=0;
	}
}


//心跳线程函数，用于检测客户端连接状态
// 该函数会定期检查每个连接的心跳状态，如果超过一定时间没有收到心跳信号，则认为该连接已断开，并进行相应的清理工作。
// 该函数会在独立的线程中运行，定期检查每个连接的心跳状态。
void *heartbeat_thread(void* arg){
	while(1){
		sleep(HEARTBEAT_INTERVAL); // 每隔 HEARTBEAT_INTERVAL 秒检查一次心跳状态
		
		time_t current_time = time(NULL); // 获取当前时间
		for(int i=0;i<MAXCON-1;i++){
			if(used[i]&&!downloading[i]){
				
				if(current_time-last_heartbeat[i]>HEARTBEAT_TIMEOUT){
					// 如果超过 HEARTBEAT_TIMEOUT 秒没有收到心跳信号，认为连接已断开
					cleanup_client(i);
					continue;
				}
				
				//发送心跳包
				memset(spemsg[i], 0, sizeof(spemsg[i]));
				strcpy(spemsg[i],"Heartbeat");
				sendonemsg(connfd[i], spemsg[i]);

			}
		}
	}
	return NULL;
}

inline int Process(int ID){
	char op[20];
	memset(op, 0, sizeof(op));
	
	if(buf[ID][0]==':'){
		int p = 0;
		// 提取命令部分（改为和示例代码一致的解析方式）
		while(buf[ID][p]!='\0'&&buf[ID][p]!=' '){
			op[p]=buf[ID][p];
			p++;
		}
		op[p]='\0';
		
		// 跳过空格，找到参数开始位置
		while(buf[ID][p]!='\0'&&buf[ID][p]==' '){
			p++;			
		}
		
		// 将参数部分移到buf开头
		if(buf[ID][p]!='\0'){
			int param_start = p;
			int param_len = strlen(buf[ID]) - param_start;
			memmove(buf[ID], buf[ID] + param_start, param_len + 1);
		}else{
			buf[ID][0] = '\0';  // 没有参数
		}
		
		if(op[1] == 'n'){
			// 新用户登陆，判断是否重名
			// 重名返回错误信息
			for(int i=0;i<MAXCON-1;i++){
				if(used[i]){
					if(memcmp(names[i],buf[ID],MAXNAME)==0){
						memset(spemsg[ID],0,sizeof(spemsg[ID]));
						strcpy(spemsg[ID],"Error(2): 姓名重复，请重试。");
						sendonemsg(connfd[ID],spemsg[ID]);
						return 0;
					}
				}				
			}
			
			// 不重名则发送提示信息
			strcpy(names[ID],buf[ID]);
			sprintf(buf[ID], "%s(%s:%d)进入了聊天室", names[ID],
					inet_ntop(AF_INET, &chiladdr[ID].sin_addr, str, sizeof(str)),
					ntohs(chiladdr[ID].sin_port));
			
			memset(spemsg[ID],0,sizeof(spemsg[ID]));
			strcpy(spemsg[ID],"成功进入聊天室");
			sendmsgtoall(ID);
				
			return 0;
		}
		
		if(op[1] == 'r'){
			// 改名，判断是否重名
			// 重名返回错误信息
			char newname[MAXNAME];
			for(int i=0;i<MAXCON-1;i++){
				if(i==ID) continue;
				
				if(used[i]){
					if(memcmp(names[i],buf[ID],MAXNAME)==0){
						memset(spemsg[ID],0,sizeof(spemsg[ID]));
						strcpy(spemsg[ID],"Error(3): 姓名重复，请重试。");
						sendonemsg(connfd[ID],spemsg[ID]);
						return 0;
					}
				}				
			}
			
			// 不重名则发送提示信息
			strcpy(newname,buf[ID]);
			memset(buf[ID],0,strlen(buf[ID]));
			sprintf(buf[ID], "%s(%s:%d)改名为%s", names[ID],
					inet_ntop(AF_INET, &chiladdr[ID].sin_addr, str, sizeof(str)),
					ntohs(chiladdr[ID].sin_port), newname);
			
			memset(names[ID],0,strlen(names[ID]));
			strcpy(names[ID],newname);
			memset(spemsg[ID], 0, sizeof(spemsg[ID]));
			strcpy(spemsg[ID], "改名成功");
			sendmsgtoall(ID);  // 只调用一次
			return 0;
		}
		
		if(op[1] == 'q'){
			// 用户退出时，给其他所用用户发送提示信息
			memset(buf[ID],0,strlen(buf[ID]));
			sprintf(buf[ID], "%s(%s:%d)离开了聊天室", names[ID],
					inet_ntop(AF_INET, &chiladdr[ID].sin_addr, str, sizeof(str)),
					ntohs(chiladdr[ID].sin_port));
					
			memset(spemsg[ID], 0, sizeof(spemsg[ID]));
			sendmsgtoall(ID);
			memset(names[ID], 0, sizeof(names[ID]));
			return 1; // 给线程函数返回1，用作后续处理
		}
		
		if(op[1] == 's'){
			// 给请求的用户发送所有用户信息
			memset(spemsg[ID], 0, sizeof(spemsg[ID]));
			strcpy(spemsg[ID], "IP              Port   name");
			sendonemsg(connfd[ID], spemsg[ID]);
			
			for(int i=0;i<MAXCON-1;i++){
				if(used[i]){
					memset(spemsg[ID], 0, sizeof(spemsg[ID]));
					sprintf(spemsg[ID], "%-16s%-7d%s",
							inet_ntop(AF_INET, &chiladdr[i].sin_addr, str, sizeof(str)),
							ntohs(chiladdr[i].sin_port), names[i]);
					sendonemsg(connfd[ID], spemsg[ID]);
				}
			}
			return 0;
		}
		
		if(op[1] == 'f'){
			// 给请求的用户发送服务器端文件
			system("mkdir -p server_file");
			system("ls server_file > ./file.txt");
			FILE* filename=fopen("./file.txt","r");
			
			if(filename != NULL){
				memset(spemsg[ID], 0, sizeof(spemsg[ID]));
				strcpy(spemsg[ID], "服务器文件列表:");
				sendonemsg(connfd[ID], spemsg[ID]);
				
				while(fgets(spemsg[ID],MAXLINE,filename)!=NULL){
					spemsg[ID][strlen(spemsg[ID])-1]='\0';
					sendonemsg(connfd[ID],spemsg[ID]);
				}
				fclose(filename);
			}else{
				memset(spemsg[ID], 0, sizeof(spemsg[ID]));
				strcpy(spemsg[ID], "无法读取文件列表");
				sendonemsg(connfd[ID], spemsg[ID]);
			}
			system("rm -f file.txt");
			
			return 0;
		}
		
		if(op[1] == 'u'){
			// 服务器端接收文件
			char filename[MAXLINE];
			char filepath[MAXLINE];
			char command[MAXLINE];

			memset(filename, 0, sizeof(filename));
			memset(filepath, 0, sizeof(filepath));
			memset(command, 0, sizeof(command));

			strcpy(filename, buf[ID]);
			
			system("mkdir -p server_file");
			sprintf(filepath, "server_file/%s", filename);
			sprintf(command, "rm -f %s", filepath);

			//判断是否有同名文件
			FILE *ff=fopen(filepath, "rb");
			if(ff != NULL){ // 判断文件已存在应为 ff != NULL
				memset(spemsg[ID], 0, sizeof(spemsg[ID]));
				strcpy(spemsg[ID], "Error(5): 文件已存在，请重试。");
				sendonemsg(connfd[ID], spemsg[ID]);
				fclose(ff);
				return 0;
			}

			//创建文件失败
			FILE* fp = fopen(filepath, "wb");
			if(fp==NULL){
				memset(spemsg[ID], 0, sizeof(spemsg[ID]));
				strcpy(spemsg[ID], "Error(4): 文件创建失败，请重试。");
				sendonemsg(connfd[ID], spemsg[ID]);
				return 0;
			}

			//开始接收文件
			while(1){
				memset(filebuf[ID], 0, sizeof(filebuf[ID]));
				n = read(connfd[ID], filebuf[ID], MAXFILE);
				if(n<=0){
					// 接收途中客户端断连，发送提示信息并删除未完全接受的文件
					sprintf(buf[ID], "%s(%s:%d)离开了聊天室", names[ID],
							inet_ntop(AF_INET, &chiladdr[ID].sin_addr, str, sizeof(str)),
							ntohs(chiladdr[ID].sin_port));
					memset(spemsg[ID], 0, sizeof(spemsg[ID]));
					sendmsgtoall(ID);
					close(connfd[ID]);
					memset(names[ID], 0, sizeof(names[ID]));
					used[ID] = 0;
					fclose(fp);
					system(command); // 删除未完成的文件
					return 1; 
				}
				if(strcmp(filebuf[ID], FINISHFLAG) == 0){
					// 接收完成，发送提示信息
					memset(buf[ID], 0, sizeof(buf[ID]));
					sprintf(buf[ID], "%s(%s:%d)上传了文件%s", names[ID],
							inet_ntop(AF_INET, &chiladdr[ID].sin_addr, str, sizeof(str)),
							ntohs(chiladdr[ID].sin_port), filename);

					memset(spemsg[ID], 0, sizeof(spemsg[ID]));
					strcpy(spemsg[ID], "文件上传成功");
					sendmsgtoall(ID);
					fclose(fp);
					return 0;
				}
				
				fwrite(filebuf[ID], sizeof(char), n, fp);
				//filebuf[ID]中前n个字节的数据写入到文件fp中，实现文件的保存。
				fflush(fp);
			}
		}
		
		if(op[1] == 'd'){
			// 服务器发送文件
			char filename[MAXLINE];
			char filepath[MAXLINE];
			char command[MAXLINE];

			memset(filename, 0, sizeof(filename));
			memset(filepath, 0, sizeof(filepath));
			memset(command, 0, sizeof(command));
			strcpy(filename, buf[ID]);
			system("mkdir -p server_file");
			sprintf(filepath, "./server_file/%s", filename);
			sprintf(command, "rm -f %s", filepath);

			FILE *fp = fopen(filepath, "rb");
			if(fp==NULL){
				memset(spemsg[ID], 0, sizeof(spemsg[ID]));
				strcpy(spemsg[ID], "Error(6): 文件不存在，请重试。");
				sendonemsg(connfd[ID], spemsg[ID]);
				return 0;
			}
			struct stat st;
			stat(filepath, &st);
			// 发送文件大小（改为只发送数字，和客户端解析保持一致）
			memset(spemsg[ID], 0, sizeof(spemsg[ID]));
			sprintf(spemsg[ID], "%ld", st.st_size);
			write(connfd[ID], spemsg[ID], strlen(spemsg[ID]));
			usleep(10000); // 等待客户端处理
			
			downloading[ID] = 1; // 设置正在下载标志

			while((n = fread(filebuf[ID], sizeof(char), MAXFILE, fp)) > 0){
				// 从文件中读取数据到 filebuf[ID] 中，直到文件结束。
				write(connfd[ID], filebuf[ID], n); // 将读取的数据发送给客户端
				memset(filebuf[ID], 0, sizeof(filebuf[ID]));
				// 清空 filebuf[ID]，为下一次读取做准备			
			}

			// 发送完成标志
			memset(spemsg[ID], 0, sizeof(spemsg[ID]));
			strcpy(spemsg[ID], FINISHFLAG);
			usleep(1000000); // 等待客户端处理完最后一个数据包
			write(connfd[ID], spemsg[ID], strlen(spemsg[ID]));

			downloading[ID] = 0; // 清除正在下载标志
			fclose(fp);
			return 0;
		}
	}else{
		// 非命令，作为消息发送给所有用户，并给消息来源发送提示信息
		char original_msg[MAXLINE];
		strcpy(original_msg, buf[ID]);  // 保存原始消息
		
		sprintf(buf[ID], "%s(%s:%d): %s", names[ID],
				inet_ntop(AF_INET, &chiladdr[ID].sin_addr, str, sizeof(str)),
				ntohs(chiladdr[ID].sin_port), original_msg);
		memset(spemsg[ID], 0, sizeof(spemsg[ID]));
		strcpy(spemsg[ID], "消息发送成功");
		sendmsgtoall(ID);
		return 0;
	}
	
	return 0;	//返回0表示没有错误
	
}

inline void sendonemsg(int sockfd,char* msg){

	strcat(msg,"\n");
	
	write(sockfd,msg,strlen(msg));
	
}


inline void sendmsgtoall(int ID){
	
	if(strlen(spemsg[ID])!=0){
		
		sendonemsg(connfd[ID],spemsg[ID]);
	}
	
	for(int i=0;i<MAXCON-1;i++){
		if(i==ID){
			continue;
		}
		else if(used[i]&&(!downloading[i])){
		
			sendonemsg(connfd[i],buf[ID]);
			
		}		
	}
	
}

//TRD是每个客户端连接所对应的线程函数。作用是处理客户端的消息交互，接受客户端的数据
//处理数据并会送回应。当客户端断开时，线程还需要通知其他用户并清理资源。
void *TRD(void *arg){		//创建的时候传入的是NULL
	int ID=*(int *)arg;
	
	while(1){
		while(!used[ID]);	//只有由用户连接，才可以使用
		memset(buf[ID],0,sizeof(buf[ID]));
		
		n=read(connfd[ID],buf[ID],MAXLINE);
		if(n<=0){
			sprintf(buf[ID],"%s(%s:%d)离开聊天室",names[ID],
				inet_ntop(AF_INET,&chiladdr[ID].sin_addr,str,sizeof(str)),
				ntohs(chiladdr[ID].sin_port));
			
			memset(spemsg[ID],0,sizeof(spemsg[ID]));
			sendmsgtoall(ID);
			close(connfd[ID]);
			memset(names[ID],0,strlen(names[ID]));
			used[ID]=0;
		}
		
		buf[ID][n]=0;
		printf("Received from %s at PORT %d: %s\n",		\
				inet_ntop(AF_INET,&chiladdr[ID].sin_addr,str,sizeof(str)),	\
				ntohs(chiladdr[ID].sin_port), buf[ID]);
				
		int q=Process(ID);
		
		if(q==1){
			close(connfd[ID]);
			used[ID]=0;
		}
		
	}
}