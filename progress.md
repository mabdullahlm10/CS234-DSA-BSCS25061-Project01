# Time-Travel Debugger - Progress

## 6th October 2026

### Current Progress

- Created the Github repo for Project 01.
- Added the provided 'server.cpp' starter code.
- Reviewed the requirements and overall project structure.
- Identified the data structures and functions that need to be implemented.

### Current Stage

Project setup and preparation for Phase 01 implementation.

### Next Steps

- Implement the custom Stack data structure.
- Implement the Timeline doubly linked list.
- Begin Pass 0x0 validation.

## 12th October 2026

### Current Progress
- Implemented the custom linked-list Stack.
- Implemented push, pop, peek, isEmpty, depth, and snapshot copying.
- Implemented the Timeline as a custom doubly linked list.
- Implemented snapshot recording, timeline traversal start, and step counting.
- Tested the Stack and Timeline functions successfully.
- Implemented Pass 0x0 helper functions for reading nonblank lines and extracting instruction words.
- Implemented program validation for func / func_end matching.
- Added checks for nested function declarations, unmatched func_end, missing func_end, and missing function names.
- Implemented Pass 0x1 resolve record writing and reading.
- Added function offset bookkeeping and pending CALL patching.
- Added detection for missing main and undefined function calls.
- Verified that CALL records are patched with the correct function offsets.
- Implemented Pass 0x2 tokenization.
- Added helper functions for finding, reading, and updating variables inside Frames.
- Implemented Snapshot creation from the custom call stack.
- Implemented execution for set, add, sub, mul, and div instructions.
- Implemented function calls using Frames on the custom Stack.
- Implemented func_end handling and copying modified arguments back to the caller.
- Added Timeline snapshot recording during execution.
- Implemented Pass 0x3 serialization.
- Completed TTDB header writing with magic bytes, version, step count, and index offset.
- Implemented Snapshot serialization from the Timeline.
- Implemented the dense Snapshot index using the allowed built-in vector.
- Completed an end-to-end Windows test using a sample C-- program.
- Successfully generated resolve.bin and session.tdbg through the complete Phase 01 pipeline.

### Current Stage
Phase 01 implementation completed and tested successfully on Windows.

### Next Steps
- If the deadline is extended, test the project on Linux and resolve any portability issues to target the Windows + Linux bonus.
