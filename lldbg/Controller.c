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

#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <assert.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#include "Socket.h"
#include "SocketBuf.h"
#include "Dump.h"

#ifndef OS_WIN
#include <sys/types.h>
#include <signal.h>
#endif

typedef enum
{
    CMD_INVALID = -1,
    CMD_STEP = 0,
    CMD_NEXT,
    CMD_OUT,
    CMD_RUN,
    CMD_LISTL,
    CMD_LISTU,
    CMD_LISTG,
    CMD_PRINTSTACK,
    CMD_WATCH,
    CMD_EXEC,
    CMD_SETB,
    CMD_DELB,
    CMD_LISTB,
    CMD_MEMORY,
    CMD_ENB,
    CMD_DISB,
    CMD_HELP,
    CMD_FRAME,
    CMD_ASD,
    CMD_LS,
} CmdType;

/*
** THIS ARRAY MUST CORRESPOND EXACTLY TO THE ABOVE ENUM TYPE!!!
*/
const char * g_cmds[] =
{
    "s",
    "n",
    "o",
    "r",
    "ll",
    "lu",
    "lg",
    "ps",
    "w",
    "e",
    "sb",
    "db",
    "lb",
    "m",
    "en",
    "dis",
    "h",
    "frame",
    "asd",
    "ls",
    0,
};

//Connected peer is a localhost address
static int s_local;
//Remote pid, send via BREAK command
static int s_remote_pid;

#ifdef OS_WIN
#define snprintf    _snprintf
#define putenv      _putenv
#else
#define DEF_SIG     SIGUSR2
static int s_ldb_sig = DEF_SIG;
#endif

#define MAX_SRCPATH 512
static char *s_src_paths[MAX_SRCPATH];
static int s_nsrc_path;

static void mainloop(SOCKET s);
static int extractArgs(char * buf, char * argv[]);
static CmdType validateArgs(char * argv[], int argc);
static int sendCmd(SOCKET s, CmdType t, char * argv[], int argc);
static int waitForBreakOrQuit(SocketBuf * sb, const char ** file, const char ** lineno, const char ** fullpath);
static int waitForResponseFirstLine(SocketBuf * sb);
static int showError(SocketBuf * sb);
static int listL(SocketBuf * sb);
static int printStack(SocketBuf * sb);
static int watch(SocketBuf * sb);
static int listB(SocketBuf * sb);
static int watchM(SocketBuf * sb, char * argv[], int argc);
static void showHelp();

#define CMD_LINE 1024
#define MAX_ARGS 8

static int Usage(const char *cmd)
{
    printf("Original RLdb 2.0.0 Copyright (C) 2009 Zhang Lei(louirobert@gmail.com) All rights reserved\n"
        "Modified lldbg 1.0 Copyright (C) 2016 Wen Xichang(wenxichang@163.com)\n"
        "Usage:\n"
        "    %s [options] <command> [args]\n"
        "Options:\n"
        "    -a,--addr <XXX.XXX.XXX.XXX>   -- specify listening address\n"
        "    --port <XXXX>                 -- specify listening port\n"
        "    -s,--source <dir>             -- add source dir\n"
        "    -p,--pid <pid>                -- attach to process\n", cmd);
    exit(1);
}

#ifdef OS_WIN
static int initSocket()
{
    WSADATA wsaData;
    return WSAStartup(MAKEWORD(2, 2), &wsaData) ? -1 : 0;
}

void uninitSocket()
{
    WSACleanup();
}

#else
#define initSocket() 0
#define uninitSocket()
#endif

static void addSourceDir(const char *dir)
{
    if (s_nsrc_path < MAX_SRCPATH) {
        s_src_paths[s_nsrc_path] = strdup(dir);
        if (s_src_paths[s_nsrc_path])
            s_nsrc_path++;
    }
}

static int isLocalConnection(SOCKET s)
{
    struct sockaddr_in peer, sock;
#ifdef OS_LINUX
    socklen_t len;
#else
    int len;
#endif
    len = sizeof(peer);
    getpeername(s, (struct sockaddr *)&peer, &len);
    len = sizeof(sock);
    getsockname(s, (struct sockaddr *)&sock, &len);
    return memcmp(&peer.sin_addr, &sock.sin_addr, sizeof(peer.sin_addr)) == 0;
}

