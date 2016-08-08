/******************************************************************************
* Copyright (C) 2009 Zhang Lei.  All rights reserved.
* Modification work Copyright (C) 2016 Wen Xichang.
*
* Permission is hereby granted, free of charge, to any person obtaining
* a copy of this software and associated documentation files (the
* "Software"), to deal in the Software without restriction, including
* without limitation the rights to use, copy, modify, merge, publish,
* distribute, sublicense, and/or sell copies of the Software, and to
* permit persons to whom the Software is furnished to do so, subject to
* the following conditions:
*
* The above copyright notice and this permission notice shall be
* included in all copies or substantial portions of the Software.
*
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
******************************************************************************/

#include <lua.h>
#include <lauxlib.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <string.h>
#include <assert.h>
#include <signal.h>

#include "list.h"

#ifdef OS_WIN
#include <io.h>     //access
#define strtoull _strtoui64
#endif

#ifdef OS_LINUX
#include <unistd.h> //access, getcwd
#define _MAX_PATH PATH_MAX

static char * _fullpath(char * absPath, const char * relPath, size_t maxLen)
{
    char * ret = 0;
    assert(absPath && relPath && maxLen > 1);
    if (*relPath == '/') {
        size_t len = strlen(relPath);
        if (len <= maxLen - 1) {
            strcpy(absPath, relPath);
            ret = absPath;
        }
    }
    else {
        ret = getcwd(absPath, maxLen);
        if (ret) {
            size_t len = strlen(absPath);
            size_t rlen = strlen(relPath);
            if (absPath[len - 1] != '/' && len < maxLen && rlen) {
                absPath[len++] = '/';
            }
            if (len + rlen < maxLen) {
                strcpy(absPath + len, relPath);
                ret = absPath;
            }
        }
    }
    return ret;
}

#endif

#include "Protocol.h"

typedef enum
{
    STEP = 1,
    NEXT,
    STEP_OUT,
    FINISH,
    RUN
} CMD;

//Multiple states, may use in embeded program
#define MAX_STATE       1024
#define MAX_LINENO      65536

static lua_State *s_states[MAX_STATE];
static int s_nstate;

//Debugger remote socket
static SOCKET s_dbg_sock = INVALID_SOCKET;
static int s_signaled = 0;

//Hope this level is enough for lua calls :-)
#define INIT_LEVEL      100000000

//DebugInfo
static CMD s_cmd = STEP;
static int s_level = INIT_LEVEL;
static int s_blevel = 0;

//For cache value
static int s_cacheval_ref = LUA_NOREF;
static lua_State *s_cacheval_L = NULL;

//Breakpoints
typedef struct BRK
{
    struct hlist_node hlist;
    struct list_head list;
    char *file;
    int lineno;
    int enable;
} BRK;

//Breakpoints seq, hashed
static struct hlist_head s_breaks[MAX_LINENO];

//Breakpoints ordered
static LIST_HEAD(s_break_head);

static BRK *BRKNew(const char *path, int lineno)
{
    BRK *b = calloc(1, sizeof(BRK));
    if (!b)
        return NULL;
    
    b->file = strdup(path);
    if (!b->file) {
        free(b);
        return NULL;
    }
    b->lineno = lineno;
    b->enable = 1;
    
    list_add_tail(&b->list, &s_break_head);
    return b;
}

static void BRKFree(BRK *b)
{
    hlist_del(&b->hlist);
    list_del(&b->list);
    
    free(b->file);
    free(b);
}

static void getFileName(char * out, const char * file, size_t outLen)
{
    const char * p = strrchr(file, '/');
    if (p) {
        ++p;
    }
    else {
        p = strrchr(file, '\\');
        if (p) {
            ++p;
        }
        else {
            p = file;
        }
    }

    strncpy(out, p, outLen);
    out[outLen - 1] = 0;
}

static void hook(lua_State *L, lua_Debug *ar);

static void onGC(void)
{
    if (s_dbg_sock != INVALID_SOCKET) {
        SendQuit(s_dbg_sock);
        closesocket(s_dbg_sock);
        s_dbg_sock = INVALID_SOCKET;
    }
}

static SOCKET tryConnectToDebugger(void)
{
    unsigned short port = 2679;
    char * p;

    //read config and set up connection with a remote controller
    p = getenv("LDB_PORT");
    if (p && atoi(p)) {    //REMOTE_LDB's value is sth. like "192.168.0.1:6688".
        port = atoi(p);
    }
    
    return Connect("127.0.0.1", port);
}

static void rldbSignaled(int sig)
{
    int i;
    
    for (i = 0; i < s_nstate; ++i) {
        lua_sethook(s_states[i], hook, LUA_MASKLINE | LUA_MASKCALL | LUA_MASKRET, 0);
    }

    s_signaled = 1;
}

