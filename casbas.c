/* casbas.c */

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tvc.h"

static struct {
    int debug;
    int overw;
} opt = {
    0, 0
};

static void ParseArgs (int *pargc, char ***pargv);

static FILE *efopen (const char *name, const char *mode);
static void  efread (FILE *f, void *buff, size_t len);
static void *emalloc (int n);

static void GetHeaderData (const CASHDR *ch, CASHDR_DATA *cd);
static void SetHeaderData (const CASHDR_DATA *cd, CASHDR *ch);

static void Cas2Bas (FILE *f, FILE *g);
static void Bas2Cas (FILE *f, FILE *g);

static const char *charmap[2][256];

#define TYPE_CAS 1
#define TYPE_BAS 2

static struct {
    const char *iname;
    const char *imode;
    const char *oname;
    const char *omode;
    int itype;
} var;

static void TipVizsg (void);

int main (int argc, char **argv)
{
    FILE *f, *g;

    ParseArgs (&argc, &argv);
    if (argc<2) {
        fprintf (stderr, "usage:\n"
                         "\tcasbas [options] <casfile> [<basfile>]\n"
                         "\tcasbas [options] <basfile> [<casfile>]\n"
                         "options:\n"
                         "\t-d debug\n"
                         "\t-o overwrite existing file\n");
        return 4;
    }

    memset (&var, 0, sizeof (var));

    var.iname =  argv[1];
    if (argc>2) {
        opt.overw = 1;
        var.oname = argv[2];
    }

    TipVizsg ();

    f = efopen (var.iname, var.imode);
    if (! opt.overw) {
        g = fopen (var.oname, "r");
        if (g!=NULL) {
            fclose (g);
            fprintf (stderr, "Output file '%s' already exists!\n",
                     var.oname);
            exit (35);
        }
    }
    g = efopen (var.oname, var.omode);

    if (var.itype == TYPE_CAS) Cas2Bas (f, g);
    else Bas2Cas (f, g);

    fclose (f);

    return 0;
}

static const char hdig [] = "0123456789ABCDEF";

static int HexDig (int x)
{
    const char *p;

    p= strchr (hdig, toupper(x));
    if (!p) exit (37);
    return p-hdig;
}

typedef struct BuffData {
    char *ptr;
    unsigned len;
} BuffData;

static const char uhun [] = "ÁÉÍÓÖÕÚÜÛ";
static const char lhun [] = "áéíóöõúüû";

static int TrLine (BuffData *b)
{
    unsigned i, j;
    int c;
    int rc;
    const char *p;

        for (i=0, j=0; i<b->len; ++i) {
            p = strchr (uhun, b->ptr[i]);
            if (p) {
                b->ptr[j++] = (char)(0x00 + p-uhun);
                continue;
            }
            p = strchr (lhun, b->ptr[i]);
            if (p) {
                b->ptr[j++] = (char)(0x10 + p-lhun);
                continue;
            }
            if (b->ptr[i]=='\\') {
                if (i+1>=b->len) return -1;
                if (b->ptr[i+1]=='\\') {
                    b->ptr[j++]='\\';
                    ++i;
                    continue;
                }
                if ((b->ptr[i+1]!='t' && b->ptr[i+1]!='x') || i+3>=b->len || 
                    !isxdigit(b->ptr[i+2]) || !isxdigit(b->ptr[i+3])) {
                        rc= -1;
                        goto RETURN;
                    }
                    c= HexDig (b->ptr[i+2])*16 + HexDig (b->ptr[i+3]);
                    if (b->ptr[i+1]=='t') {
                            if (c<0x20 || c>=0xe0) {
                            rc= -1;
                            goto RETURN;
                        }
                        if (c>=0x80 && c<0xa0) c -= 0x80;
                    }
                    b->ptr[j++]= (char)c;
                    i+=3;
                    continue;
                }
                b->ptr[j++]= b->ptr[i];
            }
    b->len = j;
    rc = 0;
RETURN:
    return rc;
}

static void Chomp (BuffData *b)
{
    if (b->len>0 && b->ptr[b->len-1]=='\n') b->ptr[--b->len]= '\0';
    if (b->len>0 && b->ptr[b->len-1]=='\r') b->ptr[--b->len]= '\0';
}

