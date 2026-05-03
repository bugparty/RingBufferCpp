### Ring Buffer Implementation Details

#### Internal Pointers
- `tail_`: Points to the oldest element in the buffer. This is where `front()` and `begin()` start.
- `head_`: Points to the position where the next element will be inserted by `push_back()`.
- `size_`: The current number of elements stored in the buffer.

#### Key Operations

- `push_back(value)`: 
  - Adds a new element to the buffer at the `head_` position.
  - If the buffer is not full, it increments the `size_` and moves `head_` forward.
  - If the buffer is full (overwrite mode), it replaces the oldest element at `tail_`, then advances both `head_` and `tail_` to maintain the fixed capacity.
- `pop_front()`: 
  - Removes the oldest element from the buffer (located at `tail_`).
  - It destroys the element at the current `tail_`, advances `tail_` to the next element, and decrements `size_`.
  - This operation is only valid when the buffer is not empty.

#### Behavior
When the buffer is full and `Overwrite` is enabled, a `push_back()` will:
1. Destroy the element at `head_` (which is also the oldest element, `tail_`).
2. Construct the new element at `head_`.
3. Advance both `head_` and `tail_` to maintain the circular structure.
