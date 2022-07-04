/* wavread.c */

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tvc.h"

#define ACT_WAVREAD   1
#define ACT_SEQREAD   2
#define ACT_PULSEREAD 3
#define ACT_BITREAD   4
#define ACT_BYTEREAD  5
static struct {
    const char *progname;
    int action;
    int debug;
} opt = {
    "wavread",
    0,
    0
};

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

/* SIGN | usec | bytes (depending on Hertz) | factor to lead */
/*      |      | 38400  44100  48000        | */
/* -----+------+ ---------------------------+--------------- */
/* lead |  470 | 18.05  20.73  22.56        | 1.00           */
/* sync |  736 | 28.26  32.46  35.33        | 1.5660         */
/* bit0 |  552 | 21.20  24.34  26.50        | 1.1745         */
/* bit1 |  388 | 14.90  17.11  18.62        | 0.8255         */

/* note: 'min' and 'max' might be defined as macro */
typedef struct lenrange {
    int minv;
    int maxv;
} lenrange;

#define IsInInterval(val,plenrange) \
    (((long)(val)) >= (long)(plenrange)->minv && \
     ((long)(val)) <= (long)(plenrange)->maxv)

/* sequence-read */
/* a sequence consist of consecutive positive/negative/zero bytes (here 0x80 is zero) */
typedef struct Seq {
    long pos;    /* file-offset */
    int  sign;   /* -1/0/1 */
    long len;
} Seq;

/* an impulse: a positive and a negative part together */
/* (positive-then-negative or negative-then-positive) */
/* reading impulses starts after a lot of zeroes; */
/* reading zeroes _after_ valid impulses will return EOF, */
/* but you can call PulseReadReset to go back to the initial state */
#define MINZEROES 1000 /* minimum number of 0x80 bytes (silence) before the leader */
typedef struct Pulse {
    long pos;    /* file-offset */
    long len;    /* length: total */
    long len1, len2;
} Pulse;

/* a Bit: reading bits is possible after having found a synchron-block */
/* there is BitReadReset that resets state */
typedef struct Bit {
    long pos;    /* file-offset */
    long len;    /* length */
    int  val;    /* 0/1 */
} Bit;

typedef struct Byte {
    long pos;    /* file-offset */
    long len;    /* length */
    int  val;    /* 0..255 */
} Byte;

/* hierachy of read-operations:
   WavOpen   calls WavRead
   WavOpen   sets GB.seq.sta   to STA_INIT
   WavOpen   sets GB.pulse.sta to STA_INIT
   WavOpen   sets GB.bit.sta   to STA_INIT
   WavOpen   sets GB.byte.sta  to STA_INIT
   SeqRead   calls WavRead
   PulseRead calls SeqRead
   BitRead   calls PulseRead
   ByteRead  calls BitRead
 */
#define STA_FILLED 0
#define STA_EOF    (-1)
#define STA_INIT   1

typedef struct State {
/* WAV-read */
    FILE *wfile;
    struct {        /* WAV file-header: we ignore it, assuming 8bit unsigned @ 44100 Hz */
        unsigned char bytes[64];
        unsigned len;
    } fh;
    struct {
        int  sta;             /* 0/-1/1 = next fields are filled / EOF / before the first read */
        unsigned char cache;  /* eloreolvasott byte */
        long pos;             /* az elobbi pozicioja a file-ban */
    } wav;
    struct {
        int sta;     /* 0/-1/1 = next field is filled / EOF / before the first read */
        Seq s;
    } seq;
    struct {
        int sta;     /* 0/-1/1 = next field is filled / EOF / before the first read */
        Pulse p;
    } pulse;
    struct {
        int sta;     /* 0/-1/1 = next field is filled / EOF / before the first read */
        Bit b;
    } bit;
    struct {
        int sta;     /* 0/-1/1 = next field is filled / EOF / before the first read */
        Byte b;
    } byte;
    int state;
/* the following values are calculated from the measured 'lead'-length */
    lenrange lead;
    lenrange sync;
    lenrange bit0;
    lenrange bit1;
} State;