static void LTrim (BuffData *b)
{
    while (b->len>0 && b->ptr[0]==' ') {
        --b->len;
        ++b->ptr;
    }
}

static int GetWord (BuffData *b, BuffData *w)
{
    const char *p, *plim, *wd, *wdlim;

    p= b->ptr;
    plim = p + b->len;
    while (p<plim && isspace (*p)) ++p;
    wd = p;
    while (p<plim && ! isspace (*p)) ++p;
    wdlim = p;
    while (p<plim && isspace (*p)) ++p;

    w->ptr = (char *)wd;
    w->len = wdlim - wd;
    b->len -= (p - b->ptr);
    b->ptr = (char *)p;

    return w->len ? 0 : EOF;
}

static void StripLabel (BuffData *b, BuffData *label)
{
    BuffData btmp= *b;
    BuffData mylabel= {NULL, 0};

    GetWord (&btmp, &mylabel);
    if (mylabel.len==5 &&
        isxdigit((unsigned char)mylabel.ptr[0]) &&
        isxdigit((unsigned char)mylabel.ptr[1]) &&
        isxdigit((unsigned char)mylabel.ptr[2]) &&
        isxdigit((unsigned char)mylabel.ptr[3]) &&
        mylabel.ptr[4]==':') {
        *b= btmp;
        if (label) *label= mylabel;
    } else {
        if (label) label->len= 0;
    }
}

static int GetLineno (BuffData *b, unsigned *no)
{
    long num= 0;

    while (b->len>0 && isdigit (b->ptr[0])) {
        num *= 10;
        num += b->ptr[0] - '0';
        if (num>65535L) return -1;
        --b->len;
        ++b->ptr;
    }
    *no = (unsigned)num;
    return 0;
}

static int TokLine (BuffData *b)
{
    unsigned i, j, k, cmplen;
    int tok, c;
    int found, fndlen, diff;
    int state;

    state= 0; /* Kell tokenizálni */

    for (i=0, j=0; i<b->len;) {
        if (state==0) {
            for (tok= BASIC_TOKEN_END, found=-1;
                 found==-1 && tok>= BASIC_TOKEN_START; --tok) {
                cmplen = strlen (charmap [0][tok]);
                if (b->len - i >= cmplen) {
                    for (k=0, diff=0; !diff && k<cmplen; ++k) {
                        c = b->ptr[i+k];
                            if (c>=0x61 && c<=0x7a) c -= 0x20;          /* a-z -> A-Z */
                            else if (c>=0x90 && c<=0x9a) c -= 0x10;     /* á-û -> Á-Û */
                        diff = c != charmap[0][tok][k];
                    }
                    if (! diff) {
                        found= tok;
                        fndlen = cmplen;
                    }
                }
            }
            if (found != -1) {
                if (found==BASIC_TOKEN_REM ||
                    found==BASIC_TOKEN_COMMENT) {
                    state = 4; /* Megjegyzés */
                } else if (found==BASIC_TOKEN_DATA) {
                    state = 2; /* DATA  */
                }
                b->ptr[j++]= (char)found;
                i += fndlen;
            } else {
                if (b->ptr[i]=='"') state ^= 1; /* Macskaköröm */
                c = b->ptr[i++];
                    if (c>=0x61 && c<=0x7a) c -= 0x20;          /* a-z -> A-Z */
                    else if (c>=0x90 && c<=0x9a) c -= 0x10;     /* á-û -> Á-Û */
                b->ptr[j++]= (char)c;
            }
        } else {
            c = (int)(unsigned char)b->ptr[i++];
            if (c=='"') state ^= 1; /* Macskaköröm */
            else if (state==2) { /* DATA-sor, macskaköröm nélkül */
                if (b->ptr[i]==':') {
                    c= BASIC_TOKEN_COLON;
                    state= 0;                    /* DATA-sor vége, kell tokenizálni */
                } else if (b->ptr[i]=='!') {
                    c= BASIC_TOKEN_COMMENT;
                    state= 4;                    /* DATA-sor vége, komment kezdete */
                }
            }
            b->ptr[j++]= (char)c;
        }
    }
    b->ptr[j++] = (char)BASIC_LINEND;
    b->len= j;
    return 0;
}