#ifdef OS_WIN
static DWORD WINAPI waitSig(LPVOID lpParam)
{
    char name[128];
    HANDLE notify;

    sprintf(name, "Global\\lldb_sigevent_%d", GetCurrentProcessId());

    notify = CreateEventA(NULL, FALSE, FALSE, name);
    if (!notify)
        return -1;

    while (1) {
        if (WaitForSingleObject(notify, INFINITE) == WAIT_OBJECT_0) {
            rldbSignaled(0);
        }
    }
}
#endif

#ifdef OS_WIN
__declspec(dllexport)
#endif
int luaopen_lldb(lua_State * L)
{
    static int sig_installed;
    int i;
    
    if (!sig_installed) {
#ifdef OS_WIN
        CreateThread(NULL, 0, waitSig, NULL, 0, NULL);
#else
        const char *sig = getenv("LDB_SIG");
        if (sig && atoi(sig)) {
            signal(atoi(sig), rldbSignaled);
        } else {
            signal(SIGUSR2, rldbSignaled);
        }
#endif
        if (getenv("LDB_STARTUP") && *getenv("LDB_STARTUP") == '1') {
            s_dbg_sock = tryConnectToDebugger();
        }
        
        atexit(onGC);
        sig_installed = 1;
    }
    
    for (i = 0; i < s_nstate; ++i) {
        if (s_states[i] == L)
            goto end_ret;
    }
    
    if (s_nstate >= MAX_STATE) {
        fprintf(stderr, "Max lua state reached: %d\n", s_nstate);
        goto end_ret;
    }
    s_states[s_nstate++] = L;

    //Debugger present, break immediately
    if (s_dbg_sock != INVALID_SOCKET)
        lua_sethook(L, hook, LUA_MASKLINE | LUA_MASKCALL | LUA_MASKRET, 0);

end_ret:
    lua_pushboolean(L, 1);
    return 1;
}

static int prompt(lua_State *L, lua_Debug * ar);
static int checkBreakPoint(lua_State *L, lua_Debug * ar);

static void clearhooks(void)
{
    int i;
    for (i = 0; i < s_nstate; ++i)
        lua_sethook(s_states[i], hook, 0, 0);
    
    //Clear cache value
    if (s_cacheval_L) {
        luaL_unref(s_cacheval_L, LUA_REGISTRYINDEX, s_cacheval_ref);
        s_cacheval_L = NULL;
        s_cacheval_ref = LUA_NOREF;
    }
    
    //Clear breakpoints
    for (i = 0; i < MAX_LINENO; ++i) {
        struct hlist_node *pos, *next;
        
        hlist_for_each_safe(pos, next, &s_breaks[i]) {
            BRK *b = hlist_entry(pos, BRK, hlist);
            BRKFree(b);
        }
    }
}

void hook(lua_State * L, lua_Debug * ar)
{
    int event = ar->event;
    int top = lua_gettop(L);
    
    lua_getinfo(L, "nSl", ar);
    
    if (ar->currentline < 0)
        return;
    
    //Connect to debugger when signaled
    if (s_signaled) {
        s_signaled = 0;
        if (s_dbg_sock == INVALID_SOCKET) {
            s_dbg_sock = tryConnectToDebugger();
            if (s_dbg_sock == INVALID_SOCKET) {
                clearhooks();
                return;
            }
        }
        //Connect success, break in current line and wait debugger's cmd
        s_cmd = STEP;
    }

    if (event == LUA_HOOKLINE) {
        int rc = 0;

        if (s_cmd == STEP) {
            rc = prompt(L, ar);
        }
        else if (s_cmd == NEXT) {
            if (s_blevel && s_level <= s_blevel)
                rc = prompt(L, ar);
            else
                rc = checkBreakPoint(L, ar);
        }
        else if (s_cmd == STEP_OUT) {
            if (s_blevel && s_level < s_blevel)
                rc = prompt(L, ar);
            else
                rc = checkBreakPoint(L, ar);
        }
        else if (s_cmd == FINISH) {
            //prompt(L, ar);
        }
        else if (s_cmd == RUN) {
            rc = checkBreakPoint(L, ar);
        }

        //If a socket IO error or a protocol error happened, stop debugging
        //without informing the remote Controller.
        if (rc < 0) {
            clearhooks();
            closesocket(s_dbg_sock);
            s_dbg_sock = INVALID_SOCKET;
        }
    }
    else {
        assert(event != LUA_HOOKCOUNT);

        if (event == LUA_HOOKCALL) {
            s_level++;
        }
        else if (event == LUA_HOOKRET || event == LUA_HOOKTAILRET) {
            s_level--;
        }
    }
    assert(top == lua_gettop(L));
}

