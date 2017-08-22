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

// Amount of concurrent connections allowed
#define POLL_SIZE_CONST 10000
#define POLL_MAIN_MEMORY_SIZE POLL_SIZE_CONST * (sizeof(struct pollfd))
#define POLL_MEMORY_SIZE (POLL_SIZE_CONST / 3) * (sizeof(int))

// Amount of threads to use
#define THREADS 3

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

void terminate(int num) {
  running = 0;
}

void handle(int clientSocketFD) {
  int backendSocketFD;
  int length;
  char buffer[SIZE];

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

void handleThread(void *vargp) {
  // ID Of Thread
  int id = *(int*)vargp;

  printf("thread %d created\n", id);

  // File Descriptors to Poll
  int *fds;

  // Number of File Descriptors to Poll
  int nfds;

  // Iterators
  int i, j;

  // Cleaning Status
  int clean = 0;

  while(1) {
    pthread_mutex_lock(&mutexes[id]);
    nfds = nfdsList[id];
    if(states[id] == 0) {
      printf("stopping thread %d\n", id);
      pthread_mutex_unlock(&mutexes[id]);
      break;
    } else if(nfds == 0) {
      pthread_mutex_unlock(&mutexes[id]);
    } else {
      printf("thread going through connections\n");
      fds = fdsList[id];
      for(i = 0; i < nfds; i++) {
        handle(fds[i]);
        fds[i] = -1;
        nfdsList[id]--;
        if(clean == 0) {
          clean = 1;
        }
      }
      if(clean == 1) {
        for(i = 0; i < nfds; i++) {
          if(fds[i] == -1) {
            for(j = i; j < nfds; j++) {
              fds[j] = fds[j + 1];
            }
          }
        }
        clean = 0;
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

  // Cleaning Status
  int clean = 0;

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
  ret = listen(serverSocketFD, SIZE);

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

          printf("polling new connection\n");
        }
      }

      for(i = 1; i < nfds; i++) {
        if(fds[i].revents == POLLIN) {
          pthread_mutex_lock(&mutexes[id]);
          printf("new connection ready for I/O\n");
          fdsList[id][nfdsList[id]] = fds[i].fd;
          nfdsList[id]++;
          printf("new nfds for thread %d is %d\n", id, nfdsList[id]);
          fds[i].fd = -1;
          nfds--;
          pthread_mutex_unlock(&mutexes[id]);
          if(clean == 0) {
            clean = 1;
          }
          if((++id) == THREADS) {
            id = 0;
          }
        }
      }

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

  // Stop threads and free memory
  freeaddrinfo(backendAddrs);
  for(i = 0; i < THREADS; i++) {
    pthread_mutex_lock(&mutexes[i]);
    states[i] = 0;
    pthread_mutex_unlock(&mutexes[i]);
    printf("unlocked and set\n");
    pthread_join(tids[i], NULL);
    free(fdsList[i]);
  }

  return 0;
}
