# My Shell (msh)

A simple shell written in C.

#### Here are some of the supported features:

- Executing existing binaries and commands from the system. E.g. `cd`, `ls`, `date`, `wc`, `cat` and more.
- Re-using previous command line arguments with command `history`.
- Commands are appended to a history file, `.msh_history` in the `$HOME` directory.
- Filename expansion with globbing is supported, using these characters `*, ?, [], ~`.
- Handles basic I/O redirection using `>, >>, <` for files and piping I/O between processes with `|`.

### Quick Setup:

1. Clone this repository with

```
git clone https://github.com/billwonggg/msh.git
```

2. Compile and create a binary

```
gcc msh.c -o msh
```

3. Run the shell

```
./msh
```
