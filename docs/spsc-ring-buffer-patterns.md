# SPSC Ring Buffer Patterns: From Vyukov's Bounded Queue to Production Extensions

## 1. Introduction

A Single-Producer Single-Consumer (SPSC) queue is a fundamental concurrent data structure where exactly one thread produces data and exactly one thread consumes it. This constraint enables highly efficient lock-free implementations that outperform general-purpose concurrent queues.

### Why SPSC Matters

The SPSC pattern appears frequently in performance-critical systems:

- **Logging systems**: One thread generating logs, another writing to disk
- **Metrics collection**: Sampling thread producing data, aggregation thread consuming
- **Audio/video processing**: Producer capturing frames, consumer processing them
- **Event systems**: Event generator and event handler in separate threads
- **Inter-process communication**: Shared memory queues between processes

### Lock-Free vs Wait-Free

**Lock-free**: At least one thread makes progress even if others are blocked. The system as a whole moves forward.

**Wait-free**: Every thread completes its operation in a bounded number of steps, regardless of other threads.

Vyukov's bounded SPSC queue is lock-free but not wait-free—the producer may spin if the consumer hasn't freed a slot yet. However, in practice with a reasonably-sized buffer and active consumer, operations complete quickly.

### When SPSC is the Right Tool

Choose SPSC when:
- Your system has a natural single-producer, single-consumer topology
- You need maximum throughput with minimal latency
- Memory overhead must be bounded and predictable
- You can tolerate bounded buffering (not infinite queues)

Avoid SPSC when:
- Multiple producers or consumers exist (use MPMC instead)
- You need strict wait-free guarantees
- Unbounded buffering is required
- The access pattern doesn't match the SPSC topology

---

## 2. Vyukov's Foundational Patterns

Dmitry Vyukov's bounded SPSC queue introduces several key patterns that make it both correct and efficient. Understanding these patterns is essential for implementing or modifying SPSC queues.

### 2.1 Monotonic Counters

Traditional ring buffer implementations use modular indices that wrap around:

```
Traditional approach (problematic):
head = (head + 1) % capacity
tail = (tail + 1) % capacity

Problems:
- Wraparound creates ambiguity
- Full vs empty detection requires additional state
- ABA problem possible in some scenarios
```

Vyukov's approach uses **monotonic counters** that never wrap:

```
head: 0, 1, 2, 3, 4, 5, 6, ... (always increasing)
tail: 0, 1, 2, 3, 4, 5, 6, ... (always increasing)

Slot index: counter % capacity
```

**Benefits:**

1. **No wraparound bugs**: Counters grow to 2^64-1, effectively never wrapping in practice
2. **Simple full/empty detection**:
   - Empty when `head == tail`
   - Full when `head - tail >= capacity`
3. **No ABA problem**: The monotonic nature makes it impossible for a slot to appear unchanged when it has actually been modified

**Conceptual visualization:**

```
Capacity = 4

Time T0: head=0, tail=0 (empty)
  slots: [ _ | _ | _ | _ ]

Time T1: head=2, tail=0 (2 items)
  slots: [ A | B | _ | _ ]
  indices: 0%4=0, 1%4=1

Time T2: head=2, tail=1 (1 item, consumed A)
  slots: [ _ | B | _ | _ ]

Time T3: head=5, tail=3 (2 items, wrapped slots)
  slots: [ _ | _ | E | D ]
  indices: 3%4=3, 4%4=0

Notice: head and tail keep growing, slots wrap via modulo
```

### 2.2 Sequence Numbers

Sequence numbers solve a critical problem: **how does a producer know when a slot is safe to write, and how does a consumer know when a slot is ready to read?**

Each slot has an associated sequence number that tracks its state:

```
Sequence number semantics:
- Even: slot is empty (ready for producer)
- Odd: slot is full (ready for consumer)

Initial state: sequence[i] = i (all empty)
```

**Producer sequence validation:**

```
Before writing to slot (head % capacity):
  expected_seq = head
  actual_seq = sequence[head % capacity]

  if actual_seq == expected_seq:
    // Slot is empty, safe to write
  else:
    // Slot is full, consumer hasn't read it yet
```

**Consumer sequence validation:**

