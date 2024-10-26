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
    if ((slot_idx = search_gate(gate, plane_id)) >= 0) {
      result.start_time = slot_idx;
      result.gate_number = gate_idx;
      result.end_time = get_time_slot_by_idx(gate, slot_idx)->end_time;
      break;
    }
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

time_info_t schedule_plane(int plane_id, int start, int duration, int fuel) {
  time_info_t result = {-1, -1, -1};
  gate_t *gate;
  int gate_idx, slot;
  for (gate_idx = 0; gate_idx < AIRPORT_DATA->num_gates; gate_idx++) {
    gate = get_gate_by_idx(gate_idx);
    if ((slot = assign_in_gate(gate, plane_id, start, duration, fuel)) >= 0) {
      result.start_time = slot;
      result.gate_number = gate_idx;
      result.end_time = slot + duration;
      break;
    }
  }
  return result;
}

airport_t *create_airport(int num_gates) {
  airport_t *data = NULL;
  size_t memsize = 0;
  if (num_gates > 0) {
    memsize = sizeof(airport_t) + (sizeof(gate_t) * (unsigned)num_gates);
    data = calloc(1, memsize);
  }
  if (data)
    data->num_gates = num_gates;
  return data;
}

void initialise_node(int airport_id, int num_gates, int listenfd) {
  AIRPORT_ID = airport_id;
  AIRPORT_DATA = create_airport(num_gates);
  if (AIRPORT_DATA == NULL)
    exit(1);
  airport_node_loop(listenfd);
}

