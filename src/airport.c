#include "airport.h"

/** This is the main file in which you should implement the airport server code.
 *  There are many functions here which are pre-written for you. You should read
 *  the comments in the corresponding `airport.h` header file to understand what
 *  each function does, the arguments they accept and how they are intended to
 *  be used.
 *
 *  You are encouraged to implement your own helper functions to handle requests
 *  in airport nodes in this file. You are also permitted to modify the
 *  functions you have been given if needed.
 */

/* This will be set by the `initialise_node` function. */
static int AIRPORT_ID = -1;

/* This will be set by the `initialise_node` function. */
static airport_t *AIRPORT_DATA = NULL;

/* thread pool def'ns */
#define THREAD_POOL_SIZE 4
#define QUEUE_SIZE 100

// Data structure for connec'n queue and initialisation
typedef struct conn_queue_t {
    int connections[QUEUE_SIZE];
    int front;
    int rear;
    int count;
    pthread_mutex_t mutex;
    pthread_cond_t not_empty;
    pthread_cond_t not_full;
} conn_queue_t;

static conn_queue_t conn_queue;

void init_queue(conn_queue_t *q) {
    q->front = 0;
    q->rear = 0;
    q->count = 0;
    pthread_mutex_init(&q->mutex, NULL);
    pthread_cond_init(&q->not_empty, NULL);
    pthread_cond_init(&q->not_full, NULL);
}

