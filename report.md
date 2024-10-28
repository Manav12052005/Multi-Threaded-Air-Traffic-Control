# Report
## Overview
This report describes a sample implementation for an ATC system for Canberra that utilises Multi-Threaded and Fine-Grained Locking strategies for communication between Canberra airport and a distributed network of airport servers. 
The main ATC process is responsible for launching these airport servers for this implementation. 

Controller server:
- Acts as the proxy between clients and the airport nodes. 
- It receives client requests:
    - Scheduling flight landings.
    - Querying flight informaton.
- It listens for incoming client communication on a designated port.
- It utilises a thread pool to handle multiple connections concurrently.
    - This is done to reduce overhead.

Airport Node servers:
- Have respective gate schedules.
- Three types of requests
    - SCHEDULE
    - PLANE_STATUS
    - TIME_STATUS
- Thread Safety Mechanism - Fine Grained Locking System
    - Each gate has an associated Mutex Lock.
    - Requests or queries on gate schedules lock the corresponding gate.
    - This is done to eliminate simultaneous conflicting operations.

### Request Handling and Forwarding -

Parsing:
- The sscanf function extracts command type and associated parameters.
- Validates request based on appropriate number of arguments.
- Error message produced for invalid commands.

Forwarding:
- Controller established connection to airport node using pre-assigned port number.
- The entire request string is forwarded verbatim.

## Request Format
- Forwarding Strategy:
    
    In this implementation the request string is directly transferred over to the airport node by the controller node. No changes or optimisations are applied to the string.
- Justifications:
    - Debugging:

        Retaining attributes such as airport_num aid in verifying if requests have been correctly routed and their path is easily traceable if this attribute is still present in the request string.
    - Maintainability:
        
        As both the controller server and airport server are implemented on the same request formatting protocol, the potential for mismatches or errors in request-handling are reduced. The uniformity allows for more scalability and easy maintenance.

## Extensions

### Multithreading within the Controller Node 
Overview
- Allows for multiple client connections concurrently.
- Reduces Latency.
- Improved overall throughput.
 
#### Thread pool implementation

Number of threads chosen = 4

Justification:
- Only one was necessary but as tests had 2 clients I used 2.
- More than 4 instances of thread utilisation force queuing up in the request queue.
- Did not use more than 4 as it takes up unnecessary CPU space.
- For an airport application, research suggests we would require dozens as many more clients such as weather and meteorology information would also need to be taken into account.

Request Queue:
- A bounded queue of size 100 is used to store incoming client connfd's.
- Serves as the central point of 
    - Enqueing by main thread.
    - Dequeing by worker threads.
- The [request_queue_t](https://gitlab.cecs.anu.edu.au/u7782612/comp2310-2024-assignment-2/-/blob/485f07c0e0f2cbb98b7ec03b7e23c47898c02fe5/src/controller.c#L44-52) structure contains:
    - Queue state
    - front and rear index 
    - current connection count
    - mutex and condition variables.

Worker Threads:
- A fixed number is spawned during initialisation.
- Each worker continously waits for available connections in the request queue.
- Following dequeing, a worker thread processes client requests by
    - reading input
    - parsing commands
    - forwarding them to the appropriate airport node
    - sending back responses
- They operate in detached state for better resource-management.
 
 Synchronisation Mechanisms:
- Mutex guards access to the queue.
    - Prevents race conditions during multiple threads trying to enqueue and dequeue at the same time.
- Condition variables make the threads wait and signal them when there is a new connection.

### Distribution of Client Connections
This is done using the shared request queue.

Enqueuing Connections: The main server after accepting a connections, puts the connfd information into the shared request queue. if the queue is full the main thread blocks it till space becomes available

Dequeuing and Processing: When worker thread, who are continously attempting to dequeue a connection are finally successful, they then process the commands in the client request.

Efficiency: The queue is FIFO and hence maintains fairness. This implementation also tries to balance the workload between the worker threads for more efficiency.

#### Impact on performance
Increased Throughput: 

Due to more client requests being proccessed parallel to each other, it allows for more instructions to be processed per unit time. For a real-world airport situation, this is very ideal.

Reduced Latency: 

The concurrent handling of client requests equaled to faster responses being delivered back and overall is a plus point for less communication delay.

Resource Management:

Having a fixed thread pool instead of creating a new thread every single time limits the expensiture of processing power and limits potenial for system crashes and bottlenecks.

### Multithreading within Airport Nodes
Overview
- Multithreading for each node
- Fixed thread pool size
- Per-Gate mutexes
- Detached Threads

Number of threads chosen: 4

Justification : Same as Controller Node.

#### Distribution of Client Connections
This is done using the shared connection queue.

Enqueuing Connections: The main server after accepting a connections, puts the connfd information into the shared request queue. if the queue is full the main thread blocks it till space becomes available

Dequeuing and Processing: Same as controller node. This uses a rio_t structure for buffered reading. 

Efficiency: Same as controller node.

### Protection of Schedule Accesses
The schedule of each gate was guarded with a per-gate mutex. This is a implementation of fine-grained locking scheme which instead of locking the whole schedule, it only locks the specif gate we are concerned about.

#### Locking strategy
Lock Granularity: Each gate_t structure contains its own mutex (gate_lock).

- When a worker thread wants to make a change, it first acquires the associated mutex with the concerned gate. 
- This approach allows multiple threads to operate on several different gates parallelly.
- This is prevents serialisation of schedule processes which would happen if we used a global mutex lock.

Lock acquisition: When accessing a certain gate to either schedule a plane landing or querying information, the thread first locks the mutex lock for the gate.
This is added in the provided helper function [schedule_plane](https://gitlab.cecs.anu.edu.au/u7782612/comp2310-2024-assignment-2/-/blob/main/src/airport.c?ref_type=heads#L73-91)

Avoiding Deadlocks: The threads are only allowed to hold at most one gate-lock (mutex) at any specific moment. This and the locks being placed through a simple loop sequentially takes the chances of circular wait conditions out of the error scope.

#### Impact on performance
Increased Throughput: 

The increased number of recieved requests per unit time increases number of cammands processed and overall more clients are able to be added to the system, so this is a plus for scalability.  

Reduced Latency: 

The concurrent handling of client requests equaled to faster responses being delivered back and overall is a plus point for less communication delay.

Resource Management:

Having a fixed thread pool instead of creating a new thread every single time limits the expensiture of processing power and limits potenial for system crashes and bottlenecks.

## Testing

The biggest challenge I faced during the implementation of this program is the designing of the TIME_STATUS command. This command takes a lot of invalidation conditions and returns a dynamic multi-line response. The command demanded thorugh debugging doe to multiple read and write statements entering infinite loops. Specifically the following command in test 3 TIME_STATUS 0 0 0 900 caused my program to only return this error and then timed out while writing out the other request responses. This was fixed by ensuring the duration invalidation did not end up in a infinite loop. Once this was done the issue still persisted. The fix for this further problem required setting a fixed expected response_lines variable which told the controller node how many lines to expect when processing a TIME_STATUS command.