#define MAXLINE 1024

static void Bas2Cas (FILE *f, FILE *g)
{
    char line [3+MAXLINE+1], *l;
    int ln, ll, basend, autorun;
    BuffData b, w;
    unsigned no;
    unsigned prgsize, totsize;
    BASLINE *bl;
    CASHDR ch;
    CASHDR_DATA cd;

    memset (&ch, 0, sizeof (ch));
    fwrite (&ch, 1, sizeof (ch), g);

    ln= 0;
    basend= 0;
    prgsize = 0;
    autorun= 0;
    l = line + 3;
    while (fgets (l, MAXLINE, f)) {
        ++ln;
        ll = strlen (l);
        if (ll==0 || l[ll-1]!='\n') {
            fprintf (stderr, "line #%d is too long or contains '\\0'\n", ln);
            exit (35);
        }
        b.len = ll;
        b.ptr = l;

        Chomp (&b);
        LTrim (&b);
        StripLabel (&b, NULL);
        if (b.len==0) continue;

        if (! isdigit (b.ptr[0])) { /* Nincs sorszám */
            GetWord (&b, &w);
            if (w.len == 7 &&
                (memcmp (w.ptr, "AUTORUN", 7)==0 ||
                 memcmp (w.ptr, "autorun", 7)==0)) {
                autorun= 1;
                continue;

            } else if (w.len != 5 ||
                (memcmp (w.ptr, "BYTES", 5)!=0 && 
                 memcmp (w.ptr, "bytes", 5)!=0)) goto SYNERR;
            GetWord (&b, &w);
            if (w.len>0 && w.ptr[0]=='\'') {
                --w.len;
                ++w.ptr;
                if (w.len>0 && w.ptr[w.len-1]=='\'') --w.len;
            }
            if (w.len==0) continue;
            if (TrLine (&w)) goto SYNERR;
            if (! basend) {
                basend= 1;
                line[0] = BASIC_PRGEND;
                fwrite (line, 1, 1, g);
                ++prgsize;
            }
            fwrite (w.ptr, 1, w.len, g);
            prgsize += w.len;
            continue;
        }
        if (basend) goto SYNERR;

        if (GetLineno (&b, &no)) goto SYNERR;
        LTrim (&b);

        if (TrLine (&b)) goto SYNERR;
        TokLine (&b);
        if (b.len > 252) {
            fprintf (stderr, "Tokenized line is too long"
                     " (line #%d (basic %u) len=%d)\n",
                     ln, no, b.len);
            exit (40);
        }
        bl = (BASLINE *)(b.ptr - sizeof (BASLINE));
        bl->len = (unsigned char)(sizeof (BASLINE) + b.len);
        bl->no[0] = (unsigned char)(no&0xff);
        bl->no[1] = (unsigned char)(no>>8);

        fwrite (bl, 1, bl->len, g);
        prgsize += bl->len;
        continue;

SYNERR: fprintf (stderr, "Syntax error in line #%d\n", ln);
        exit (38);
    }
    if (! basend) {
/*        basend= 1; */
        line[0] = BASIC_PRGEND;
        fwrite (line, 1, 1, g);
        ++prgsize;
    }
    totsize = prgsize + sizeof (CASHDR);

    memset (&cd, 0, sizeof (cd));
    cd.blocknum =  (unsigned short)(totsize/128);
    cd.lastblock = (unsigned short)(totsize%128);
    cd.prgsize = (unsigned short)prgsize;
    cd.type = PRGFILE_TYPE_PROG;
    cd.autorun = (unsigned char)(autorun ? 0xff : 0x00);
    SetHeaderData (&cd, &ch);

    fseek (g, 0, SEEK_SET);
    fwrite (&ch, 1, sizeof (ch), g);
}

