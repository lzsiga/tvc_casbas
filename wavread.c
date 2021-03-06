/* wavread.c */

#include <errno.h>
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_Windows)
#define strcasecmp(s,t) strcmpi(s,t)
#endif

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
    int nocache;
} opt = {
    "wavread",
    0,
    0,
    0
};

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

/* position and length in the WAV-file */
typedef struct WavPos {
    long pos, len;
} WavPos;

/* sequence-read */
/* a sequence consist of consecutive positive/negative/zero bytes (here 0x80 is zero) */
typedef struct Seq {
    WavPos wp;
    int  sign;   /* -1/0/1 */
} Seq;

/* an impulse: a positive and a negative part together */
/* (positive-then-negative or negative-then-positive) */
/* reading impulses starts after a lot of zeroes; */
/* reading zeroes _after_ valid impulses will return EOF, */
/* but you can call PulseReadReset to go back to the initial state */
#define MINZEROES 1000 /* minimum number of 0x80 bytes (silence) before the leader */
typedef struct Pulse {
    WavPos wp;
    long len1, len2;
} Pulse;

/* a Bit: reading bits is possible after having found a synchron-block */
/* there is BitReadReset that resets state */
typedef struct Bit {
    WavPos wp;
    int  val;    /* 0/1 */
} Bit;

typedef struct Byte {
    WavPos wp;
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

/* 'wp' parameter: returns the position of the first bit in the block */
static int GetBytes (void *to, int size, WavPos *wp);

static void Dump (long pos, int n, const void *p);

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
    int ss, i;
    TBLOCKHDR tbh;
    TSECTHDR  tsh;
    TSECTEND  tse;
    char sect [280];

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
    }

    while (1) {
        WavPos wp;
HEADWAIT:
        ByteReadReset();
        if (GB.wav.sta==STA_EOF) break;

        GetBytes (&tbh, sizeof (tbh), &wp);
        if (BlockCheck (&tbh, TBLOCKHDR_BLOCK_HEAD, wp.pos)) continue;
HEADFOUND:
        GetBytes (&tsh, sizeof (tsh), NULL);
        if ((ss= tsh.size)==0) ss= 256;
        GetBytes (sect, ss, NULL);
        fprintf (stderr,"name is \"%.*s\"\n", sect[0], sect+1);
        StartCas (sect[0], sect+1);
        WriteCas (ss-1-sect[0], sect+1+sect[0]);

        GetBytes (&tse, sizeof (tse), &wp);
        fprintf (stderr,"%lx -----HEAD----END----\n", wp.pos);

        ByteReadReset();

        GetBytes (&tbh, sizeof (tbh), &wp);
        if (BlockCheck (&tbh, TBLOCKHDR_BLOCK_DATA, wp.pos)) {
            AbortCas ();
            if (BlockCheck (&tbh, TBLOCKHDR_BLOCK_HEAD, wp.pos) == 0)
                goto HEADFOUND;
            else
                continue;
        }
        for (i=0; i<tbh.nsect; ++i) {
            GetBytes (&tsh, sizeof (tsh), &wp);
            if (tsh.sectno != i+1) {
                fprintf (stderr,"Bad sector number %d (waited=%d), aborting\n",
                        tsh.sectno, i+1);
                AbortCas ();
                goto HEADWAIT;
            }
            fprintf (stderr,"%lx -----SECTOR-%d-BEGIN---\n", wp.pos, i+1);
            if ((ss= tsh.size)==0) ss= 256;
            GetBytes (sect, ss, &wp);
            WriteCas (ss, sect);
            GetBytes (&tse, sizeof (tse), &wp);
            fprintf (stderr,"%lx -----SECTOR-%d-END---\n", wp.pos, i+1);
        }
        fprintf (stderr,"%lx -----DATA-END---\n", wp.pos);
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
        fprintf (stderr,"%lx Wrong block type %02x, ignoring\n", pos, 
                tbh->blocktype);
        return -1;
    }
    return 0;
}

static int GetBytes (void *to, int size, WavPos *wp)
{
    int i;
    unsigned char *p = to;
    WavPos mywp;

    if (GB.byte.sta==STA_INIT) {
        ByteRead();
    }

    if (!wp) wp= &mywp;
    wp->pos= wp->len= 0;
/*  wp= GB.byte.b.wp; <FIXME> */
    for (i=0; i<size && GB.byte.sta==STA_FILLED; ++i) {
        if (i==0) *wp= GB.byte.b.wp;
        else      wp->len += GB.byte.b.wp.len;
        p[i]= (unsigned char)GB.byte.b.val;
        ByteRead();
    }
    if (opt.debug) {
        Dump (wp->pos, i, p);
    }
    if (i != size) {
        fprintf(stderr, "GetBytes: incomplete read %d vs %d\n",
            (int)i, (int)size);
        exit (12);
    }
    return i;
}

