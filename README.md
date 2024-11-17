# Canberra ATC System

## Overview

The Canberra Air Traffic Control (ATC) System is a robust, multi-threaded application designed to manage and streamline communication between Canberra Airport and a distributed network of airport servers. Leveraging fine-grained locking strategies, this system ensures thread-safe operations, enhancing performance and scalability to handle multiple client requests efficiently.

## Features

- **Controller Server**: Acts as a proxy between clients and airport nodes, handling flight scheduling and information queries.
- **Airport Node Servers**: Manage individual gate schedules with thread-safe operations using fine-grained locking.
- **Multi-Threading**: Utilizes thread pools in both controller and airport nodes to handle concurrent connections, reducing latency and increasing throughput.
- **Fine-Grained Locking**: Ensures thread safety by locking individual gates during operations, allowing parallel processing without conflicts.
- **Scalable Architecture**: Designed to support numerous clients and additional data sources like weather and meteorology information.

## Architecture

### Controller Server

- **Proxy Functionality**: Routes client requests to the appropriate airport nodes.
- **Client Requests Handled**:
  - Scheduling flight landings.
  - Querying flight information.
- **Concurrency Management**:
  - Listens on a designated port for incoming connections.
  - Utilizes a thread pool with a fixed number of threads (default: 4) to manage multiple client connections simultaneously.
- **Request Queue**:
  - Implements a bounded FIFO queue (size: 100) to store incoming client connection file descriptors.
  - Ensures fair distribution of workload among worker threads.

### Airport Node Servers

- **Gate Management**: Each airport node manages its own gate schedules.
- **Supported Requests**:
  - `SCHEDULE`: Schedule a flight landing.
  - `PLANE_STATUS`: Query the status of a specific plane.
  - `TIME_STATUS`: Retrieve time-based status information.
- **Thread Safety**:
  - Utilizes fine-grained locking with a mutex for each gate to prevent conflicting operations.
  - Allows multiple threads to operate on different gates concurrently.

## Request Handling and Forwarding

### Parsing

- Uses `sscanf` to extract command types and parameters.
- Validates requests based on the number of arguments.
- Produces error messages for invalid commands.

### Forwarding

- The controller establishes a connection to the airport node using a pre-assigned port number.
- Forwards the entire request string verbatim to maintain traceability and reduce protocol mismatches.

## Multithreading Implementation

### Controller Node

- **Thread Pool**: Fixed size (default: 4 threads) to handle client connections.
- **Worker Threads**: Continuously dequeue and process client requests from the shared request queue.
- **Synchronization**:
  - Mutexes guard access to the request queue.
  - Condition variables manage thread waiting and signaling for new connections.

### Airport Node Servers

- **Thread Pool**: Similar fixed size (default: 4 threads) for handling gate-specific requests.
- **Worker Threads**: Use a shared connection queue to manage and process incoming requests.
- **Synchronization**:
  - Per-gate mutexes ensure that only one thread can modify a gate's schedule at a time, avoiding race conditions and deadlocks.

## Locking Strategy

- **Fine-Grained Locking**: Each gate has its own mutex (`gate_lock`), allowing multiple gates to be managed in parallel without interference.
- **Deadlock Prevention**: Threads hold at most one gate-lock at any given time and acquire locks in a sequential manner to avoid circular wait conditions.

## Performance Impact

- **Increased Throughput**: Parallel processing of client requests allows for more operations per unit time, enhancing system scalability.
- **Reduced Latency**: Concurrent handling leads to faster response times, minimizing communication delays.
- **Efficient Resource Management**: Fixed thread pools limit CPU usage and prevent system bottlenecks, ensuring stable performance under load.

## Testing

The implementation faced challenges with the `TIME_STATUS` command, which required handling multiple invalidation conditions and dynamic multi-line responses. Issues such as infinite loops during read/write operations were resolved by:

- Ensuring duration validations did not cause infinite loops.
- Implementing a fixed expected `response_lines` variable to manage the number of lines expected from `TIME_STATUS` responses.

Comprehensive debugging and testing ensured the reliability and correctness of the command handling mechanisms.

## Getting Started

### Prerequisites

- **Compiler**: GCC or any compatible C compiler.
- **Libraries**: POSIX threads (`pthread`), standard C libraries.

### Installation

1. **Clone the Repository**
   ```bash
   git clone https://github.com/yourusername/canberra-atc-system.git
   cd canberra-atc-system

2. **Build the Project**
   ```bash
   make

### Running the System

1. **Start the Controller Server**
   ```bash
   ./controller
2. Start Airport Node Servers
   ```bash
   ./airport_node <port_number>
    Replace <port_number> with the designated port for each airport node.

3. Connect Clients Clients can connect to the controller server on the designated port to schedule flights or query information.

### Usage

**Request Format**
Clients send requests in the following format:
  COMMAND_TYPE PARAMETERS

Commands:
  ```bash
  SCHEDULE: Schedule a flight landing.
  PLANE_STATUS: Query the status of a specific plane.
  TIME_STATUS: Retrieve time-based status information.
  Example
  SCHEDULE FL123 Gate5 10:30
  PLANE_STATUS FL123
  TIME_STATUS 0 0 0 900