/*
** Check if the current line contains a breakpoint. If yes, break and prompt
** for user, and reset statck level to 0 preparing for the next "OVER" command.
*/
int checkBreakPoint(lua_State * L, lua_Debug * ar)
{
    int breakpoint = 0;
    char path[_MAX_PATH + 1];
    struct hlist_node *pos;
    
    lua_getinfo(L, "Sl", ar);
    
    if (ar->currentline >= MAX_LINENO)
        return 0;

    getFileName(path, ar->short_src, _MAX_PATH);
    
#ifdef OS_WIN
    _strlwr(path);
#endif

    hlist_for_each(pos, &s_breaks[ar->currentline]) {
        BRK *b = hlist_entry(pos, BRK, hlist);
        if (b->enable && !strcmp(path, b->file)) {
            breakpoint = 1;
            break;
        }
    }
    
    if (breakpoint) {
        return prompt(L, ar);
    }
    return 0;
}

static int getCmd(SOCKET s, char * buf, int bufLen, char ** argv);
static int listLocals(lua_State * L, lua_Debug * ar, char * argv[], int argc, SOCKET s);
static int listUpVars(lua_State * L, lua_Debug * ar, char * argv[], int argc, SOCKET s);
static int listGlobals(lua_State * L, lua_Debug * ar, char * argv[], int argc, SOCKET s);
static int printStack(lua_State * L, SOCKET s);
static int watch(lua_State * L, lua_Debug * ar, char * argv[], int argc, SOCKET s);
static int exec(lua_State * L, lua_Debug * ar, char * argv[], int argc, SOCKET s);
static int setBreakPoint(lua_State * L, const char * src, char * argv[], int argc, SOCKET s);
static int oprBreakPoint(lua_State * L, const char * opr, char * argv[], int argc, SOCKET s);
static int listBreakPoints(lua_State * L, SOCKET s);
static int watchMemory(char * argv[], int argc, SOCKET s);

/*
** Return -1 when a socket io error happens, or 0 when succeed.
*/
int prompt(lua_State * L, lua_Debug * ar)
{
    SOCKET s = s_dbg_sock;
    int top = lua_gettop(L);
    char path[_MAX_PATH + 1];
    char name[_MAX_PATH];
    
    lua_getinfo(L, "nSl", ar);
    if (!_fullpath(path, ar->short_src, _MAX_PATH)) {
        strncpy(path, ar->short_src, _MAX_PATH);
        path[_MAX_PATH] = 0;
    }

    getFileName(name, ar->short_src, _MAX_PATH);
    
    if (SendBreak(s, name, ar->currentline, path) < 0) {
        fprintf(stderr, "Socket error!\n");
        return -1;
    }

    //Each prompt, we set s_level to INIT_LEVEL, and reset s_blevel;
    s_level = INIT_LEVEL;
    s_blevel = 0;
    
    while (1) {
        char buf[PROT_MAX_CMD_LEN];
        char * argv[PROT_MAX_ARGS];
        int argc;
        char * pCmd;
        char ** pArgv;
        int rc;

        argc = getCmd(s, buf, PROT_MAX_CMD_LEN, argv);
        if (argc == -1) {
            fprintf(stderr, "Socket or protocol error!\n");
            return -1;
        }
        if (argc == 0) {
            if (SendErr(s, "Invalid command!") < 0) {
                fprintf(stderr, "Socket error!\n");
                return -1;
            }
            continue;
        }
        pCmd = argv[0];
        argc--;
        pArgv = argv + 1;

        if (!strcmp(pCmd, "s")) {
            s_cmd = STEP;           //Step command don't need s_blevel, breaks all the time
            break;
        }
        else if (!strcmp(pCmd, "n")) {
            s_cmd = NEXT;
            s_blevel = s_level;     //Next breaks when s_level <= s_blevel
            break;
        }
        else if (!strcmp(pCmd, "o")) {
            s_cmd = STEP_OUT;
            s_blevel = s_level;     //Step out breaks when s_level < s_blevel
            break;
        }
        else if (!strcmp(pCmd, "f")) {
            s_cmd = FINISH;
            break;
        }
        else if (!strcmp(pCmd, "r")) {
            s_cmd = RUN;
            break;
        }
        else if (!strcmp(pCmd, "ll")) {
            rc = listLocals(L, ar, pArgv, argc, s);
        }
        else if (!strcmp(pCmd, "lu")) {
            rc = listUpVars(L, ar, pArgv, argc, s);
        }
        else if (!strcmp(pCmd, "lg")) {
            rc = listGlobals(L, ar, pArgv, argc, s);
        }
        else if (!strcmp(pCmd, "w")) {
            rc = watch(L, ar, pArgv, argc, s);
        }
        else if (!strcmp(pCmd, "ps")) {
            rc = printStack(L, s);
        }
        else if (!strcmp(pCmd, "sb")) {
            rc = setBreakPoint(L, ar->short_src, pArgv, argc, s);
        }
        else if (!strcmp(pCmd, "db") || !strcmp(pCmd, "en") || !strcmp(pCmd, "dis")) {
            rc = oprBreakPoint(L, pCmd, pArgv, argc, s);
        }
        else if (!strcmp(pCmd, "lb")) {
            rc = listBreakPoints(L, s);
        }
        else if (!strcmp(pCmd, "e")) {
            rc = exec(L, ar, pArgv, argc, s);
        }
        else if (!strcmp(pCmd, "m")) {
            rc = watchMemory(pArgv, argc, s);
        }
        else {
            rc = SendErr(s, "Invalid command!");
        }

        if (rc < 0) {
            fprintf(stderr, "Socket or protocol error!\n");
            return -1;
        }
    }

    assert(top == lua_gettop(L));
    return 0;
}

