#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include <netdb.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <errno.h>

#define error(msg) printf("\x1b[31m[Coat] ERROR:\x1b[0m " msg "\n");

// Size of a buffer
#define SIZE 4096

// Amount of threads to use
#define THREADS 3

// Amount of concurrent connections allowed per thread
#define POLL_SIZE_CONST 1000

// Size of all concurrent client connections
#define POLL_MAIN_MEMORY_SIZE POLL_SIZE_CONST * (sizeof(struct pollfd))

// Size of a thread's concurrent client connections
#define POLL_MEMORY_SIZE (POLL_SIZE_CONST / THREADS) * (sizeof(int))

// Global Configuration
int POLL_SIZE = POLL_SIZE_CONST;
static const char *host = "127.0.0.1";
static const char *port = "8000";

// Hints and addresses for backend
struct addrinfo backendHints = {0};
struct addrinfo *backendAddrs;

// File Descriptors to Handle
int *fdsList[THREADS];

// Number of File Descriptors to Handle
int nfdsList[THREADS] = {0};

// Thread IDs
pthread_t tids[THREADS];

// Conditions for Thread Safety
pthread_cond_t conditions[THREADS] = {PTHREAD_COND_INITIALIZER};

// Mutexes for Thread Safety
pthread_mutex_t mutexes[THREADS] = {PTHREAD_MUTEX_INITIALIZER};

// State of threads
volatile int states[THREADS];

// State of main thread
volatile int running = 1;

// Terminate main thread
void terminate(int num) {
  running = 0;
}

// Transfer data between two sockets
int transfer(int from, int to) {
  int length = -1;
  int ret;
  char buffer[SIZE];
  while((length = read(from, buffer, SIZE)) > 0) {
    ret = write(to, buffer, length);
    if(ret == -1) {
      return -1;
    }
    if(length < SIZE) {
      break;
    }
  }
  return length;
}

// Handle client connection
void handle(int clientSocketFD) {
  int backendSocketFD;
  int ret;

  backendSocketFD = socket(backendAddrs->ai_family, backendAddrs->ai_socktype, backendAddrs->ai_protocol);
  ret = connect(backendSocketFD, backendAddrs->ai_addr, backendAddrs->ai_addrlen);

  if(ret == -1) {
    error("Could not establish connection with backend.");
  } else {
    ret = transfer(clientSocketFD, backendSocketFD);

    if(ret != -1 || (errno == EWOULDBLOCK || errno == EAGAIN)) {
      transfer(backendSocketFD, clientSocketFD);
    }
  }

  close(backendSocketFD);
  close(clientSocketFD);
}

void handleThread(void *vargp) {
  // ID Of Thread
  int id = *(int*)vargp;

  // File Descriptors to Poll
  int *fds;

  // Number of File Descriptors to Poll
  int nfds;

  // Iterators
  int i, j;

  while(1) {
    pthread_mutex_lock(&mutexes[id]);
    nfds = nfdsList[id];
    if(states[id] == 0) {
      pthread_mutex_unlock(&mutexes[id]);
      break;
    } else if(nfds == 0) {
      pthread_mutex_unlock(&mutexes[id]);
    } else {
      fds = fdsList[id];
      for(i = 0; i < nfds; i++) {
        handle(fds[i]);
        fds[i] = -1;
      }
      for(i = 0; i < nfdsList[id];) {
        if(fds[i] == -1) {
          for(j = i; j < (--nfdsList[id]); j++) {
            fds[j] = fds[j + 1];
          }
        } else {
          i++;
        }
      }
      pthread_mutex_unlock(&mutexes[id]);
    }
  }
}

