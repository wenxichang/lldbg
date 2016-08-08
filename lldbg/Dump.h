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

#ifndef __DUMP_H__
#define __DUMP_H__

#include <stdio.h>

/*
** Return 0 when there's no more data to dump. Return 1 when there's pending data
** to dump. Return -1 when error, which will cause Dump to break.
*/
typedef int (* DataProvider)(void * userdata, const char ** buf, size_t * size);

/*
** Return 0 when sucess. Return -1 when DataProvider error.
*/
int Dump(size_t addr, DataProvider dp, void * userdata, FILE * out,
    const char * header, const char * footer);

#endif