/*
** Get command from via socket s and put it in buf; then extract arguments in
** buf, which are separated by one single space. The result argument array is
** stored in argv, which can hold PROT_MAX_ARGS arguments at most. The actual number
** of arguments is returned. If a socket IO error happens, -1 is returned.
*/
int getCmd(SOCKET s, char * buf, int bufLen, char ** argv)
{
    int argc = 0;
    char * end;
    char * p = buf;
    int received = RecvCmd(s, buf, bufLen);
    if (received < 0) {
        return -1;
    }
    end = buf + received;
    *end = 0;

    while (p < end && argc < PROT_MAX_ARGS) {
        while (*p == ' ' && p < end)
            ++p;

        if (*p != '"') {
            argv[argc++] = p;
            while (*p != ' ' && p < end)
                ++p;
            if (p == end)
                break;
        }
        else {
            argv[argc++] = ++p;
            p = strchr(p, '"');
            if (!p)
                return -2;  //The end '"' is not found!
        }
        *p++ = 0;
    }
    return argc;
}

/*
** Print one line text containing a variable name and its value into sb.
** Variable value is on top of L. L stays unchanged after call.
*/
static void printVar(SocketBuf * sb, const char * name, lua_State * L)
{
    int type = lua_type(L, -1);

    if (name)
        SB_Print(sb, "%s\n", name);

    switch(type) {
        case LUA_TSTRING: {
            size_t len;
            const char * str = lua_tolstring(L, -1, &len);
            int truncLen = len > PROT_MAX_STR_LEN ? PROT_MAX_STR_LEN : len;
            SB_Print(sb, "s%p:%d:%d:%Q\n", str, len, truncLen,
                str, truncLen); //%Q requires two arguments: buf and length
            break;
        }
        case LUA_TNUMBER: {
            /*
            ** LUA_NUMBER may be double or integer, So a runtime check may be required.
            ** Otherwise SB_Print may be crashed.
            */
            SB_Print(sb, "n%N\n", lua_tonumber(L, -1));
            break;
        }
        case LUA_TTABLE: {
            SB_Print(sb, "t%p\n", lua_topointer(L, -1));
            break;
        }
        case LUA_TFUNCTION: {
            SB_Print(sb, "f%p\n", lua_topointer(L, -1));
            break;
        }
        case LUA_TUSERDATA: {
            SB_Print(sb, "u%p\n", lua_touserdata(L, -1));
            break;
        }
        case LUA_TLIGHTUSERDATA: {
            SB_Print(sb, "U%p\n", lua_touserdata(L, -1));
            break;
        }
        case LUA_TBOOLEAN: {
            SB_Print(sb, "b%d\n", lua_toboolean(L, -1) ? 1 : 0);
            break;
        }
        case LUA_TTHREAD: {
            SB_Print(sb, "d%p\n", lua_topointer(L, -1));
            break;
        }
        case LUA_TNIL: {
            SB_Print(sb, "l\n");
            break;
        }
    }
}

typedef struct
{
    lua_State * L;
    lua_Debug * ar;
} Args_ll;

static int ll(Args_ll * args, SocketBuf * sb);

/*
** Input format:
** ll [stack level]
**
** Output format:
** OK
** Name Value
** Name Value
** ...
**
** L stays unchanged.
*/
int listLocals(lua_State * L, lua_Debug * ar, char * argv[], int argc, SOCKET s)
{
    struct lua_Debug AR;
    int level;
    Args_ll args;

    if (argc > 0) {
        level = strtol(argv[0], NULL, 10);
    }
    else {
        level = 1;
    }

    if (--level != 0) {
        if (!lua_getstack(L, level, &AR)) {
            return SendErr(s, "No local variable info available at stack level %d.",
                level + 1);
        }
        ar = &AR;
    }
    args.L = L;
    args.ar = ar;
    return SendOK(s, (Writer)ll, &args);
}

