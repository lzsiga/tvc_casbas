/* wavread.c */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tvc.h"

typedef struct ImpInfo {
    long begin;
    long h;
    long l;
    long cnt;
} ImpInfo;

typedef struct HalfImpInfo {
    int dir;
    long begin;
    long igncnt;
    long cnt;
    long totcnt;
} HalfImpInfo;

typedef struct ByteInfo {
    long begin;
    long cnt;
} ByteInfo;

static void ParseArgs (int *pargc, char ***pargv);

static FILE *efopen (const char *name, const char *mode);
static void  efread (FILE *f, void *buff, size_t len);

static void *emalloc (int n);

static int WaitSync (FILE *f);

static long GetHalfImp (FILE *f, HalfImpInfo *inf);
static long GetImp (FILE *f, ImpInfo *ii);
static int  GetByte (FILE *f, ByteInfo *inf);

static int GetBytes (FILE *f, void *to, int size);

static void Dump (long pos, int n, const void *p);

#define WavRead(c,f)   if ((c=fgetc(f))!=EOF) ++ninwav
#define WavUnread(c,f) { ungetc (c, f); --ninwav; }

static long ninwav= 0;

static struct {
    int action;
    int debug;
} opt = {
    0,
    0
};

static int BlockCheck (const TBLOCKHDR *tbh, int type, long pos);

static void StartCas (size_t namelen, const char *name);
static void WriteCas (size_t len, const void *data);
static void CloseCas (void);
static void AbortCas (void);

int main (int argc, char **argv)
{
    FILE *f;
    char hdr[44];
    int b, ss, i;
    TBLOCKHDR tbh;
    TSECTHDR  tsh;
    TSECTEND  tse;
    char sect [280];
    long n;
    long pos;
    int leave, rc;

    ParseArgs (&argc, &argv);

    if (argc!=2) {
        fprintf (stderr, "usage: wavread <file>\n");
        exit (8);
    }
    f = efopen (argv[1], "rb");
    efread (f, hdr, sizeof (hdr));
    ninwav += sizeof (hdr);

    if (opt.action==1) {
        ImpInfo ii;
        n= 0;
        while (GetImp (f, &ii)!=EOF) {
            printf ("%05ld %06lx-%06lx %ld+%ld=%ld\n",
                    ++n, ii.begin, ii.begin + ii.cnt - 1,
                    ii.h, ii.l, ii.cnt);
        }
        exit (12);

    } else if (opt.action==2) {
        HalfImpInfo ii;
        n= 0;
        while (GetHalfImp (f, &ii)!=EOF) {
            printf ("%05ld %06lx-%06lx %c %ld+%2ld = %2ld\n",
                    ++n, ii.begin, ii.begin + ii.totcnt - 1,
                    ii.dir>0 ? '+' : '-', 
                    ii.igncnt, ii.cnt, ii.totcnt);
        }
        goto VEGE;

    } else if (opt.action==3) {
        n= 0;
        for (leave= 0; ! leave; ) {
            while ((b= GetByte (f, NULL))>=0) {
                printf ("%05ld   %02x\n", ++n, b);
                }
                printf ("-----\n");

                leave= WaitSync (f)==EOF;
        }
        goto VEGE;
    }

    while (1) {
HEADWAIT:
        rc= WaitSync (f);
        if (rc==EOF) break;

        pos = ninwav;
        GetBytes (f, &tbh, sizeof (tbh));
        if (BlockCheck (&tbh, TBLOCKHDR_BLOCK_HEAD, pos)) continue;
HEADFOUND:
        GetBytes (f, &tsh, sizeof (tsh));
        if ((ss= tsh.size)==0) ss= 256;
        GetBytes (f, sect, ss);
        printf ("name is \"%.*s\"\n", sect[0], sect+1);
        StartCas (sect[0], sect+1);
        WriteCas (ss-1-sect[0], sect+1+sect[0]);

        GetBytes (f, &tse, sizeof (tse));
        printf ("%lx -----HEAD----END----\n", ninwav);

        WaitSync (f);

        pos = ninwav;
        GetBytes (f, &tbh, sizeof (tbh));
        if (BlockCheck (&tbh, TBLOCKHDR_BLOCK_DATA, pos)) {
            AbortCas ();
            if (BlockCheck (&tbh, TBLOCKHDR_BLOCK_HEAD, pos) == 0)
                goto HEADFOUND;
            else
                continue;
        }
        for (i=0; i<tbh.nsect; ++i) {
            pos = ninwav;
                GetBytes (f, &tsh, sizeof (tsh));
            if (tsh.sectno != i+1) {
                printf ("Bad sector number %d (waited=%d), aborting\n",
                        tsh.sectno, i+1);
                AbortCas ();
                goto HEADWAIT;
            }
            printf ("%lx -----SECTOR-%d-BEGIN---\n", pos, i+1);
            if ((ss= tsh.size)==0) ss= 256;
                GetBytes (f, sect, ss);
            WriteCas (ss, sect);
            pos = ninwav;
                GetBytes (f, &tse, sizeof (tse));
            printf ("%lx -----SECTOR-%d-END---\n", pos, i+1);
        }
        printf ("%lx -----DATA-END---\n", ninwav);
        CloseCas ();
    }
VEGE:
    fclose (f);
    return 0;
}

