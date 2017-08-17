#include <stdio.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <unistd.h>
#include <netdb.h>
#include <errno.h>

#define SIZE 4096

static const char *host = "127.0.0.1";
static const char *port = "8000";

void handle(const char *clientPort, int clientSocketFD) {
  struct addrinfo hints = {0};
  struct addrinfo *addrs;
  struct addrinfo *currentAddrs;

  int backendSocketFD;
  int buffer[SIZE];
  int length;

  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;

  getaddrinfo(host, clientPort, &hints, &addrs);

  for(currentAddrs = addrs; currentAddrs != NULL; currentAddrs = currentAddrs->ai_next) {
    backendSocketFD = socket(currentAddrs->ai_family, currentAddrs->ai_socktype, currentAddrs->ai_protocol);

    if(backendSocketFD != -1) {
      if(connect(backendSocketFD, currentAddrs->ai_addr, currentAddrs->ai_addrlen) == 0) {
        break;
      } else {
        close(backendSocketFD);
      }
    }
  }

  freeaddrinfo(addrs);

  while((length = read(clientSocketFD, buffer, SIZE)) != 0) {
    write(backendSocketFD, buffer, length);
  }

  while((length = read(backendSocketFD, buffer, SIZE)) != 0) {
    write(clientSocketFD, buffer, length);
  }
  
  close(clientSocketFD);
}

int main(int argc, const char *argv[]) {
  const char *clientPort = argv[1];

  struct addrinfo hints = {0};
  struct addrinfo *addrs;
  struct addrinfo *currentAddrs;

  int clientSocketFD;
  int serverSocketFD;

  int reuseAddr = 1;

  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_flags = AI_PASSIVE;

  getaddrinfo(NULL, port, &hints, &addrs);

  for(currentAddrs = addrs; currentAddrs != NULL; currentAddrs = currentAddrs->ai_next) {
    serverSocketFD = socket(currentAddrs->ai_family, currentAddrs->ai_socktype, currentAddrs->ai_protocol);

    if(serverSocketFD != -1) {
      setsockopt(serverSocketFD, SOL_SOCKET, SO_REUSEADDR, &reuseAddr, sizeof(reuseAddr));

      if(bind(serverSocketFD, currentAddrs->ai_addr, currentAddrs->ai_addrlen) == 0) {
        break;
      } else {
        close(serverSocketFD);
      }
    }
  }

  freeaddrinfo(addrs);
  listen(serverSocketFD, 1);

  while(1) {
    clientSocketFD = accept(serverSocketFD, NULL, NULL);
    handle(clientPort, clientSocketFD);
  }

  printf("======= Coat =======");
  return 0;
}
