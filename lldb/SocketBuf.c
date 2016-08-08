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
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include "SocketBuf.h"

#ifdef OS_LINUX
#define _gcvt gcvt
#define _CVTBUFSIZE 512
#endif

#ifdef OS_WIN
static int g_socketInited = 0;
BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved)
{
    if (fdwReason == DLL_PROCESS_ATTACH) {
        WSADATA wsaData;
        if (WSAStartup(MAKEWORD(2, 2), &wsaData))
            return FALSE;
        g_socketInited = 1;
    }
    else if (fdwReason == DLL_PROCESS_DETACH) {
        if (g_socketInited)
            WSACleanup();
    }
    return TRUE;
}
#endif


void SB_Init(SocketBuf * sb, SOCKET s)
{
    sb->s = s;
    sb->avail = SOCKET_BUF_CAP;
    sb->p = sb->buf;
    sb->ioerr = 0;
}

void SB_Reset(SocketBuf * sb)
{
    sb->avail = SOCKET_BUF_CAP;
    sb->p = sb->buf;
    sb->ioerr = 0;
}

int SB_Add(SocketBuf * sb, const void * data, int len)
{
    const char * d = (const char *)data;
    while (len > 0) {
        int l = sb->avail >= len ? len : sb->avail;
        memcpy(sb->p, d, l);
        sb->avail -= l;
        sb->p += l;
        len -= l;
        d += l;

        if (!len)
            break;

        if (SendData(sb->s, sb->buf, SOCKET_BUF_CAP) < 0) {
            sb->ioerr = 1;
            return -1;
        }

        sb->avail = SOCKET_BUF_CAP;
        sb->p = sb->buf;
    }
    return 0;
}

/*
** Functions like SB_Add except that SB_AddRepeat repeats adding character ch count count,
** rather than adds a block of memory.
*/
static int SB_AddRepeat(SocketBuf * sb, char ch, int count)
{
    while (count > 0) {
        int l = sb->avail >= count ? count : sb->avail;
        memset(sb->p, ch, l);
        sb->avail -= l;
        sb->p += l;
        count -= l;

        if (!count)
            break;

        if (SendData(sb->s, sb->buf, SOCKET_BUF_CAP) < 0) {
            sb->ioerr = 1;
            return -1;
        }

        sb->avail = SOCKET_BUF_CAP;
        sb->p = sb->buf;
    }
    return 0;
}

static char g_map[16] = {'0', '1', '2', '3', '4', '5', '6', '7', '8', '9', 'a', 'b', 'c', 'd', 'e', 'f'};

#define HB(ch) (((ch) >> 4) & 0x0F)
#define LB(ch) ((ch) & 0x0F)

#define ENCODE(buf, ch)\
    do {\
        (buf)[0] = g_map[HB(ch)]; \
        (buf)[1] = g_map[LB(ch)]; \
    } while (0);

/*
** Functions like SB_Add, but SB_AddQuote first encodes the input str, then adds
** the encoded string to buffer. The encoding method is simple: represent the value
** of a char variable in two ANSI readable characters. For example, if there's
** char a = 0x80;
** then encode(a) == "80"
*/
static int SB_AddQuote(SocketBuf * sb, const char * str, int len)
{
    const char * end = str + len;
    while (str < end) {
        //Fill buf in sb until the buf is full or end of str is reached.
        while (sb->avail >= 2 && str < end) {
            ENCODE(sb->p, *str);
            sb->p += 2;
            sb->avail -= 2;
            ++str;
        }
        //Return if str is totally put into buf.
        if (str == end)
            break;

        //Send current full buf in sb if there's more data in str.
        if (SendData(sb->s, sb->buf, SOCKET_BUF_CAP - sb->avail) < 0) {
            sb->ioerr = 1;
            return -1;
        }
        //Reset sb for the next filling.
        sb->avail = SOCKET_BUF_CAP;
        sb->p = sb->buf;
    }
    return 0;
}

