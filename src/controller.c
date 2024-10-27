/*
 * controller.c - Air Traffic Control Controller Node
 */

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include "airport.h"
#include "network_utils.h" // Include necessary network utilities

#define PORT_STRLEN 6
#define DEFAULT_PORTNUM 1024
#define MIN_PORTNUM 1024
#define MAX_PORTNUM 65535

/** Struct that contains information associated with each airport node. */
typedef struct airport_node_info {
  int id;    /* Airport identifier */
  int port;  /* Port num associated with this airport's listening socket */
  pid_t pid; /* PID of the child process for this airport. */
} node_info_t;

/** Struct that contains parameters for the controller node and ATC network as
 *  a whole. */
typedef struct controller_params_t {
  int listenfd;               /* file descriptor of the controller listening socket */
  int portnum;                /* port number used to connect to the controller */
  int num_airports;           /* number of airports to create */
  int *gate_counts;           /* array containing the number of gates in each airport */
  node_info_t *airport_nodes; /* array of info associated with each airport */
} controller_params_t;

controller_params_t ATC_INFO;

/* Thread pool definitions */
#define THREAD_POOL_SIZE 4
#define QUEUE_SIZE 100

typedef struct request_queue_t {
    int connections[QUEUE_SIZE];
    int front;
    int rear;
    int count;
    pthread_mutex_t mutex;
    pthread_cond_t not_empty;
    pthread_cond_t not_full;
} request_queue_t;

static request_queue_t request_queue;

/* Initialize the request queue */
void init_request_queue(request_queue_t *q) {
    q->front = 0;
    q->rear = 0;
    q->count = 0;
    pthread_mutex_init(&q->mutex, NULL);
    pthread_cond_init(&q->not_empty, NULL);
    pthread_cond_init(&q->not_full, NULL);
}

/* Enqueue a connection */
void enqueue_request(request_queue_t *q, int connfd) {
    pthread_mutex_lock(&q->mutex);
    while (q->count == QUEUE_SIZE) {
        pthread_cond_wait(&q->not_full, &q->mutex);
    }
    q->connections[q->rear] = connfd;
    q->rear = (q->rear + 1) % QUEUE_SIZE;
    q->count++;
    pthread_cond_signal(&q->not_empty);
    pthread_mutex_unlock(&q->mutex);
}

/* Dequeue a connection */
int dequeue_request(request_queue_t *q) {
    pthread_mutex_lock(&q->mutex);
    while (q->count == 0) {
        pthread_cond_wait(&q->not_empty, &q->mutex);
    }
    int connfd = q->connections[q->front];
    q->front = (q->front + 1) % QUEUE_SIZE;
    q->count--;
    pthread_cond_signal(&q->not_full);
    pthread_mutex_unlock(&q->mutex);
    return connfd;
}

