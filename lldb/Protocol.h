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

#ifndef __PROTOCOL_H__
#define __PROTOCOL_H__

#include "Socket.h"
#include "SocketBuf.h"

/*
** Max length of command from the controller, including the terminating zero.
*/
#define PROT_MAX_CMD_LEN 1024

/*
** Max number of arguments contained in one command. The command itself counts,
** i.g. command "ll 2" containing 2 arguments.
*/
#define PROT_MAX_ARGS 8

/*
** Max length of string to be sent to controller, when a value of type string
** presents.
*/
#define PROT_MAX_STR_LEN 256

/*
** Connect to a remote controller.
*/
SOCKET Connect(const char * addr, unsigned short port);

/*
** Send break message.
** Return 0 when success, or -1 when socket error.
**
** Message format:
** BR
** File
** Line Number
**
*/
int SendBreak(SOCKET s, const char * file, int line, const char * fullpath);

/*
** Send quit message.
**
** Message format:
** QT
**
*/
int SendQuit(SOCKET s);

/*
** Respond with error.
** Return 0 when success, or -1 when socket error.
**
** Message format:
** ER
** msg-body
**
*/
int SendErr(SOCKET s, const char * fmt, ...);

/*
** User defined writer function. When called, should return 1 when there are
** more data to write, and 0 when no more, and a negative when some error
** happens. SendOK will return the code by writer. So a writer should not
** return -1 in order to be distinguished from Socket IO Error(-1) returned by
** SendOK.
*/
typedef int (* Writer)(void * writerData, SocketBuf * sb);

/*
** Respond with OK.
** Return 0 when success, or -1 when socket error.
**
** Message format:
** OK
** msg-body
**
*/
int SendOK(SOCKET s, Writer writer, void * writerData);

/*
** Wait for command from remote controller.
** The comand must be a one-line text without a end-of-line character but a
** terminating zero(EOF).
** Return the payload length, excluding the end EOF character.
*/
int RecvCmd(SOCKET s, char * buf, int len);

#endif