int ll(Args_ll * args, SocketBuf * sb)
{
    int i = 1;
    const char * name;

    while ((name = lua_getlocal(args->L, args->ar, i++))) {
        if (name[0] != '(')   //(*temporary)
            printVar(sb, name, args->L);
        lua_pop(args->L, 1);
    }
    return 0;
}

static int lu(lua_State * L, SocketBuf * sb);

/*
** Input format:
** lu [stack level]
**
** Output format:
** OK
** Name Value
** Name Value
** ...
**
** L stays unchanged.
*/
int listUpVars(lua_State * L, lua_Debug * ar, char * argv[], int argc, SOCKET s)
{
    struct lua_Debug AR;
    int level;
    int rc;

    if (argc > 0) {
        level = strtol(argv[0], NULL, 10);
    }
    else {
        level = 1;
    }

    if (--level != 0) {
        if (!lua_getstack(L, level, &AR)) {
            return SendErr(s, "No up variable info available at stack level %d.",
                level + 1);
        }
        ar = &AR;
    }

    lua_getinfo(L, "f", ar);
    rc = SendOK(s, (Writer)lu, L);
    lua_pop(L, 1);
    return rc;
}

int lu(lua_State * L, SocketBuf * sb)
{
    int i = 1;
    const char * name;

    while ((name = lua_getupvalue(L, -1, i++))) {
        printVar(sb, name, L);
        lua_pop(L, 1);
    }
    return 0;
}

static int lg(lua_State * L, SocketBuf * sb);

/*
** Input format:
** lg [stack level]
**
** Output format:
** OK
** Name Value
** Name Value
** ...
**
** L stays unchanged.
*/
int listGlobals(lua_State * L, lua_Debug * ar, char * argv[], int argc, SOCKET s)
{
    struct lua_Debug AR;
    int level;
    int rc;

    if (argc > 0) {
        level = strtol(argv[0], NULL, 10);
    }
    else {
        level = 1;
    }

    if (--level != 0) {
        if (!lua_getstack(L, level, &AR)) {
            return SendErr(s, "No global variable info available at stack level %d.",
                level + 1);
        }
        ar = &AR;
    }

    lua_getinfo(L, "f", ar);
    lua_getfenv(L, -1);
    assert(lua_istable(L, -1));
    rc = SendOK(s, (Writer)lg, L);
    lua_pop(L, 2);
    return rc;
}

static int isID(const char * name)
{
    if (!(isalpha(*name) || *name == '_'))
        return 0;
    ++name;
    while (isalnum(*name) || *name == '_')
        ++name;
    return !*name ? 1 : 0;
}

int lg(lua_State * L, SocketBuf * sb)
{
    lua_pushnil(L);
    while (lua_next(L, -2)) {
        if (lua_isstring(L, -2)) {
            size_t len;
            const char * name = lua_tolstring(L, -2, &len);
            if (strlen(name) == len && isID(name))
                printVar(sb, name, L);
        }
        lua_pop(L, 1);
    }
    return 0;
}

static int lookupVar(lua_State * L, lua_Debug * ar, int level, char scope,
    const char * name, int nameLen);
static int lookupField(lua_State * L, const char * field);
static int w(lua_State * L, SocketBuf * sb);

/*
** Input format:
** w <level> <l|u|g> <name>[fields] [r]
** or:
** w [fields] [r]
**
** in which, fields have the form like |n123.4|b0|s"hello"|s008b917a|f006c4560|...
** Output format:
** OK
** Detail
**
** L stays unchanged.
*/
int watch(lua_State * L, lua_Debug * ar, char * argv[], int argc, SOCKET s)
{
    int remember = 0;
    char * fields = NULL;
    int rc;
    int top = lua_gettop(L);

    if (argc >= 3) {
        int level = strtol(argv[0], NULL, 10);
        char scope = argv[1][0];
        char * name = argv[2];
        char * nameEnd = strchr(name, '|');
        int nameLen = nameEnd ? nameEnd - name : strlen(name);

        if (level < 1 || argv[1][1] != 0 || !(scope == 'l' || scope == 'u' || scope == 'g'))
            return SendErr(s, "Invalid argument!");
        if (!lookupVar(L, ar, level, scope, name, nameLen)) {
            assert(lua_gettop(L) == top);
            return SendErr(s, "Variable is not found!");
        }
        if (argc > 3 && !strcmp(argv[3], "r"))
            remember = 1;
        fields = nameEnd;
    }
    else {
        if (s_cacheval_L != L || s_cacheval_ref == LUA_NOREF) {
            assert(lua_gettop(L) == top);
            return SendErr(s, "Variable is not found!");
        }
        
        lua_rawgeti(L, LUA_REGISTRYINDEX, s_cacheval_ref);
        if (lua_isnil(L, -1)) {
            lua_pop(L, 1);
            assert(lua_gettop(L) == top);
            return SendErr(s, "Variable is not found!");
        }
        if (argc > 0)
            fields = argv[0];
        if (argc > 1 && !strcmp(argv[1], "r"))
            remember = 1;
    }

    if (fields && !lookupField(L, fields)) {
        lua_pop(L, 1);
        assert(lua_gettop(L) == top);
        return SendErr(s, "Field is not found!");
    }

    rc = SendOK(s, (Writer)w, L);
    if (remember) {
        if (s_cacheval_L) {
            luaL_unref(s_cacheval_L, LUA_REGISTRYINDEX, s_cacheval_ref);
        }
        
        s_cacheval_ref = luaL_ref(L, LUA_REGISTRYINDEX);
        s_cacheval_L = L;
        
        if (fields)
            lua_pop(L, 1);
    }
    else {
        lua_pop(L, fields ? 2 : 1);
    }

    assert(lua_gettop(L) == top);
    return rc;
}

