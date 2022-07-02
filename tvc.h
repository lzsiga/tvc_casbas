/* tvc.h */

#ifndef TVC_H
#define TVC_H

#define PEEK2(ptr) (unsigned short)\
                    ((((unsigned char *)(ptr))[0]) + \
                    (((((unsigned char *)(ptr))[1]) << 8)))

#define POKE2(ptr,value) \
    { \
    ((unsigned char *)(ptr))[0] = (unsigned char)(value); \
    ((unsigned char *)(ptr))[1] = (unsigned char)(((unsigned short)(value))>>8); \
    }

/* tape block-header */
typedef struct TBLOCKHDR {
    unsigned char magic1;       /* offset: 0x00, length: 1, value: 0x00 */
    unsigned char magic2;       /* offset: 0x01, length: 1, value: 0x6A 0b01101010 */
    unsigned char blocktype;    /* offset: 0x02, length: 1 */
    unsigned char filetype;     /* offset: 0x03, length: 1 */
    unsigned char protect;      /* offset: 0x04, length: 1 0 = not protected*/
    unsigned char nsect;        /* offset: 0x05, length: 1 number of sectors*/
} TBLOCKHDR;

#define TBLOCKHDR_MAGIC1      0x00 /* offset 0x00 */
#define TBLOCKHDR_MAGIC2      0x6a /* offset 0x01 */
#define TBLOCKHDR_BLOCK_HEAD  0xff /* offset 0x02 (nsect is 1) */
#define TBLOCKHDR_BLOCK_DATA  0x00 /* offset 0x02 */
#define TBLOCKHDR_FILE_BUFF   0x01 /* offset 0x03: data - buffered (buffersize: 256byte) */
#define TBLOCKHDR_FILE_UNBUFF 0x11 /* offset 0x03: code - not buffered (contigous) */

typedef struct TSECTHDR {
    unsigned char sectno;        /* offset: 0x00, length: 1, 0- */
    unsigned char size;          /* offset: 0x01, length: 1, 0=>256 */
} TSECTHDR;

typedef struct TSECTEND {
    unsigned char eof;           /* offset: 0x00, length: 1; 0==EOF */
    unsigned char crc [2];       /* offset: 0x01, length: 2 */
} TSECTEND;

/* sector layout:
   TSECTHDR 2 bytes
   data     1..256 bytes
   TSECTEND 3 bytes
 */

/* headerblock-layout:
   (sync: 10240*470usec 1*736 usec)
   TBLOCKHDR 6 bytes (blocktype==TBLOCKHDR_BLOCK_HEAD)
   TSECTHDR  2 bytes (sectno==0)
   fnamelen  1 byte  (max 10)
   fname     0-10 byte
   PRGFILEHDR 16 byte (csak TBLOCKHDR.filetype==TBLOCKHDR_FILE_UNBUFF esetén)
   TSECTEND  3 bytes
   (lead off: 5*470usec)
 */

/* datablock-layout:
   (sync: 5120*470usec 1*736 usec)
   TBLOCKHDR 6 bytes (blocktype==TBLOCKHDR_BLOCK_DATA)
   repeated nsect times:
     TSECTHDR  2 bytes (sectno==0..)
     databytes 1-256 bytes
     TSECTEND  3 bytes
   (lead off: 5*470usec)
 */

typedef struct CPMHDR {
    unsigned char magic;         /* offset: 0x00, length: 1 */
    unsigned char dunno;         /* offset: 0x01, length: 1 */
    unsigned char blocknum [2];  /* offset: 0x02, length: 2, little endian  - number of full blocks */
    unsigned char lastblock [2]; /* offset: 0x04, length: 2, little endian  - bytes in last block */
    unsigned char fill [122];    /* offset: 0x06, length: 122, zeroes */
} CPMHDR;

#define CPMHDR_MAGIC 0x11

typedef struct PRGFILEHDR {
    unsigned char magic;         /* offset:  0, length:  1 */
    unsigned char type;          /* offset:  1, length:  1 */
    unsigned char prgsize [2];   /* offset:  2, length:  2, little endian */
    unsigned char autorun;       /* offset:  4, length:  1 */
    unsigned char fill2 [10];    /* offset:  5, length: 10, zeroes */
    unsigned char version;       /* offset: 15, length:  1 */
} PRGFILEHDR;

#define PRGFILE_MAGIC 0x00
#define PRGFILE_TYPE_PROG 0x01
#define PRGFILE_TYPE_DATA 0x00
#define PRGFILE_AUTORUN 0xFF

typedef struct CASHDR {
    CPMHDR cph;                  /* offset: 0x00, length: 128 */
    PRGFILEHDR pfh;              /* offset: 0x10, length: 16  */
} CASHDR;

typedef struct CASHDR_DATA {
    unsigned short blocknum;
    unsigned short lastblock;
    unsigned short prgsize;
    unsigned char  type;
    unsigned char  autorun;
    unsigned char  version;
} CASHDR_DATA;

/* Check: blocknum*128 + lastbock == prgsize + 144 */

typedef struct BASLINE {
    unsigned char len;     /* including itself */
    unsigned char no [2];  /* little endian */
} BASLINE;

#define BASIC_LINEND 0xff /* line terminator */
#define BASIC_PRGEND 0x00 /* program terminator */

#define BASIC_PROGBASE 6639 /* BASIC program begins here */

#define BASIC_TOKEN_START   0x90
#define BASIC_TOKEN_END     0xfe /* 0xff is the terminator, not a token */
#define BASIC_TOKEN_DATA    0xfb /* should not tokenize within DATA */
#define BASIC_TOKEN_COMMENT 0xfe /* should not tokenize after '!' */
#define BASIC_TOKEN_REM     0xfc /* should not tokenize after REM */
#define BASIC_TOKEN_COLON   0xfd /* should tokenize after ':' (if not in comment) */

#endif