void airport_node_loop(int listenfd) {
  int connfd;
  socklen_t clientlen;
  struct sockaddr_storage clientaddr;
  rio_t rio;
  char buf[MAXLINE], response[MAXLINE];

  while (1) {
    clientlen = sizeof(clientaddr);
    connfd = accept(listenfd, (SA *)&clientaddr, &clientlen);
    if (connfd < 0) {
      perror("accept");
      continue;
    }

    rio_readinitb(&rio, connfd);

    // Read one request
    ssize_t n = rio_readlineb(&rio, buf, MAXLINE);
    if (n <= 0) {
      close(connfd);
      continue; // Client closed the connection or error occurred
    }

    // Parse the request
    char request_type[MAXLINE];
    int airport_num;
    char rest_of_request[MAXLINE];

    int num_parsed = sscanf(buf, "%s %d %[^\n]", request_type, &airport_num, rest_of_request);
    if (num_parsed < 2) {
      sprintf(response, "Error: Invalid request provided\n");
      rio_writen(connfd, response, strlen(response));
      close(connfd);
      continue;
    }

    // Validate airport_num
    if (airport_num != AIRPORT_ID) {
      sprintf(response, "Error: Airport %d does not exist\n", airport_num);
      rio_writen(connfd, response, strlen(response));
      close(connfd);
      continue;
    }

    if (strcmp(request_type, "SCHEDULE") == 0) {
      int plane_id, earliest_time, duration, fuel;
      if (sscanf(rest_of_request, "%d %d %d %d", &plane_id, &earliest_time, &duration, &fuel) != 4) {
        sprintf(response, "Error: Invalid request provided\n");
        rio_writen(connfd, response, strlen(response));
        close(connfd);
        continue;
      }

      // Input validation
      if (earliest_time < 0 || earliest_time >= NUM_TIME_SLOTS) {
        sprintf(response, "Error: Invalid 'earliest' time (%d)\n", earliest_time);
        rio_writen(connfd, response, strlen(response));
        close(connfd);
        continue;
      }
      if (duration < 0) {
        sprintf(response, "Error: Invalid 'duration' value (%d)\n", duration);
        rio_writen(connfd, response, strlen(response));
        close(connfd);
        continue;
      }
      if (earliest_time + duration >= NUM_TIME_SLOTS) {
        sprintf(response, "Error: Invalid 'duration' value (%d)\n", duration);
        rio_writen(connfd, response, strlen(response));
        close(connfd);
        continue;
      }
      if (fuel < 0) {
        sprintf(response, "Error: Invalid 'fuel' value (%d)\n", fuel);
        rio_writen(connfd, response, strlen(response));
        close(connfd);
        continue;
      }
      if (earliest_time + fuel >= NUM_TIME_SLOTS) {
        sprintf(response, "Error: Invalid 'fuel' value (%d)\n", fuel);
        rio_writen(connfd, response, strlen(response));
        close(connfd);
        continue;
      }

      // Schedule the plane
      time_info_t result = schedule_plane(plane_id, earliest_time, duration, fuel);

      if (result.gate_number >= 0) {
        int start_time = result.start_time;
        int end_time = result.end_time;
        int gate_num = result.gate_number;

        // Cast IDX_TO_HOUR and IDX_TO_MINS to int
        int start_hour = IDX_TO_HOUR(start_time);
        int start_mins = (int)IDX_TO_MINS(start_time);
        int end_hour = IDX_TO_HOUR(end_time);
        int end_mins = (int)IDX_TO_MINS(end_time);

        sprintf(response, "SCHEDULED %d at GATE %d: %02d:%02d-%02d:%02d\n",
                plane_id, gate_num,
                start_hour, start_mins,
                end_hour, end_mins);
      } else {
        sprintf(response, "Error: Cannot schedule %d\n", plane_id);
      }
      rio_writen(connfd, response, strlen(response));

    } else if (strcmp(request_type, "PLANE_STATUS") == 0) {
      int plane_id;
      if (sscanf(rest_of_request, "%d", &plane_id) != 1) {
        sprintf(response, "Error: Invalid request provided\n");
        rio_writen(connfd, response, strlen(response));
        close(connfd);
        continue;
      }

      time_info_t result = lookup_plane_in_airport(plane_id);

      if (result.gate_number >= 0) {
        int start_time = result.start_time;
        int end_time = result.end_time;
        int gate_num = result.gate_number;

        // Cast IDX_TO_HOUR and IDX_TO_MINS to int
        int start_hour = IDX_TO_HOUR(start_time);
        int start_mins = (int)IDX_TO_MINS(start_time);
        int end_hour = IDX_TO_HOUR(end_time);
        int end_mins = (int)IDX_TO_MINS(end_time);

        sprintf(response, "PLANE %d scheduled at GATE %d: %02d:%02d-%02d:%02d\n",
                plane_id, gate_num,
                start_hour, start_mins,
                end_hour, end_mins);
      } else {
        sprintf(response, "PLANE %d not scheduled at airport %d\n", plane_id, AIRPORT_ID);
      }
      rio_writen(connfd, response, strlen(response));

    } else if (strcmp(request_type, "TIME_STATUS") == 0) {
      int gate_num, start_idx, duration;
      if (sscanf(rest_of_request, "%d %d %d", &gate_num, &start_idx, &duration) != 3) {
        sprintf(response, "Error: Invalid request provided\n");
        rio_writen(connfd, response, strlen(response));
        close(connfd);
        continue;
      }

      // Validate gate_num
      if (gate_num < 0 || gate_num >= AIRPORT_DATA->num_gates) {
        sprintf(response, "Error: Invalid 'gate' value (%d)\n", gate_num);
        rio_writen(connfd, response, strlen(response));
        close(connfd);
        continue;
      }

      // Validate start_idx and duration
      if (start_idx < 0 || start_idx >= NUM_TIME_SLOTS) {
        sprintf(response, "Error: Invalid 'start' time (%d)\n", start_idx);
        rio_writen(connfd, response, strlen(response));
        close(connfd);
        continue;
      }
      if (duration < 0) {
        sprintf(response, "Error: Invalid 'duration' value (%d)\n", duration);
        rio_writen(connfd, response, strlen(response));
        close(connfd);
        continue;
      }
      if (start_idx + duration >= NUM_TIME_SLOTS) {
        sprintf(response, "Error: Invalid 'duration' value (%d)\n", duration);
        rio_writen(connfd, response, strlen(response));
        close(connfd);
        continue;
      }

      gate_t *gate = get_gate_by_idx(gate_num);
      if (gate == NULL) {
        sprintf(response, "Error: Invalid 'gate' value (%d)\n", gate_num);
        rio_writen(connfd, response, strlen(response));
        close(connfd);
        continue;
      }

      int end_idx = start_idx + duration;
      for (int idx = start_idx; idx <= end_idx; idx++) {
        time_slot_t *ts = get_time_slot_by_idx(gate, idx);
        if (ts == NULL) {
          continue; // Skip invalid time slots
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

    } else {
      sprintf(response, "Error: Invalid request provided\n");
      rio_writen(connfd, response, strlen(response));
    }

    // Close the connection after processing the request
    close(connfd);
  }
}