static int notifyRemote(int pid)
{
#ifdef OS_WIN
    char name[128];
    HANDLE notify;

    sprintf(name, "Global\\lldb_sigevent_%d", pid);

    notify = OpenEventA(EVENT_MODIFY_STATE, FALSE, name);
    if (notify == NULL) {
        printf("\nNot local debugging or remote pid is not avaiable\n?>");
        return -1;
    }

    SetEvent(notify);
    CloseHandle(notify);
    return 0;
#else
    return kill((pid_t)pid, s_ldb_sig);
#endif
}

static void interrupt(int sig)
{
    if (s_local && s_remote_pid > 0 ) {
        if (notifyRemote(s_remote_pid)) {
            printf("\nFailed to interrupt process: %d\n?>", s_remote_pid);
        }
    } else {
        printf("\nNot local debugging or remote pid is not avaiable\n?>");
    }

#ifdef OS_WIN
    signal(SIGINT, interrupt);
#endif
}

#ifdef OS_WIN
#define APPEND(s) do {                   \
    int ls = strlen(s);                  \
    if (sizeof(argbuf) - bi > ls) {      \
        memcpy(&argbuf[bi], (s), ls);    \
        bi += ls;                        \
        argbuf[bi] = 0;                  \
    }                                    \
} while(0)

static HANDLE s_child_process = NULL;

static void termChild()
{
    if (s_child_process)
        TerminateProcess(s_child_process, 1);
}

static void startProgram(int argc, char * argv[])
{
    char argbuf[2048];
    int bi = 0;
    int i;
    STARTUPINFOA sinfo;
    PROCESS_INFORMATION pinfo;

    assert(argc > 0);

    APPEND("\"");
    APPEND(argv[0]);
    APPEND("\"");

    for (i = 1; i < argc; ++i) {
        APPEND(" ");
        APPEND(argv[i]);
    }

    ZeroMemory(&sinfo, sizeof(STARTUPINFOA));

    //Child process will not process CTRL+C
    SetConsoleCtrlHandler(NULL, TRUE);
    if (CreateProcessA(NULL, argbuf, NULL, NULL, FALSE, 0, NULL, NULL, &sinfo, &pinfo) == FALSE) {
        printf("Can not execute: %s", argbuf);
        exit(1);
    }

    //Restore ours
    SetConsoleCtrlHandler(NULL, FALSE);
    s_child_process = pinfo.hProcess;

    atexit(termChild);
}
#else
static void startProgram(int argc, char * argv[])
{
    pid_t pid;
    
    assert(argc > 0);

    if ((pid = fork()) < 0) {
        perror("fork");
        exit(1);
    }

    if (pid == 0) {
        setpgid(0, 0);        //Dont need SIGINT from shell
        execvp(argv[0], (char *const *)argv);
        perror("exec");
        exit(1);
    }
}
#endif

#define NEXT_ARG()    do { i++; if (i >= argc) Usage(argv[0]); }while(0)

int main(int argc, char * argv[])
{
    SOCKET s;
    SOCKET a;
    struct sockaddr_in addr;
    char addrStr[64] = {0};
    unsigned short port = 0;
    int prog_idx = 0;
    int prog_pid = 0;

#ifdef OS_LINUX
    const char *sig = getenv("LDB_SIG");
    
    if (sig && atoi(sig))
        s_ldb_sig = atoi(sig);
#endif

    if (argc > 1) {
        int i = 1;
        for (; i < argc; i++) {
            if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
                Usage(argv[0]);
            }
            else if (!strcmp(argv[i], "-a") || !strcmp(argv[i], "--addr")) {
                NEXT_ARG();
                strncpy(addrStr, argv[i], 63);
                addrStr[63] = 0;
            }
            else if (!strcmp(argv[i], "--port")) {
                NEXT_ARG();
                port = (unsigned short)atoi(argv[i]);
            }
            else if (!strcmp(argv[i], "-p") || !strcmp(argv[i], "--pid")) {
                NEXT_ARG();
                prog_pid = atoi(argv[i]);
                if (prog_pid <= 0)
                    Usage(argv[0]);
            }
            else if (!strcmp(argv[i], "-s") || !strcmp(argv[i], "--source")) {
                NEXT_ARG();
                addSourceDir(argv[i]);
            }
            else {
                prog_idx = i;
                break;        //Stop parsing
            }
        }
    }
    if (addrStr[0] == 0)
        strcpy(addrStr, "127.0.0.1");
    if (port == 0)
        port = 2679;

    if (initSocket()) {
        printf("initSocket failed!\n");
        return -1;
    }

    s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) {
        printf("Socket error!\n");
        uninitSocket();
        return -1;
    }