static const char * nextArg(const char * fmt, char * flag, int * width,
    char * type, const char ** endArg)
{
    const char * p = fmt;
    const char * beginArg;
    char ch;

    while (1) {
        while (*p != '%' && *p)
            ++p;

        if (!*p)
            return NULL;

        if (*(p + 1) == '%') {
            p += 2;
            continue;
        }

        beginArg = p;
        ch = *++p;
        if (ch == '0' || ch == '-' || ch == '+' || ch == ' ' || ch == '#') {
            *flag = ch;
            ++p;
            ch = *p;
        }
        else {
            *flag = 0;
        }

        if (isdigit(ch)) {
            int w = ch - '0';
            while (isdigit(*++p)) {
                w = w * 10 + *p - '0';
            }
            *width = w;
            ch = *p;
        }
        else {
            *width = -1;
        }

        assert(ch != '.' && "Precision is not supported yet!");
        assert(ch != 'h' && ch != 'l' && ch != 'I' && "Type prefix is not supported yet!");

        if (ch == 's' || ch == 'x' || ch == 'd' || ch == 'N' || ch == 'Q' || ch == 'p') {
            *type = ch;
            *endArg = ++p;
            break;
        }
        else {
            assert(0 && "Invalid format type!");
        }

    }
    return beginArg;
}

int SB_VPrint(SocketBuf * sb, const char * fmt, va_list ap)
{
    const char * p = fmt;
    int rc = 0;

    while (*p) {
        char flag;
        int width;
        char type;
        const char * argEnd;
        const char * arg = nextArg(p, &flag, &width, &type, &argEnd);
        if (!arg) {
            rc = SB_Add(sb, p, strlen(p));
            break;
        }
        if ((rc = SB_Add(sb, p, arg - p)) < 0)
            break;
        p = argEnd;

        if (type == 'd') {
            int num;
            char buf[33];

            assert(!flag && width == -1);
            num = va_arg(ap, int);
            sprintf(buf, "%d", num);
            rc = SB_Add(sb, buf, strlen(buf));
        }
        else if (type == 'p') {
            void *p;
            char buf[33];
            
            assert(!flag && width == -1);
            p = va_arg(ap, void *);
            sprintf(buf, "%p", p);
            rc = SB_Add(sb, buf, strlen(buf));
        }
        else if (type == 'x') {
            unsigned int num;
            char buf[33];
            int len;

            assert(flag == '0');
            num = va_arg(ap, unsigned int);
            sprintf(buf, "%x", num);
            len = strlen(buf);
            if (len < width && (rc = SB_AddRepeat(sb, '0', width - len)) < 0)
                break;
            rc = SB_Add(sb, buf, len);
        }
        else if (type == 's') {
            const char * str;

            assert(!flag && width == -1);
            str = va_arg(ap, const char *);
            rc = SB_Add(sb, str, strlen(str));
        }
        else if (type == 'N') {
            double d;
            char buf[_CVTBUFSIZE];
            int len;

            assert(!flag && width == -1);
            d = va_arg(ap, double);
            _gcvt(d, 100, buf);
            len = strlen(buf);
            if (buf[len - 1] == '.')
                --len;
            rc = SB_Add(sb, buf, len);
        }
        else if (type == 'Q') {
            const char * str;
            int len;

            assert(!flag && width == -1);
            str = va_arg(ap, const char *);
            len = va_arg(ap, int);
            rc = SB_AddQuote(sb, str, len);
        }

        if (rc < 0)
            break;
    }

    return rc;
}

int SB_Print(SocketBuf * sb, const char * fmt, ...)
{
    int rc = 0;
    va_list ap;

    if (sb->ioerr)
        return -1;

    va_start(ap, fmt);
    rc = SB_VPrint(sb, fmt, ap);
    va_end(ap);
    return rc;
}

int SB_Send(SocketBuf * sb)
{
    int rc;
    if (sb->ioerr)
        return -1;

    rc = SendData(sb->s, sb->buf, SOCKET_BUF_CAP - sb->avail);
    sb->ioerr = rc < 0 ? 1 : 0;
    return rc;
}

int SendData(SOCKET s, const void * buf, int len)
{
    const char * b = (const char *)buf;
    while (len > 0) {
        int sent = send(s, b, len, 0);
        if (sent == SOCKET_ERROR)
            return -1;
        len -= sent;
        b += sent;
    }
    return 0;
}
