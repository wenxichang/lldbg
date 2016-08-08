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

#include <stdarg.h>
#include "Socket.h"

#ifndef SOCKET_BUF_CAP
#define SOCKET_BUF_CAP 4096
#endif

/*
** NOTE:
** The following functions with prefix SB_ all operates on a Socket Bufffer
** defined immediately below. The main purpose of the series of function is to
** reduce the times to call send and to provide convenient methods for sending
** formatted string.
** All these functions return a negative on failure, and zero on success.
** IMPORTANT:
** All these functions, when encounter a socket failure, will set an error flag(ioerr),
** and the next call to these functions with ioerr set will fail. SB_Reset can
** clear the error flag.
*/
typedef struct {
    SOCKET s;
    char * p;
    int avail;
    int ioerr;
    char buf[SOCKET_BUF_CAP];
} SocketBuf;

/*
** Init a Socket Buffer
*/
void SB_Init(SocketBuf * sb, SOCKET s);

/*
** Reset a Socket Buffer, that is, clear error flag and reset buffer state to init state.
*/
void SB_Reset(SocketBuf * sb);

/*
** Add a format string to buffer in a Socket Buffer. If the buffer is full, then
** send the its content and rest the buffer and continue filling the buffer with
** the rest of string.
** The fmt argument specifies a format like printf does, but with more restriction
** and some extension.
*/
int SB_Print(SocketBuf * sb, const char * fmt, ...);

int SB_VPrint(SocketBuf * sb, const char * fmt, va_list ap);

/*
** Add data to buffer in a Socket Buffer. If the buffer is full, then send the
** buffer's content and reset the buffer and continue filling the buffer with the
** rest data.
*/
int SB_Add(SocketBuf * sb, const void * data, int len);

/*
** Send the content in buffer right now.
*/
int SB_Send(SocketBuf * sb);

int SendData(SOCKET s, const void * buf, int len);

#endif