#if defined(OS_LINUX) && defined(SO_REUSEADDR)
    {
        int reuse = 1;
        if (setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char*)&reuse, sizeof(reuse)) < 0)
            perror("setsockopt(SO_REUSEADDR) failed");
    }
#endif

    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = inet_addr(addrStr);
    addr.sin_port = htons(port);

    if (bind(s, (struct sockaddr *)&addr, sizeof(addr)) == SOCKET_ERROR
        || listen(s, 1) == SOCKET_ERROR) {
        printf("Socket error!\nIP %s Port %d\n", addrStr, (int)port);
        closesocket(s);
        uninitSocket();
        return -1;
    }

    if (prog_pid > 0) {
        notifyRemote(prog_pid);
    }
    else if (prog_idx > 0) {
        putenv("LDB_STARTUP=1");
        startProgram(argc - prog_idx, &argv[prog_idx]);
    }
    else {
        printf("<command> or -p <pid> is required.\n\n");
        Usage(argv[0]);
    }

    printf("Original RLdb 2.0.0 Copyright (C) 2009 Zhang Lei\n");
    printf("Modified lldbg 1.0 Copyright (C) 2016 Wen Xichang\n");
    printf("Waiting at %s:%d for remote debuggee...\n", addrStr, (int)port);
    do {
        a = accept(s, NULL, NULL);
    } while (a == SOCKET_ERROR);

    s_local = isLocalConnection(a);
    signal(SIGINT, interrupt);
    
    if (s_local) {
        printf("Connected from localhost!\n");
    } else {
        printf("Connected from remote host!\n");
    }
    
    closesocket(s);
    mainloop(a);
    closesocket(a);
    uninitSocket();
    return 0;
}

static FILE *checkFile(const char * path, const char ** err)
{
    FILE *fp;
    
    fp = fopen(path, "r");
    if (!fp) {
        *err = strerror(errno);
        return NULL;
    }
    
    if (fgetc(fp) == 0x1b) {    //Precompiled bytecode(lua/luajit)
        fclose(fp);
        *err = "Binary source file";
        return NULL;
    }
    
    rewind(fp);
    return fp;
}

static FILE *getFile(const char *file, const char * fullpath, const char ** err)
{
    FILE *fp;
    int i;
    *err = NULL;
    
    if (fullpath) {
        if ((fp = checkFile(fullpath, err)))
            return fp;
    }
    
    for (i = 0; i < s_nsrc_path; ++i) {
        char path[1024];
        snprintf(path, sizeof(path), "%s/%s", s_src_paths[i], file);
        if ((fp = checkFile(path, err)))
            return fp;
    }
    
    if (*err == NULL)
        *err = "No such file or directory";
    
    return NULL;
}

static void showSource(const char * file, int lineno, const char * fullpath, int count)
{
    int i;
    FILE *fp;
    char linebuf[4096];
    const char *err = NULL;
    
    assert(file);
    
    fp = getFile(file, fullpath, &err);
    if (!fp) {
        printf("%s: %s\n", file, err);
        return;
    }
    
    for (i = 0; i < lineno - 1; ++i) {
        fgets(linebuf, sizeof(linebuf), fp);
    }
    
    for (i = 0; i < count; ++i) {
        if (fgets(linebuf, sizeof(linebuf), fp)) {
            char *p = strchr(linebuf, '\n');
            if (p)
                *p = 0;
            
            p = strchr(linebuf, '\r');
            if (p)
                *p = 0;
            
            printf("%d: %s\n", lineno, linebuf);
            lineno++;
        }
    }
    
    fclose(fp);
}

static int ls(const char * file, int line, const char * fullpath, int argc, const char ** argv)
{
    int count = 10;
    if (argc == 2) {
        line = atoi(argv[1]);
    } else if (argc == 3) {
        file = argv[1];
        line = atoi(argv[2]);
        fullpath = NULL;
    } else if (argc == 4) {
        file = argv[1];
        line = atoi(argv[2]);
        count = atoi(argv[3]);
        fullpath = NULL;
    }
    
    if (line <= 0)
        line = 1;
    if (count <= 0)
        count = 10;
    
    showSource(file, line, fullpath, count);
    return line + count;
}

