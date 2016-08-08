/******************************************************************************
* Copyright (C) 2009 Zhang Lei.  All rights reserved.
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

#include <assert.h>
#include "Protocol.h"

//For GetCurrentProcessId and getpid
#ifdef OS_WIN
#include <Windows.h>
#else
#include <unistd.h>
#endif

SOCKET Connect(const char * addrStr, unsigned short port)
{
    SOCKET s;
    struct sockaddr_in addr;
    assert(addrStr);

    s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) {
        return INVALID_SOCKET;
    }
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = inet_addr(addrStr);
    addr.sin_port = htons(port);

    if (connect(s, (struct sockaddr *)&addr, sizeof(addr)) == SOCKET_ERROR) {
        closesocket(s);
        return INVALID_SOCKET;
    }
    return s;
}

int SendBreak(SOCKET s, const char * file, int line, const char * fullpath)
{
    SocketBuf sb;
#ifdef OS_WIN
    int pid = GetCurrentProcessId();
#else
    int pid = getpid();
#endif

    SB_Init(&sb, s);
    SB_Print(&sb, "BR\n%s\n%d\n%d\n%s\n\n", file, line, pid, fullpath);
    SB_Add(&sb, "", 1); //Add the End-of-flow(EOF)
    return SB_Send(&sb);
}

int SendQuit(SOCKET s)
{
    return SendData(s, "QT\n\n", sizeof("QT\n\n")); //Including the EOF
}

int SendErr(SOCKET s, const char * fmt, ...)
{
    SocketBuf sb;
    va_list ap;

    SB_Init(&sb, s);
    SB_Add(&sb, "ER\n", sizeof("ER\n") - 1);
    va_start(ap, fmt);
    SB_VPrint(&sb, fmt, ap);
    va_end(ap);
    SB_Add(&sb, "\n", sizeof("\n")); //Include the End-of-flow(EOF)
    return SB_Send(&sb);
}

int SendOK(SOCKET s, Writer writer, void * writerData)
{
    SocketBuf sb;
    int rc = 0;

    SB_Init(&sb, s);
    SB_Add(&sb, "OK\n", sizeof("OK\n") - 1);
    if (writer)
        while ((rc = writer(writerData, &sb)) == 1);
    SB_Add(&sb, "\n", sizeof("\n")); //Include the End-of-flow(EOF)
    SB_Send(&sb);
    return (rc == 0 && !sb.ioerr) ? 0 : (rc < 0 ? rc : -1);
}

int RecvCmd(SOCKET s, char * buf, int len)
{
    char * p = buf;
    int avail = len;
    int received = 0;

    while (avail > 0) {
        int l = recv(s, p, avail, 0);
        if (l == SOCKET_ERROR)
            return -1;

        received += l;
        if (p[l - 1] == 0)
            return received - 1; //Return payload length, excluding the EOF character.

        p += l;
        avail -= l;
    }

    return -2;  //Too long
}