static void Cas2Bas (FILE *f, FILE *g)
{
    CASHDR ch;
    CASHDR_DATA cd;
    const unsigned char *prg, *prglim;
    const BASLINE *line, *nextline;
    const unsigned char *p, *pend;
    unsigned no, ni;
    int state, c;

    efread (f, &ch, sizeof (ch));
    GetHeaderData (&ch, &cd);

    if (opt.debug) {
            fprintf (stderr, "blocks=%u*128 + %u=%u, prgsize=%u, type=%u, autorun=%u\n",
                 cd.blocknum, cd.lastblock,
                 cd.blocknum * 128 + cd.lastblock,
                     cd.prgsize,
                 cd.type, cd.autorun);
    }

    prg = emalloc (cd.prgsize);
    efread (f, (void *)prg, cd.prgsize);
    if (opt.debug) {
        fprintf (stderr, "program loaded\n");
    }

    if (cd.autorun) {
        fprintf (g, "AUTORUN\n");
    }

    line= (const BASLINE *)prg;
    prglim = prg + cd.prgsize;

    while ((unsigned char *)line < prglim &&
            line->len != BASIC_PRGEND) {

        if (line->len < sizeof (BASLINE)) {
            fprintf (stderr, "Broken BASIC program, exiting\n");
            exit (32);
        }

        nextline = (BASLINE *)((unsigned char *)line + line->len);
        no = line->no[0] + (line->no[1] << 8);
        fprintf (g, "%4u ", no);

        p = (unsigned char *)line + sizeof (*line);
        pend = (unsigned char *)nextline;
        if (p <= pend-1 && pend[-1]==BASIC_LINEND) --pend;

        for (state= 0; p<pend; ++p) {
            c = *p;
            fprintf (g, "%s", charmap [state!=0][c]);

            if (c=='"') state ^= 1;                     /* macskakörmök között nem kell tokenizálni */
            else if ((state&1)==0) {
                if (c==BASIC_TOKEN_DATA) state |= 2;        /* DATA-sorban nem kell tokenizálni */
                else if (c==BASIC_TOKEN_COLON) state &= ~2; /* itt a DATA-sor vége */
                else if (c==BASIC_TOKEN_COMMENT ||
                         c==BASIC_TOKEN_REM) state |= 4;  /* megjegyzésben nem kell tokenizálni */
            }
        }
        fprintf (g, "\n");

        line = nextline;
    }

    p= (unsigned char *)line;
    if (p<prglim && *p==BASIC_PRGEND) ++p;

    for (ni=0; p<prglim; ++p) {
        if (++ni==1) fprintf (g, "%04x: BYTES '", (unsigned)(p-prg + BASIC_PROGBASE));
        fprintf (g, "\\x%02x", *p);
        if (ni==10) {
             fprintf (g, "'\n");
             ni= 0;
        }
    }
    if (ni) {
        fprintf (g, "'\n");
    }
}

static void GetHeaderData (const CASHDR *ch, CASHDR_DATA *cd)
{
    if (ch->cph.magic != CPMHDR_MAGIC ||
        ch->pfh.magic != PRGFILE_MAGIC ||
        (ch->pfh.type != PRGFILE_TYPE_DATA && 
         ch->pfh.type != PRGFILE_TYPE_PROG)) {
        fprintf (stderr, "Bad CAS-header\n");
        exit (32);
    }
    cd->blocknum  = PEEK2 (ch->cph.blocknum);
    cd->lastblock = PEEK2 (ch->cph.lastblock);
    cd->prgsize   = PEEK2 (ch->pfh.prgsize);
    cd->type      = ch->pfh.type;
    cd->autorun   = ch->pfh.autorun;
    cd->version   = ch->pfh.version;
}

static void SetHeaderData (const CASHDR_DATA *cd, CASHDR *ch)
{
    memset (ch, 0, sizeof (*ch));
    ch->cph.magic   = CPMHDR_MAGIC;
    ch->pfh.magic   = PRGFILE_MAGIC;
    ch->pfh.type    = cd->type;
    ch->pfh.autorun = cd->autorun;
    ch->pfh.version = cd->version;

    POKE2 (ch->cph.blocknum,  cd->blocknum);
    POKE2 (ch->cph.lastblock, cd->lastblock);
    POKE2 (ch->pfh.prgsize,   cd->prgsize);
}