void mainloop(SOCKET s)
{
    SocketBuf sb;
    char frame[12] = { 0 };
    
    SB_Init(&sb, s);
    
    /* setup default frame */
    frame[0] = '1';
    
    while (1) {
        int rc;
        const char * _file;
        const char * _lineno;
        const char * _fullpath;
        int line = 1;
        char file[128];
        char fullpath[1024];
        
        //Wait for a BREAK or QUIT message...
        rc = waitForBreakOrQuit(&sb, &_file, &_lineno, &_fullpath);
        if (rc < 0) {
            printf("Socket or protocol error!\n");
            break;
        }
        if (!rc) {
            printf("Remote script is over!\n");
            break;
        }
        
        line = atoi(_lineno);
        strncpy(file, _file, sizeof(file));
        file[sizeof(file) - 1] = 0;
        strncpy(fullpath, _fullpath, sizeof(fullpath));
        fullpath[sizeof(fullpath) - 1] = 0;
        
        printf("Break At \"%s:%d\"\n", file, line);
        showSource(file, line, fullpath, 1);
        
        while (1) {
            char buf[CMD_LINE];
            char * argv[MAX_ARGS];
            int argc;
            CmdType t = CMD_INVALID;

            //Prompt user...
            printf("?>");
            fgets(buf, CMD_LINE, stdin);
            if ((argc = extractArgs(buf, argv)) > 0)
                t = validateArgs(argv, argc);
            if (argc < 1 || t == CMD_INVALID) {
                printf("Invalid command! Type 'h' for help.\n");
                continue;
            }

            if (t == CMD_HELP) {
                showHelp();
                continue;
            }

            if (t == CMD_FRAME) {
                if (argc == 2) {
                    strncpy(frame, argv[1], sizeof(frame));
                    frame[sizeof(frame) - 1] = 0;
                } else {
                    printf("Current default level: %s\n", frame);
                }
                continue;
            }
            
            if (t == CMD_ASD) {
                if (argc == 2) {
                    addSourceDir(argv[1]);
                }
                continue;
            }
            
            if (t == CMD_LS) {
                line = ls(file, line, fullpath, argc, (const char **)argv);
                continue;
            }
            
            //Setup default level
            if (argc == 1 && (t == CMD_LISTL || t == CMD_LISTG || t == CMD_LISTU)) {
                argv[argc++] = frame;
                printf("Use default level: %s\n", frame);
            }
            
            //Send command...
            if (sendCmd(s, t, argv, argc) < 0) {
                printf("Socket error!\n");
                return;
            }

            if (t == CMD_STEP || t == CMD_OUT || t == CMD_RUN || t == CMD_NEXT)
                break;

            //Wait for result message...
            rc = waitForResponseFirstLine(&sb);
            if (rc < 0) {
                printf("Socket or protocol error!\n");
                return;
            }

            //Show result...
            if (rc == 0) {
                if (showError(&sb) < 0) {
                    printf("Socket or protocol error!\n");
                    return;
                }
                continue;
            }

            switch (t) {
                case CMD_LISTL:
                case CMD_LISTU:
                case CMD_LISTG:
                {
                    rc = listL(&sb);
                    break;
                }

                case CMD_PRINTSTACK: {
                    rc = printStack(&sb);
                    break;
                }

                case CMD_WATCH: {
                    rc = watch(&sb);
                    break;
                }
//
//                case CMD_EXEC: {
//                    rc = exec(&sb);
//                    break;
//                }
//
                case CMD_SETB:
                case CMD_DELB:
                case CMD_ENB:
                case CMD_DISB:
                {
                    //No content in this case, so read out the rest and drop it.
                    rc = SB_Read(&sb, SB_R_LEFT);
                    assert(sb.end);
                    break;
                }

                case CMD_LISTB: {
                    rc = listB(&sb);
                    break;
                }

                case CMD_MEMORY: {
                    rc = watchM(&sb, argv, argc);
                    break;
                }

                default: {
                    assert(0 && "Impossibility!");
                }
            }

            if (rc < 0) {
                printf("Socket or protocol error!\n");
                return;
            }
        }
    }
}

int extractArgs(char * buf, char * argv[])
{
    char * p = buf;
    int argc = 0;

    while (*p && argc < MAX_ARGS) {
        while (isspace(*p))
            ++p;
        if (!*p)
            break;

        argv[argc++] = p;
        if (*p != '"') {
            while (!isspace(*p) && *p)
                ++p;
            if (isspace(*p))
                *p++ = 0;
        }
        else {
            while (*++p != '"' && *p);
            if (!*p)
                return -1;  //End '"' not found!
            *++p = 0;
            ++p;
        }
    }
    return argc;
}