/*
** Look up a lua variable with specified stack level, scope and name.
** Return 1 when found and push it on top of L; otherwise 0 is returned and
** L stays unchanged.
*/
int lookupVar(lua_State * L, lua_Debug * ar, int level, char scope, const char * name, int nameLen)
{
    lua_Debug AR;
    int found = 0;

    if (level != 1) {
        if (!lua_getstack(L, level - 1, &AR))
            return 0;
        ar = &AR;
    }

    if (scope == 'l') {
        int i = 1;
        const char * p;

        lua_pushnil(L); //place holder
        while ((p = lua_getlocal(L, ar, i++))) {
            if (nameLen == strlen(p) && !strncmp(p, name, nameLen)) {
                found = 1;
                lua_replace(L, -2); //The same name may have multi values(though it's odd!), use the last!
            }
            else {
                lua_pop(L, 1);
            }
        }
        if (!found)
            lua_pop(L, 1);
    }
    else if (scope == 'u') {
        int i = 1;
        const char * p;

        lua_getinfo(L, "f", ar);
        while ((p = lua_getupvalue(L, -1, i++))) {
            if (nameLen == strlen(p) && !strncmp(p, name, nameLen)) {
                found = 1;
                break;
            }
            else {
                lua_pop(L, 1);
            }
        }
        lua_remove(L, found ? -2 : -1);
    }
    else {
        lua_getinfo(L, "f", ar);
        lua_getfenv(L, -1);
        assert(lua_istable(L, -1));
        lua_pushlstring(L, name, nameLen);
        lua_gettable(L, -2);
        if (lua_isnil(L, -1)) {
            lua_pop(L, 3);
        }
        else {
            found = 1;
            lua_replace(L, -3);
            lua_pop(L, 1);
        }
    }

    return found;
}

static int nextField(const char * fields, const char ** begin, const char ** end)
{
    assert(fields);
    if (*fields++ != '|' || !*fields)
        return 0;

    *begin = fields;
    if (fields[0] == 's' && fields[1] == '\'') {
        while (1) {
            const char * p = strchr(fields + 2, '\'');
            if (!p)
                return 0;
            ++p;
            if (*p != '|' && *p != 0)
                return 0;
            *end = p;
            break;
        }
    }
    else {
        const char * p = strchr(fields + 1, '|');
        *end = p ? p : fields + strlen(fields);
    }
    return 1;
}

/*
** Get a table field by comparing its value's pointer and type with the specified
** one.
** Return 1 and push the value on top of L when success; otherwise return 0 and
** push nothing.
*/
static int getFieldValueByPtr(lua_State * L, void * ptr, int type)
{
    lua_pushnil(L);
    while (lua_next(L, -2)) {
        int t = lua_type(L, -1);

        if (t == LUA_TTABLE || t == LUA_TFUNCTION || t == LUA_TTHREAD) {
            if (lua_topointer(L, -1) == ptr) {
                lua_remove(L, -2);
                return 1;
            }
        }
        else if (t == LUA_TUSERDATA || t == LUA_TLIGHTUSERDATA) {
            if (lua_touserdata(L, -1) == ptr) {
                lua_remove(L, -2);
                return 1;
            }
        }
        else if (t == LUA_TSTRING) {
            if (lua_tostring(L, -1) == ptr) {
                lua_remove(L, -2);
                return 1;
            }
        }

        lua_pop(L, 1);
    }
    return 0;
}