static void TipVizsg (void)
{
    int len;
    const char *oext;
    char *oname;

    len = strlen (var.iname);
    if (len<5 || var.iname[len-4]!='.') {
HIBA:        fprintf (stderr, "input filename '%s' should be *.cas or *.bas\n",
                 var.iname);
        exit (16);
    }
    if (strcmp (&var.iname[len-3], "CAS")==0) {
        oext = "BAS";
        var.omode = "w";
        var.imode = "rb";
        var.itype = TYPE_CAS;

    } else if (strcmp (&var.iname[len-3], "cas")==0) {
        oext = "bas";
        var.omode = "w";
        var.imode = "rb";
        var.itype = TYPE_CAS;

    } else if (strcmp (&var.iname[len-3], "BAS")==0) {
        oext = "CAS";
        var.omode = "wb";
        var.imode = "r";
        var.itype = TYPE_BAS;

    } else if (strcmp (&var.iname[len-3], "bas")==0) {
        oext = "cas";
        var.omode = "wb";
        var.imode = "r";
        var.itype = TYPE_BAS;
    } else goto HIBA;

    if (var.oname==NULL) {
        oname = emalloc (len+1);
        memcpy (oname, var.iname, len-3);
        memcpy (oname+len-3, oext, 3);
        oname [len] = '\0';
        var.oname = oname;
    }
    if (opt.debug) {
            fprintf (stderr, "%s/%s -> %s/%s\n",
                 var.iname, var.imode,
                 var.oname, var.omode);
    }
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

static const char *charmap[2][256] = {
{
  "Á", "É", "Í",  "Ó", "Ö", "Õ", "Ú", "Ü", "Û", "\\t89", "\\t8a", "\\t8b", "\\t8c", "\\t8d", "\\t8e", "\\t8f",
  "á", "é", "í",  "ó", "ö", "õ", "ú", "ü", "û", "\\t99", "\\t9a", "\\t9b", "\\t9c", "\\t9d", "\\t9e", "\\t9f",

  " ", "!", "\"", "#", "$", "%", "&", "'", "(", ")", "*", "+", ",",    "-", ".", "/",
  "0", "1", "2",  "3", "4", "5", "6", "7", "8", "9", ":", ";", "<",    "=", ">", "?",
  "@", "A", "B",  "C", "D", "E", "F", "G", "H", "I", "J", "K", "L",    "M", "N", "O",
  "P", "Q", "R",  "S", "T", "U", "V", "W", "X", "Y", "Z", "[", "\\\\", "]", "^", "_",
  "`", "a", "b",  "c", "d", "e", "f", "g", "h", "i", "j", "k", "l",    "m", "n", "o",
  "p", "q", "r",  "s", "t", "u", "v", "w", "x", "y", "z", "{", "|",    "}", "~", "\\t7f",

  "\\x80", "\\x81", "\\x82", "\\x83", "\\x84", "\\x85", "\\x86", "\\x87",
  "\\x88", "\\x89", "\\x8a", "\\x8b", "\\x8c", "\\x8d", "\\x8e", "\\x8f",

  "Cannot ",   "No ",       "Bad ",     "rgument",
  " missing",  ")",         "(",        "&",
  "+",         "<",         "=",        "<=",
  ">",         "<>",        ">=",       "^",
  ";",         "/",         "-",        "=<",
  ",",         "><",        "=>",       "#",
  "*",         "TOKEN#A9",  "TOKEN#AA", "POLIGON",
  "RECTANGLE", "ELLIPSE",   "BORDER",   "USING",
  "AT",        "ATN",       "XOR",      "VOLUME",
  "TO",        "THEN",      "TAB",      "STYLE",
  "STEP",      "RATE",      "PROMPT",   "PITCH",
  "PAPER",     "PALETTE",   "PAINT",    "OR",
  "ORD",       "OFF",       "NOT",      "MODE",
  "INK",       "INKEY$",    "DURATION", "DELAY",
  "CHARACTER", "AND",       "TOKEN#CA", "TOKEN#CB",
  "EXCEPTION", "RENUMBER",  "FKEY",     "AUTO",
  "LPRINT",    "EXT",       "VERIFY",   "TRACE",
  "STOP",      "SOUND",     "SET",      "SAVE",
  "RUN",       "RETURN",    "RESTORE",  "READ",
  "RANDOMIZE", "PRINT",     "POKE",     "PLOT",
  "OUT",       "OUTPUT",    "OPEN",     "ON",
  "OK",        "NEXT",      "NEW",      "LOMEM",
  "LOAD",      "LLIST",     "LIST",     "LET",
  "INPUT",     "IF",        "GRAPHICS", "GOTO",
  "GOSUB",     "GET",       "FOR",      "END",
  "ELSE",      "DIM",       "DELETE",   "DEF",
  "CONTINUE",  "CLS",       "CLOSE",    "DATA",
  "REM",       ":",         "!",        "\\xff"
}, {
  "Á", "É", "Í",  "Ó", "Ö", "Õ", "Ú", "Ü", "Û", "\\t89", "\\t8a", "\\t8b", "\\t8c", "\\t8d", "\\t8e", "\t8f",
  "á", "é", "í",  "ó", "ö", "õ", "ú", "ü", "û", "\\t99", "\\t9a", "\\t9b", "\\t9c", "\\t9d", "\\t9e", "\t9f",

  " ", "!", "\"", "#", "$", "%", "&", "'", "(", ")", "*", "+", ",", "-", ".", "/",
  "0", "1", "2",  "3", "4", "5", "6", "7", "8", "9", ":", ";", "<", "=", ">", "?",
  "@", "A", "B",  "C", "D", "E", "F", "G", "H", "I", "J", "K", "L",    "M", "N", "O",
  "P", "Q", "R",  "S", "T", "U", "V", "W", "X", "Y", "Z", "[", "\\\\", "]", "^", "_",
  "`", "a", "b",  "c", "d", "e", "f", "g", "h", "i", "j", "k", "l",    "m", "n", "o",
  "p", "q", "r",  "s", "t", "u", "v", "w", "x", "y", "z", "{", "|",    "}", "~", "\t7f",

  "\\x80", "\\x81", "\\x82", "\\x83", "\\x84", "\\x85", "\\x86", "\\x87",
  "\\x88", "\\x89", "\\x8a", "\\x8b", "\\x8c", "\\x8d", "\\x8e", "\\x8f",
  "\\x90", "\\x91", "\\x92", "\\x93", "\\x94", "\\x95", "\\x96", "\\x97",
  "\\x98", "\\x99", "\\x9a", "\\x9b", "\\x9c", "\\x9d", "\\x9e", "\\x9f",

  "\\ta0", "\\ta1", "\\ta2", "\\ta3", "\\ta4", "\\ta5", "\\ta6", "\\ta7",
  "\\ta8", "\\ta9", "\\taa", "\\tab", "\\tac", "\\tad", "\\tae", "\\taf",
  "\\tb0", "\\tb1", "\\tb2", "\\tb3", "\\tb4", "\\tb5", "\\tb6", "\\tb7",
  "\\tb8", "\\tb9", "\\tba", "\\tbb", "\\tbc", "\\tbd", "\\tbe", "\\tbf",

  "\\tc0", "\\tc1", "\\tc2", "\\tc3", "\\tc4", "\\tc5", "\\tc6", "\\tc7",
  "\\tc8", "\\tc9", "\\tca", "\\tcb", "\\tcc", "\\tcd", "\\tce", "\\tcf",
  "\\td0", "\\td1", "\\td2", "\\td3", "\\td4", "\\td5", "\\td6", "\\td7",
  "\\td8", "\\td9", "\\tda", "\\tdb", "\\tdc", "\\tdd", "\\tde", "\\tdf",

  "\\xe0", "\\xe1", "\\xe2", "\\xe3", "\\xe4", "\\xe5", "\\xe6", "\\xe7",
  "\\xe8", "\\xe9", "\\xea", "\\xeb", "\\xec", "\\xed", "\\xee", "\\xef",
  "\\xf0", "\\xf1", "\\xf2", "\\xf3", "\\xf4", "\\xf5", "\\xf6", "\\xf7",
  "\\xf8", "\\xf9", "\\xfa", "\\xfb", "\\xfc", "\\xfd", "\\xfe", "\\xff"
}};

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
    opt.debug = 0;
    opt.overw = 0;
 
    while (--argc && **++argv=='-' && parse_arg) {
        switch (argv[0][1]) {
        case 'd': case 'D':
             opt.debug = 1;
             break;
        case 'o': case 'O':
             opt.overw = 1;
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
