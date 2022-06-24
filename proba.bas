AUTORUN
   1 REM az AUTORUN lehetõleg a .BAS file legelsõ sora legyen
   2 REM A .BAS file-ban iso-8859-2 van, nem CP852 vagy UTF-8
1000 PRINT "TVC-nosztalgia"
1010 X$="árvíztûrõ tükörfúrógép"
1020 GOTO 1010
9000 REM a BASIC-program vége után esetleg adat vagy gépi kód lehet
9001 REM a BYTES kulcsszóval bevezetve
BYTES \x0a\x0b\x0c\x0d
BYTES \x1a\x1b\x1c\x1d