/* Worker thread function */
void *controller_worker(void *arg) {
    while (1) {
        int connfd = dequeue_request(&request_queue);
        // Handle the connection
        rio_t rio_client, rio_airport;
        char buf[MAXLINE], response[MAXLINE];

        rio_readinitb(&rio_client, connfd);

        while (1) {
            ssize_t n = rio_readlineb(&rio_client, buf, MAXLINE);
            if (n <= 0) {
                break; // Client closed the connection or error occurred
            }

            // Parse the request
            char request_type[MAXLINE];
            int airport_num;
            char rest_of_request[MAXLINE] = {0}; // Initialize to empty string

            int num_parsed = sscanf(buf, "%s %d %[^\n]", request_type, &airport_num, rest_of_request);

            // Initial validation: at least command and airport_num should be present
            if (num_parsed < 2) {
                sprintf(response, "Error: Invalid request provided\n");
                rio_writen(connfd, response, strlen(response));
                continue;
            }

            // Further validation based on request type
            int valid_request = 1; // Flag to determine if request is valid
            int expected_response_lines = 1; // Default

            if (strcmp(request_type, "SCHEDULE") == 0) {
                int plane_id, earliest_time, duration, fuel;
                // Expecting 4 additional arguments
                if (sscanf(rest_of_request, "%d %d %d %d", &plane_id, &earliest_time, &duration, &fuel) != 4) {
                    valid_request = 0;
                }
            } else if (strcmp(request_type, "TIME_STATUS") == 0) {
                int gate_num, start_idx, duration;
                // Expecting 3 additional arguments
                if (sscanf(rest_of_request, "%d %d %d", &gate_num, &start_idx, &duration) != 3) {
                    valid_request = 0;
                } else {
                    expected_response_lines = duration + 1; // Inclusive
                }
            } else if (strcmp(request_type, "PLANE_STATUS") == 0) {
                int plane_id;
                // Expecting 1 additional argument
                if (sscanf(rest_of_request, "%d", &plane_id) != 1) {
                    valid_request = 0;
                }
            } else {
                // Unknown command
                valid_request = 0;
            }

            if (!valid_request) {
                sprintf(response, "Error: Invalid request provided\n");
                rio_writen(connfd, response, strlen(response));
                continue;
            }

            // Validate airport_num
            if (airport_num < 0 || airport_num >= ATC_INFO.num_airports) {
                sprintf(response, "Error: Airport %d does not exist\n", airport_num);
                rio_writen(connfd, response, strlen(response));
                continue;
            }

            // Get the port number of the airport node
            int airport_port = ATC_INFO.airport_nodes[airport_num].port;
            char port_str[PORT_STRLEN];
            int airport_fd;

            snprintf(port_str, PORT_STRLEN, "%d", airport_port);

            if ((airport_fd = open_clientfd("localhost", port_str)) < 0) {
                sprintf(response, "Error: Cannot connect to airport %d\n", airport_num);
                rio_writen(connfd, response, strlen(response));
                continue;
            }

            // Forward the request to the airport node
            rio_writen(airport_fd, buf, n);

            // Initialize Rio for airport_fd
            rio_readinitb(&rio_airport, airport_fd);

            // Read the first response line from the airport node
            ssize_t m = rio_readlineb(&rio_airport, response, MAXLINE);
            if (m <= 0) {
                // If no response, send error
                sprintf(response, "Error: No response from airport %d\n", airport_num);
                rio_writen(connfd, response, strlen(response));
                close(airport_fd);
                continue;
            }

            // Check if the response is an error message
            if (strncmp(response, "Error:", 6) == 0) {
                // Send the error message to the client
                rio_writen(connfd, response, m);
                close(airport_fd);
                continue;
            }

            // If not an error, send the first line and proceed to read the remaining lines
            rio_writen(connfd, response, m);

            // Calculate remaining lines to read
            int remaining_lines = expected_response_lines - 1;
            for (int i = 0; i < remaining_lines; i++) {
                ssize_t m_next = rio_readlineb(&rio_airport, response, MAXLINE);
                if (m_next <= 0) {
                    // If not enough response lines, send error and stop
                    sprintf(response, "Error: Incomplete response from airport %d\n", airport_num);
                    rio_writen(connfd, response, strlen(response));
                    break;
                }
                rio_writen(connfd, response, m_next);
            }

            close(airport_fd);
        }

        close(connfd);
    }
    return NULL;
}


/** @brief The main server loop of the controller.
 *
 *  @todo  Implement this function!
 */
void controller_server_loop(void) {
    int listenfd = ATC_INFO.listenfd;
    int connfd;
    socklen_t clientlen;
    struct sockaddr_storage clientaddr;

    // Initialize the request queue
    init_request_queue(&request_queue);

    // Create worker threads
    pthread_t threads[THREAD_POOL_SIZE];
    for (int i = 0; i < THREAD_POOL_SIZE; i++) {
        if (pthread_create(&threads[i], NULL, controller_worker, NULL) != 0) {
            perror("pthread_create");
            exit(1);
        }
        pthread_detach(threads[i]); // Detached mode
    }

    while (1) {
        clientlen = sizeof(clientaddr);
        if ((connfd = accept(listenfd, (SA *)&clientaddr, &clientlen)) < 0) {
            perror("accept");
            continue;
        }

        // Enqueue the connection for worker threads to handle
        enqueue_request(&request_queue, connfd);
    }
}

/** @brief A handler for reaping child processes (individual airport nodes).
 *         It may be helpful to set a breakpoint here when trying to debug
 *         issues that cause your airport nodes to crash.
 */
void sigchld_handler(int sig) {
  while (waitpid(-1, 0, WNOHANG) > 0)
    ;
  return;
}

/** You should not modify any of the functions below this point, nor should you
 *  call these functions from anywhere else in your code. These functions are
 *  used to handle the initial setup of the Air Traffic Control system.
 */

/** @brief This function spawns child processes for each airport node, and
 *         opens a listening socket for the controller to use.
 */