/* Enqueue a connection */
void enqueue(conn_queue_t *q, int connfd) {
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
int dequeue(conn_queue_t *q) {
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

time_info_t schedule_plane(int plane_id, int start, int duration, int fuel) {
  time_info_t result = {-1, -1, -1};
  gate_t *gate;
  int gate_idx, slot;
  for (gate_idx = 0; gate_idx < AIRPORT_DATA->num_gates; gate_idx++) {
    gate = get_gate_by_idx(gate_idx);
    // Lock the gate before attempting to assign -- Individual Gate Locking
    pthread_mutex_lock(&gate->gate_lock);
    if ((slot = assign_in_gate(gate, plane_id, start, duration, fuel)) >= 0) {
      result.start_time = slot;
      result.gate_number = gate_idx;
      result.end_time = slot + duration;
      pthread_mutex_unlock(&gate->gate_lock);
      break;
    }
    pthread_mutex_unlock(&gate->gate_lock);
  }
  return result;
}

/* Worker thread function intended to handle client requests */
void *worker_thread(void *arg) {
    while (1) {
        int connfd = dequeue(&conn_queue);
        // Handle the connection
        rio_t rio_client, rio_airport;
        char buf[MAXLINE], response[MAXLINE];

        rio_readinitb(&rio_client, connfd);//rio initialisation

        while (1) {
            ssize_t n = rio_readlineb(&rio_client, buf, MAXLINE); //reading a line
            if (n <= 0) {
                break; // No input
            }

            // Parsing logic
            char request_type[MAXLINE];
            int airport_num;
            char rest_of_request[MAXLINE] = {0}; // empty string

            int num_parsed = sscanf(buf, "%s %d %[^\n]", request_type, &airport_num, rest_of_request);

            // Initial validation: queries without command and airport_num are pre-invalidated.
            if (num_parsed < 2) {
                sprintf(response, "Error: Invalid request provided\n");
                rio_writen(connfd, response, strlen(response));
                continue;
            }

            // Valid airport_num error handling
            if (airport_num != AIRPORT_ID) {
                sprintf(response, "Error: Airport %d does not exist\n", airport_num);
                rio_writen(connfd, response, strlen(response));
                continue;
            }

            //SCHEDULE command error handling
            if (strcmp(request_type, "SCHEDULE") == 0) {
                int plane_id, earliest_time, duration, fuel;
                //not enough arguments 
                if (sscanf(rest_of_request, "%d %d %d %d", &plane_id, &earliest_time, &duration, &fuel) != 4) {
                    sprintf(response, "Error: Invalid request provided\n");
                    rio_writen(connfd, response, strlen(response));
                    continue;
                }
                // Invalid earliest time error
                if (earliest_time < 0 || earliest_time >= NUM_TIME_SLOTS) {
                    sprintf(response, "Error: Invalid 'earliest' time (%d)\n", earliest_time);
                    rio_writen(connfd, response, strlen(response));
                    continue;
                }
                // Invalid duration errors - 2
                if (duration < 0) {
                    sprintf(response, "Error: Invalid 'duration' value (%d)\n", duration);
                    rio_writen(connfd, response, strlen(response));
                    continue;
                }
                if (earliest_time + duration > NUM_TIME_SLOTS) {
                    sprintf(response, "Error: Invalid 'duration' value (%d)\n", duration);
                    rio_writen(connfd, response, strlen(response));
                    continue;
                }

                // Schedule the plane
                time_info_t result = schedule_plane(plane_id, earliest_time, duration, fuel);

                //Successful SCHEDULED command response
                if (result.gate_number >= 0) {
                    int start_time = result.start_time;
                    int end_time = result.end_time;
                    int gate_num = result.gate_number;

                    // time to string conversion code as given in assignment spec
                    int start_hour = IDX_TO_HOUR(start_time);
                    int start_mins = (int)IDX_TO_MINS(start_time);
                    int end_hour = IDX_TO_HOUR(end_time);
                    int end_mins = (int)IDX_TO_MINS(end_time);

                    sprintf(response, "SCHEDULED %d at GATE %d: %02d:%02d-%02d:%02d\n",
                            plane_id, gate_num,
                            start_hour, start_mins,
                            end_hour, end_mins);
                } else {
                  //Unsuccessful error
                    sprintf(response, "Error: Cannot schedule %d\n", plane_id);
                }
                rio_writen(connfd, response, strlen(response));

            } //PLANE_STATUS command validation
            else if (strcmp(request_type, "PLANE_STATUS") == 0) {
                int plane_id;
                //not enough arguments 
                if (sscanf(rest_of_request, "%d", &plane_id) != 1) {
                    sprintf(response, "Error: Invalid request provided\n");
                    rio_writen(connfd, response, strlen(response));
                    continue;
                }
                //plane lookup
                time_info_t result = lookup_plane_in_airport(plane_id);

                //plane found
                if (result.gate_number >= 0) {
                    int start_time = result.start_time;
                    int end_time = result.end_time;
                    int gate_num = result.gate_number;

                    // time to string conversion code as given in assignment spec
                    int start_hour = IDX_TO_HOUR(start_time);
                    int start_mins = (int)IDX_TO_MINS(start_time);
                    int end_hour = IDX_TO_HOUR(end_time);
                    int end_mins = (int)IDX_TO_MINS(end_time);

                    sprintf(response, "PLANE %d scheduled at GATE %d: %02d:%02d-%02d:%02d\n",
                            plane_id, gate_num,
                            start_hour, start_mins,
                            end_hour, end_mins);
                } else {
                  //plane not found
                    sprintf(response, "PLANE %d not scheduled at airport %d\n", plane_id, AIRPORT_ID);
                }
                rio_writen(connfd, response, strlen(response));

            } //TIME_STATUS command handling 
            else if (strcmp(request_type, "TIME_STATUS") == 0) {
                int gate_num, start_idx, duration;
                //not enough arguments 
                if (sscanf(rest_of_request, "%d %d %d", &gate_num, &start_idx, &duration) != 3) {
                    sprintf(response, "Error: Invalid request provided\n");
                    rio_writen(connfd, response, strlen(response));
                    continue;
                }

                // Invalid gate_num error
                if (gate_num < 0 || gate_num >= AIRPORT_DATA->num_gates) {
                    sprintf(response, "Error: Invalid 'gate' value (%d)\n", gate_num);
                    rio_writen(connfd, response, strlen(response));
                    continue;
                }
                // Invalid start_idx error
                if (start_idx < 0 || start_idx >= NUM_TIME_SLOTS) {
                    sprintf(response, "Error: Invalid 'start' time (%d)\n", start_idx);
                    rio_writen(connfd, response, strlen(response));
                    continue;
                }
                // Invalid duration errors - 2
                if (duration < 0) {
                    sprintf(response, "Error: Invalid 'duration' value (%d)\n", duration);
                    rio_writen(connfd, response, strlen(response));
                    continue;
                }
                if (start_idx + duration >= NUM_TIME_SLOTS) {
                    sprintf(response, "Error: Invalid 'duration' value (%d)\n", duration);
                    rio_writen(connfd, response, strlen(response));
                    continue;
                }

                //get gate
                gate_t *gate = get_gate_by_idx(gate_num);
                if (gate == NULL) {
                  //wrong gate error
                    sprintf(response, "Error: Invalid 'gate' value (%d)\n", gate_num);
                    rio_writen(connfd, response, strlen(response));
                    continue;
                }

                int end_idx = start_idx + duration;

                // Locking the specific gate before accessing its schedule
                pthread_mutex_lock(&gate->gate_lock);

                for (int idx = start_idx; idx <= end_idx; idx++) {
                    time_slot_t *ts = get_time_slot_by_idx(gate, idx);
                    if (ts == NULL) {
                        continue; // Skipping invalid time slots
                    }
                    char status = ts->status == 1 ? 'A' : 'F';
                    int flight_id = ts->status == 1 ? ts->plane_id : 0;

                    // Cast IDX_TO_HOUR and IDX_TO_MINS to int
                    int current_hour = IDX_TO_HOUR(idx);
                    int current_mins = (int)IDX_TO_MINS(idx);

                    sprintf(response, "AIRPORT %d GATE %d %02d:%02d: %c - %d\n",
                            AIRPORT_ID, gate_num,
                            current_hour, current_mins,
                            status, flight_id);
                    rio_writen(connfd, response, strlen(response));
                }

                // Unlocking the gate after accessing its schedule
                pthread_mutex_unlock(&gate->gate_lock);

            } else {
                sprintf(response, "Error: Invalid request provided\n");
                rio_writen(connfd, response, strlen(response));
            }
        }
        close(connfd);
    }
    return NULL;
}

gate_t *get_gate_by_idx(int gate_idx) {
  if ((gate_idx) < 0 || (gate_idx >= AIRPORT_DATA->num_gates))
    return NULL;
  else
    return &AIRPORT_DATA->gates[gate_idx];
}

time_slot_t *get_time_slot_by_idx(gate_t *gate, int slot_idx) {
  if ((slot_idx < 0) || (slot_idx >= NUM_TIME_SLOTS))
    return NULL;
  else
    return &gate->time_slots[slot_idx];
}

int check_time_slots_free(gate_t *gate, int start_idx, int end_idx) {
  time_slot_t *ts;
  int idx;
  for (idx = start_idx; idx <= end_idx; idx++) {
    ts = get_time_slot_by_idx(gate, idx);
    if (ts->status == 1)
      return 0;
  }
  return 1;
}

int set_time_slot(time_slot_t *ts, int plane_id, int start_idx, int end_idx) {
  if (ts->status == 1)
    return -1;
  ts->status = 1; /* Set to be occupied */
  ts->plane_id = plane_id;
  ts->start_time = start_idx;
  ts->end_time = end_idx;
  return 0;
}

int add_plane_to_slots(gate_t *gate, int plane_id, int start, int count) {
  int ret = 0, end = start + count;
  time_slot_t *ts = NULL;
  for (int idx = start; idx <= end; idx++) {
    ts = get_time_slot_by_idx(gate, idx);
    ret = set_time_slot(ts, plane_id, start, end);
    if (ret < 0) break;
  }
  return ret;
}

int search_gate(gate_t *gate, int plane_id) {
  int idx, next_idx;
  time_slot_t *ts = NULL;
  for (idx = 0; idx < NUM_TIME_SLOTS; idx = next_idx) {
    ts = get_time_slot_by_idx(gate, idx);
    if (ts->status == 0) {
      next_idx = idx + 1;
    } else if (ts->plane_id == plane_id) {
      return idx;
    } else {
      next_idx = ts->end_time + 1;
    }
  }
  return -1;
}

time_info_t lookup_plane_in_airport(int plane_id) {
  time_info_t result = {-1, -1, -1};
  int gate_idx, slot_idx;
  gate_t *gate;
  for (gate_idx = 0; gate_idx < AIRPORT_DATA->num_gates; gate_idx++) {
    gate = get_gate_by_idx(gate_idx);
    // Lock the gate before searching -- Gate wise locking
    pthread_mutex_lock(&gate->gate_lock);
    if ((slot_idx = search_gate(gate, plane_id)) >= 0) {
      result.start_time = slot_idx;
      result.gate_number = gate_idx;
      result.end_time = get_time_slot_by_idx(gate, slot_idx)->end_time;
      pthread_mutex_unlock(&gate->gate_lock);
      break;
    }
    pthread_mutex_unlock(&gate->gate_lock);
  }
  return result;
}

int assign_in_gate(gate_t *gate, int plane_id, int start, int duration, int fuel) {
  int idx, end = start + duration;
  for (idx = start; idx <= (start + fuel) && (end < NUM_TIME_SLOTS); idx++) {
    if (check_time_slots_free(gate, idx, end)) {
      add_plane_to_slots(gate, plane_id, idx, duration);
      return idx;
    }
    end++;
  }
  return -1;
}

airport_t *create_airport(int num_gates) {
  airport_t *data = NULL;
  size_t memsize = 0;
  if (num_gates > 0) {
    memsize = sizeof(airport_t) + (sizeof(gate_t) * (unsigned)num_gates);
    data = calloc(1, memsize);
  }
  if (data) {
    data->num_gates = num_gates;
    // initialising each gate's mutex
    for (int i = 0; i < num_gates; i++) {
      pthread_mutex_init(&data->gates[i].gate_lock, NULL);
    }
  }
  return data;
}

void initialise_node(int airport_id, int num_gates, int listenfd) {
  AIRPORT_ID = airport_id;
  AIRPORT_DATA = create_airport(num_gates);
  if (AIRPORT_DATA == NULL)
    exit(1);

  // initialising the connection queue
  init_queue(&conn_queue);

  // Creating worker threads
  pthread_t threads[THREAD_POOL_SIZE];
  for (int i = 0; i < THREAD_POOL_SIZE; i++) {
    if (pthread_create(&threads[i], NULL, worker_thread, NULL) != 0) {
      perror("pthread_create");
      exit(1);
    }
    pthread_detach(threads[i]); // Detached mode implementation
  }

  airport_node_loop(listenfd);

  // destroying all gate mutexes
  for (int i = 0; i < num_gates; i++) {
    pthread_mutex_destroy(&AIRPORT_DATA->gates[i].gate_lock);
  }

  free(AIRPORT_DATA);
}

void airport_node_loop(int listenfd) {
  int connfd;
  socklen_t clientlen;
  struct sockaddr_storage clientaddr;

  while (1) {
    clientlen = sizeof(clientaddr);
    connfd = accept(listenfd, (SA *)&clientaddr, &clientlen);
    if (connfd < 0) {
      perror("accept");
      continue;
    }
    // Enqueuing the connection for worker threads to handle
    enqueue(&conn_queue, connfd);
  }
}
