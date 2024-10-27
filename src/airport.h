#ifndef AIRPORT_HEADER
#define AIRPORT_HEADER

#include "network_utils.h"
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef ENABLE_LOG
#define LOG(...)                                     \
  do {                                               \
    fprintf(stderr, "[" __FILE__ ":%d] ", __LINE__); \
    fprintf(stderr, __VA_ARGS__);                    \
  } while (0)

#else
#define LOG(...)
#endif

/* Each gate schedules is broken up into 48 half-hour time slots. */
#define NUM_TIME_SLOTS 48

/** Macros to convert an index value to hour/minutes. **/
#define IDX_TO_HOUR(idx) (((idx) >> 1))
#define IDX_TO_MINS(idx) ((idx) & 1 ? 30lu : 0lu)

/** Struct Definitions for airports and their schedules. **/

typedef struct airport_t airport_t;

struct time_slot_t {
  /* If the `status` is 1, this time slot has a flight assigned to this gate. */
  int status;
  /* ID of plane occupying this slot. Should be 0 if this slot is free. */
  int plane_id;
  /* When occupied, this is the index of the time slot in which the plane first
   * landed at this gate. */
  int start_time;
  /* When occupied, this is the index of the time slot in which the plane will
   * leave this gate. */
  int end_time;
};

typedef struct time_slot_t time_slot_t;

/** This `gate_t` structure now includes a mutex for fine-grained locking. */
struct gate_t {
  pthread_mutex_t gate_lock;          // Mutex for this specific gate
  time_slot_t time_slots[NUM_TIME_SLOTS];
};

typedef struct gate_t gate_t;

/** Each airport has a number of gates, and an array of those gate schedules.
 *  @note: This structure definition uses a "flexible array member" to represent
 *         the variable number of gates.
 */
struct airport_t {
  int num_gates;          // Number of gates in this airport
  gate_t gates[];         // Array of each gate.
};

/** This structure is used to represent a (gate index, start time, end time)
 *  triple. This is used as a return value for functions
 */
typedef struct time_info_t time_info_t;

struct time_info_t {
  int gate_number;
  int start_time;
  int end_time;
};

/** Helper functions and macros defined for you to use. **/

/** @brief Allocates sufficient memory for an airport struct containing all
 *         information needed in an individual airport node.
 *
 *  @param num_gates the number of gates to create in this airport node.
 *
 *  @returns A pointer to an `airport_t` structure.
 *
 *  @note Certain extension tasks (such as introducing multithreading) may
 *  require you to modify this function.
 */
airport_t *create_airport(int num_gates);

/** @brief This function is called after forking a child process to instantiate
 *         and run an individual airport node.
 *
 *  @param airport_id The identifier of this airport node.
 *  @param num_gates  The number of gates associated with this airport
 *  @param listenfd   The listening socket this airport will use to accept
 *                    connections from the controller.
 *
 *  @note If any step of the initialisation fails, the subprocess of the airport
 *        node will exit with return code 1.
 */
void initialise_node(int airport_id, int num_gates, int listenfd);

/** The following functions all require the airport to be instantiated  */

/** @brief Returns a pointer to the `gate_idx`th gate schedule of the "global"
 *         airport struct. Returns `NULL` if `gate_idx` out of range.
 */
gate_t *get_gate_by_idx(int gate_idx);

/** @brief Returns a pointer to the `slot_idx`th time slot in a gate. If the
 *         given `slot_idx` is out of range, returns NULL.
 */
time_slot_t *get_time_slot_by_idx(gate_t *gate, int slot_idx);

/** @brief  Checks whether the time slots of a given gate in the range
 *          `[start_idx]..[end_idx]` (inclusive) are all currently unoccupied.
 *
 *  @return 1 if ALL slots are unoccupied, 0 if not.
 */
int check_time_slots_free(gate_t *gate, int start_idx, int end_idx);

/** @brief Instantiates the members of the time slot pointed to by `ts` based
 *         on the `plane_id`, `start_idx` and `end_idx` arguments. If the time
 *         slot is already marked as occupied, this function returns `-1` and
 *         the values in the time slot are not modified.
 */
int set_time_slot(time_slot_t *ts, int plane_id, int start_idx, int end_idx);

/** @brief   Marks the time slots `[start]..[start_count]` (inclusive) of the
 *           given `gate` as occupied by a plane.
 *
 *  @returns `0` if all time slots successfully set, `-1` if there was an issue
 *           assigning any of the time slots.
 *
 *  @warning In the case of a "partial success" where assigning the first `n`
 *           slots was successful, but the `n+1`th was already occupied, this
 *           function will return -1 to indicate an error, but any successfully
 *           updated time slots will remain modified. It is a good idea to call
 *           `check_time_slots_free` prior to calling this function.
 */
int add_plane_to_slots(gate_t *gate, int plane_id, int start, int count);

/** @brief   Searches the given `gate` for a time slot assigned to `plane_id`.
 *
 *  @returns The index in the gate schedule at which the given `plane_id` first
 *           appears, or -1 if the plane is not scheduled in this gate.
 */
int search_gate(gate_t *gate, int plane_id);

/** @brief   Searches through each gate of the airport for information about
 *           when a flight given by `plane_id` is
 *
 *  @returns A `time_info_t` structure that contains the gate number and start
 *           time for a given plane id. If the plane_id is not found anywhere in
 *           this airport, then each value in the struct will be set to `-1`.
 * */
time_info_t lookup_plane_in_airport(int plane_id);

/** @brief   Attempt to assign the given flight in this `gate`, based on its
 *           required parameters (earliest landing time, duration of time to
 *           remain in the gate, remaining fuel).
 *
 *           If this function returns an index >= 0, this means the schedule was
 *           updated so that each time slot in range `[start]..[start+duration]`
 *           (inclusive) was updated to contain this flight information.
 *
 *  @param gate     Pointer to the gate that the plane is added to.
 *  @param plane_id Plane identifier
 *  @param start    Earliest time slot index this flight can be placed in
 *  @param duration Number of time slots this flight will remain in the gate for
 *  @param fuel     Indicator for the remaining fuel this plane has.
 *
 *  @note    There are certain guarantees this function makes for the
 *           relationship between the assigned start time and the given
 *           parameters:
 *
 *           - `assigned >= start`
 *
 *           - `assigned + duration < NUM_TIME_SLOTS`
 *
 *           - `assigned + start <= fuel`
 *
 *  @returns The starting time index this plane was assigned to, or -1 if the
 *           plane could not be assigned any slot in this gate.
 */
int assign_in_gate(gate_t *gate, int plane_id, int start, int duration, int fuel);

/** @brief  The main server loop for an individual airport node.
 *
 *  @todo  Implement this function!
 *
 *  @param listenfd This is the file descriptor of the listening socket for the
 *         airport to use when awaiting connections from the main controller
 *         node.
 */
void airport_node_loop(int listenfd);

#endif