static int allDigits(char * str)
{
    while (isdigit(*str))
        ++str;
    return *str ? 0 : 1;
}

CmdType validateArgs(char * argv[], int argc)
{
    CmdType t = CMD_INVALID;

    if (argc > 0) {
        char * p = argv[0];

        if (!strcmp(p, "s")) {
            if (argc == 1)
                t = CMD_STEP;
        }
        else if (!strcmp(p, "n")) {
            if (argc == 1)
                t = CMD_NEXT;
        }
        else if (!strcmp(p, "o")) {
            if (argc == 1)
                t = CMD_OUT;
        }
        else if (!strcmp(p, "r") || !strcmp(p, "c")) {
            if (argc == 1)
                t = CMD_RUN;
        }
        else if (!strcmp(p, "ll")) {
            if (argc == 1 || (argc == 2 && allDigits(argv[1])))
                t = CMD_LISTL;
        }
        else if (!strcmp(p, "lu")) {
            if (argc == 1 || (argc == 2 && allDigits(argv[1])))
                t = CMD_LISTU;
        }
        else if (!strcmp(p, "lg")) {
            if (argc == 1 || (argc == 2 && allDigits(argv[1])))
                t = CMD_LISTG;
        }
        else if (!strcmp(p, "w")) {
            if (argc > 1) {
                if (allDigits(argv[1]) && argc > 3 && argv[2][1] == 0
                    && (argv[2][0] == 'l' || argv[2][0] == 'u' || argv[2][0] == 'g')) {
                    if (argc == 5) {
                        if (argv[4][0] == 'r' && argv[4][1] == 0)
                            t = CMD_WATCH;
                    }
                    else if (argc == 4)
                        t = CMD_WATCH;
                }
                else if (argv[1][0] == '|') {
                    if (argc == 3) {
                        if (argv[2][0] == 'r' && argv[2][1] == 0)
                            t = CMD_WATCH;
                    }
                    else if (argc == 2)
                        t = CMD_WATCH;
                }
            }
        }
        else if (!strcmp(p, "ps") || !strcmp(p, "bt")) {
            if (argc == 1)
                t = CMD_PRINTSTACK;
        }
        else if (!strcmp(p, "sb") || !strcmp(p, "b")) {
            if (argc == 3 && allDigits(argv[2]))
                t = CMD_SETB;
        }
        else if (!strcmp(p, "db")) {
            if (argc == 2 && allDigits(argv[1]))
                t = CMD_DELB;
        }
        else if (!strcmp(p, "lb")) {
            if (argc == 1)
                t = CMD_LISTB;
        }
        else if (!strcmp(p, "dis")) {
            if (argc == 2 && allDigits(argv[1]))
                t = CMD_DISB;
        }
        else if (!strcmp(p, "en")) {
            if (argc == 2 && allDigits(argv[1]))
                t = CMD_ENB;
        }
        else if (!strcmp(p, "m")) {
            if (argc == 3) {
                char * end;
                char * end2;
                strtoul(argv[1], &end, 0);
                strtoul(argv[2], &end2, 0);
                if (!*end && !*end2)
                    t = CMD_MEMORY;
            }
        }
        else if (!strcmp(p, "h")) {
            t = CMD_HELP;
        }
        else if (!strcmp(p, "f")) {
            if (argc == 1 || (argc == 2 && allDigits(argv[1])))
                t = CMD_FRAME;
        }
        else if (!strcmp(p, "asd")) {
            if (argc == 2)
                t = CMD_ASD;
        }
        else if (!strcmp(p, "ls") || !strcmp(p, "l")) {
            t = CMD_LS;
        }
        else if (!strcmp(p, "q") || !strcmp(p, "quit")) {
            printf("Bye\n");
            exit(0);
        }
    }
    return t;
}

static int SendData(SOCKET s, const char * buf, int len)
{
    while (len > 0) {
        int sent = send(s, buf, len, 0);
        if (sent == SOCKET_ERROR)
            return -1;
        len -= sent;
        buf += sent;
    }
    return 0;
}

