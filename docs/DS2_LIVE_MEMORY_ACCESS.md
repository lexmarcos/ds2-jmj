# Reading and writing DS2 memory from Linux

The game runs under Proton inside a pressure-vessel container, but its threads
are ordinary Linux threads and its address space is an ordinary one. Both are
reachable from the host with no injection, no rebuild and no CI round trip.

## Finding the process

The Wine process keeps the Windows name:

```bash
for pid in $(ls /proc | grep -E '^[0-9]+$'); do
  tr '\0' ' ' < /proc/$pid/cmdline 2>/dev/null | grep -q 'DarkSoulsII.exe' && echo $pid
done
```

Match `DarkSoulsII.exe` and take the one whose `comm` is `DarkSoulsII.exe`;
the launcher, `steam.exe` and `Injector.exe` all carry the string too.

## The image is not relocated

Wine maps the PE at its preferred base, so **static addresses from the binary
are live addresses**:

```text
0x140000000 -> "MZ"
```

An address read out of `objdump` can be used against `/proc/<pid>/mem`
unchanged. There is no ASLR slide to compute.

## Reading

`/proc/<pid>/mem` is `rw` for the owning user and `ptrace_scope` is 1, which
permits it here. Seek to the address and read:

```python
f = open("/proc/%d/mem" % pid, "rb", 0)
f.seek(0x141616cf8)
ptr, = struct.unpack('<Q', f.read(8))
```

## Writing

The same file opened `"wb"` accepts writes, verified by rewriting a field with
its own value and reading it back. This means a byte patch can be applied to
the running game and reverted seconds later, which replaces the
edit-commit-CI-install-relaunch loop for anything that is a patch rather than a
log.

Keep writes small and reversible: read the original bytes first, keep them, and
restore on the way out.

## Why this matters

The injector remains the right home for a shipped patch. For **investigation**
it is the slow path: every hypothesis cost a CI build and a game restart. With
direct access a hypothesis costs seconds, and the game does not have to be
restarted to try the next one.

## Disassembly

`objdump` reads the PE directly and takes about three seconds for the whole
27MB image:

```bash
x86_64-w64-mingw32-objdump -d --no-show-raw-insn DarkSoulsII.exe > ds2.asm
```

That produces a grep-able 5.6M line listing, which answered "who calls this"
faster than Ghidra could open the project. Ghidra is still needed for
decompilation and RTTI, but not for navigation.

Note the image has two `.text` sections and a `.bind` section, so linear
disassembly has misaligned stretches; a reference that greps to nothing may
still exist inside one of them.
