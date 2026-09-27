# Upstream issue draft: NukiBleEsp32, Success reported on ACCEPTED instead of COMPLETE

**Where to file:** https://github.com/I-Connect/NukiBleEsp32/issues. `iranl/NukiBleEsp32`, the fork Nuki Hub
uses, has issues disabled. At the time of writing, `I-Connect/NukiBleEsp32@main` and `iranl/NukiBleEsp32` both point to
`a576a19b1bde0c9f8a4b4cb070756ba07b148163`, so the permalinks below resolve in both repos.

**Existing issues:** none found (searched on 2026-09-27). I-Connect issues matching "accepted", "complete",
"CommandStatus", "twice", "timeout retry" and "Complete Empty" turned up nothing relevant. The closest is #37
(intermittent `lockAction failed: 99`), which is about connection loss. A downstream report of the symptom exists:
technyon/nuki_hub#278 ("Lock.open executed multiple times": unlatch ran 2–3 times with retries enabled; closed without a
root cause).

---

## Title

`cmdChallAccStateMachine` returns Success right after ACCEPTED and never waits for COMPLETE (`(CommandStatus)Command::Empty == CommandStatus::Complete`)

## Summary

For `CommandWithChallengeAndAccept` commands (`lockAction`, `keypadAction`, Opener `lockAction`), the `CmdAccepted`
state is supposed to wait for the lock's Status `COMPLETE` (0x00) message. The check at `NukiBle.hpp:413` instead
casts `lastMsgCodeReceived` (a `Command`) to `CommandStatus`:

```cpp
} else if ((CommandStatus)lastMsgCodeReceived == CommandStatus::Complete) {
```

When `CmdAccepted` is entered, `lastMsgCodeReceived` has just been reset to `Command::Empty` (0x0000), which is
numerically equal to `CommandStatus::Complete` (0x00). On the next poll, about 10 ms later, the branch matches and
`CmdResult::Success` is returned. The lock is still moving at that point. A real COMPLETE would set
`lastMsgCodeReceived = Command::Status` (0x000E). That casts to 0x0E, so the branch can never match on an actual
COMPLETE message.

As a result, `Success` means "accepted", not "completed". The `ErrorReport` branches in `CmdAccepted` (motor blocked,
canceled, busy, …) are effectively dead code. They only fire if the error arrives within one ~10 ms poll interval.

## Affected version

`a576a19` (current `main`). The line dates from `39bd00f` ("major refactoring to reduce duplicate code",
2022-06-05) and has been unchanged since. Both Smart Lock and Opener are affected, because they share the template.

## Code references

