## MiniShell
A simplified Linux interactive shell with support for:
- external commands
- built-in commands
  - `cd`
  - `exit`
  - `fg`
  - `bg`
  - `jobs`
  - `unset`
  - `export`
- I/O redirection
- pipelines
- signal handling
- variable assignment & environment export
- foreground & background command execution with basic job control

### Build
```
make clean release
./release/minishell
```

### Examples
Multiple redirections:
```
MS: >testfile 5>&1 6>&2  sh -c 'echo HELLO >&5; echo WORLD >&6'
WORLD
MS: cat testfile
HELLO
```

Pipelines with redirects:
```
MS: 5>&1  sh -c 'echo hello world! >&5' | sed 's/hello/goodbye/' | cat -v
goodbye world!
```

Stop signal places synchronous commands in the background:
```
MS: sh -c 'sleep 5; killall -SIGSTOP sleep;' & sleep 100
[0] 1234
...5 secs later...
[1] Stopped
```
