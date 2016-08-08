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

#ifndef __SOCKETBUF_H__
#define __SOCKETBUF_H__

#include "Socket.h"

#ifndef SOCKET_BUF_CAP
#define SOCKET_BUF_CAP 1024
#endif

#ifndef SOCKET_BUF_TMP
#define SOCKET_BUF_TMP 1024
#endif

#if SOCKET_BUF_TMP > SOCKET_BUF_CAP
#error "Socket temp buf can not be greater than a single socket buf."
#endif

typedef struct {
    SOCKET s;
    char lbuf[SOCKET_BUF_CAP];
    int eobL;
    char rbuf[SOCKET_BUF_CAP];
    int eobR;
//    char temp[SOCKET_BUF_TMP];
//    int tempLen;
    int end;
    int err;
} SocketBuf;

void SB_Init(SocketBuf * sb, SOCKET s);

#define SB_R_LEFT -2
#define SB_R_RIGHT -3
/*
** Read specified bytes into sb('s left buffer). When bytes is -2/-3, read until
** the left/right buffer is full or the EOF is reached.
** Return the bytes read. When a socket IO error happens, -1 is returned.
*/
int SB_Read(SocketBuf * sb, int bytes);

/*
** Should return 0 on success and a negative on error.
*/
typedef int (* UserParser)(void * userdata, const char * word, int length);

/*
** Read a flow into sb while parsing it. Call the parser when a word is found,
** which is separated by specified separaters.
** Return 0 when success, -1 when a socket error happens, or -2 when a word is
** too long(greater than SOCKET_BUF_TMP), or a negative returned by a user parser.
** A user parser should not return -1 nor -2 in order to be distinguished.
*/
int SB_ReadAndParse(SocketBuf * sb, const char * separaters, UserParser parser, void * userdata);

#endif