/*
** Get a table field. Return 1 and push the field value on top of L; otherwise
** return 0 and push nothing.
** The table is on top of L. The field is sth. like "n123.456", "f008bae20", etc.
*/
static int getFieldValue(lua_State * L, const char * fieldBegin, const char * fieldEnd)
{
    char * end;

    if (*fieldBegin == 'n') {
        double num = strtod(fieldBegin + 1, &end);
        if (end != fieldEnd)
            return 0;
        lua_pushnumber(L, num);
        lua_gettable(L, -2);
    }
    else if (*fieldBegin == 's' && fieldBegin[1] == '\'') {
        const char * str = fieldBegin + 2;
        assert(*(fieldEnd - 1) == '\'');
        lua_pushlstring(L, str, fieldEnd - str - 1);
        lua_gettable(L, -2);
    }
    else if (*fieldBegin == 'b') {
        int n = strtol(fieldBegin + 1, &end, 0);
        if (end != fieldEnd)
            return 0;
        lua_pushboolean(L, n);
        lua_gettable(L, -2);
    }
    else if (*fieldBegin == 'U') {
        void *ptr = (void *)strtoull(fieldBegin + 1, &end, 0);
        if (end != fieldEnd)
            return 0;
        lua_pushlightuserdata(L, ptr);
        lua_gettable(L, -2);
    }
    else {
        void *ptr;
        int t;

        switch (*fieldBegin) {
            case 't':
                t = LUA_TTABLE;
                break;
            case 'u':
                t = LUA_TTABLE;
                break;
            case 'f':
                t = LUA_TTABLE;
                break;
            case 'd':
                t = LUA_TTABLE;
                break;
            default:
                return 0;
        }

        ptr = (void *)strtoull(fieldBegin + 1, &end, 0);
        if (end != fieldEnd)
            return 0;
        return getFieldValueByPtr(L, (void *)ptr, t);
    }
    return 1;
}

/*
** Look up a field in a lua value on top of L. If the field is found, it's pushed
** on top of L and 1 is returned; otherwise nothing is pushed and 0 is returned.
** field is a descriptive string like '|n123|s"something"|f0088abe8|...'.
** Specially, "|" is a field representing the value itself. And particularly,
** "|m" is a legal field denoting the metatable. So surprisingly, any Lua value
** , not limited to table, can have subfields, because according to offical Lua
** document, "Every value in Lua may have a metatable". For example, consider
** "a = 123", then for variable a, 'a|m|s"__add"' is legal.
*/
int lookupField(lua_State * L, const char * field)
{
    const char * subfieldBegin;
    const char * subfieldEnd;

    assert(field && *field);

    lua_pushvalue(L, -1);
    while (*field && nextField(field, &subfieldBegin, &subfieldEnd)) {
        if (*subfieldBegin == 'm') {
            if (!lua_getmetatable(L, -1))
                break;
        }
        else {
            if (!lua_istable(L, -1))
                break;
            if (!getFieldValue(L, subfieldBegin, subfieldEnd))
                break;
        }
        lua_replace(L, -2);
        field = subfieldEnd;
    }

    if (field[0] && !(field[0] == '|' && field[1] == 0)) {
        lua_pop(L, 1);
        return 0;
    }
    return 1;
}

int w(lua_State * L, SocketBuf * sb)
{
    int t = lua_type(L, -1);
    int meta;
    if (t != LUA_TNIL && (meta = lua_getmetatable(L, -1)))
        lua_pop(L, 1);
    printVar(sb, NULL, L);

    switch (t) {
        case LUA_TTABLE: {
            SB_Print(sb, "%d\n", meta ? 1 : 0);
            lua_pushnil(L);
            while (lua_next(L, -2)) {
                lua_pushvalue(L, -2);
                printVar(sb, NULL, L);
                lua_pop(L, 1);
                printVar(sb, NULL, L);
                lua_pop(L, 1);
            }
            break;
        }
        case LUA_TUSERDATA: {
            int size = lua_objlen(L, -1);
            SB_Print(sb, "%d\n%d\n", meta ? 1 : 0, size);
            break;
        }
        case LUA_TFUNCTION: {
            lua_Debug ar;
            lua_pushvalue(L, -1);
            lua_getinfo(L, ">S", &ar);
            SB_Print(sb, "%d\n%s\n%s\n%d\n%d\n", meta ? 1 : 0, ar.what,
                ar.short_src, ar.linedefined, ar.lastlinedefined);
            break;
        }
        case LUA_TNUMBER: {
            SB_Print(sb, "%d\n", meta ? 1 : 0);
            break;
        }
        case LUA_TSTRING: {
            SB_Print(sb, "%d\n", meta ? 1 : 0);
            break;
        }
        case LUA_TBOOLEAN: {
            SB_Print(sb, "%d\n", meta ? 1 : 0);
            break;
        }
        case LUA_TLIGHTUSERDATA: {
            SB_Print(sb, "%d\n", meta ? 1 : 0);
            break;
        }
        case LUA_TTHREAD: {
            int status = lua_status(lua_tothread(L, -1));
            SB_Print(sb, "%d\n%d\n", meta ? 1 : 0, status);
            break;
        }
    }
    return 0;
}

static int ps(lua_State * L, SocketBuf * sb);