```
Before reading from slot (tail % capacity):
  expected_seq = tail + 1
  actual_seq = sequence[tail % capacity]

  if actual_seq == expected_seq:
    // Slot is full, safe to read
  else:
    // Slot is empty, nothing to consume
```

**After operations:**

```
Producer writes slot:
  sequence[head % capacity] = head + 1  // Make it odd (full)

Consumer reads slot:
  sequence[tail % capacity] = tail + capacity  // Make it even (empty)
```

**Why this works:**

The sequence number encodes both the slot's state and expected position. Because `head` and `tail` are monotonic, the sequence number can be predicted at any point in time. This eliminates race conditions without locks.

### 2.3 Cache Line Padding

False sharing occurs when multiple threads modify variables that reside on the same cache line. Even though the variables are independent, the cache coherency protocol causes contention.

**Problem scenario:**

```
struct BadHeader {
  atomic<uint64_t> head;  // Modified by producer
  atomic<uint64_t> tail;  // Modified by consumer
};

If both are on same cache line:
- Producer writes head → invalidates consumer's cache line
- Consumer writes tail → invalidates producer's cache line
- Performance degrades significantly
```

**Solution: Align to cache line boundaries:**

```
struct GoodHeader {
  alignas(64) atomic<uint64_t> head;  // Producer-only cache line
  alignas(64) atomic<uint64_t> tail;  // Consumer-only cache line
};

Memory layout:
  [ 64 bytes for head ] [ 64 bytes for tail ]
  ^ producer cache line  ^ consumer cache line

Now producer and consumer don't contend on cache lines
```

**Cache line size:** Typically 64 bytes on modern x86/ARM processors. Use `alignas(64)` to ensure separation.

### 2.4 Memory Ordering

Correct memory ordering is crucial for lock-free data structures. Vyukov's pattern uses acquire/release semantics, which are more efficient than sequential consistency (seq_cst) while maintaining correctness.

**SPSC memory ordering rules:**

```
Producer:
  1. Read tail with acquire (need to see consumer's latest updates)
  2. Write to slot (relaxed, no ordering needed yet)
  3. Update head with release (publish the slot to consumer)

Consumer:
  1. Read head with acquire (need to see producer's latest updates)
  2. Read from slot (relaxed, data already visible)
  3. Update tail with release (publish consumption to producer)
```

**Why acquire/release is sufficient:**

- **Producer only reads `tail`**: Needs to see consumer's progress (acquire)
- **Consumer only reads `head`**: Needs to see producer's progress (acquire)
- **Each side owns its counter**: No contention on writes (release for visibility)

**Incorrect ordering causes subtle bugs:**

```
Wrong: relaxed ordering on head/tail updates
  Producer writes data, updates head with relaxed
  Consumer might see updated head but stale data!

Correct: release ordering on updates
  Producer writes data, updates head with release
  Guarantees consumer sees data when it sees new head
```

---

## 3. Core Operations

This section describes the conceptual flow of push and pop operations. These patterns form the foundation of Vyukov's bounded SPSC queue.

### 3.1 Push Operation (Producer)

**Conceptual algorithm:**

```
function try_push(value):
  slot_index = head % capacity

  // Check if slot is empty (sequence validation)
  expected_seq = head
  if sequence[slot_index] != expected_seq:
    return false  // Buffer full

  // Write value to slot
  slots[slot_index] = value

  // Update sequence to mark slot as full
  sequence[slot_index] = head + 1

  // Publish new head
  head = head + 1

  return true
```

**Key insights:**

1. **Sequence check first**: Only proceed if slot is empty
2. **Write before sequence update**: Data must be visible when sequence changes
3. **Head update last**: Publishing head makes the slot visible to consumer
4. **Memory ordering**: Head update uses release semantics

**Why `expected_seq = head` works:**

- Initially, `sequence[i] = i` (all slots empty)
- After producer writes slot 0: `sequence[0] = 1` (odd, full)
- After consumer reads slot 0: `sequence[0] = 0 + capacity` (even, empty again)
- Next time producer checks slot 0: `head = capacity`, `expected_seq = capacity`
- Matches! Slot is empty again

### 3.2 Pop Operation (Consumer)

**Conceptual algorithm:**