static int BlockCheck (const TBLOCKHDR *tbh, int type, long pos)
{
    if (tbh->magic1 != TBLOCKHDR_MAGIC1 ||
        tbh->magic2 != TBLOCKHDR_MAGIC2) {
        printf ("%lx Wrong block found, ignoring\n", pos);
        return -1;

    } else if (tbh->blocktype != type) {
        printf ("%lx Wrong block type %02x, ignoring\n", pos, 
                tbh->blocktype);
        return -1;
    }
    return 0;
}

static int GetBytes (FILE *f, void *to, int size)
{
    int i;
    unsigned char *p = to;
    int c;
    long pos;

    pos = ninwav;

    for (i=0; i<size; ++i) {
        c= GetByte (f, NULL);
        if (c<0) break;
        p[i]= (unsigned char)c;
    }
    if (opt.debug) {
        Dump (pos, i, p);
    }
    return i;
}

static void Dump (long pos, int n, const void *p)
{
    const unsigned char *ptr;
    int j;

    ptr= p;

    printf ("%06lx ", pos);
    for (j=0; j<n; ++j) {
        printf ("%02x ", ptr[j]);
    }
    printf ("\n");
}

#define MINSYNC 29

static struct GBstate {
    int state;
    int ldlen; /* average length of leading pulses e.g. 18 */
#define GB_lead 0 /* looking for leading (multiples 470 usec pulse (20.727 byte at 44100Hz)) */
} GB= {
/* state */ 0,
/* ldlen */ 0
};

static int GetByte (FILE *f, ByteInfo *bi)
{
    long pos;
    long cnt;
    int bits, byte;
    int ok;
    ImpInfo ii;

    if (GB.state==0) {
        unsigned long sum;

/* szinkront keresunk: 100 db 17 es 25 kozotti impulzus */
        for (ok= 0, cnt= 0; ok<100; ) {
            if (cnt>=17 && cnt<=25) {
                ok++;
                sum += cnt;
            } else {
                ok= 0;
                sum= 0;
            }
            cnt= GetImp (f, &ii);
            if (cnt==EOF) return EOF;
        }
        GB.ldlen= (sum + ok/2)/ok;
        if (opt.debug >= 1) {
            fprintf (stderr, "Leading has been found, average len %d\n",
                     GB.ldlen);
        }

/****   while (cnt>=17 && cnt<=25) { ***/
        while (cnt<=26) {
            cnt= GetImp (f, &ii);
            if (cnt==EOF) return EOF;
        }
        GB.state= 1;
        goto S1;

    } else if (GB.state==1) {
        cnt = GetImp (f, &ii);
S1:        if (cnt<MINSYNC) {
            fprintf (stderr, "Sync missed (%ld<=%d)\n", cnt, MINSYNC);
            exit (12);
        }
        printf ("%lx Sync found len=%ld\n", ii.begin, ii.cnt);
        GB.state= 2;
        goto S2;

    } else if (GB.state==2) {
S2:        pos = ninwav;
        for (bits= 0, byte= 0; bits<8; ++bits) {
            byte >>= 1;
                cnt = GetImp (f, &ii);
            if (cnt<=20)      byte |= 128;
            else if (cnt>=21) byte |= 0;
            else if (bits==0) { /* Újra synchron jön */
                GB.state= 0;
                return -2;
            } else {
                fprintf (stderr, "cnt=%ld unknown (at %lx)\n", cnt, ii.begin);
                exit (44);
            }
        }
        if (bi) {
            bi->begin = pos;
            bi->cnt   = ninwav - pos;
        }
        return byte;
    }
    return EOF;
}

static int WaitSync (FILE *f)
{
    int c;

    do {
        WavRead (c, f);
        if (c==EOF) return EOF;
    } while (c!=0x80);

    GB.state = 0;

    return 0;
}

#define Poz() \
    while (c>0x80) { \
        ++hcnt; \
        ++cnt; \
        WavRead (c, f); \
        if (c==EOF) return EOF; \
    }

#define Neg() \
    while (c<=0x80) { \
        ++lcnt; \
        ++cnt; \
        WavRead (c, f); \
        if (c==EOF) return EOF; \
    }