Enums, [`src/NukiConstants.h#L63-L69`](https://github.com/I-Connect/NukiBleEsp32/blob/a576a19b1bde0c9f8a4b4cb070756ba07b148163/src/NukiConstants.h#L63-L69):

```cpp
enum class CommandStatus : uint8_t {
  Complete = 0x00,
  Accepted = 0x01
};

enum class Command : uint16_t {
  Empty                         = 0x0000,
```

`Command::Status = 0x000E` ([L82](https://github.com/I-Connect/NukiBleEsp32/blob/a576a19b1bde0c9f8a4b4cb070756ba07b148163/src/NukiConstants.h#L82)).

State machine, `src/NukiBle.hpp`:
- [L365-L372](https://github.com/I-Connect/NukiBleEsp32/blob/a576a19b1bde0c9f8a4b4cb070756ba07b148163/src/NukiBle.hpp#L365-L372):
  on ACCEPTED, sets `CmdAccepted` and resets `lastMsgCodeReceived = Command::Empty`.
- [L373-L380](https://github.com/I-Connect/NukiBleEsp32/blob/a576a19b1bde0c9f8a4b4cb070756ba07b148163/src/NukiBle.hpp#L373-L380):
  the COMPLETE-without-ACCEPTED path. This one is correct: it checks `Command::Status` and `receivedStatus`.
- [L384-L420](https://github.com/I-Connect/NukiBleEsp32/blob/a576a19b1bde0c9f8a4b4cb070756ba07b148163/src/NukiBle.hpp#L384-L420):
  the `CmdAccepted` state. The faulty check is at
  [L413](https://github.com/I-Connect/NukiBleEsp32/blob/a576a19b1bde0c9f8a4b4cb070756ba07b148163/src/NukiBle.hpp#L413).
- [L28-L57](https://github.com/I-Connect/NukiBleEsp32/blob/a576a19b1bde0c9f8a4b4cb070756ba07b148163/src/NukiBle.hpp#L28-L57):
  `executeAction` polls the state machine every 10 ms.

Message handling:
- [`src/NukiBle.cpp#L1660-L1670`](https://github.com/I-Connect/NukiBleEsp32/blob/a576a19b1bde0c9f8a4b4cb070756ba07b148163/src/NukiBle.cpp#L1660-L1670):
  a Status message sets `receivedStatus = data[0]`.
- [`src/NukiLock.cpp#L1000`](https://github.com/I-Connect/NukiBleEsp32/blob/a576a19b1bde0c9f8a4b4cb070756ba07b148163/src/NukiLock.cpp#L1000):
  sets `lastMsgCodeReceived = returnCode`.

Callers:
- [`src/NukiLock.cpp#L36`](https://github.com/I-Connect/NukiBleEsp32/blob/a576a19b1bde0c9f8a4b4cb070756ba07b148163/src/NukiLock.cpp#L36)
  (`lockAction`) and [#L51](https://github.com/I-Connect/NukiBleEsp32/blob/a576a19b1bde0c9f8a4b4cb070756ba07b148163/src/NukiLock.cpp#L51)
  (`keypadAction`).
- [`src/NukiOpener.cpp#L50`](https://github.com/I-Connect/NukiBleEsp32/blob/a576a19b1bde0c9f8a4b4cb070756ba07b148163/src/NukiOpener.cpp#L50).

## Reproduction

1. Enable `debugNukiCommunication` and call `nukiLock.lockAction(LockAction::Unlatch)` on a locked Smart Lock.
2. The log shows `command ACCEPTED`, then `COMMAND SUCCESS`, and `lockAction()` returns `Success`, roughly 10 ms
   after ACCEPTED. `command COMPLETE` is logged afterwards, seconds later, once the motor has finished.
3. Block the bolt, or hold the key in the lock, so the action fails with `K_ERROR_MOTOR_BLOCKED` (0x42) or
   `K_ERROR_CANCELED` (0x46) after being accepted. `lockAction()` still returns `Success`.

## Impact

- Callers can't tell whether an action completed or failed after acceptance. `Failed` and `Lock_Busy` can only be
  returned before ACCEPTED, or within the first poll after it.
- Retries are ambiguous. Downstream code, such as Nuki Hub's retry loop, retries any non-`Success` result. The one
  real way to double-execute is a `TimeOut` in `CmdSent`
  ([L357-L364](https://github.com/I-Connect/NukiBleEsp32/blob/a576a19b1bde0c9f8a4b4cb070756ba07b148163/src/NukiBle.hpp#L357-L364)):
  the lock received and ran the command, but ACCEPTED was lost or the link dropped. That `TimeOut` looks the same as
  "never received". Repeating lock/unlock does no harm. Repeating unlatch or an Opener actuation opens the door again.
- A naive fix makes this worse. If L413 is corrected but a timeout in `CmdAccepted` still returns plain `TimeOut`,
  then every link drop during the motor run (the longest phase) becomes a retryable failure for a command the lock
  has definitely accepted.

## Suggested fix

1. Check the actual COMPLETE message in `CmdAccepted`, as L373 already does:

   ```cpp
   } else if (lastMsgCodeReceived == Command::Status && (CommandStatus)receivedStatus == CommandStatus::Complete) {
   ```

2. Give "accepted but completion unconfirmed" its own result, so callers never retry once the lock has accepted:

   ```cpp
   enum CmdResult : uint8_t {
     Success   = 1,   // COMPLETE received
     Failed    = 2,
     TimeOut   = 3,   // no ACCEPTED/COMPLETE: lock may or may not have received the command
     Working   = 4,
     NotPaired = 5,
     Lock_Busy = 6,
     Accepted  = 7,   // ACCEPTED received, then COMPLETE/ErrorReport not seen (timeout or disconnect)
     Error     = 99
   };
   ```

   In `CmdAccepted`, return `CmdResult::Accepted` instead of `TimeOut` on timeout (L389-L396).

3. Add a separate completion timeout, because a motor run (unlatch plus hold) can exceed the 3 s default
   `commandTimeoutDuration`. For example, add `setCompletionTimeout(ms)` and use it only in `CmdAccepted`. The
   default should be long enough for a full unlatch, around 10–15 s.

4. Optional, for callers that want the old fire-and-forget behaviour: add a flag or overload (such as
   `lockAction(..., bool waitForComplete = true)`) that returns `Accepted` right after ACCEPTED.

Steps 1–3 change behaviour. `lockAction()` would then block until the motor finishes, and post-accept errors would
surface as `Failed`/`Lock_Busy`. This is worth a changelog note so downstream projects (Nuki Hub,
ESPHome_nuki_lock) can treat `Accepted` as not retryable.