```
function try_pop():
  slot_index = tail % capacity

  // Check if slot is full (sequence validation)
  expected_seq = tail + 1
  if sequence[slot_index] != expected_seq:
    return empty  // Buffer empty

  // Read value from slot
  value = slots[slot_index]

  // Update sequence to mark slot as empty
  sequence[slot_index] = tail + capacity

  // Publish new tail
  tail = tail + 1

  return value
```

**Key insights:**

1. **Expected sequence is `tail + 1`**: Looking for full slot (odd number)
2. **Read before sequence update**: Data must be read before marking empty
3. **Tail update last**: Publishing tail frees the slot for producer
4. **Memory ordering**: Tail update uses release semantics

### 3.3 Full/Empty Detection

Using monotonic counters, detection is trivial:

```
Buffer state: (head - tail) items

Empty: head == tail
Full:  (head - tail) >= capacity

No modulo arithmetic, no ambiguous states
```

**Comparison with traditional ring buffer:**

```
Traditional approach (modular indices):
  Empty: head == tail
  Full:  (head + 1) % capacity == tail
  Problem: One slot wasted to distinguish full from empty

Vyukov approach (monotonic counters):
  Empty: head == tail
  Full:  (head - tail) >= capacity
  No wasted slots, unambiguous state
```

### 3.4 Memory Ordering Summary

| Operation | Counter Read | Counter Write | Rationale |
|-----------|--------------|---------------|-----------|
| Push check full | `tail` (acquire) | - | Must see consumer's progress |
| Push commit | - | `head` (release) | Publish data to consumer |
| Pop check empty | `head` (acquire) | - | Must see producer's progress |
| Pop commit | - | `tail` (release) | Free slot for producer |

---

## 4. Advanced Patterns and Extensions

While Vyukov's foundational patterns handle the core SPSC scenario, production systems often require additional features. This section covers common extensions.

### 4.1 Overwrite Semantics

Some systems prefer overwriting old data rather than blocking when full. This is common in:

- **Metrics collection**: Newer samples are more valuable
- **Logging**: Recent logs matter more than old ones
- **Signal processing**: Want latest data, not historical

**Overwrite implementation pattern:**

```
function push_overwrite(value):
  slot_index = head % capacity

  // Check if buffer is full
  if (head - tail) >= capacity:
    // Overwrite oldest element
    // Slot at 'tail' will be replaced

    // Option 1: Move tail forward (consumer will skip)
    tail = tail + 1

    // Option 2: Track overflow for monitoring
    overflow_count = overflow_count + 1

  // Write value (may overwrite if full)
  slots[slot_index] = value
  sequence[slot_index] = head + 1

  head = head + 1
```

**Key considerations:**

1. **Consumer synchronization**: Consumer's `tail` might lag behind producer's
2. **Sequence number management**: Must remain consistent for consumer
3. **Overflow tracking**: Useful for monitoring and debugging
4. **Non-trivial types**: Destructor must be called for overwritten objects

**Destructor safety for non-trivial types:**

```
if buffer full and overwriting:
  if not is_trivially_destructible<T>:
    old_value = slots[tail % capacity]
    old_value.~T()  // Explicit destructor call
  new(slots[tail % capacity]) T(value)  // Placement new
```

### 4.2 Storage Policy Abstraction

Different deployment scenarios require different storage backends:

**Heap storage** (default, single-process):
```
- Simple allocation with new/delete
- No cross-process capability
- Fast allocation/deallocation
```

**Shared memory storage** (cross-process):
```
- POSIX shm_open() + mmap()
- Persistent across process boundaries
- Requires explicit cleanup (shm_unlink)
- Must handle initialization races
```

**Storage policy interface:**

```
Concept: StoragePolicy<T, N>

Required operations:
  - header(): Returns pointer to header (counters, metadata)
  - slots(): Returns pointer to data array
  - valid(): Checks if storage was initialized correctly
  - is_creator(): Distinguishes creator from opener (shared memory)
```

**Benefits of policy-based design:**

1. **Zero overhead**: Compile-time polymorphism, no virtual calls
2. **Flexibility**: Easy to add new storage types (file-backed, huge pages, etc.)
3. **Testability**: Can mock storage for unit tests
4. **Type safety**: Storage size verified at compile time