/*
** Input format:
** ps
**
** Output format:
** OK
** File
** Line Number
** Function Name
** Name What
** File
** Line Number
** Function Name
** Name What
** ...
**
** L stays unchanged.
*/
int printStack(lua_State * L, SOCKET s)
{
    return SendOK(s, (Writer)ps, L);
}

int ps(lua_State * L, SocketBuf * sb)
{
    struct lua_Debug ar;
    int i = 0;

    while (lua_getstack(L, i, &ar)) {
        lua_getinfo(L, "nSl", &ar);
        SB_Print(sb, "%s\n%d\n%s\n%s\n", ar.short_src, ar.currentline,
            ar.name ? ar.name : "[N/A]", *ar.what ? ar.what : "[N/A]");
        i++;
    }
    return 0;
}

/*
** Input format:
** sb <File> <Line>
**
** Output format:
** OK
**
*/
int setBreakPoint(lua_State * L, const char * src, char * argv[], int argc, SOCKET s)
{
    int line;
    const char * file;
    char path[_MAX_PATH + 1];
    struct hlist_node *pos, *next;
    int found = 0;
    
    if (argc < 2 || (line = strtol(argv[1], NULL, 10)) <= 0) {
        return SendErr(s, "Invalid argument!");
    }

    if (line >= MAX_LINENO) {
        return SendErr(s, "Invalid line number!");
    }
    
    if (!strcmp(argv[0], "."))
        file = src;
    else
        file = argv[0];

    getFileName(path, file, _MAX_PATH);

#ifdef OS_WIN
    _strlwr(path);
#endif

    hlist_for_each_safe(pos, next, &s_breaks[line]) {
        BRK *b = hlist_entry(pos, BRK, hlist);
        if (!strcmp(path, b->file)) {
            found = 1;
            break;
        }
    }
    
    if (!found) {
        BRK *b = BRKNew(path, line);
        if (!b)
            return SendErr(s, "Out of memory!");
        
        hlist_add_head(&b->hlist, &s_breaks[line]);
    }
    
    return SendOK(s, NULL, NULL);
}

/*
** Input format:
** db <index>
** en <index>
** dis <index>
**
** Output format:
** OK
**
*/
int oprBreakPoint(lua_State * L, const char * opr, char * argv[], int argc, SOCKET s)
{
    int idx;
    BRK *targetB = NULL;
    int i = 1;
    struct list_head *pos;
    
    if (argc < 1 || (idx = strtol(argv[0], NULL, 10)) <= 0) {
        return SendErr(s, "Invalid argument!");
    }
    
    list_for_each(pos, &s_break_head) {
        BRK *b = list_entry(pos, BRK, list);
        if (i == idx) {
            targetB = b;
            break;
        }
        ++i;
    }
    
    if (!targetB)
        return SendErr(s, "Breakpoint not found!");
    
    if (!strcmp(opr, "db")) {
        BRKFree(targetB);
    } else if (!strcmp(opr, "en")) {
        targetB->enable = 1;
    } else if (!strcmp(opr, "dis")) {
        targetB->enable = 0;
    } else {
        assert(0);
    }
    
     return SendOK(s, NULL, NULL);
}

static int lb(lua_State * L, SocketBuf * sb);

/*
** Input format:
** lb
**
** Output format:
** OK
** File
** Line Number
** File
** Line Number
** ...
**
*/
int listBreakPoints(lua_State * L, SOCKET s)
{
    return SendOK(s, (Writer)lb, L);
}

int lb(lua_State * L, SocketBuf * sb)
{
    int i = 1;
    struct list_head *pos;
    list_for_each(pos, &s_break_head) {
        BRK *b = list_entry(pos, BRK, list);
        SB_Print(sb, "%d\n%s\n%d\n%d\n", i, b->file, b->lineno, b->enable);
        ++i;
    }
    
    return 0;
}

/*
** L stays unchanged.
*/
int exec(lua_State * L, lua_Debug * ar, char * argv[], int argc, SOCKET s)
{
    return -2;
}

/*
** Input format:
** m <addr> <len>
**
** Output format:
** OK
** content
**
** Warning:
** This function may read an unreadable address and cause a hard/OS exception!
*/
int watchMemory(char * argv[], int argc, SOCKET s)
{
    void * addr;
    unsigned int len;
    SocketBuf sb;

    if (argc < 2 || (addr = (void *)strtoull(argv[0], NULL, 0)) == 0
        || (len = strtoull(argv[1], NULL, 0)) <= 0
        || (unsigned long)((unsigned long)addr + len) < (unsigned long)addr) //overflow!
    {
        return SendErr(s, "Invalid argument!");
    }

    SB_Init(&sb, s);
    SB_Print(&sb, "OK\n%08x\n", len);
    SB_Add(&sb, addr, len);
    return SB_Send(&sb);
}