int sendCmd(SOCKET s, CmdType t, char * argv[], int argc)
{
    //The buffer size is exactly the same with the one used by fgets in main().
    //So it's convenient to use strcat without worrying about buffer overflow!
    char cmdline[CMD_LINE];
    const char * cmd = g_cmds[t];
    int i;

    strcpy(cmdline, cmd);
    for (i = 1; i < argc; i++) {
        strcat(cmdline, " ");
        strcat(cmdline, argv[i]);
    }

    return SendData(s, cmdline, strlen(cmdline) + 1);
}

int waitForBreakOrQuit(SocketBuf * sb, const char ** file, const char ** lineno, const char ** fullpath)
{
    int rc;
    char * p = sb->lbuf;
    
    rc = SB_Read(sb, SB_R_LEFT);
    if (rc < 0 || !sb->end)
        return -1;

    if (!strncmp(p, "BR\n", 3)) {
        p += 3;
        *file = p;
        p = strchr(p, '\n');
        if (!p)
            return -1;
        *p++ = 0;
        *lineno = p;
        p = strchr(p, '\n');
        if (!p)
            return -1;
        *p++ = 0;
        s_remote_pid = atoi(p);
        p = strchr(p, '\n');
        if (!p)
            return -1;
        *p++ = 0;
        *fullpath = p;
        p = strchr(p, '\n');
        if (!p)
            return -1;
        *p = 0;
        return 1;
    }
    else if (!strcmp(p, "QT\n\n")) {
        return 0;
    }
    return -1;
}

int waitForResponseFirstLine(SocketBuf * sb)
{
    char * p = sb->lbuf;
    if (SB_Read(sb, 3) < 0)
        return -1;
    if (!strncmp(p, "OK\n", 3)) {
        return 1;
    }
    else if (!strncmp(p, "ER\n", 3)) {
        return 0;
    }
    return -1;
}

int showError(SocketBuf * sb)
{
    char * p = sb->lbuf;
    if (SB_Read(sb, SB_R_LEFT) < 0 || !sb->end)
        return -1;

    fprintf(stdout, "%s", p);
    return 0;
}

typedef enum
{
    LV_NAME = 1,
    LV_VALUE
} State_lv;

static int lv(State_lv * st, const char * str, int length);

int listL(SocketBuf * sb)
{
    State_lv st = LV_NAME;
    return SB_ReadAndParse(sb, "\n", (UserParser)lv, &st);
}

static const char * typestr(char t)
{
    const char * tstr;
    switch (t) {
        case 's':
            tstr = "STR";
            break;
        case 'n':
            tstr = "NUM";
            break;
        case 't':
            tstr = "TAB";
            break;
        case 'f':
            tstr = "FNC";
            break;
        case 'u':
            tstr = "URD";
            break;
        case 'U':
            tstr = "LUD";
            break;
        case 'b':
            tstr = "BLN";
            break;
        case 'l':
            tstr = "NIL";
            break;
        case 'd':
            tstr = "THD";
            break;
        default:
            tstr = "";
    }
    return tstr;
}

static void output(const char * str, int length)
{
    int i;
    for (i = 0; i < length; ++i)
        fputc(str[i], stdout);
}

#define dec(ch) \
    ((ch) >= '0' && (ch) <= '9' ? (ch) - '0' : (ch) - 'a' + 10)

#define decode(s, ch) \
    do {\
        ch = (dec((s)[0]) << 4) | dec((s)[1]);\
    } while (0);

static void outputEncStr(const char * str, int length)
{
    const char * end = str + length;
    char ch;
    for (; str < end; str += 2) {
        decode(str, ch);
        fputc(ch, stdout);
    }
}

static int outputStr(const char * str, int length)
{
    const char * end = str + length;
    const char * p = strchr(str, ':');
    int len;
    if (!p || p > end)
        return -1;
    output(str, p - str);
    str = p + 1;
    fputs(" Length:", stdout);
    p = strchr(str, ':');
    if (!p || p > end)
        return -1;
    output(str, p - str);
    fputs(" Truncated-to:", stdout);
    str = p + 1;
    p = strchr(str, ':');
    if (!p || p > end)
        return -1;
    output(str, p - str);
    fputs(" Content:", stdout);
    len = strtol(str, NULL, 10);
    str = p + 1;
    if (end - str != len * 2)
        return -1;
    outputEncStr(str, end - str);
    return 0;
}

