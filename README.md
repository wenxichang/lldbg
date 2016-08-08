# What is lldbg

lldbg(local lua debugger) modified from RLdb(http://luaforge.net/projects/rldb/). Major changes:

1. Add ability to attach to other process that runs lua scripts(like gdb --pid)
2. Multiple lua_State support(although multithreading is not supported)
3. Faster breakpoint check
4. Add ability to view source code, precompiled(lua/luajit) bytecode support
5. Now support n/s/o/c debug command(for Next line/Step in/Step out/Continue)

# Usage

## Build

```
cd lldb && make;
cd lldbg && make
```

MSVC build - Create a project and compile lldb/\*.c to lldb.dll and lldbg/\*.c to lldbg.exe. Remember to add -DOS_WIN to projects.

## Run
**require "lldb"** and everything is ready, this line will **NOT** slow down your script, lldb works on SIGUSR2 signal(default in linux), and EventObject in windows, so debugger will keep deactived till lldbg starts.

# Command line help
shoud run lldbg --help to see more.

# Debugging help
shoud type 'h' when debugging to see more, sorry :-(.

# Tips
1. In linux, send SIGUSR2 to a process will terminate it(so be careful using lldbg -p)
2. Embeded lua-engine program can use code below to setup lldb support while create a new state(nobody want to care about the debugger):

```
lua_getglobal(L, "require");
lua_pushliteral(L, "lldb");
if (lua_pcall(L, 1, 0, 0)) {
    lua_pop(L, 1);
}
```