static State GB;

static FILE *efopen (const char *name, const char *mode);
static void *emalloc (int n);

static void ParseArgs (int *pargc, char ***pargv);

static void WavOpen (const char *name);
static void WavClose(void);
static int  WavRead (void);
static size_t WavReadBytes (void *to, size_t len);

static int SeqRead (void);
static int PulseRead (void);
static int PulseReadReset (void);
static int BitRead (void);
static int BitReadReset (void);
static int ByteRead (void);
static int ByteReadReset (void);

/* 'Zero' is 0x80 in this context; 0x00..0x7f is negative, 0x81..0xff is positive */
static int WaitZero ();

static long GetHalfImp (FILE *f, HalfImpInfo *inf);
static long GetImp (ImpInfo *ii);
static int  GetByte (ByteInfo *inf);

static int GetBytes (void *to, int size);

static void Dump (long pos, int n, const void *p);

static long ninwav= 0;

static int BlockCheck (const TBLOCKHDR *tbh, int type, long pos);

static void StartCas (size_t namelen, const char *name);
static void WriteCas (size_t len, const void *data);
static void CloseCas (void);
static void AbortCas (void);

static void DumpWavBytes (void);
static void DumpSequences (void);
static void DumpPulses (void);
static void DumpBits (void);
static void DumpBytes (void);

int main (int argc, char **argv)
{
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
    WavOpen(argv[1]);

    if (opt.action==ACT_WAVREAD) {
        DumpWavBytes();
        goto VEGE;

    } else if (opt.action==ACT_SEQREAD) {
        DumpSequences();
        goto VEGE;

    } else if (opt.action==ACT_PULSEREAD) {
        DumpPulses();
        goto VEGE;

    } else if (opt.action==ACT_BITREAD) {
        DumpBits();
        goto VEGE;

    } else if (opt.action==ACT_BYTEREAD) {
        DumpBytes();
        goto VEGE;

    } else if (opt.action==1) {
        ImpInfo ii;
        n= 0;
        while (GetImp (&ii)!=EOF) {
            printf ("%05ld %06lx-%06lx %ld+%ld=%ld\n",
                    ++n, ii.begin, ii.begin + ii.cnt - 1,
                    ii.h, ii.l, ii.cnt);
        }
        exit (12);

    } else if (opt.action==2) {
        HalfImpInfo ii;
        n= 0;
        while (GetHalfImp (NULL, &ii)!=EOF) {
            printf ("%05ld %06lx-%06lx %c %ld+%2ld = %2ld\n",
                    ++n, ii.begin, ii.begin + ii.totcnt - 1,
                    ii.dir>0 ? '+' : '-', 
                    ii.igncnt, ii.cnt, ii.totcnt);
        }
        goto VEGE;

    } else if (opt.action==3) {
        n= 0;
        for (leave= 0; ! leave; ) {
            while ((b= GetByte (NULL))>=0) {
                printf ("%05ld   %02x\n", ++n, b);
            }
            printf ("-----\n");

            leave= WaitZero ()==EOF;
        }
        goto VEGE;
    }

    while (1) {
HEADWAIT:
        rc= WaitZero ();
        if (rc==EOF) break;

        pos = ninwav;
        GetBytes (&tbh, sizeof (tbh));
        if (BlockCheck (&tbh, TBLOCKHDR_BLOCK_HEAD, pos)) continue;
HEADFOUND:
        GetBytes (&tsh, sizeof (tsh));
        if ((ss= tsh.size)==0) ss= 256;
        GetBytes (sect, ss);
        printf ("name is \"%.*s\"\n", sect[0], sect+1);
        StartCas (sect[0], sect+1);
        WriteCas (ss-1-sect[0], sect+1+sect[0]);

        GetBytes (&tse, sizeof (tse));
        printf ("%lx -----HEAD----END----\n", ninwav);

        WaitZero ();

        pos = ninwav;
        GetBytes (&tbh, sizeof (tbh));
        if (BlockCheck (&tbh, TBLOCKHDR_BLOCK_DATA, pos)) {
            AbortCas ();
            if (BlockCheck (&tbh, TBLOCKHDR_BLOCK_HEAD, pos) == 0)
                goto HEADFOUND;
            else
                continue;
        }
        for (i=0; i<tbh.nsect; ++i) {
            pos = ninwav;
                GetBytes (&tsh, sizeof (tsh));
            if (tsh.sectno != i+1) {
                printf ("Bad sector number %d (waited=%d), aborting\n",
                        tsh.sectno, i+1);
                AbortCas ();
                goto HEADWAIT;
            }
            printf ("%lx -----SECTOR-%d-BEGIN---\n", pos, i+1);
            if ((ss= tsh.size)==0) ss= 256;
                GetBytes (sect, ss);
            WriteCas (ss, sect);
            pos = ninwav;
                GetBytes (&tse, sizeof (tse));
            printf ("%lx -----SECTOR-%d-END---\n", pos, i+1);
        }
        printf ("%lx -----DATA-END---\n", ninwav);
        CloseCas ();
    }
VEGE:
    WavClose ();
    return 0;
}

