#include "memcached_client.h"
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h> 
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>

#define HOST "127.0.0.1"
#define PORT "11211"


// #define ADD "add"
#define MAX_RECV_SIZE 4200
#define MSG_MAX_SIZE 128

int send_entry(inumber_t block, void* buf, size_t size, char* operation);

int sock;
void init_connection(){
  sock = socket(AF_INET, SOCK_STREAM, 0);
  if(sock == -1){
    perror("couldn't create socket\n");
    exit(EXIT_FAILURE);

  }
  setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &(int){ 1 }, sizeof(int));
  struct addrinfo hints, *res;
  memset(&hints, 0, sizeof hints);
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  

  if (getaddrinfo(HOST, PORT, &hints, &res) != 0) {
    fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(sock));
    exit(EXIT_FAILURE);
  }
  if (connect(sock, res->ai_addr, res->ai_addrlen) != 0){
    printf("couldn't connect. %s\n", strerror(errno));
    exit(EXIT_FAILURE);
  }

  // don't know if i should be freeing this here.
  freeaddrinfo(res);
}

void close_connection(){
  close(sock);
}

void send_all(void* buf, size_t size){
  int total_sent = 0;
  int sent = 0;

  while(total_sent < size){
    buf += sent;
    sent = send(sock, buf, size, 0);
    if(sent == -1){
      perror("couldn't send data\n");
      exit(EXIT_FAILURE);
    }
    total_sent += sent;
  }
}

/*
  receives until the amount of bytes are received
  which is specified in the packet itself
  returns the data_length, not total received length.
 */
int recv_all(void* buf){
  char recvd[MAX_RECV_SIZE];
  memset(recvd, 0, MAX_RECV_SIZE);
  int recvd_size = recv(sock, recvd, MAX_RECV_SIZE, 0);

  if(recvd_size <= 0){
    perror("receive error\n");
    exit(EXIT_FAILURE);
  }
  if(strncmp(recvd, "END", 3) == 0){
    printf("not found\n");
    return -1;
  }

  if(strncmp(recvd, "VALUE", 5) != 0){
    for(int i = 0; i < 10; i++)
      printf("%c", recvd[i]);
    printf("recvd_size = %d\n", recvd_size);
  }

  assert(strncmp(recvd, "VALUE", 5) == 0);
  int index = 0;
  for(int i = 0; i < 3; index++){
    if(recvd[index] == ' ')
      i++;
  }
  size_t data_length = atoi(recvd + index);
  while(recvd[index-2] != '\r' && recvd[index-1] != '\n' && index < recvd_size)
    index++;
  while(strncmp(recvd + index + data_length + 2, "END", 3) != 0){
    recvd_size += recv(sock, recvd + recvd_size, MAX_RECV_SIZE, 0);
  }
  memcpy(buf, recvd + index, data_length);
  return data_length;
}

int add_block(inumber_t block, void* buf){
  return send_entry(block, buf, BLOCK_SIZE, "add");
}

int get_block(inumber_t block, void* buf){
  int status = get_entry(block, buf, BLOCK_SIZE);
  if(status == -1 || status == 0)
    return -1;
  return 0;
}

int update_block(inumber_t block, void* buf){
  return update_entry(block, buf, BLOCK_SIZE);
}

int remove_block(inumber_t block){
  return remove_entry(block);
}

int send_entry(inumber_t block, void* buf, size_t size, char* operation){
  char statement[MSG_MAX_SIZE];
  // memset(statement, 0, MSG_MAX_SIZE);
  int stat_len = sprintf(statement, "%s %llu 0 0 %lu\r\n", operation, block, size);
  // send_all(statement, stat_len);

  char to_send[size + 2 + stat_len];
  memcpy(to_send, statement, stat_len);
  memcpy(to_send + stat_len, buf, size + 2);
  to_send[size + stat_len] = '\r';
  to_send[size + 1 + stat_len] = '\n';

  send_all(to_send, size + 2 + stat_len);
  char recv_buf [MSG_MAX_SIZE];
  memset(recv_buf, 0, MSG_MAX_SIZE);
  int recv_length = recv(sock, recv_buf, MSG_MAX_SIZE, 0);

  if (strcmp(recv_buf, "STORED\r\n") == 0){
    return 0;
  } else if(strcmp(recv_buf, "SERVER_ERROR out of memory storing object\r\n")){
    printf("%s", recv_buf);
    return -ENOMEM;    
  } else {
    printf("%s", recv_buf);
    return -1;
  }
}

int get_entry(inumber_t block, char* buf, size_t size){
  char get_statement[MSG_MAX_SIZE];
  memset(get_statement, 0, MSG_MAX_SIZE);
  int stat_len = sprintf(get_statement, "get %llu\r\n", block);
  // send_block()
  send_all(get_statement, stat_len);
  return recv_all(buf);
}

int update_entry(inumber_t block, void* buf, size_t size){
  return send_entry(block, buf, size, "set");
}

int remove_entry(inumber_t block){
  char statement[MSG_MAX_SIZE];
  memset(statement, 0, MSG_MAX_SIZE);
  int stat_len = sprintf(statement, "delete %llu\r\n", block);
  send_all(statement, stat_len);

  char recv_buf [MSG_MAX_SIZE];
  memset(recv_buf, 0, MSG_MAX_SIZE);
  int recv_length = recv(sock, recv_buf, MSG_MAX_SIZE, 0);
  if(strcmp(recv_buf, "DELETED\r\n") == 0){
    return 0;
  }
  return -1;
}


int flush_all(){
  char* flush_statement = "flush_all\r\n";
  send_all(flush_statement, strlen(flush_statement));
  char ok[MSG_MAX_SIZE];
  memset(ok, 0, MSG_MAX_SIZE);
  recv(sock, ok, MSG_MAX_SIZE, 0);
  if (strcmp(ok, "OK\r\n") == 0){
    return 0;
  } else {
    printf("%s", ok);
    return -1;
  }
}

// int main(){

//   init_connection();
//   struct disk_inode inode;
//   inode.inumber = 5;
//   inode.length = 4;
//   assert(sizeof(struct disk_inode) == BLOCK_SIZE);
//   add_entry(inode.inumber, (void*)&inode, sizeof(disk_inode));
//   struct disk_inode buf;// = malloc(sizeof(struct disk_inode));
//   get_entry(inode.inumber, (void*)&buf, sizeof(disk_inode));
//   printf("%lld\n", buf.inumber);
//   printf("%u\n", buf.length);
  
//   inode.length = 1012;


//   struct disk_inode buf2;
//   update_entry(inode.inumber, (void*)&inode, sizeof(disk_inode));
//   get_entry(inode.inumber, (void*)&buf2, sizeof(disk_inode));
  

//   printf("%lld\n", buf2.inumber);
//   printf("%u\n", buf2.length);
//   // // printf)
//   flush_all();
//   return 0;
// }