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

typedef struct Header Header;
struct Header {
  char* name;
  char* value;
};

typedef enum HttpToken HttpToken;
enum HttpToken {GET, HEAD};

typedef enum HttpVersion HttpVersion;
enum HttpVersion {HTTP1_1};

typedef struct HttpRequest HttpRequest;
struct HttpRequest {
  HttpToken token;
  char* target;
  HttpVersion version;
  Header* headers;
  char* body;
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

int parse_request_line(char req_line[], HttpRequest* hr) {
  char* end_token = strchr(req_line, ' ');
  if (end_token == NULL) {
    return -1;
  }
  
  int token_size = end_token - req_line;
  if (token_size == 3 && strncmp(req_line, "GET", token_size) == 0) {
    hr->token = GET;
  } else if (token_size == 4 && strncmp(req_line, "HEAD", token_size) == 0) {
    hr->token = HEAD;
  } else {
    return -1;
  }

  char* start_target = end_token + 1;
  char* end_target = strchr(start_target, ' ');
  if (end_target == NULL) {
    return -1;
  }

  int target_size = end_target - start_target;
  hr->target = malloc((target_size + 1) * sizeof(char));
  strncpy(hr->target, start_target, target_size);
  hr->target[target_size] = '\0';

  char* start_version = end_target + 1;
  int version_size = strlen(start_version);
  if (version_size == 8 && strcmp(start_version, "HTTP/1.1") == 0) {
    hr->version = HTTP1_1;
  } else {
    return -1;
  }

  printf("token: %d\ntarget: %s\nversion: %d\n", hr->token, hr->target, hr->version);
  
  return 0;
}

int parse_http_request(char req[], HttpRequest* hr) {
  char* end_request_line = strstr(req, "\r\n");
  if (end_request_line == NULL) {
    return -1;
  }
  int request_line_size = end_request_line - req;
  char request_line[request_line_size + 1];
  strncpy(request_line, req, request_line_size);
  request_line[request_line_size] = '\0';

  if (parse_request_line(request_line, hr) == -1) {
    return -1;
  }
      
}

int free_http_request(HttpRequest* hr) {
  free(hr->target);
  free(hr->body);
}

int get_response(char req[], char resp[] ) {
  HttpRequest hr = {0};
  
  if (parse_http_request(req, &hr) == -1) {
    return -1;
  }
  free_http_request(&hr);
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
    get_response(buf, NULL);
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
