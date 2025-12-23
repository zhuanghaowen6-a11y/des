# Hold Timer Expired - Root Cause Analysis

## Problem Summary
BGP sessions are failing with "Hold timer expired" errors. Analysis of `logs_1/` shows the first Hold Timer Expired occurred at VT=1318.107 for R1's connection to R2.

## Timeline of Events (R1 perspective)

### VT=61.001 - BGP Sessions Established
- **fd 12**: Listening socket (0.0.0.0:179) - created at VT=0.000
- **fd 15**: Accepted connection from R3 (10.0.3.3:51093)
- **fd 16**: Accepted connection from R2 (10.0.2.2:49299)
- Both BGP sessions successfully established

### VT=1261.003 - Connection to R3 Lost
- R1 sends KEEPALIVE on fd 16 (to R2) - **SUCCESS**
- R1 tries to send on fd 14 (outgoing connection to R3) - **FAILS** with "Peer connection closed"
- R1 closes fd 14
- R3 BGP session closed

### VT=1261.005 - R1 Still Has Active Connection to R2
- R1 sends more data on fd 16 (to R2) - **SUCCESS**
- **fd 16 is still open and active**

### VT=1261.007 - R1 Blocks on poll()
From `desd_n3.log`:
```
[DESD-received-message] Received message (Type: HOOK_TO_DESD, Event: ROUTER_BLOCK_REQUEST, ReqID: req_4564_1) from R1.
[DESD-pop-queue] Processing event ROUTER_BLOCK_REQUEST for R1 at VT=1261.007 (EventID: 15774)
[DEBUG-SELECT] R1 SELECT_CALL: timeout=57098ms, monitoring 1 fd(s), pending_connections=0
[DESD] R1 blocked on SELECT_CALL (ReqID: req_4564_1) with 57098ms timeout. Registered TIMEOUT_EVENT (ID: 15775) at VT=1318.105.
```

**KEY FINDING**: R1's poll() is monitoring **only 1 fd** with a 57-second timeout.

### VT=1318.105 - poll() Times Out
- The poll() timeout expires (57 seconds later)
- R1 returns from poll()

### VT=1318.107 - Hold Timer Expired
```
bird: 2025-12-23 05:33:44.959 [0002] <RMT> r2: Error: Hold timer expired
```
- R1 closes fd 16
- R2 BGP session closed

## Root Cause Analysis

### What fd was being monitored?
The poll() was monitoring **only 1 fd**, which must be **fd 12 (the listening socket)**, because:
1. fd 14 was already closed at VT=1261.005
2. fd 15 was closed earlier (connection to R3 failed)
3. fd 16 was still open but **NOT included in the poll() fds array**

### Why did Hold Timer expire?
BGP Hold Timer is typically 180 seconds, but KEEPALIVE messages must be sent every 60 seconds (Hold Timer / 3). The timer expired because:

1. **R1 was not monitoring fd 16** in its poll() call
2. R1 could not receive KEEPALIVE messages from R2 on fd 16
3. After 57 seconds of not receiving any KEEPALIVE, the Hold Timer expired

### Is this a libdeshook bug?
**NO**. The libdeshook code correctly:
1. Marked fd 16 as a DES-managed socket when it was accepted
2. Would have sent fd 16 to desd if BIRD had included it in the poll() fds array
3. The `poll_internal()` function filters fds based on what BIRD passes to poll()

From `libdeshook.c` lines 1000-1006:
```c
for (nfds_t i = 0; i < nfds; i++) {
    int fd = fds[i].fd;
    if (fd >= 0 && fd < MAX_TRACKED_FDS && socket_fds[fd]) {
        json_t *fd_info = json_object();
        json_object_set_new(fd_info, "fd", json_integer(fds[i].fd));
        json_object_set_new(fd_info, "events", json_integer(fds[i].events));
        json_array_append_new(monitored_fds_array, fd_info);
    }
}
```

This code only sends fds that BIRD included in the `fds` array to poll().

### Is this a BIRD bug?
**LIKELY YES**. BIRD should have been monitoring fd 16 (the active BGP connection to R2) in its poll() call, but it only monitored fd 12 (the listening socket).

Possible reasons:
1. **BIRD's event loop bug**: After closing fd 14 (R3 connection), BIRD may have incorrectly removed fd 16 from its monitored fd set
2. **BIRD's socket management bug**: BIRD may have lost track of fd 16 after the R3 connection failed
3. **Thread safety issue**: BIRD 3 uses multiple threads, and there may be a race condition in updating the monitored fd set

## Verification Steps

To confirm this is a BIRD issue, we need to:

1. **Add debug logging to libdeshook** to print all fds passed to poll():
   ```c
   // In poll_internal(), before sending to desd:
   fprintf(stderr, "[LIBDESHOOK-DEBUG] R%d poll() called with %d fds:\n", my_router_id, (int)nfds);
   for (nfds_t i = 0; i < nfds; i++) {
       fprintf(stderr, "[LIBDESHOOK-DEBUG]   fd=%d events=0x%x\n", fds[i].fd, fds[i].events);
   }
   ```

2. **Check if fd 16 is in the fds array** when poll() is called at VT=1261.007

3. **If fd 16 is missing**: This confirms BIRD is not monitoring the active connection
4. **If fd 16 is present**: Then libdeshook has a bug in filtering or sending fds to desd

## Recommended Fix

### Short-term workaround:
- Increase BGP Hold Timer to a much larger value (e.g., 600 seconds) to give more time for debugging

### Long-term fix:
- If confirmed as a BIRD bug: Report to BIRD developers with detailed logs
- If it's a libdeshook bug: Fix the fd filtering logic in `poll_internal()`

## Additional Observations

From the desd log, we can see that R2 had pending packets that R1 never received:
```
[DESD] R2 has 10 pending packet(s) (data arrived on fd:14 but not monitored by select, pending_connections=0, currently blocked on SELECT_CALL).
[DESD] R2 has 11 pending packet(s) (data arrived on fd:14 but not monitored by select, pending_connections=0, currently blocked on SELECT_CALL).
```

This confirms that:
1. R2 was sending KEEPALIVE messages to R1
2. The messages arrived at desd
3. But R1 was not monitoring the correct fd to receive them
4. The messages piled up as "pending packets"

## Conclusion

The Hold Timer Expired issue is caused by **BIRD not including active BGP connection fds in its poll() call**. After one BGP session fails (R3), BIRD appears to stop monitoring the other active session (R2), leading to Hold Timer expiration.

This is most likely a **BIRD bug**, not a DES simulator bug. The next step is to add detailed logging to confirm which fds BIRD is passing to poll().
