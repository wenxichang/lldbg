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
#include <string.h>
#include <ctype.h>
#include "SocketBuf.h"

void SB_Init(SocketBuf * sb, SOCKET s)
{
    sb->s = s;
    sb->eobL = 0;
    sb->eobR = 0;
//    sb->tempLen = 0;
    sb->end = 0;
    sb->err = 0;
}

static void SB_Reset(SocketBuf * sb)
{
//    sb->tempLen = 0;
    sb->end = 0;
    sb->err = 0;
}

static int RecvData(SOCKET s, char * buf, int len)
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
            return received - 1;    //Return payload length, excluding the EOF character.

        p += l;
        avail -= l;
    }

    return len; //Buffer is full, but EOF is not reached.
}

int SB_Read(SocketBuf * sb, int bytes)
{
    int rc;
    SB_Reset(sb);

    if (bytes == SB_R_LEFT) {
        rc = RecvData(sb->s, sb->lbuf, SOCKET_BUF_CAP);
    }
    else if (bytes == SB_R_RIGHT) {
        rc = RecvData(sb->s, sb->rbuf, SOCKET_BUF_CAP);
    }
    else {
        rc = RecvData(sb->s, sb->lbuf, bytes < SOCKET_BUF_CAP ? bytes : SOCKET_BUF_CAP);
    }

    if (rc < 0) {
        sb->err = 1;
        return -1;
    }

    if (bytes < 0 && rc < SOCKET_BUF_CAP)
        sb->end = 1;

    return rc;
}

typedef enum
{
    ERR = -1,
    INIT = 0,
    EOB,
    EOF,
    SEPARATERS,
    TEXT
} ParserState;

int SB_ReadAndParse(SocketBuf * sb, const char * separaters, UserParser parser, void * userdata)
{
    char * p = (char *)&sb->eobR;
    char * start = p;   //Start position of a word or a block of space
    int inLeft = 0;     //whether p is in left buffer
    ParserState st = EOB;
    int rc = 0;
    char temp[SOCKET_BUF_TMP];
    int tempLen = 0;    //length of available str in temp

    SB_Reset(sb);
    while (st != EOF && st != ERR) {
        switch (st) {
            case EOB: {
                if (SB_Read(sb, inLeft ? SB_R_RIGHT : SB_R_LEFT) < 0) {
                    rc = -1;
                    st = ERR;
                    break;
                }
                p = inLeft ? sb->rbuf : sb->lbuf;
                inLeft = inLeft ? 0 : 1;
                start = p;

                //if (isspace(*p))
                if (*p && strchr(separaters, *p))
                    st = SEPARATERS;
                else if (*p)
                    st = TEXT;
                else {
                    if (tempLen > 0)
                        rc = parser(userdata, temp, tempLen);

                    if (rc < 0) {
                        st = ERR;
                    }
                    else {
                        rc = 0;
                        st = EOF;
                    }
                }
                break;
            }

            case SEPARATERS: {
                if (tempLen > 0) {
                    rc = parser(userdata, temp, tempLen);
                    if (rc < 0) {
                        st = ERR;
                        break;
                    }
                    else {
                        rc = 0;
                    }
                    tempLen = 0;
                }

                //while (isspace(*p))
                while (*p && strchr(separaters, *p))
                    ++p;

                if (*p) {
                    st = TEXT;
                    start = p;
                }
                else if (sb->end)
                    st = EOF;
                else
                    st = EOB;

                break;
            }

            case TEXT: {
                //while (!isspace(*p) && *p)
                while (*p && !strchr(separaters, *p))
                    ++p;

                if (*p)
                    st = SEPARATERS;
                else if (sb->end)
                    st = EOF;
                else
                    st = EOB;

                if (tempLen > 0) {
                    if (p - start + tempLen > SOCKET_BUF_TMP) {
                        st = ERR;
                        rc = -2;
                        break;
                    }
                    memcpy(temp + tempLen, start, p - start);
                    tempLen += p - start;
                    rc = parser(userdata, temp, tempLen);
                    tempLen = 0;
                }
                else if (st != EOB) {
                    rc = parser(userdata, start, p - start);
                }
                else {
                    if (p - start > SOCKET_BUF_TMP) {
                        st = ERR;
                        rc = -1;
                        break;
                    }
                    memcpy(temp, start, p - start);
                    tempLen = p - start;
                }

                if (rc < 0) {
                    st = ERR;
                }
                else {
                    rc = 0;
                    start = p;
                }

                break;
            }

            default: {
                assert(0 && "Impossibility!");
            }
        }
    }
    return rc;
}

#if 0
static int SB_Parse(SocketBuf * sb, int bufId, UserParser parser, void * userdata);

int SB_ReadAndParse(SocketBuf * sb, UserParser parser, void * userdata)
{
    int rc = 0;
    SB_Reset(sb);
    while (1) {
        rc = SB_Read(sb, SB_R_LEFT);
        if (rc < 0)
            break;

        rc = SB_Parse(sb, 0, parser, userdata);
        if (rc <= 0)
            break;  //0 on End, while negative on error

        rc = SB_Read(sb, SB_R_RIGHT);
        if (rc < 0)
            break;

        rc = SB_Parse(sb, 1, parser, userdata);
        if (rc <= 0)
            break;  //0 on End, while negative on error
    }
    return rc;
}

/*
** Success return:
** 0 when the end of flow is reached, or 1 when not.
** Failure return:
** -2 when a word is too long to be held, or a negative(should be neither -1
** nor -2) when a user parser fails.
*/
int SB_Parse(SocketBuf * sb, int bufId, UserParser parser, void * userdata)
{
    char * buf = bufId ? sb->rbuf : sb->lbuf;
    char * p = buf;
    int rc;

    if (sb->tempLen > 0) {
        int len;
        if (isspace(*p)) {
            len = sb->tempLen;
        }
        else {
            while (!isspace(*p) && *p)
                ++p;
            len = sb->tempLen + (p - buf);
            if (len >= SOCKET_BUF_TMP)
                return -2; //Too long
            memcpy(sb->temp + sb->tempLen, buf, p - buf);
        }
        rc = parser(userdata, sb->temp, len);
        if (rc < 0)
            return rc;  //Parser error
        sb->tempLen = 0;
    }

    while (1) {
        char * end;
        int eof;    //End of flow?
        int eob;    //End of buffer?

        while (isspace(*p))
            ++p;

        if (!*p) {
            return sb->end ? 0 : 1;
        }

        end = p;
        while (!isspace(*end) && *end)
            ++end;

        eof = !*end && sb->end ? 1 : 0;
        eob = !eof && !*end ? 1 : 0;

        if (!eob) {
            rc = parser(userdata, sb->p, end - p);
            if (rc < 0)
                return rc;
        }
        if (eof)
            return 0;
        if (eob)
            return 1;
        p = ++end;
    }
    return 0;
}
#endif