static long GetImp (FILE *f, ImpInfo *inf)
{
    int c;
    long cnt, hcnt, lcnt;
    long pos;

    pos = ninwav;

/* Megvárjuk egy impulzus elejét
    do {
        WavRead (c, f);
        if (c==EOF) return EOF;
    } while (c<0x80);
*/

    cnt= 0;
    hcnt= 0;
    lcnt= 0;

    WavRead (c, f);
    if (c==EOF) return EOF;

    Neg ();
    Poz ();

    WavUnread (c, f);

    if (inf) {
        inf->begin = pos;
        inf->h = hcnt;
        inf->l = lcnt;
        inf->cnt = cnt;
    }

    return cnt;
}

static long GetHalfImp (FILE *f, HalfImpInfo *inf)
{
    int c, dir;
    long cnt, igncnt;
    long pos;

    pos = ninwav;
    igncnt = 0;
    cnt = 0;

/* Megvárjuk egy impulzus elejét */
    WavRead (c, f);
    if (c==EOF) return EOF;

    while (c==0x80) {
        ++igncnt;
        WavRead (c, f);
        if (c==EOF) return EOF;
    }

    if (c>0x80) {
        dir = 1; /* pozitív */
        while (c>=0x7e) {
            ++cnt;
            WavRead (c, f);
            if (c==EOF) return EOF;
        }
    } else {
        dir = -1; /* negatív */
        while (c<=0x82) {
            ++cnt;
            WavRead (c, f);
                if (c==EOF) return EOF;
        }
    }
    WavUnread (c, f);

    if (inf) {
        inf->begin = pos;
        inf->igncnt = igncnt;
        inf->cnt = cnt;
        inf->totcnt = igncnt + cnt;
        inf->dir = dir;
    }

    return cnt;
}

static FILE *efopen (const char *name, const char *mode)
{
    FILE *f;

    f = fopen (name, mode);
    if (f) return f;
    fprintf (stderr, "Error opening file '%s' mode '%s",
             name, mode);
    perror ("'");
    exit (32);
    return NULL;
}

static void efread (FILE *f, void *buff, size_t len)
{
    size_t rd;

    errno = 0;
    rd = fread (buff, 1, len, f);
    if (rd==len) return;
    if (errno) {
        perror ("Error reading file");
    } else {
        fprintf (stderr, "fread: not enough data (%u<%u)", (unsigned)rd, (unsigned)len);
    }
}

static void ParseArgs (int *pargc, char ***pargv)
{
    int argc;
    char **argv;
    int parse_arg;
    char *progname;
 
    argc = *pargc;
    argv = *pargv;
    parse_arg = 1;
    progname = argv[0];
    opt.action = 0;
    opt.debug = 0;
 
    while (--argc && **++argv=='-' && parse_arg) {
        switch (argv[0][1]) {
        case 'i': case 'I':
             opt.action = 1;
             break;
        case 'h': case 'H':
             opt.action = 2;
             break;
        case 'b': case 'B':
             opt.action = 3;
             break;
        case 'd': case 'D':
             ++opt.debug;
             break;
        case 0: case '-': parse_arg = 0; break;
        default:
            fprintf (stderr, "Unknown option '%s'\n", *argv);
            exit (4);
        }
    }
    ++argc;
    *--argv = progname;
    *pargc = argc;
    *pargv = argv;
}
static int CasState= 0;
static char *CasName= NULL;
static FILE *CasFile= NULL;

static void AbortCas (void)
{
    if (CasState) {
        fclose (CasFile);
        CasFile = NULL;
        remove (CasName);
        free (CasName);
        CasName = NULL;
        CasState= 0;
    }
}

static void StartCas (size_t namelen, const char *name)
{
    CPMHDR cpm;

    if (CasState) {
        AbortCas ();
    }
    CasName = emalloc (namelen+4+1);
    sprintf (CasName, "%.*s.cas", (int)namelen, name);
    CasFile = efopen (CasName, "wb");
    CasState= 1;

    memset (&cpm, 0, sizeof (cpm));
    cpm.magic = CPMHDR_MAGIC;
    fwrite (&cpm, 1, sizeof (cpm), CasFile);
}

static void WriteCas (size_t len, const void *data)
{
    if (CasState==0) exit(32);
    fwrite (data, 1, len, CasFile);
}

static void CloseCas (void)
{
    fclose (CasFile);
    CasFile = NULL;
    free (CasName);
    CasName = NULL;
    CasState= 0;
}

static void *emalloc (int n)
{
    void *p;

    p = malloc (n);
    if (p) return p;
    fprintf (stderr, "Out of memory (malloc (%d))\n", n);
    exit (33);
    return NULL;
}
