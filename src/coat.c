#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include <netdb.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <errno.h>

#define error(msg) printf("\x1b[31m[Coat] ERROR:\x1b[0m " msg "\n");

#define SIZE 4096
#define POLL_SIZE_CONST 100000
#define POLL_MEMORY_SIZE POLL_SIZE_CONST * (sizeof(struct pollfd))

int POLL_SIZE = POLL_SIZE_CONST;
static const char *host = "127.0.0.1";
static const char *port = "8000";

struct handleArguments {
  int clientSocketFD;
  struct addrinfo *backendAddrs;
};

void handle(void *argumentsPointer) {
  int backendSocketFD;
  int length;
  char buffer[SIZE];

  struct handleArguments *arguments = argumentsPointer;
  int clientSocketFD = arguments->clientSocketFD;
  struct addrinfo *backendAddrs = arguments->backendAddrs;

  backendSocketFD = socket(backendAddrs->ai_family, backendAddrs->ai_socktype, backendAddrs->ai_protocol);
  length = connect(backendSocketFD, backendAddrs->ai_addr, backendAddrs->ai_addrlen);

  if(length == -1) {
    error("Could not establish connection with backend.");
  } else {
    while((length = read(clientSocketFD, buffer, SIZE)) > 0) {
      write(backendSocketFD, buffer, length);
    }

    while((length = read(backendSocketFD, buffer, SIZE)) > 0) {
      write(clientSocketFD, buffer, length);
      if(length < SIZE) {
        break;
      }
    }
  }

  close(backendSocketFD);
  close(clientSocketFD);
}

int main(int argc, const char *argv[]) {
  // Client Port
  const char *clientPort = argv[1];

  // Hints and addresses for client
  struct addrinfo hints = {0};
  struct addrinfo *addrs;

  // Hints and addresses for backend
  struct addrinfo backendHints = {0};
  struct addrinfo *backendAddrs;

  // File descriptors for the client, server, and backend
  int clientSocketFD;
  int serverSocketFD;

  // Reuse address
  int reuseAddr = 1;

  // File Descriptors to Poll
  struct pollfd *fds = malloc(POLL_MEMORY_SIZE);

  // Number of File Descriptors to Poll
  int nfds = 1;

  // Iterators
  int i, j;

  // Thread ID
  pthread_t tid;

  // Return value for system calls
  int ret;

  // Clean
  int clean = 0;

  // Connect to backend
  backendHints.ai_family = AF_UNSPEC;
  backendHints.ai_socktype = SOCK_STREAM;
  backendHints.ai_protocol = 0;

  getaddrinfo(host, clientPort, &backendHints, &backendAddrs);

  // Listen on port
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  backendHints.ai_protocol = 0;
  hints.ai_flags = AI_PASSIVE;

  getaddrinfo(NULL, port, &hints, &addrs);
  serverSocketFD = socket(addrs->ai_family, addrs->ai_socktype, addrs->ai_protocol);
  setsockopt(serverSocketFD, SOL_SOCKET, SO_REUSEADDR, &reuseAddr, sizeof(reuseAddr));
  fcntl(serverSocketFD, F_SETFL, (fcntl(serverSocketFD, F_GETFL, 0) | O_NONBLOCK));
  bind(serverSocketFD, addrs->ai_addr, addrs->ai_addrlen);
  freeaddrinfo(addrs);
  ret = listen(serverSocketFD, SIZE);

  if(ret == -1) {
    error("Could not listen on specified port.");
  }

  // Clear polling list
  memset(fds, 0, POLL_MEMORY_SIZE);

  // Add listening file descriptor to polling list
  fds[0].fd = serverSocketFD;
  fds[0].events = POLLIN;

  printf("======= Coat =======\n");

  while(1) {
    // Poll until socket is ready to read
    ret = poll(fds, nfds, -1);

    if(ret != -1) {
      if(fds[0].revents == POLLIN) {
        // Listening socket can accept connections
        while((clientSocketFD = accept(serverSocketFD, NULL, NULL)) != -1) {
          // Add client socket to polling list
          if(nfds == POLL_SIZE) {
            realloc(fds, (++POLL_SIZE) * (sizeof(struct pollfd)));
          }

          fds[nfds].fd = clientSocketFD;
          fds[nfds].events = POLLIN;
          nfds++;
        }
      }

      // Go through all file descriptors
      for(i = 1; i < nfds; i++) {
        if(fds[i].revents == POLLIN) {
          // Client socket can accept connections
          struct handleArguments arguments;
          arguments.clientSocketFD = fds[i].fd;
          arguments.backendAddrs = backendAddrs;
          pthread_create(&tid, NULL, (void *)&handle, (void *)&arguments);
          pthread_detach(tid);
          fds[i].fd = -1;
          if(clean == 0) {
            clean = 1;
          }
        }
      }

      // Clean closed connections
      if(clean == 1) {
        for(i = 1; i < nfds; i++) {
          if(fds[i].fd == -1) {
            for(j = i; j < nfds; j++) {
              fds[j] = fds[j + 1];
            }
          }
        }
        clean = 0;
      }
    }
  }

  freeaddrinfo(backendAddrs);
  free(fds);
  return 0;
}