int main(int argc, const char *argv[]) {
  // Client Port
  const char *clientPort = argv[1];

  // Hints and addresses for client
  struct addrinfo hints = {0};
  struct addrinfo *addrs;

  // File descriptors for the client and server
  int clientSocketFD;
  int serverSocketFD;

  // Reuse address
  int reuseAddr = 1;

  // Return value for system calls
  int ret;

  // Iterator
  int i, j;

  // IDs of threads to create
  int ids[THREADS];

  // File Descriptors to Poll
  struct pollfd *fds = malloc(POLL_MAIN_MEMORY_SIZE);

  // Number of Items to Poll
  int nfds = 1;

  // ID of thread to load balance
  int id = 0;

  // Get backend information
  backendHints.ai_family = AF_UNSPEC;
  backendHints.ai_socktype = SOCK_STREAM;
  backendHints.ai_protocol = 0;

  getaddrinfo(host, clientPort, &backendHints, &backendAddrs);

  // Listen on port
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_protocol = 0;
  hints.ai_flags = AI_PASSIVE;

  getaddrinfo(NULL, port, &hints, &addrs);
  serverSocketFD = socket(addrs->ai_family, addrs->ai_socktype, addrs->ai_protocol);
  setsockopt(serverSocketFD, SOL_SOCKET, SO_REUSEADDR, &reuseAddr, sizeof(reuseAddr));
  fcntl(serverSocketFD, F_SETFL, (fcntl(serverSocketFD, F_GETFL, 0) | O_NONBLOCK));
  bind(serverSocketFD, addrs->ai_addr, addrs->ai_addrlen);
  freeaddrinfo(addrs);
  ret = listen(serverSocketFD, POLL_SIZE_CONST);

  if(ret == -1) {
    error("Could not listen on specified port.");
  }

  // Setup main polling lists
  memset(fds, 0, POLL_MAIN_MEMORY_SIZE);
  fds[0].fd = serverSocketFD;
  fds[0].events = POLLIN;

  // Setup file descriptor lists and start threads
  for(i = 0; i < THREADS; i++) {
    // Create state
    states[i] = 1;

    // Create ID
    ids[i] = i;

    // Create list
    fdsList[i] = malloc(POLL_MEMORY_SIZE);
    memset(fdsList[i], 0, POLL_MEMORY_SIZE);

    // Create thread
    pthread_create(&tids[i], NULL, (void*)&handleThread, (void*)&ids[i]);
  }

  // Clear Memory when Process is Interrupted
  signal(SIGINT, terminate);

  // Log
  printf("======= Coat =======\n");

  while(running) {
    // Accept a connection
    ret = poll(fds, nfds, -1);

    // Distribute to threads
    if(ret != -1) {
      if(fds[0].revents == POLLIN) {
        // Listening socket can accept connections
        while((clientSocketFD = accept(serverSocketFD, NULL, NULL)) != -1) {
          // Add client to polling list
          if(nfds == POLL_SIZE) {
            // Reallocate memory if clients exceed concurrency limit
            realloc(fds, (++POLL_SIZE) * (sizeof(struct pollfd)));
            for(i = 0; i < THREADS; i++) {
              pthread_mutex_lock(&mutexes[i]);
              realloc(fdsList[i], (POLL_SIZE) * (sizeof(struct pollfd)));
              pthread_mutex_unlock(&mutexes[i]);
            }
          }

          fds[nfds].fd = clientSocketFD;
          fds[nfds].events = POLLIN;
          nfds++;
        }
      }

      for(i = 1; i < nfds; i++) {
        if(fds[i].revents == POLLIN) {
          pthread_mutex_lock(&mutexes[id]);
          fdsList[id][nfdsList[id]] = fds[i].fd;
          nfdsList[id]++;
          fds[i].fd = -1;
          pthread_mutex_unlock(&mutexes[id]);
          if((++id) == THREADS) {
            id = 0;
          }
        }
      }

      for(i = 0; i < nfds;) {
        if(fds[i].fd == -1) {
          for(j = i; j < (--nfds); j++) {
            fds[j] = fds[j + 1];
          }
        } else {
          i++;
        }
      }
    }
  }

  // Close server socket
  close(serverSocketFD);

  // Free pending client connections
  free(fds);

  // Free backend addresses
  freeaddrinfo(backendAddrs);

  // Stop threads and free pending client connections
  for(i = 0; i < THREADS; i++) {
    pthread_mutex_lock(&mutexes[i]);
    states[i] = 0;
    pthread_mutex_unlock(&mutexes[i]);
    pthread_join(tids[i], NULL);
    free(fdsList[i]);
  }

  return 0;
}
