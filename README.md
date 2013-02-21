Forked from http://sourceforge.net/projects/sevenzip

7zip_explode
=============

Modified version of 7zip which supports archive exploding an archive into its constituent blocks. 

## Building.
A Visual Studio 2010 solution is provided in CPP/7zip/Bundles/Alone which can be used to build it.

## Usage:

Exploding:
```
$ Alone p test/KISS_TEST_SET.7z -otest/

7-Zip (A) 9.20  Copyright (c) 1999-2010 Igor Pavlov  2010-11-18
Outputting into : test/

Exploding : test/KISS_TEST_SET.7z

Archive has 22 blocks
/ [0 blocks]
 KISS_TEST_SET [0 blocks]
  KISS_0000_Video_Baby [5 blocks]
  KISS_0000_Video_Parent [4 blocks]
  KISS_0000_Audio [3 blocks]
  KISS_0000_TEMP [3 blocks]
  KISS_0000_OXI [2 blocks]
   KISS_0307_BABY [3 blocks]
  KISS_0000_SURVEY [2 blocks]

Saving as 'test/KISS_TEST_SET/KISS_0000_Video_Baby/Baby_0000_1.AVI.7z'
Saving as 'test/KISS_TEST_SET/KISS_0000_Video_Baby/Baby_0000_2.AVI.7z'
Saving as 'test/KISS_TEST_SET/KISS_0000_Video_Baby/Baby_0000_3.AVI.7z'
Saving as 'test/KISS_TEST_SET/KISS_0000_Video_Baby/Baby_0000_4.AVI.7z'

```

The `-d` switch can be used to limit the explosion depth. For instance, if you only want to explode the archive into
a separate one for the first 2 directory levels, you'd do

` Alone p archive.7z -d2 `