static int printVar(const char * str, int length)
{
    const char * tstr = typestr(str[0]);
    if (*tstr == 0)
        return -1;

    fprintf(stdout, "Type:%s \tValue:", tstr);
    switch(str[0]) {
        case 's': {
            if (outputStr(str + 1, length - 1) < 0)
                return -1;
            break;
        }

        case 'n':
        case 'b':
        case 't':
        case 'f':
        case 'u':
        case 'U':
        case 'd':
        {
            output(str + 1, length - 1);
            break;
        }

        case 'l':
            fprintf(stdout, "nil");
            break;
    }
    return 0;
}

int lv(State_lv * st, const char * str, int length)
{
    if (*st == LV_NAME) {
        fprintf(stdout, "Name:");
        output(str, length);
        fputc(' ', stdout);
        fputc('\t', stdout);
        *st = LV_VALUE;
    }
    else {
        if (printVar(str, length) < 0)
            return -3;

        fputc('\n', stdout);
        *st = LV_NAME;
    }
    return 0;
}

typedef enum
{
    PS_FILE,
    PS_LINE,
    PS_NAME,
    PS_WHAT
} State_ps;

static int ps(State_ps * st, const char * word, int length);

int printStack(SocketBuf * sb)
{
    State_ps st = PS_FILE;
    return SB_ReadAndParse(sb, "\n", (UserParser)ps, &st);
}

int ps(State_ps * st, const char * word, int length)
{
    switch (*st) {
        case PS_FILE: {
            fputs("At \"", stdout);
            output(word, length);
            fputc(':', stdout);
            *st = PS_LINE;
            break;
        }
        case PS_LINE: {
            output(word, length);
            fputs("\" \t", stdout);
            *st = PS_NAME;
            break;
        }
        case PS_NAME: {
            output(word, length);
            fputs(" \t", stdout);
            *st = PS_WHAT;
            break;
        }
        case PS_WHAT: {
            output(word, length);
            fputc('\n', stdout);
            *st = PS_FILE;
            break;
        }
    }
    return 0;
}

typedef enum
{
    W_VAR = 1,  //for all
    W_META,     //for all
    W_KEY,      //for table
    W_VAL,      //for table
    W_SIZE,     //for full userdata
    W_WHAT,     //for function
    W_SRC,      //for function
    W_FIRSTLINE,//for function
    W_LASTLINE, //for function
    W_STATUS    //for thread
} State_w;

typedef struct
{
    State_w st;
    State_w st2;    //What state after W_META
} Arg_w;

static int w(Arg_w * args, const char * word, int length);

int watch(SocketBuf * sb)
{
    Arg_w args = { W_VAR, 0 };
    return SB_ReadAndParse(sb, "\n", (UserParser)w, &args);
}

int w(Arg_w * args, const char * word, int length)
{
    switch (args->st) {
        case W_KEY: {
            fputs("--------------------------------------------------\n", stdout);
            if (printVar(word, length) < 0)
                return -3;
            fputc('\n', stdout);
            args->st = W_VAL;
            break;
        }

        case W_VAL: {
            if (printVar(word, length) < 0)
                return -3;
            fputc('\n', stdout);
            args->st = W_KEY;
            break;
        }

        case W_VAR: {
            if (printVar(word, length) < 0)
                return -3;
            fputc('\n', stdout);
            args->st = W_META;
            switch (word[0]) {
                case 't': {
                    args->st2 = W_KEY;
                    break;
                }
                case 'u': {
                    args->st2 = W_SIZE;
                    break;
                }
                case 'f': {
                    args->st2 = W_WHAT;
                    break;
                }
                case 'd': {
                    args->st2 = W_STATUS;
                    break;
                }
                default: {
                    args->st2 = 0;
                }
            }
            break;
        }

        case W_META: {
            if (length != 1)
                return -3;

            if (word[0] == '1') {
                fputs("HasMetatable:Yes\n", stdout);
            }
            else {
                fputs("HasMetatable:No\n", stdout);
            }
            args->st = args->st2;
            break;
        }

        case W_SIZE: {
            fputs("Size:", stdout);
            output(word, length);
            fputc('\n', stdout);
            args->st = 0;
            break;
        }

        case W_WHAT: {
            fputs("What:", stdout);
            output(word, length);
            args->st = W_SRC;
            break;
        }

        case W_SRC: {
            fputs(" \tFile:", stdout);
            output(word, length);
            args->st = W_FIRSTLINE;
            break;
        }

        case W_FIRSTLINE: {
            fputs(" \tLineDefined:", stdout);
            output(word, length);
            args->st = W_LASTLINE;
            break;
        }

        case W_LASTLINE: {
            fputs(" \tLastLine:", stdout);
            output(word, length);
            fputc('\n', stdout);
            args->st = 0;
            break;
        }

        case W_STATUS: {
            fputs("Status:", stdout);
            output(word, length);
            fputc('\n', stdout);
            args->st = 0;
            break;
        }

        default: {
            return -3;
        }
    }
    return 0;
}