static void Dump (long pos, int n, const void *p)
{
    const unsigned char *ptr;
    int j;

    ptr= p;

    fprintf (stderr,"%06lx ", pos);
    for (j=0; j<n; ++j) {
        fprintf (stderr,"%02x ", ptr[j]);
    }
    fprintf (stderr,"\n");
}

static void CalcIntervals (double avgheadlen);

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

        case 'n': case 'N':
            if (strcasecmp (argv[0], "-nocache")==0) {
                opt.nocache= 1;
                break;
            } goto UNKOPT;

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
    unsigned i;

    if (CasState) {
        AbortCas ();
    }
    CasName = emalloc (namelen+4+1);
    sprintf (CasName, "%.*s.cas", (int)namelen, name);
    for (i=0; i<namelen; ++i) {
        if (!isalnum(CasName[i]) && !strchr("-_@", CasName[i]))
            CasName[i]= '_';
    }
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
    double v1, fv1, cv1, fault1c;
    double v2, fv2, cv2, fault2f;

    if (base <= 0 || fact1 >= fact2) {
        fprintf (stderr, "*** ceil_floor: invalid parameters: %g %g %g\n",
            base, fact1, fact2);
        exit(12);
    }

    v1= base*fact1;
    fv1= floor(v1);
    cv1= ceil(v1);
    fault1c= cv1 - v1;

    v2= base*fact2;
    fv2= floor(v2);
    cv2= ceil(v2);
    fault2f= v1 - fv2;

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
    int c;

    if (GB.wav.sta==STA_EOF) return EOF;
    if (GB.wav.sta!=STA_INIT) ++GB.wav.pos;

    c= fgetc (GB.wfile);
    if (c==EOF) {
        GB.wav.sta= STA_EOF;
    } else {
        GB.wav.sta= STA_FILLED;
        GB.wav.cache= (unsigned char)c;
    }
    return c;
}

static size_t WavReadBytes (void *to, size_t len) {
    unsigned char *p0= to;
    unsigned char *plim= p0+len;
    unsigned char *p= p0;

    if (len==0) return 0;
    if (GB.wav.sta==STA_EOF) return 0;

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
    GB.seq.s.wp.pos= GB.wav.pos;
    GB.seq.s.wp.len= 1;
    GB.seq.s.sign= SignOfByte (GB.wav.cache);
    WavRead();

    while (GB.wav.sta == STA_FILLED &&
           SignOfByte(GB.wav.cache)==GB.seq.s.sign) {
        ++GB.seq.s.wp.len;
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
            foundzeroes= GB.seq.s.sign==0 && GB.seq.s.wp.len >= MINZEROES;
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
                sZero.wp.pos, sZero.wp.len, sFirst.wp.pos);
        sFirst= GB.seq.s;
    } else {
        sFirst= GB.seq.s;
        if (sFirst.sign==0) {
            fprintf(stderr,
                "PulseRead: after valid impulse found zeroes at"
                " %06lx (len=%ld); reset state\n",
                sFirst.wp.pos, sFirst.wp.len);
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
                (long)sFirst.wp.pos, (long)sFirst.wp.len, (int)sFirst.sign,
                (long)sNext.wp.pos,  (long)sNext.wp.len,  (int)sNext.sign);
        GB.pulse.sta= STA_EOF;
        return EOF;
    }
    SeqRead();
    GB.pulse.p.wp.pos= sFirst.wp.pos;
    GB.pulse.p.wp.len= sFirst.wp.len + sNext.wp.len;
    GB.pulse.p.len1= sFirst.wp.len;
    GB.pulse.p.len2= sNext.wp.len;
    GB.pulse.sta= STA_FILLED;
    return 0;
}

static int PulseReadReset (void)
{
    GB.pulse.sta= STA_INIT;
    return PulseRead();
}