void initialise_network(void) {
  char port_str[PORT_STRLEN];
  int num_airports = ATC_INFO.num_airports;
  int lfd, idx, port_num = ATC_INFO.portnum;
  node_info_t *node;
  pid_t pid;

  snprintf(port_str, PORT_STRLEN, "%d", port_num);
  if ((ATC_INFO.listenfd = open_listenfd(port_str)) < 0) {
    perror("[Controller] open_listenfd");
    exit(1);
  }

  for (idx = 0; idx < num_airports; idx++) {
    node = &ATC_INFO.airport_nodes[idx];
    node->id = idx;
    node->port = ++port_num;
    snprintf(port_str, PORT_STRLEN, "%d", port_num);
    if ((lfd = open_listenfd(port_str)) < 0) {
      perror("open_listenfd");
      continue;
    }
    if ((pid = fork()) == 0) {
      close(ATC_INFO.listenfd);
      initialise_node(idx, ATC_INFO.gate_counts[idx], lfd);
      exit(0);
    } else if (pid < 0) {
      perror("fork");
    } else {
      node->pid = pid;
      fprintf(stderr, "[Controller] Airport %d assigned port %s\n", idx, port_str);
      close(lfd);
    }
  }

  signal(SIGCHLD, sigchld_handler);
  controller_server_loop();
  exit(0);
}

/** @brief Prints usage information for the program and then exits. */
void print_usage(char *program_name) {
  printf("Usage: %s [-n N] [-p P] -- [gate count list]\n", program_name);
  printf("  -n: Number of airports to create.\n");
  printf("  -p: Port number to use for controller.\n");
  printf("  -h: Print this help message and exit.\n");
  exit(0);
}

/** @brief   Parses the gate counts provided for each airport given as the final
 *           argument to the program.
 *
 *  @param list_arg argument string containing the integer list
 *  @param expected expected number of integer values to read from the list.
 *
 *
 *  @returns An allocated array of gate counts for each airport, or `NULL` if
 *           there was an issue in parsing the gate counts.
 *
 *  @warning If a list of *more* than `expected` integers is given as an argument,
 *           then all integers after the nth are silently ignored.
 */
int *parse_gate_counts(char *list_arg, int expected) {
  int *arr, n = 0, idx = 0;
  char *end, *buff = list_arg;
  if (!list_arg) {
    fprintf(stderr, "Expected gate counts for %d airport nodes.\n", expected);
    return NULL;
  }
  end = list_arg + strlen(list_arg);
  arr = calloc(1, sizeof(int) * (unsigned)expected);
  if (arr == NULL)
    return NULL;

  while (buff < end && idx < expected) {
    if (sscanf(buff, "%d%n%*c%n", &arr[idx++], &n, &n) != 1) {
      break;
    } else {
      buff += n;
    }
  }

  if (idx < expected) {
    fprintf(stderr, "Expected %d gate counts, got %d instead.\n", expected, idx);
    free(arr);
    arr = NULL;
  }

  return arr;
}

/** @brief Parses and validates the arguments used to create the Air Traffic
 *         Control Network. If successful, the `ATC_INFO` variable will be
 *         initialised.
 */
int parse_args(int argc, char *argv[]) {
  int c, ret = 0, *gate_counts = NULL;
  int atc_portnum = DEFAULT_PORTNUM;
  int num_airports = 0;
  int max_portnum = MAX_PORTNUM;

  while ((c = getopt(argc, argv, "n:p:h")) != -1) {
    switch (c) {
    case 'n':
      sscanf(optarg, "%d", &num_airports);
      max_portnum -= num_airports;
      break;
    case 'p':
      sscanf(optarg, "%d", &atc_portnum);
      break;
    case 'h':
      print_usage(argv[0]);
      break;
    case '?':
      fprintf(stderr, "Unknown Option provided: %c\n", optopt);
      ret = -1;
    default:
      break;
    }
  }

  if (num_airports <= 0) {
    fprintf(stderr, "-n must be greater than 0.\n");
    ret = -1;
  }
  if (atc_portnum < MIN_PORTNUM || atc_portnum >= max_portnum) {
    fprintf(stderr, "-p must be between %d-%d.\n", MIN_PORTNUM, max_portnum);
    ret = -1;
  }

  if (ret >= 0) {
    if ((gate_counts = parse_gate_counts(argv[optind], num_airports)) == NULL)
      return -1;
    ATC_INFO.num_airports = num_airports;
    ATC_INFO.gate_counts = gate_counts;
    ATC_INFO.portnum = atc_portnum;
    ATC_INFO.airport_nodes = calloc((unsigned)num_airports, sizeof(node_info_t));
  }

  return ret;
}

int main(int argc, char *argv[]) {
  if (parse_args(argc, argv) < 0)
    return 1;
  initialise_network();
  return 0;
}
