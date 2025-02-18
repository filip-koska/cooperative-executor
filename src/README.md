# table of contents
- executor - a single-threaded executor based on cooperative multitasking; tasks yield when waiting for I/O operation
- mio - an intermediary structure that handles communication between the tasks, the executor and the OS via epoll
- future_examples - some simple Futures
- future_combinators - Futures that allow chaining two Futures together into a single task
- err - utility functions for handling errors of standard functions and system calls
