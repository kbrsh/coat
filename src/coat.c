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

// Amount of concurrent connections allowed per thread
#define POLL_SIZE_CONST 100000
#define POLL_MEMORY_SIZE POLL_SIZE_CONST * (sizeof(struct pollfd))

// Amount of threads to use
#define THREADS 3

// Global Configuration
int POLL_SIZE = POLL_SIZE_CONST;
static const char *host = "127.0.0.1";
static const char *port = "8000";

// Hints and addresses for backend
struct addrinfo backendHints = {0};
struct addrinfo *backendAddrs;

// File Descriptors to Poll
struct pollfd *fdsList[THREADS];

// Number of File Descriptors to Poll
int nfdsList[THREADS] = {0};

// Thread IDs
pthread_t tids[THREADS];

// Conditions for Thread Safety
pthread_cond_t conditions[THREADS] = {PTHREAD_COND_INITIALIZER};

// Mutexes for Thread Safety
pthread_mutex_t mutexes[THREADS] = {PTHREAD_MUTEX_INITIALIZER};

// State of threads
int state = 1;

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

  // File Descriptors to Poll
  struct pollfd *fds;

  // Number of File Descriptors to Poll
  int nfds;

  // Iterators
  int i, j;

  // Cleaning Status
  int clean = 0;

  // Return value of system calls
  int ret;

  printf("thread started\n");

  while(1) {
    pthread_mutex_lock(&mutexes[id]);
    fds = fdsList[id];
    nfds = nfdsList[id];
    // printf("locked and in loop %d\n", nfds);
    if(state == 0) {
      pthread_mutex_unlock(&mutexes[id]);
      break;
    } else if(nfds == 0) {
      pthread_cond_wait(&conditions[id], &mutexes[id]);
      pthread_mutex_unlock(&mutexes[id]);
    } else {
      printf("awaiting connection\n");
      // Poll until socket is ready to read
      ret = poll(fds, nfds, -1);

      printf("connection\n");

      if(ret != -1) {
        // Go through all file descriptors
        for(i = 0; i < nfds; i++) {
          if(fds[i].revents == POLLIN) {
            // Client socket can accept connections
            handle(fds[i].fd);
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
  int i;

  // ID of thread to load balance
  int id;

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

  // Setup polling lists, and start threads
  for(i = 0; i < THREADS; i++) {
    // Create polling list
    fdsList[i] = malloc(POLL_MEMORY_SIZE);
    memset(fdsList[i], 0, POLL_MEMORY_SIZE);

    // Create thread
    pthread_create(&tids[i], NULL, (void*)&handleThread, (void*)&i);
  }

  // Clear Memory when Process is Interrupted
  signal(SIGINT, terminate);

  // Log
  printf("======= Coat =======\n");

  i = 0;
  while(running) {
    // Accept a connection
    clientSocketFD = accept(serverSocketFD, NULL, NULL);

    // Distribute to threads
    if(clientSocketFD != -1) {
      id = i++;
      pthread_mutex_lock(&mutexes[id]);
      fdsList[id][nfdsList[id]].fd = clientSocketFD;
      fdsList[id][nfdsList[id]].events = POLLIN;
      nfdsList[id]++;
      pthread_cond_signal(&conditions[id]);
      pthread_mutex_unlock(&mutexes[id]);
      printf("unlocked and loaded %d\n", nfdsList[id]);
      if(i == THREADS) {
        i = 0;
      }
    }
  }

  // Stop threads
  for(i = 0; i < THREADS; i++) {
    pthread_mutex_lock(&mutexes[i]);
  }
  state = 0;
  for(i = 0; i < THREADS; i++) {
    pthread_mutex_unlock(&mutexes[i]);
  }

  // Free memory
  freeaddrinfo(backendAddrs);
  for(i = 0; i < THREADS; i++) {
    pthread_join(tids[i], NULL);
    free(fdsList[i]);
  }

  return 0;
}