static int BitRead_FindSync(void)
{
    size_t sumlen;
    int leadtries= 20, leadunit= 100;
    int leadfound= 0;
    int i, j;
    double headavglen;

    for (j= 0; j<leadtries && !leadfound; ++j) {
        lenrange range;
        int rngerr;

        sumlen= 0;
        for (i=0; i<leadunit && GB.pulse.sta==STA_FILLED; ++i) {
    /*      fprintf (stderr,"pulse at %06lx len=%ld\n", GB.pulse.p.pos, GB.pulse.p.len); */
            sumlen += GB.pulse.p.wp.len;
            PulseRead();
        }
        if (GB.pulse.sta==STA_EOF) {
            GB.bit.sta= STA_EOF;
            return STA_EOF;
        }
        headavglen= sumlen/leadunit;

        range.minv= floor(headavglen*0.95);
        range.maxv= ceil(headavglen*1.05);
        fprintf (stderr, "BitRead_FindSync: avg=%g range=[%d,%d]\n", headavglen, range.minv, range.maxv);

        rngerr= 0;
        for (i=0; i<leadunit && !rngerr && GB.pulse.sta==STA_FILLED; ++i) {
    /*      fprintf (stderr,"pulse at %06lx len=%ld\n", GB.pulse.p.pos, GB.pulse.p.len); */
            rngerr= ! IsInInterval (GB.pulse.p.wp.len, &range);
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
           IsInInterval (GB.pulse.p.wp.len, &GB.lead)) {
        PulseRead();
    }
    if (GB.pulse.sta != STA_EOF &&
           IsInInterval (GB.pulse.p.wp.len, &GB.sync)) {
        fprintf (stderr, "BitRead_FindSync: found the sync at %06lx-%06lx (len=%d)\n",
            (long)GB.pulse.p.wp.pos,
            (long)(GB.pulse.p.wp.pos + GB.pulse.p.wp.len),
            (int)GB.pulse.p.wp.len);
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
    } else {
        PulseRead();
    }
    if (GB.pulse.sta==STA_EOF) {
        GB.bit.sta= STA_EOF;
        return EOF;
    }
    if (IsInInterval (GB.pulse.p.wp.len, &GB.bit0)) {
        GB.bit.b.wp= GB.pulse.p.wp;
        GB.bit.b.val= 0;
    } else if (IsInInterval (GB.pulse.p.wp.len, &GB.bit1)) {
        GB.bit.b.wp= GB.pulse.p.wp;
        GB.bit.b.val= 1;
    } else {
        fprintf(stderr,
                "BitRead: after valid bits found non-bit at"
                " %06lx (len=%ld); reset state\n",
                GB.pulse.p.wp.pos, GB.pulse.p.wp.len);
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
    int nbit= 0;
    int byteval= 0;

    if (GB.byte.sta == STA_EOF) return EOF;
    if (GB.bit.sta==STA_INIT) BitRead();
    if (GB.bit.sta==STA_EOF) {
        GB.byte.sta= STA_EOF;
        return EOF;
    }
    if (GB.byte.sta==STA_INIT) {
        GB.byte.sta= STA_FILLED;
    }

    while (nbit<8 && GB.bit.sta==STA_FILLED) {
        if (nbit==0) {
            GB.byte.b.wp= GB.bit.b.wp;
        } else {
            GB.byte.b.wp.len += GB.bit.b.wp.len;
        }
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
                (long)GB.byte.b.wp.pos, nbit);
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
        fprintf (stderr,"%06lx: %02x\n",
                (long)psave,
                (unsigned char)csave);
    } else {
        fprintf (stderr,"%06lx= %02x (*%ld)\n",
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
        fprintf (stderr,"%06lx: %c *%ld\n",
            (long)GB.seq.s.wp.pos,
            SignToChar (GB.seq.s.sign),
            (long)GB.seq.s.wp.len);
        SeqRead();
    }
}

static void DP_print (size_t nsave, const Pulse *psave)
{
    if (nsave==1) {
        fprintf (stderr,"%06lx: %ld (%ld+%ld)\n",
            (long)psave->wp.pos,
            (long)psave->wp.len,
            (long)psave->len1,
            (long)psave->len2);
    } else {
        fprintf (stderr,"%06lx= %ld (%ld+%ld) (*%ld)\n",
            (long)psave->wp.pos,
            (long)psave->wp.len,
            (long)psave->len1,
            (long)psave->len2,
            (long)nsave);
    }
    fflush(stdout);
}

static void DumpPulses (void)
{
    Pulse pcache;
    size_t ncache= 0;

    if (GB.pulse.sta == STA_INIT) PulseRead();

ELEJE:
    while (GB.pulse.sta == STA_FILLED) {
        if (opt.nocache) {
            DP_print (1, &GB.pulse.p);
        } else {
            if (ncache!=0 &&
                (GB.pulse.p.wp.len != pcache.wp.len  ||
                 GB.pulse.p.len1   != pcache.len1 ||
                 GB.pulse.p.len2   != pcache.len2)) {
                DP_print (ncache, &pcache);
                ncache= 0;
            }
            if (ncache==0) {
                ncache= 1;
                pcache= GB.pulse.p;
            } else {
                ++ncache;
            }
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
            GB.bit.b.wp.pos, GB.bit.b.val, GB.bit.b.wp.len);
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
            GB.byte.b.wp.pos, GB.byte.b.val, GB.byte.b.wp.len);
        fflush(stdout);
        ByteRead ();
    }
    if (GB.byte.sta==STA_EOF && GB.pulse.sta!=STA_EOF) { /* tranzitiv fugges */
        ByteReadReset();
        goto ELEJE;
    }
}
