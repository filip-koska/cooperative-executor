# table of contents
- executor - a single-threaded executor based on cooperative multitasking; tasks yield when waiting for I/O operation
- mio - an intermediary structure that handles communication between the tasks, the executor and the OS via epoll
- future - interface for a Future, a task that can start and end its computation in a non-sequential way
- future_examples - some simple Futures
- future_combinators - Futures that allow chaining two Futures together into a single task
- err - utility functions for handling errors of standard functions and system calls
- debug - utility function for debug operation logging
- waker - structure used to "wake" Futures, that have been waiting for an I/O event