### 4.3 Multi-Process Considerations

SPSC queues work across process boundaries, enabling efficient IPC (Inter-Process Communication).

**Shared memory setup:**

```
Creator process:
  1. shm_open("/queue_name", O_CREAT | O_RDWR, 0666)
  2. ftruncate(fd, sizeof(header) + sizeof(slots))
  3. mmap() to map into address space
  4. Initialize header (counters, sequence numbers)

Consumer process:
  1. shm_open("/queue_name", O_RDWR, 0666)
  2. mmap() to map into address space
  3. Verify compatibility (schema version, capacity)
```

**Critical considerations:**

1. **Initialization races**: Use atomic operations to prevent double-initialization
2. **PID tracking**: Store creator's PID for debugging and cleanup
3. **Schema versioning**: Detect version mismatches between processes
4. **Cleanup strategy**: Who calls shm_unlink()? Usually external orchestration needed

**Memory ordering in shared memory:**

Atomic operations work correctly in shared memory on most modern platforms, but verify:
- x86: Works correctly with standard atomic operations
- ARM: May require explicit memory barriers in some cases
- Always use C++ `std::atomic` with proper memory ordering

### 4.4 Overflow Tracking

Monitoring queue overflow is essential for system health and capacity planning.

**What to track:**

```
overflow_count: Number of times push was rejected (reject mode)
               or elements were overwritten (overwrite mode)

Use cases:
- Capacity planning: Increase buffer size if overflow is frequent
- Backpressure detection: Slow down producer if consumer can't keep up
- SLA monitoring: Alert if overflow exceeds threshold
```

**Where to store statistics:**

- **In header**: Cross-process visible, survives process restart
- **Atomic counter**: Lock-free updates, minimal overhead
- **Reset mechanism**: Allow resetting for monitoring intervals

---

## 5. Design Trade-offs

Choosing the right concurrent queue requires understanding the trade-offs between different approaches.

### 5.1 SPSC vs MPMC

| Aspect | SPSC Queue | MPMC Queue |
|--------|-----------|------------|
| Throughput | Higher (no contention) | Lower (atomic operations on shared counters) |
| Latency | Lower and more predictable | Higher variance due to contention |
| Memory overhead | Lower (single set of counters) | Higher (multiple sequences, complex state) |
| Flexibility | Restricted to 1 producer, 1 consumer | Any number of producers/consumers |
| Implementation complexity | Simpler | More complex (avoiding ABA, helping schemes) |

**Decision criteria:**

Choose SPSC when your system topology matches the pattern. Don't use MPMC "just in case" – the performance penalty is real.

**Common mistake:** Using MPMC queue for SPSC scenario because "it's more flexible." This sacrifices 2-3x throughput for unnecessary flexibility.

### 5.2 Bounded vs Unbounded Queues

**Bounded (fixed capacity):**

```
Pros:
  - Predictable memory usage
  - Backpressure naturally enforced
  - Cache-friendly (contiguous memory)
  - No allocation during operation

Cons:
  - Can fill up, requiring handling of full condition
  - Capacity must be chosen carefully
  - May waste memory if oversized
```

**Unbounded (dynamic growth):**

```
Pros:
  - Never blocks producer (assuming sufficient memory)
  - Adapts to varying load

Cons:
  - Unpredictable memory usage
  - Can cause OOM in producer-consumer imbalance
  - Allocations during operation (latency spikes)
  - Less cache-friendly (linked structure)
```

**Recommendation:** Prefer bounded queues in production systems. Unbounded queues hide problems that surface as out-of-memory crashes.

### 5.3 Capacity Planning

Choosing the right capacity requires understanding your workload:

```
Buffer size formula (approximate):
  capacity >= (production_rate - consumption_rate) × burst_duration

Example:
  Production rate: 1M items/sec
  Consumption rate: 800K items/sec (consumer slower during GC)
  Burst duration: 100ms (max expected pause)

  capacity >= (1M - 0.8M) × 0.1s = 20K items

  Add safety margin: capacity = 40K or 64K (power of 2)
```

**Monitoring for capacity planning:**

