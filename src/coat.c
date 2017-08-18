#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <unistd.h>
#include <netdb.h>
#include <fcntl.h>
#include <poll.h>
#include <errno.h>

#define SIZE 4096
#define POLL_SIZE_CONST 10000
#define POLL_MEMORY_SIZE POLL_SIZE_CONST * (sizeof(struct pollfd))

int POLL_SIZE = POLL_SIZE_CONST;
static const char *host = "127.0.0.1";
static const char *port = "8000";

int handle(const char *clientPort, int clientSocketFD, int backendSocketFD) {
  int length;
  int buffer[SIZE];

  while(1) {
    length = read(clientSocketFD, buffer, SIZE);
    if(length < 0) {
      return 0;
    } else if(length == 0) {
      break;
    } else {
      write(backendSocketFD, buffer, length);
    }
  }

  while(1) {
    length = read(backendSocketFD, buffer, SIZE);

    if(length <= 0) {
      return 0;
    } else {
      write(clientSocketFD, buffer, length);
    }
  }

  return 1;
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
  int backendSocketFD;

  // Reuse address
  int reuseAddr = 1;

  // File Descriptors to Poll
  struct pollfd *fds = malloc(POLL_MEMORY_SIZE);

  // Number of File Descriptors to Poll
  int nfds = 1;

  // Iterators
  int i, j;

  // Return value for system calls
  int ret;

  // Return value for handler
  int handleRet;

  // Connect to backend
  backendHints.ai_family = AF_UNSPEC;
  backendHints.ai_socktype = SOCK_STREAM;

  getaddrinfo(host, clientPort, &backendHints, &backendAddrs);
  backendSocketFD = socket(backendAddrs->ai_family, backendAddrs->ai_socktype, backendAddrs->ai_protocol);
  fcntl(backendSocketFD, F_SETFL, (fcntl(backendSocketFD, F_GETFL) | O_NONBLOCK));
  connect(backendSocketFD, backendAddrs->ai_addr, backendAddrs->ai_addrlen);
  freeaddrinfo(backendAddrs);

  // Listen on port
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_flags = AI_PASSIVE;

  getaddrinfo(NULL, port, &hints, &addrs);
  serverSocketFD = socket(addrs->ai_family, addrs->ai_socktype, addrs->ai_protocol);
  setsockopt(serverSocketFD, SOL_SOCKET, SO_REUSEADDR, &reuseAddr, sizeof(reuseAddr));
  fcntl(serverSocketFD, F_SETFL, (fcntl(serverSocketFD, F_GETFL) | O_NONBLOCK));
  bind(serverSocketFD, addrs->ai_addr, addrs->ai_addrlen);
  freeaddrinfo(addrs);
  listen(serverSocketFD, 1);

  // Add listening file descriptor to polling list
  memset(fds, 0, POLL_MEMORY_SIZE);
  fds[0].fd = serverSocketFD;
  fds[0].events = POLLIN;

  printf("======= Coat =======\n");

  while(1) {
    // Poll until socket is ready to read
    ret = poll(fds, nfds, -1);

    if(ret != -1) {
      // Go through all file descriptors
      for(i = 0; i < nfds; i++) {
        if(fds[i].revents == 0) {
          continue;
        } else if(fds[i].revents == POLLIN) {
          // Readable file descriptor
          if(fds[i].fd == serverSocketFD) {
            // Listening socket can accept connections
            while((clientSocketFD = accept(serverSocketFD, NULL, NULL)) != -1 || errno != EWOULDBLOCK) {
              // Add client socket to polling list
              if(nfds == POLL_SIZE) {
                realloc(fds, (++POLL_SIZE) * (sizeof(struct pollfd)));
              }

              // fcntl(clientSocketFD, F_SETFL, (fcntl(clientSocketFD, F_GETFL) | O_NONBLOCK));

              fds[nfds].fd = clientSocketFD;
              fds[nfds].events = POLLIN;
              nfds++;
            }
          } else {
            // Client socket can accept connections
            handleRet = handle(clientPort, fds[i].fd, backendSocketFD);

            if(handleRet == 0) {
              close(fds[i].fd);
              fds[i].fd = -1;
            }
          }
        }
      }

      if(handleRet == 0) {
        for(i = 0; i < nfds; i++) {
          if(fds[i].fd == -1) {
            for(j = i; j < nfds; j++) {
              fds[j] = fds[j + 1];
            }
          }
        }
      }
    }
  }

  free(fds);
  return 0;
}