static int BlockCheck (const TBLOCKHDR *tbh, int type, long pos)
{
    if (tbh->magic1 != TBLOCKHDR_MAGIC1 ||
        tbh->magic2 != TBLOCKHDR_MAGIC2) {
        fprintf (stderr,
            "%lx Wrong block found"
            " (magic1=%02x[expected=%02x] magic2=%02x[expected=%02x]), ignoring\n",
            pos, tbh->magic1, TBLOCKHDR_MAGIC1, tbh->magic2, TBLOCKHDR_MAGIC2);
        return -1;

    } else if (tbh->blocktype != type) {
        printf ("%lx Wrong block type %02x, ignoring\n", pos, 
                tbh->blocktype);
        return -1;
    }
    return 0;
}

static int GetBytes (void *to, int size)
{
    int i;
    unsigned char *p = to;
    int c;
    long pos;

    pos = ninwav;

    for (i=0; i<size; ++i) {
        c= GetByte (NULL);
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

static void CalcIntervals (double avgheadlen);

static int GetByte (ByteInfo *bi)
{
    long pos;
    long cnt;
    int bits, byte;
    int ok;
    ImpInfo ii;

    if (GB.state==0) {
        long sum= 0;

/* szinkront keresunk */
        for (ok= 0, cnt= 0; ok<100; ) {
            if (cnt>=17 && cnt<=25) {
                ok++;
                sum += cnt;
            } else {
                ok= 0;
                sum= 0;
            }
            cnt= GetImp (&ii);
            if (cnt==EOF) return EOF;
        }
        CalcIntervals ((double)sum/ok);

        while (cnt>=GB.lead.minv && cnt<=GB.lead.maxv) {
            cnt= GetImp (&ii);
            if (cnt==EOF) return EOF;
        }
        GB.state= 1;
        goto S1;

    } else if (GB.state==1) {
        cnt = GetImp (&ii);
S1:     if (cnt<GB.sync.minv || cnt>GB.sync.maxv) {
            fprintf (stderr, "Missed sync (expected %d-%d, found=%ld\n",
                     GB.sync.minv, GB.sync.maxv, cnt);
            exit (12);
        }
        fprintf (stderr, "%lx Sync found len=%ld\n", ii.begin, ii.cnt);
        GB.state= 2;
        goto S2;

    } else if (GB.state==2) {
S2:     pos = ninwav;
        for (bits= 0, byte= 0; bits<8; ++bits) {
            byte >>= 1;
            cnt = GetImp (&ii);
            if      (cnt>=GB.bit0.minv && cnt<=GB.bit0.maxv) byte |= 0;
            else if (cnt>=GB.bit1.minv && cnt<=GB.bit1.maxv) byte |= 0x80;
            else {             /* nem jo adatbit */
                if (bits==0) { /* szerencsere byte-hataron jott */
                    GB.state= 0;
                    return -2;
                } else {
                    fprintf (stderr, "cnt=%ld unknown (at %lx)\n", cnt, ii.begin);
                    exit (44);
                }
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

static int WaitZero ()
{
    int c;

    do {
        c= WavRead ();
        if (c==EOF) return EOF;
    } while (c!=0x80);

    GB.state = 0;

    return 0;
}

#define Poz() \
    while (c>0x80) { \
        ++hcnt; \
        ++cnt; \
        c= WavRead (); \
        if (c==EOF) return EOF; \
    }

#define Neg() \
    while (c<=0x80) { \
        ++lcnt; \
        ++cnt; \
        c= WavRead (); \
        if (c==EOF) return EOF; \
    }

static long GetImp (ImpInfo *inf)
{
    int c;
    long cnt, hcnt, lcnt;
    long pos;

    pos = ninwav;

/* Megvárjuk egy impulzus elejét
    do {
        c= WavRead ();
        if (c==EOF) return EOF;
    } while (c<0x80);
*/

    cnt= 0;
    hcnt= 0;
    lcnt= 0;

    c= WavRead ();
    if (c==EOF) return EOF;

    Neg ();
    Poz ();

/*  WavUnread (c, f); */

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

    (void)f;
    pos = ninwav;
    igncnt = 0;
    cnt = 0;

/* Megvárjuk egy impulzus elejét */
    c= WavRead ();
    if (c==EOF) return EOF;

    while (c==0x80) {
        ++igncnt;
        c= WavRead ();
        if (c==EOF) return EOF;
    }

    if (c>0x80) {
        dir = 1; /* pozitív */
        while (c>=0x7e) {
            ++cnt;
            c= WavRead ();
            if (c==EOF) return EOF;
        }
    } else {
        dir = -1; /* negatív */
        while (c<=0x82) {
            ++cnt;
            c= WavRead ();
            if (c==EOF) return EOF;
        }
    }
/* WavUnread (c, f); */

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

static void ParseArgs (int *pargc, char ***pargv)
{
    int argc;
    char **argv;
    int parse_arg;

    argc = *pargc;
    argv = *pargv;
    parse_arg = 1;
    opt.progname = argv[0];

    while (--argc && **++argv=='-' && parse_arg) {
        switch (argv[0][1]) {
        case 'b': case 'B':
            if (strcasecmp (argv[0], "-bitread")==0) {
                opt.action= ACT_BITREAD;
                break;
            } else if (strcasecmp (argv[0], "-byteread")==0) {
                opt.action= ACT_BYTEREAD;
                break;
            } goto UNKOPT;

        case 'd': case 'D':
            ++opt.debug;
            break;
        case 'h': case 'H':
            opt.action = 2;
            break;
        case 'i': case 'I':
            opt.action = 1;
            break;

        case 'p': case 'P':
            if (strcasecmp (argv[0], "-pulseread")==0) {
                opt.action= ACT_PULSEREAD;
                break;
            } goto UNKOPT;

        case 's': case 'S':
            if (strcasecmp (argv[0], "-seqread")==0) {
                opt.action= ACT_SEQREAD;
                break;
            } goto UNKOPT;

        case 'w': case 'W':
            if (strcasecmp (argv[0], "-wavread")==0) {
                opt.action= ACT_WAVREAD;
                break;
            } goto UNKOPT;

        case 0: case '-': parse_arg = 0; break;
        default: UNKOPT:
            fprintf (stderr, "Unknown option '%s'\n", *argv);
            exit (4);
        }
    }
    ++argc;
    --argv;
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

static void ceil_floor (double base, double fact1, double fact2, int *h1, int *l2)
{
    if (base <= 0 || fact1 >= fact2) {
        fprintf (stderr, "*** ceil_floor: invalid parameters: %g %g %g\n",
            base, fact1, fact2);
        exit(12);
    }

    double v1= base*fact1;
    double fv1= floor(v1);
    double cv1= ceil(v1);
    double fault1c= cv1 - v1;

    double v2= base*fact2;
    double fv2= floor(v2);
    double cv2= ceil(v2);
    double fault2f= v1 - fv2;

    if      (cv1 < fv2) *h1= cv1, *l2= fv2;
    else if (cv1==cv2)  *h1= fv1, *l2= cv2;
    else if (fault1c > fault2f + 0.5) *h1= fv1, *l2= fv2;
    else if (fault2f > fault1c + 0.5) *h1= cv1, *l2= cv2;
    else *h1= fv1, *l2= cv2;
}

static const double F_bit1_l = 388.0/470.0 * 0.95;
static const double F_bit1_h = 388.0/470.0 * 1.05;
static const double F_bit0_l = 552.0/470.0 * 0.95;
static const double F_bit0_h = 552.0/470.0 * 1.05;
static const double F_lead_l =               0.95;
static const double F_lead_h =               1.05;
static const double F_sync_l = 736.0/470.0 * 0.95;
static const double F_sync_h = 736.0/470.0 * 1.35; /* was: 1.05 */

static void CalcIntervals (double i)
{
    GB.bit1.minv= floor (i * F_bit1_l);
    ceil_floor (i, F_bit1_h, F_lead_l, &GB.bit1.maxv, &GB.lead.minv);
    ceil_floor (i, F_lead_h, F_bit0_l, &GB.lead.maxv, &GB.bit0.minv);
    ceil_floor (i, F_bit0_h, F_sync_l, &GB.bit0.maxv, &GB.sync.minv);
    GB.sync.maxv= ceil (i* F_sync_h);

    if (opt.debug>=1) {
        fprintf (stderr, "intervals: bit1: %d-%d, lead: %d-%d, bit0: %d-%d, sync: %d-%d\n",
            GB.bit1.minv, GB.bit1.maxv,
            GB.lead.minv, GB.lead.maxv,
            GB.bit0.minv, GB.bit0.maxv,
            GB.sync.minv, GB.sync.maxv);
    }
}

static void WavOpen (const char *name)
{
    GB.wfile= efopen (name, "rb");
    GB.wav.sta= STA_INIT;
    GB.wav.pos= 0;
    WavRead();
    GB.seq.sta= STA_INIT;
    GB.pulse.sta= STA_INIT;
    GB.bit.sta= STA_INIT;
    GB.byte.sta= STA_INIT;

    GB.fh.len= WavReadBytes (&GB.fh.bytes, sizeof GB.fh.bytes);
    if (opt.debug>=1) {
        fprintf (stderr, "Wav-header skipped (%u bytes)\n",
            (unsigned)GB.fh.len);
    }
}

static void WavClose (void)
{
    fclose (GB.wfile);
    GB.wfile= NULL;
}

static int WavRead (void) {
    if (GB.wav.sta==STA_EOF) return EOF;
    if (GB.wav.sta!=STA_INIT) ++GB.wav.pos;
    int c= fgetc (GB.wfile);
    if (c==EOF) {
        GB.wav.sta= STA_EOF;
    } else {
        GB.wav.sta= STA_FILLED;
        GB.wav.cache= (unsigned char)c;
    }
    return c;
}

static size_t WavReadBytes (void *to, size_t len) {
    if (len==0) return 0;
    if (GB.wav.sta==STA_EOF) return 0;
    unsigned char *p0= to;
    unsigned char *plim= p0+len;
    unsigned char *p= p0;
    while (p<plim && GB.wav.sta != EOF) {
        *p++ = GB.wav.cache;
        WavRead();
    }
    return p - p0;
}

#define SignOfByte(b) ((b)<0x80 ? (-1) : \
                       (b)>0x80 ?   1  : 0)

static int SeqRead (void)
{
    if (GB.seq.sta == STA_EOF) return EOF;
    if (GB.wav.sta == STA_EOF) {
        GB.seq.sta= STA_EOF;
        return EOF;
    }

    GB.seq.sta= STA_FILLED;
    GB.seq.s.pos= GB.wav.pos;
    GB.seq.s.len= 1;
    GB.seq.s.sign= SignOfByte (GB.wav.cache);
    WavRead();

    while (GB.wav.sta == STA_FILLED &&
           SignOfByte(GB.wav.cache)==GB.seq.s.sign) {
        ++GB.seq.s.len;
        WavRead();
    }
    return 0;
}

static int PulseRead (void)
{
    Seq sFirst, sNext;

    if (GB.pulse.sta == STA_EOF) return EOF;
    if (GB.seq.sta==STA_INIT) SeqRead();
    if (GB.seq.sta==STA_EOF) {
        GB.pulse.sta= STA_EOF;
        return EOF;
    }
    if (GB.pulse.sta == STA_INIT) {
        Seq sZero;
        int foundzeroes= 0;

        while (GB.seq.sta==STA_FILLED && !foundzeroes) {
            foundzeroes= GB.seq.s.sign==0 && GB.seq.s.len >= MINZEROES;
            if (foundzeroes) sZero= GB.seq.s;
            SeqRead();
        }
        if (!foundzeroes || GB.seq.sta == STA_EOF) {
            GB.pulse.sta= STA_EOF;
            return EOF;
        }
        sFirst= GB.seq.s;
        fprintf(stderr,
                "PulseRead: found zeroes at"
                " %06lx (len=%ld), data after it at %06lx\n",
                sZero.pos, sZero.len, sFirst.pos);
        sFirst= GB.seq.s;
    } else {
        sFirst= GB.seq.s;
        if (sFirst.sign==0) {
            fprintf(stderr,
                "PulseRead: after valid impulse found zeroes at"
                " %06lx (len=%ld); reset state\n",
                sFirst.pos, sFirst.len);
            GB.pulse.sta= STA_EOF;
            return EOF;
        }
    }
    SeqRead();
    if (GB.seq.sta==STA_EOF) {
        fprintf(stderr,
                "PulseRead: EOF after a half-pulse\n");
        GB.pulse.sta= STA_EOF;
        return EOF;
    }
    sNext= GB.seq.s;
    if (sNext.sign==0 || sFirst.sign*sNext.sign != -1) {
        fprintf(stderr,
                "The halves of the pulse doesn't match"
                " p=%06lx/l=%ld/s=%d vs p=%06lx/l=%ld/s=%d\n",
                (long)sFirst.pos, (long)sFirst.len, (int)sFirst.sign,
                (long)sNext.pos,  (long)sNext.len,  (int)sNext.sign);
        GB.pulse.sta= STA_EOF;
        return EOF;
    }
    SeqRead();
    GB.pulse.p.pos= sFirst.pos;
    GB.pulse.p.len= sFirst.len + sNext.len;
    GB.pulse.p.len1= sFirst.len;
    GB.pulse.p.len2= sNext.len;
    GB.pulse.sta= STA_FILLED;
    return 0;
}

static int PulseReadReset (void)
{
    GB.pulse.sta= STA_INIT;
    return PulseRead();
}

static int BitRead_FindSync()
{
    size_t sumlen;
    int leadtries= 20, leadunit= 100;
    int leadfound= 0;
    int i, j;
    double headavglen;

    for (j= 0; j<leadtries && !leadfound; ++j) {
        sumlen= 0;
        for (i=0; i<leadunit && GB.pulse.sta==STA_FILLED; ++i) {
    /*      printf ("pulse at %06lx len=%ld\n", GB.pulse.p.pos, GB.pulse.p.len); */
            sumlen += GB.pulse.p.len;
            PulseRead();
        }
        if (GB.pulse.sta==STA_EOF) {
            GB.bit.sta= STA_EOF;
            return STA_EOF;
        }
        headavglen= sumlen/leadunit;
        lenrange range;
        range.minv= floor(headavglen*0.95);
        range.maxv= ceil(headavglen*1.05);
        fprintf (stderr, "BitRead_FindSync: avg=%g range=[%d,%d]\n", headavglen, range.minv, range.maxv);

        int rngerr= 0;
        for (i=0; i<leadunit && !rngerr && GB.pulse.sta==STA_FILLED; ++i) {
    /*      printf ("pulse at %06lx len=%ld\n", GB.pulse.p.pos, GB.pulse.p.len); */
            rngerr= ! IsInInterval (GB.pulse.p.len, &range);
            if (rngerr) continue;
            PulseRead();
        }
        if (GB.pulse.sta==STA_EOF) {
            GB.bit.sta= STA_EOF;
            return STA_EOF;
        }
        leadfound= !rngerr;
    }
    if (!leadfound) {
        fprintf (stderr, "BitRead_FindSync: couldn't find the leader\n");
        GB.bit.sta= STA_EOF;
        return STA_EOF;
    }
    CalcIntervals (headavglen);
    while (GB.pulse.sta != STA_EOF &&
           IsInInterval (GB.pulse.p.len, &GB.lead)) {
        PulseRead();
    }
    if (GB.pulse.sta != STA_EOF &&
           IsInInterval (GB.pulse.p.len, &GB.sync)) {
        fprintf (stderr, "BitRead_FindSync: found the sync at %06lx (len=%d)\n",
            (long)GB.pulse.p.pos, (int)GB.pulse.p.len);
        GB.bit.sta= STA_FILLED;
        PulseRead();
        return 0;
    } else {
        GB.bit.sta= STA_EOF;
        return STA_EOF;
    }
}

static int BitRead (void)
{
    if (GB.bit.sta == STA_EOF) return EOF;
    if (GB.pulse.sta==STA_INIT) PulseRead();
    if (GB.pulse.sta==STA_EOF) {
        GB.bit.sta= STA_EOF;
        return EOF;
    }
    if (GB.bit.sta==STA_INIT) {
        int rc= BitRead_FindSync();
        if (rc) return STA_EOF;
    }
    PulseRead();
    if (GB.pulse.sta==STA_EOF) {
        GB.bit.sta= STA_EOF;
        return EOF;
    }
    if (IsInInterval (GB.pulse.p.len, &GB.bit0)) {
        GB.bit.b.pos= GB.pulse.p.pos;
        GB.bit.b.len= GB.pulse.p.len;
        GB.bit.b.val= 0;
    } else if (IsInInterval (GB.pulse.p.len, &GB.bit1)) {
        GB.bit.b.pos= GB.pulse.p.pos;
        GB.bit.b.len= GB.pulse.p.len;
        GB.bit.b.val= 1;
    } else {
        fprintf(stderr,
                "BitRead: after valid bits found non-bit at"
                " %06lx (len=%ld); reset state\n",
                GB.pulse.p.pos, GB.pulse.p.len);
        GB.bit.sta= STA_EOF;
        return EOF;
    }

    return 0;
}

static int BitReadReset (void)
{
    GB.bit.sta= STA_INIT;
    PulseReadReset();
    return BitRead();
}

static int ByteRead (void)
{
    if (GB.byte.sta == STA_EOF) return EOF;
    if (GB.bit.sta==STA_INIT) BitRead();
    if (GB.bit.sta==STA_EOF) {
        GB.byte.sta= STA_EOF;
        return EOF;
    }
    if (GB.byte.sta==STA_INIT) {
        GB.byte.sta= STA_FILLED;
    }

    int nbit= 0;
    int byteval= 0;
    while (nbit<8 && GB.bit.sta==STA_FILLED) {
        if (nbit==0) GB.byte.b.pos= GB.bit.b.pos;
        GB.byte.b.len += GB.bit.b.len;
        byteval >>= 1;
        if (GB.bit.b.val==1) byteval |= 0x80;
        ++nbit;
        BitRead();
    }
    if (nbit==8) {
        GB.byte.b.val= byteval;
        return 0;
    } else {
        if (nbit != 0) {
            fprintf (stderr, "ByteRead: incomplete byte read pos=%06lx nbit=%d\n",
                (long)GB.byte.b.pos, nbit);
        }
        GB.byte.sta= STA_EOF;
        return EOF;
    }
}

static int ByteReadReset (void)
{
    GB.byte.sta= STA_INIT;
    BitReadReset();
    return ByteRead();
}

static void DWB_print (size_t psave, size_t nsave, int csave)
{
    if (nsave==1) {
        printf ("%06lx: %02x\n",
                (long)psave,
                (unsigned char)csave);
    } else {
        printf ("%06lx= %02x (*%ld)\n",
                (long)psave,
                (unsigned char)csave,
                (long)nsave);
    }
    fflush(stdout);
}

static void DumpWavBytes (void)
{
    unsigned long psave= 0;
    int csave= 0;
    int nsave= 0;

    while (GB.wav.sta == STA_FILLED) {
        if (nsave==0) {
            nsave= 1;
            csave= GB.wav.cache;
            psave= GB.wav.pos;

        } else if (csave==GB.wav.cache) {
            ++nsave;

        } else {
            DWB_print (psave, nsave, csave);
            nsave= 1;
            csave= GB.wav.cache;
            psave= GB.wav.pos;
        }
        WavRead();
    }
    if (nsave>0) {
        DWB_print (psave, nsave, csave);
    }
}

#define SignToChar(s) ((s)<0 ? '-': \
                       (s)>0 ? '+': '0')

static void DumpSequences (void)
{
    if (GB.seq.sta == STA_INIT) SeqRead();

    while (GB.seq.sta == STA_FILLED) {
        printf ("%06lx: %c *%ld\n",
            (long)GB.seq.s.pos,
            SignToChar (GB.seq.s.sign),
            (long)GB.seq.s.len);
        SeqRead();
    }
}

static void DP_print (size_t nsave, const Pulse *psave)
{
    if (nsave==1) {
        printf ("%06lx: %ld (%ld+%ld)\n",
            (long)psave->pos,
            (long)psave->len,
            (long)psave->len1,
            (long)psave->len2);
    } else {
        printf ("%06lx= %ld (%ld+%ld) (*%ld)\n",
            (long)psave->pos,
            (long)psave->len,
            (long)psave->len1,
            (long)psave->len2,
            (long)nsave);
    }
    fflush(stdout);
}

static void DumpPulses (void)
{
    if (GB.pulse.sta == STA_INIT) PulseRead();

    Pulse pcache;
    size_t ncache= 0;

ELEJE:
    while (GB.pulse.sta == STA_FILLED) {
        if (ncache!=0 &&
            (GB.pulse.p.len  != pcache.len  ||
             GB.pulse.p.len1 != pcache.len1 ||
             GB.pulse.p.len2 != pcache.len2)) {
            DP_print (ncache, &pcache);
            ncache= 0;
        }
        if (ncache==0) {
            ncache= 1;
            pcache= GB.pulse.p;
        } else {
            ++ncache;
        }
        PulseRead ();
    }
    if (ncache!=0) {
        DP_print (ncache, &pcache);
        ncache= 0;
    }
    if (GB.pulse.sta==STA_EOF && GB.seq.sta!=STA_EOF) {
        PulseReadReset();
        goto ELEJE;
    }
}

static void DumpBits (void)
{
ELEJE:
    if (GB.bit.sta == STA_INIT) BitRead();
    while (GB.bit.sta==STA_FILLED) {
        printf("%06lx: %d (len=%ld)\n",
            GB.bit.b.pos, GB.bit.b.val, GB.bit.b.len);
        fflush(stdout);
        BitRead ();
    }
    if (GB.bit.sta==STA_EOF && GB.pulse.sta!=STA_EOF) {
        BitReadReset();
        goto ELEJE;
    }
}

static void DumpBytes (void)
{
ELEJE:
    if (GB.byte.sta == STA_INIT) ByteRead();
    while (GB.byte.sta==STA_FILLED) {
        printf("%06lx: %02x (len=%ld)\n",
            GB.byte.b.pos, GB.byte.b.val, GB.byte.b.len);
        fflush(stdout);
        ByteRead ();
    }
    if (GB.byte.sta==STA_EOF && GB.pulse.sta!=STA_EOF) { /* tranzitiv fugges */
        ByteReadReset();
        goto ELEJE;
    }
}
