#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Host arithmetic checks; does not test kernel lifetime or RCU."""
import subprocess, tempfile
from pathlib import Path
from c_helpers import function
code = """
#include <stdbool.h>
#include <limits.h>
#include <stddef.h>
#include <errno.h>
#include <assert.h>
typedef unsigned long pgoff_t;
#define min(a,b) ((a)<(b)?(a):(b))
#define max(a,b) ((a)>(b)?(a):(b))
""" + function('mm/readahead_limit.c', 'ra_limit_range') + function('mm/readahead_limit.c', 'ra_limit_parse') + r"""
int main(void) {
 int value=77;
 assert(ra_limit_parse("1\n",2,1,&value)==0 && value==1);
 assert(ra_limit_parse("2",1,1,&value)==-EINVAL && value==1);
 assert(ra_limit_parse("",0,INT_MAX,&value)==-EINVAL);
 assert(ra_limit_parse("-1",2,INT_MAX,&value)==-EINVAL);
 assert(ra_limit_parse("+1",2,INT_MAX,&value)==-EINVAL);
 assert(ra_limit_parse("1 ",2,INT_MAX,&value)==-EINVAL);
 assert(ra_limit_parse("1\n0",3,INT_MAX,&value)==-EINVAL);
 assert(ra_limit_parse("2147483648",10,INT_MAX,&value)==-EINVAL);
 assert(ra_limit_parse("2147483647",10,INT_MAX,&value)==0 && value==INT_MAX);
 unsigned long s=0,n=128,a=32;
 assert(ra_limit_range(&s,&n,&a,0,1)==1);
 assert(s==0 && n==9 && a==8);
 s=0;n=128;a=32;
 assert(ra_limit_range(&s,&n,&a,64,1)==1);
 assert(s==64 && n==9 && a==8);
 s=0;n=128;a=32;
 assert(ra_limit_range(&s,&n,&a,124,1)==1);
 assert(s==119 && n==9 && a==3);
 s=20;n=20;a=20;
 assert(ra_limit_range(&s,&n,&a,0,100)==0);
 assert(s==20 && n==20 && a==20);
 s=128;n=128;a=128;
 assert(ra_limit_range(&s,&n,&a,64,1)==1);
 assert(s==128 && n==8 && a==8);
 s=0;n=64;a=16;
 assert(ra_limit_range(&s,&n,&a,128,1)==1);
 assert(s==56 && n==8 && a==0);
 s=0;n=0;a=0;
 assert(ra_limit_range(&s,&n,&a,0,1)==0);
 s=0;n=128;a=0;
 assert(ra_limit_range(&s,&n,&a,0,0)==0 && n==128);
 assert(ra_limit_range(&s,&n,&a,ULONG_MAX,1)==-1 && n==128);
 s=ULONG_MAX;n=1;a=0;
 assert(ra_limit_range(&s,&n,&a,0,1)==-1 && s==ULONG_MAX);
 s=0;n=1;a=2;
 assert(ra_limit_range(&s,&n,&a,0,1)==-1 && a==2);
 s=0;n=128;a=0;
 assert(ra_limit_range(&s,&n,&a,0,1)==1 && a==0);
 s=0;n=9;a=9;
 assert(ra_limit_range(&s,&n,&a,0,1)==0 && a==9);
 return 0;
}
"""
with tempfile.TemporaryDirectory() as d:
 p=Path(d); (p/'test.c').write_text(code)
 subprocess.run(['cc','-Wall','-Werror',str(p/'test.c'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
