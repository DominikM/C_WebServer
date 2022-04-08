#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <sys/wait.h>
#include <arpa/inet.h>
#include <poll.h>

typedef struct ServerConfig ServerConfig;
struct ServerConfig {
  char* port;
  int backlog;
  int timeout;
  int num_conns;
};

void* get_in_addr(struct sockaddr *sa)
{
    if (sa->sa_family == AF_INET) {
        return &(((struct sockaddr_in*)sa)->sin_addr);
    }

    return &(((struct sockaddr_in6*)sa)->sin6_addr);
}

int read_options(int argc, char* argv[argc+1], ServerConfig* sc) {
  switch (argc) {
  case 3: sc->backlog = strtol(argv[2], NULL, 10);
  case 2: sc->port = argv[1];
  }

  return 0;    
}

int get_listener_socket(ServerConfig* sc) {
  struct addrinfo hints = {0};
  struct addrinfo* ai_results;

  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_flags = AI_PASSIVE;

  printf("creating server on port %s\n", sc->port);

  int rv;
  if ((rv = getaddrinfo(NULL, sc->port, &hints, &ai_results)) != 0) {
    fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(rv));
    return -1;
  }

  struct addrinfo* ai_result;
  int socket_fd;
  for (ai_result = ai_results; ai_result != NULL; ai_result = ai_results->ai_next) {
    socket_fd = socket(ai_result->ai_family, ai_result->ai_socktype, ai_result->ai_protocol);
    if (socket_fd == -1) {
      continue;
    }

    int yes = 1;
    if (setsockopt(socket_fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(int)) == -1) {
      perror("setsockopt");
      return -1;
    }

    if (bind(socket_fd, ai_result->ai_addr, ai_result->ai_addrlen) == 0) {
      break;
    }

    close(socket_fd);
  }

  freeaddrinfo(ai_results);

  if (listen(socket_fd, sc->backlog) == -1) {
    perror("listen");
    return -1;
  }

  printf("server successfully created!\n");

  return socket_fd;
}

int add_to_pfds(int const pfds_size, struct pollfd pfds[pfds_size],
		int* pfds_count, int const new_fd) {
  if (pfds_size == *pfds_count) {
    return -1;
  }

  pfds[*pfds_count].fd = new_fd;
  pfds[*pfds_count].events = POLLIN;

  (*pfds_count)++;
  return 0;
}

int del_from_pfds(struct pollfd pfds[], int i, int* pfds_count) {
  if (i < 0 || i >= *pfds_count) {
    return -1;
  }

  if (*pfds_count == 0) {
    return -1;
  }

  pfds[i] = pfds[*pfds_count];
  (*pfds_count)--;
  return 0;
}

int handle_listener(int* pfd_count, int const num_conns,
		    struct pollfd pfds[num_conns], int listener_fd) {
  struct sockaddr_storage their_addr;
  socklen_t their_addr_len = sizeof their_addr;
  int new_fd = accept(listener_fd, (struct sockaddr *)&their_addr, &their_addr_len);
  if (new_fd == -1) {
    perror("accept");
    return -1;
  }

  char ip_str[INET6_ADDRSTRLEN];
  printf("connection from: %s\n",
	 inet_ntop(their_addr.ss_family,
		   get_in_addr((struct sockaddr *)&their_addr),
		   ip_str, sizeof ip_str));

  if (*pfd_count < num_conns) {
    printf("there are %d remaining connections\n", num_conns - *pfd_count - 1);		 
    return add_to_pfds(num_conns, pfds, pfd_count, new_fd);
  } else {
    printf("number of connections exceed!\n");
    char buf[] = "number of connections exceed\n";
    send(new_fd, buf, sizeof buf, 0);
    close(new_fd);
  }
  return 0;
}

int handle_client(int pfd_i, int* pfd_count, struct pollfd pfds[]) {
  char buf[1000] = {0};
  int sender_fd = pfds[pfd_i].fd;
  int nbytes = recv(sender_fd, buf, sizeof buf, 0);

  if (nbytes <= 0) {
    if (nbytes == -1) {
      perror("recv");
    }

    close(sender_fd);
    del_from_pfds(pfds, pfd_i, pfd_count);
  } else {
    printf("received:\n%s", buf);
  }
  return 0;
}

int handle_connections(int listener_fd, ServerConfig const* const sc) {
  struct pollfd *pfds = malloc(sizeof *pfds * sc->num_conns);
  pfds[0].fd = listener_fd;
  pfds[0].events = POLLIN;
  int pfd_count = 1;  
  
  while (1) {
    int poll_count = poll(pfds, pfd_count, sc->timeout);
    if (poll_count == -1) {
      perror("poll");
      return -1;
    }

    for (int i = 0; i < pfd_count; i++) {
      if (pfds[i].revents & POLLIN) {
	if (pfds[i].fd == listener_fd) {
	  handle_listener(&pfd_count, sc->num_conns, pfds, listener_fd);
	} else {
	  handle_client(i, &pfd_count, pfds);
	}
      }
    }
  }
}

int main(int argc, char* argv[argc+1]) {
  ServerConfig sc = {
    .port = "1234",
    .backlog = 10,
    .timeout = 60000,
    .num_conns = 10
  };

  if (read_options(argc, argv, &sc) == -1) {
    exit(1);
  }

  int listener_fd = get_listener_socket(&sc);
  if (listener_fd == -1) {
    exit(1);
  }

  if (handle_connections(listener_fd, &sc) == -1) {
    exit(1);
  }

  return 0;
}