typedef enum
{
    LB_IDX,
    LB_FILE,
    LB_LINE,
    LB_ENABLE,
} State_lb;

static int lb(State_lb * st, const char * word, int length);

int listB(SocketBuf * sb)
{
    State_lb st = LB_IDX;
    return SB_ReadAndParse(sb, "\n", (UserParser)lb, &st);
}

int lb(State_lb * st, const char * word, int length)
{
    switch (*st) {
    case LB_IDX:
        output(word, length);
        fputs(". ", stdout);
        *st = LB_FILE;
        break;
    case LB_FILE:
        fputc('"', stdout);
        output(word, length);
        fputc(':', stdout);
        *st = LB_LINE;
        break;
    case LB_LINE:
        output(word, length);
        fputs("\"", stdout);
        *st = LB_ENABLE;
        break;
    case LB_ENABLE:
        fputs(*word == '0' ? ", disable\n" : ", enable\n", stdout);
        *st = LB_IDX;
        break;
    default:
        assert(0);
    }
    return 0;
}

#define PROVIDER_BUF_SIZE 1024

typedef struct
{
    SOCKET s;
    unsigned int len;
    char buf[PROVIDER_BUF_SIZE];
} Arg_wm;

static int provide(Arg_wm * args, const char ** buf, unsigned int * size);

int watchM(SocketBuf * sb, char * argv[], int argc)
{
    unsigned int addr = strtoul(argv[1], NULL, 0);
    unsigned int len;
    char * end;
    Arg_wm args;

    if (SB_Read(sb, 9) < 0 || sb->end)
        return -1;

    len = strtoul(sb->lbuf, &end, 16);
    if (len == 0 || *end != '\n' || sb->lbuf + 8 != end)
        return -2;

    args.s = sb->s;
    args.len = len;
    return Dump(addr, (DataProvider)provide, &args, stdout, NULL, NULL);
}

int provide(Arg_wm * args, const char ** buf, unsigned int * size)
{
    if (args->len > 0) {
        int l = recv(args->s, args->buf,
            args->len < PROVIDER_BUF_SIZE ? args->len : PROVIDER_BUF_SIZE, 0);
        if (l == SOCKET_ERROR || !l)
            return -1;
        args->len -= l;
        *size = l;
        *buf = args->buf;
        return 1;
    }
    return 0;
}

static const char *HELP_CONTENT =
"RLdb 2.0.0 Copyright (C) 2009 Zhang Lei(louirobert@gmail.com) All rights reserved.\n"
"Modified lldbg 1.0 Copyright (C) 2016 Wen Xichang(wenxichang@163.com)\n"
"\n"
"Valid commands:\n"
"  sb or b <file-path> <line-no>       -- Set a breakpoint\n"
"  db <index>                          -- Delete a breakpoint(lb to list breakpoint)\n"
"  en <index>                          -- Enable a breakpoint\n"
"  dis <index>                         -- Disable a breakpoint\n"
"  lb                                  -- List breakpoints\n"
"  f <stack-level>                     -- Set default stack-level for lg/ll/lu\n"
"  lg [stack-level]                    -- List globals\n"
"  ll [stack-level]                    -- List locals\n"
"  lu [stack-level]                    -- List upvalues\n"
"  m <start-address> <length>          -- Watch memory\n"
"  n                                   -- Run to next line\n"
"  o                                   -- Step out\n"
"  ps or bt                            -- Print calling stack\n"
"  r or c                              -- Run program until a breakpoint\n"
"  s                                   -- Step into\n"
"  w <stack-level> <l|u|g> <variable-name>[properties] [r]\n"
"    or w <properties> [r]             -- Watch a variable\n"
"  asd <source-dir>                    -- Add source dir for source searching\n"
"  ls [file] [lineno] [count]          -- View source code\n"
"\n"
"  q or quit                           -- Quit debugger\n"
"  ctrl+c                              -- Break program(local host only)\n";

void showHelp()
{
    fputs(HELP_CONTENT, stdout);
}
