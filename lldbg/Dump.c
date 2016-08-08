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

#include "Dump.h"
#include <string.h>
#include <assert.h>


#define printChar(ch, out) \
    do {\
        if ((ch) >= 32 && (ch) <= 126)\
            fputc(ch, out);\
        else\
            fputc('.', out);\
    } while (0);

#define COLUMN 16

typedef enum
{
    INIT,   //Only before first RD_Get, a RowData object is in this state.
    NORM,
    END,
    ERR
} RowDataState;

typedef struct
{
    DataProvider dp;
    void * userdata;
    size_t firstCol;
    char temp[COLUMN];
    const char * buf; //For Data Provider. Valid only when state == NORM.
    size_t len; //For Data Provider. Valid only when state == NORM.
    size_t pos; //For Data Provider. Valid only when state == NORM.
    RowDataState state;
} RowData;

static void RD_Init(RowData * rd, DataProvider dp, void * userdata, size_t firstColumn)
{
    rd->dp = dp;
    rd->userdata = userdata;
    rd->firstCol = firstColumn;
    rd->state = INIT;
}

static int RD_Get(RowData * rd, const char ** array, size_t * prelen, size_t * bodylen)
{
    if (rd->state == NORM) {
        if (rd->pos + COLUMN <= rd->len) {
            *array = rd->buf + rd->pos;
            *prelen = 0;
            *bodylen = COLUMN;
            rd->pos += COLUMN;
            return 1;
        }
        else {
            int rc;
            char * temp = rd->temp;
            const char * buf = rd->buf;
            size_t len = rd->len;
            size_t avail = len - rd->pos;
            size_t need = COLUMN - avail;

            memcpy(temp, buf + rd->pos, avail);
            while ((rc = rd->dp(rd->userdata, &buf, &len)) == 1 && len < need) {
                memcpy(temp + avail, buf, len);
                need -= len;
                avail += len;
            }

            *array = temp;
            *prelen = 0;
            if (rc == 1) {
                memcpy(temp + avail, buf, need);
                rd->buf = buf;
                rd->len = len;
                rd->pos = need;
                *bodylen = COLUMN;
            }
            else if (rc == 0) {
                rd->state = END;
                *bodylen = avail;
            }
            else {
                rd->state = ERR;
                *bodylen = avail;
            }
        }
        return *bodylen > 0 ? 1 : 0;
    }
    else if (rd->state == INIT) {
        int rc;
        char * temp = rd->temp;
        const char * buf;
        size_t len;
        size_t avail = rd->firstCol;
        size_t need = COLUMN - avail;

        while ((rc = rd->dp(rd->userdata, &buf, &len)) == 1 && len < need) {
            memcpy(temp + avail, buf, len);
            need -= len;
            avail += len;
        }

        *array = temp;
        *prelen = rd->firstCol;
        if (rc == 1) {
            memcpy(temp + avail, buf, need);
            rd->state = NORM;
            rd->buf = buf;
            rd->len = len;
            rd->pos = need;
            *bodylen = COLUMN - rd->firstCol;
        }
        else if (rc == 0) {
            rd->state = END;
            *bodylen = avail - rd->firstCol;
        }
        else {
            rd->state = ERR;
            *bodylen = avail - rd->firstCol;
        }
        return 1;
    }
    return 0;
}

int Dump(size_t addr, DataProvider dp, void * userdata, FILE * out,
    const char * header, const char * footer)
{
    RowData rd;
    size_t firstCol = addr % COLUMN;
    size_t vaddr = addr - firstCol;
    const char * columns;
    size_t prelen;
    size_t bodylen;

    RD_Init(&rd, dp, userdata, firstCol);

    if (header)
        fputs(header, out);
    else
        fprintf(out, "==========================Begin dumping at %ph=========================\n", (void *)addr);

    fputs("Address  :  0  1  2  3  4  5  6  7  8  9  A  B  C  D  E  F ;\n", out);

    while (RD_Get(&rd, &columns, &prelen, &bodylen)) {
        assert(prelen + bodylen <= COLUMN);
        fprintf(out, "%ph: ", (void *)vaddr);
        if (bodylen == COLUMN) {
            int i;
            for (i = 0; i < COLUMN; ++i) {
                fprintf(out, "%02x ", (unsigned char)columns[i]);
            }
            fputs("; ", out);
            for (i = 0; i < COLUMN; ++i) {
                printChar(columns[i], out);
            }
        }
        else {
            size_t i;
            size_t postlen = COLUMN - prelen - bodylen;

            if (prelen) {
                for (i = 0; i < prelen; ++i) {
                    fputs("   ", out);
                }
            }
            for (i = 0; i < bodylen; ++i) {
                fprintf(out, "%02x ", (unsigned char)columns[prelen + i]);
            }
            if (postlen) {
                for (i = 0; i < postlen; ++i) {
                    fputs("   ", out);
                }
            }
            fputs("; ", out);
            if (prelen) {
                for (i = 0; i < prelen; ++i) {
                    fputc(' ', out);
                }
            }
            for (i = 0; i < bodylen; ++i) {
                printChar(columns[prelen + i], out);
            }
            if (postlen) {
                for (i = 0; i < postlen; ++i) {
                    fputc(' ', out);
                }
            }
        }
        fputc('\n', out);
        vaddr += COLUMN;
    }

    if (footer)
        fputs(footer, out);
    else
        fputs("============================= End dumping memory ============================\n", out);

    return rd.state == END ? 0 : -1;
}