Track these metrics:
- Maximum observed size during normal operation
- Overflow count (reject mode) or overwrite count (overwrite mode)
- Time-to-empty after burst (how long buffer takes to drain)

### 5.4 Memory Overhead Analysis

**Vyukov's bounded SPSC queue:**

```
Per-element overhead:
  - Data: sizeof(T)
  - Sequence number: sizeof(atomic<uint64_t>) = 8 bytes
  - Padding (optional): up to 64 bytes for cache alignment

Total memory:
  sizeof(header) + N × (sizeof(T) + sizeof(sequence))
  ≈ 128 bytes (header) + N × (sizeof(T) + 8)

Example: 4096-element queue of 16-byte structs
  128 + 4096 × (16 + 8) = 128 + 98304 ≈ 96 KB
```

**Comparison with alternatives:**

- **Lock-based queue with linked list**: 2 pointers per node (16 bytes overhead) + allocation overhead
- **MPMC queue**: Multiple sequence numbers, more complex header
- **SPSC optimized**: Minimal overhead (Vyukov's approach)

---

## 6. Performance Characteristics

Understanding performance characteristics helps in system design and optimization.

### 6.1 Cache Behavior

**Ideal scenario (buffer not full/empty):**

```
Producer:
  1. Write to slot (cache line loaded for writing)
  2. Update head (cache line in exclusive state)
  → Minimal cache misses

Consumer:
  1. Read from slot (cache line loaded for reading)
  2. Update tail (cache line in exclusive state)
  → Minimal cache misses
```

**Cache miss analysis:**

```
Operation cost (approximate):
  - L1 cache hit: ~1 cycle
  - L2 cache hit: ~10 cycles
  - L3 cache hit: ~40 cycles
  - Main memory access: ~100 cycles

SPSC queue with good cache behavior:
  - Slot access: L1 or L2 hit (recently touched)
  - Counter updates: L1 hit (producer/consumer each own their cache line)
  - Throughput: Limited by memory bandwidth, not latency
```

**Optimization: Cache prefetching**

```
Producer can prefetch next slot:
  __builtin_prefetch(&slots[(head + 1) % capacity])

Consumer can prefetch next slot:
  __builtin_prefetch(&slots[(tail + 1) % capacity])

Effectiveness depends on access pattern and hardware
```

### 6.2 False Sharing Prevention

**Impact of false sharing:**

```
Without cache line padding:
  Producer updates head, invalidates consumer's cache line containing tail
  Consumer updates tail, invalidates producer's cache line containing head
  → Up to 10x performance degradation

With cache line padding:
  Producer and consumer work on independent cache lines
  → No invalidation, optimal performance
```

**Verification:**

Use performance counters to detect false sharing:

```
perf stat -e cache-references,cache-misses,L1-dcache-loads,L1-dcache-load-misses ./program

High L1-dcache-load-misses on shared variables indicates false sharing
```

### 6.3 Memory Ordering Costs

**Relative cost of memory ordering:**

```
relaxed < acquire < release < seq_cst (sequential consistency)

Approximate cycle counts (x86):
  relaxed: 1-2 cycles (no barrier)
  acquire/release: 10-20 cycles (memory barrier)
  seq_cst: 50-100 cycles (full fence, expensive)
```

**Why Vyukov uses acquire/release:**

```
Sequential consistency is overkill for SPSC:
  - Only two threads involved
  - Each thread has clear ownership (producer: head, consumer: tail)
  - Acquire/release provides necessary ordering with lower cost
```

### 6.4 NUMA Awareness

On NUMA (Non-Uniform Memory Access) systems, memory location affects performance.

**NUMA considerations:**

```
Best case:
  Producer and consumer on same NUMA node as memory
  → Fast memory access

Worst case:
  Producer on node 0, consumer on node 1, memory on node 2
  → All accesses are remote, high latency

Optimal placement:
  - Allocate memory on node where most active thread runs
  - Or use interleave policy for shared access
```

**NUMA allocation:**

```
Linux: numactl --interleave=all ./program
Or: numa_alloc_interleaved() for explicit control
```

---

## 7. Common Pitfalls

Even experienced developers make mistakes with lock-free data structures. Here are common pitfalls and how to avoid them.

### 7.1 Incorrect Memory Ordering

**Pitfall: Using relaxed ordering everywhere**

```
Wrong:
  head.store(head.load(relaxed) + 1, relaxed)

Problem:
  Consumer might see updated head but stale data in slot
  No happens-before relationship established
```

**Correct approach:**

```
Producer:
  tail_value = tail.load(acquire)  // See consumer's progress
  // ... write to slot ...
  head.store(new_head, release)    // Publish data to consumer

Consumer:
  head_value = head.load(acquire)  // See producer's progress
  // ... read from slot ...
  tail.store(new_tail, release)    // Free slot for producer
```

### 7.2 Wraparound Bugs

**Pitfall: Using modular indices without sequence numbers**

```
Wrong:
  if (head == tail) return empty;  // Looks correct
  if ((head + 1) % capacity == tail) return full;  // Ambiguous!

Problem:
  Full and empty states can be indistinguishable
  Or require wasting one slot
```

**Correct approach:**

Use monotonic counters with sequence numbers (Vyukov's pattern) or maintain an explicit size counter.

### 7.3 Destructor Safety

**Pitfall: Overwriting non-trivial objects without calling destructor**

```
Wrong:
  slots[index] = new_value;  // Assignment overwrites old object

Problem:
  Old object's destructor never called
  Memory leaks, resource leaks, undefined behavior
```

**Correct approach:**

```
if constexpr (!std::is_trivially_destructible_v<T>) {
  slots[index].~T();  // Call destructor explicitly
}
new(&slots[index]) T(new_value);  // Placement new
```

Or use `std::optional<T>` for automatic destructor calls.

### 7.4 Testing Challenges

**Pitfall: Tests pass but production fails**

Lock-free code is notoriously difficult to test because:
- Race conditions are timing-dependent
- Bugs may only manifest under specific interleavings
- Memory ordering bugs may not appear on strongly-ordered hardware (x86)

**Testing strategies:**

1. **ThreadSanitizer (TSan)**: Detects data races at runtime

   ```
   Compile with: -fsanitize=thread
   Run tests with various thread schedules
   ```

2. **Stress testing**: Run millions of operations with random delays

   ```
   for millions of iterations:
     producer: push random values with random delays
     consumer: pop and verify values
   ```

3. **Formal verification**: Use tools like CDSChecker for weak memory models

4. **Architectural diversity**: Test on x86, ARM, POWER to expose memory ordering bugs

### 7.5 ABA Problem

**Good news: Vyukov's pattern is immune to ABA**

The ABA problem occurs when:
1. Thread reads value A
2. Other threads change A → B → A
3. First thread sees A again, assumes nothing changed

**Why SPSC with monotonic counters avoids ABA:**

- Counters are monotonic (never decrease)
- Sequence numbers encode position and state
- No scenario where counter returns to previous value

**Where ABA is a problem:**

- Lock-free stacks with pop-compare-swap
- MPMC queues with atomic index updates
- Any algorithm that relies on pointer/value equality

---

## 8. Real-World Usage Patterns

### 8.1 Typical Applications

**Logging systems:**

```
Producer (application threads):
  - Generate log messages
  - Push to SPSC queue (or MPSC for multiple threads)

Consumer (logger thread):
  - Batch messages for efficiency
  - Write to disk/network

Benefits:
  - Application threads not blocked by I/O
  - Natural batching improves throughput
  - Backpressure via queue size monitoring
```

**Metrics collection:**

```
Producer (sampling thread):
  - Collect metrics (CPU, memory, etc.)
  - Push samples to queue

Consumer (aggregation thread):
  - Compute statistics (average, percentiles)
  - Send to monitoring system

Overwrite mode:
  - Prefer recent samples over old
  - Never block producer
```

**Audio/video processing:**

```
Producer (capture):
  - Record audio/video frames
  - Push to queue at fixed rate

Consumer (processing):
  - Process frames (encoding, analysis)
  - Must keep up with producer rate

Latency requirements:
  - Bounded buffer size limits latency
  - Drop frames (overwrite) if consumer too slow
```

### 8.2 Integration Patterns

**Pattern 1: Thread-per-queue**

```
Simple topology:
  Thread A (producer) → SPSC Queue → Thread B (consumer)

Easy to reason about, clear ownership
```

**Pattern 2: Pipeline of queues**

```
Complex topology:
  Thread A → Queue1 → Thread B → Queue2 → Thread C

Each queue is SPSC, pipeline as a whole is multi-stage

Considerations:
  - Backpressure propagation
  - Monitoring each queue's size
  - Handling slow stages
```

**Pattern 3: Multi-producer → SPSC converter**

```
Multiple producers → MPSC Queue → Thread M → SPSC Queue → Thread C

Thread M demultiplexes multiple producers into single consumer
Avoids MPMC queue overhead
```

### 8.3 Testing Strategies

**Unit testing:**

```
Test individual operations:
  - Push then pop, verify value
  - Push until full, verify behavior
  - Pop from empty, verify behavior
  - Overwrite mode behavior
```

**Concurrency testing:**

```
Stress test with random delays:
  Producer: push values 0, 1, 2, ... N
  Consumer: verify sequence is monotonic

  Run for millions of iterations
  Use TSan to detect races
```

**Integration testing:**

```
Test in realistic scenario:
  - Producer at realistic rate
  - Consumer with realistic processing time
  - Verify throughput and latency
  - Monitor for overflow/dropped items
```

**Failure mode testing:**

```
Test edge cases:
  - Consumer slower than producer (buffer fills)
  - Producer slower than consumer (buffer empties)
  - Burst scenarios (temporary imbalance)
  - Very large queues (cache behavior)
  - Very small queues (capacity = 1)
```

---

## 9. References

### Primary Sources

- **Dmitry Vyukov's bounded SPSC queue**: Original implementation and discussion
  - https://www.1024cores.net/home/lock-free-algorithms/queues/bounded-mpsc-queue
  - Key contribution: Monotonic counters with sequence numbers

### Memory Ordering

- **C++ Memory Model**: Herb Sutter's papers and talks
  - https://herbsutter.com/2012/08/02/strong-guarantees-for-the-c-memory-model/
- **Acquire/Release Semantics**: Jeff Preshing's blog
  - https://preshing.com/20120913/acquire-and-release-semantics/

### Lock-Free Programming

- **"C++ Concurrency in Action"** by Anthony Williams
  - Comprehensive treatment of lock-free data structures
- **"The Art of Multiprocessor Programming"** by Herlihy & Shavit
  - Theoretical foundations and practical patterns

### Tools

- **ThreadSanitizer**: Data race detection
  - https://github.com/google/sanitizers/wiki/ThreadSanitizerCppManual
- **CDSChecker**: Model checker for C/C++ concurrency
  - https://github.com/c7c7c7c7c7/cdschecker

### Related Patterns

- **MPMC queues**: Dmitry Vyukov's bounded MPMC queue
  - More complex, handles multiple producers and consumers
- **Lock-free stacks**: Treiber stack, ABA problem mitigation
- **Read-Copy-Update (RCU)**: Linux kernel pattern for read-heavy workloads

### Performance Analysis

- **Perf**: Linux performance monitoring
  - `perf stat`, `perf record`, `perf report`
- **Intel VTune**: Profiling for cache behavior
- **False sharing detection**: Performance counter analysis

---

## Conclusion

Vyukov's bounded SPSC queue represents an elegant solution to a common concurrent programming challenge. The key patterns—monotonic counters, sequence numbers, cache line padding, and acquire/release memory ordering—combine to create a data structure that is:

- **Correct**: No race conditions, clear memory ordering
- **Efficient**: Minimal overhead, optimal cache behavior
- **Practical**: Bounded memory, predictable performance

Extensions like overwrite semantics, storage policies, and cross-process support make the pattern applicable to a wide range of production scenarios.

When implementing or using SPSC queues, remember:

1. **Understand the patterns**: Don't copy code blindly; understand why each design decision was made
2. **Test rigorously**: Use TSan, stress tests, and architectural diversity
3. **Monitor in production**: Track overflow, latency, and throughput
4. **Choose the right tool**: SPSC when topology matches, MPMC when needed

The patterns described here have been battle-tested in high-performance systems across many domains. Master them, and you'll have a powerful tool for concurrent system design.
