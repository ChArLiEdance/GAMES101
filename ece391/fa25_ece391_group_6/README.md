# Team WoTaoのhappyhome

## Members

- jan39 (yocola1)
- yuheng7 (ChArLiEdance)
- taowu6 (Noctrlme)

## Note from P0lina

10.21 start filesystem

before submit

git branch         //check which branch

//check if there is someone else edit it

git fetch origin --prune
git status -sbgit

//then add

git add              //the file you edited

../util/mkfs_ktfs ../sys/ktfs.raw 8M 16 ../usr/bin/hello ../usr/games/trek


./cache_ktfs_test

gcc -std=c11 -D_FORTIFY_SOURCE=0 -iquote .. -I. -iquote ../../../../mp1 cache_ktfs_standalone.c ../cache.c ../ktfs.c ../uio.c ../error.c -o cache_ktfs_test


### **/class/ece391/app/ece391-riscv-dev**

/class/ece391/app/ece391-riscv-dev

[[MP3] MP3 Errata

A lot has changed in MP3. This is a master post to track all errors and clarifications made for MP3. Please pay attention to stay up to date.

* [10/20 20:45] Please update the Makefile in your `usr/` directory to remove all files from `ALL_TARGETS` except `hello`. The remaining files were used for testing but were not provided in your repo. Additionally, please change `UMODE := 1` to `UMODE := 0` for CP1, as `hello` will not build correctly unless this is properly set.
* [10/21 00:34] The Doxygen comment for `ktfs_flush` incorrectly states the return value is `int`. The correct return type is `void`. If an error occurs in `ktfs_flush`, you should do nothing and return immediately.
* [10/21 07:04] For `vioblk_storage_store` and `vioblk_storage_fetch`, writes & reads that exceed the end of the block device should be truncated. Do not return a negative error code in these scenarios.
* [10/21 07:23] For `ramdisk_open`, you should return negative error code when an error occurs. For `ramdisk_close`, if there is an error, you should return immediately.
* [10/21 15:17] In `vioblk_storage_store` and `vioblk_storage_fetch`, writes & reads whose `bytecnt` are not a multiple of `blksz` should be rounded down to the nearest `blksz`. The number of bytes written & read should equal the return value.
