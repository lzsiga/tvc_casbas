AUTORUN
   1 REM az AUTORUN lehet�leg a .BAS file legels� sora legyen
   2 REM A .BAS file-ban iso-8859-2 van, nem CP852 vagy UTF-8
1000 PRINT "TVC-nosztalgia"
1010 X$="�rv�zt�r� t�k�rf�r�g�p"
1020 GOTO 1010
9000 REM a BASIC-program v�ge ut�n esetleg adat vagy g�pi k�d lehet
9001 REM a BYTES kulcssz�val bevezetve
BYTES \x0a\x0b\x0c\x0d
BYTES \x1a\x1b\x1c\x1d
