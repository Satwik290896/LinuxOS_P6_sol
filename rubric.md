### B(4pts) - Changing the formatting program
- B1(1pt) - subdir created with nlink2
- B2.1(1pt) - names.txt created with nlink 1 and no garbage contents
- B2.2(1pt) - big_txt.txt created with nlink 1 and correct size
- B2.3(1pt) - big_img.jpeg created with nlink 1 and no garbage contents

### C(4pts) - Initializing and mounting the file system
- C1(4pts) - Mount returned exit code 0

### D(4pts) - Listing the contents of the root directory
`ls -a --color=never /mnt/ez`
- D1.1(2pts) - Directory listing shows 'hello.txt' and 'subdir'
- D1.2(2pts) - Directory listing has '.' and '..'

### E(5pts) - Accessing subdirectories
`ls -a --color=never /mnt/ez/subdir`
- E1(1pt) - 'names.txt', 'big_txt.txt' and 'big_img.jpeg' found in 'subdir' directory

`stat /mnt/ez/hello.txt`
- E2(1pt) - stat executed successfully

`stat /mnt/ez/file_that_does_not_exist`
- E3(1pt) - stat fails for nonexistent file

`cd /mnt/ez/subdir`
- E4.1(1pt) - chdir to 'subdir' works
- E4.2(1pt) - umount correctly fails when cwd is 'subdir'


### F(5pts) - Reading the contents of regular files
- F1(1pt) - Contents of 'hello.txt' are correct
- F2.1(1pt) - Contents of 'names.txt' extracted correctly in multiple reads
- F2.2(1pt) - Contents of 'big_txt.txt' extracted correctly in multiple reads
- F2.3(1pt) - Contents of 'big_img.jpeg' extracted correctly in multiple reads
- F2.4(1pt) - umount correctly fails when there are open files

### G(5pts) - Filesystem information
- G1.1(1pt) - mtime of 'hello.txt' is correctly read off disk
- G1.2(1pt) - link count of ezfs root is correct (nlink == 3)
- G1.3(3pts) - listing shows 'hello.txt' and 'subdir' with antagonistic(/dev/urandom) image

### H(21pts) - Writing
`echo 1 >> hello.txt`
- H1.1(1pt) - Write succeeded and new contents read successfully
- H1.2(1pt) - File size doesn't change after short write
- H1.3(1pt) - Updated contents read successfully
- H1.4(1pt) - File size increased after long write

`echo 1 > hello.txt`
- H2.1(1pt) - O_TRUNC makes file size 0
- H2.2(1pt) - Truncation correctly overwrote file
- H2.3(1pt) - Updated contents persist after remounting

`cat /mnt/ez/subdir/big_img.jpeg >> /mnt/ez/subdir/big_txt.txt`
- H3.1(1pts) - Reallocation Case1: size correct
- H3.2(3pts) - Reallocation Case1: content correct

`for i in {1..44}; do cat /mnt/ez/subdir/big_img.jpeg >> /mnt/ez/subdir/big_txt.txt; done`
Then, `cat /mnt/ez/subdir/big_img.jpeg >> /mnt/ez/subdir/big_txt.txt` should fail
- H3.3(3pts) - filesystem reaches capacity when file size gets too large

`cat /mnt/ez/subdir/big_txt.txt >> /mnt/ez/hello.txt`
- H4.1(1pts) - Reallocation Case2: size correct
- H4.2(3pts) - Reallocation Case2: content_correct correct
- H4.3(3pts) - Reallocation Case2: won't destroy subdir content

### I(12pts) - Creating new files
- I1(1pt) - 'new.txt' successfully opened with O_CREAT
- I2(1pt) - Directory listing shows 'hello.txt', 'subdir', and 'new.txt'
- I3(1pt) - New file has correct nlink, size, and mode (is a regular file)

`echo 1 >> new.txt / echo 1 > new.txt`
- I4(2pts) - New file can be written to and read from
- I5(1pt) - Directory listing for 'new.txt' persists after remounting

`for i in {1..29}; do touch $i; done`
- I6.1(2pt) - 29 more files successfully created in root dir
Then, `touch 30` should fail
- I6.2(2pts) - Root dir reaches capacity after 32 files
`cd subdir && for i in {1..7}; do touch $i; done`
- I6.3(2pts) - Filesystem reaches capacity when there are 42 inodes

### J(9pts) - Deleting files
- J1.1(1pt) - 'hello.txt' deleted successfully
- J1.2(1pt) - 'hello.txt' deletion persists after remount

`for i in {1..10}; do touch {1..14}; rm {1..14}; done`
- J2(5pts) - 14 files created and deleted (20 times)
- J3.1(1pt) - File that is still opened can be removed
- J3.2(1pt) - File that is still opened can be read/written to after removal

### K(8pts) - Making and removing directories
- K1(1pt) - 'newdir' successfully created, shows up in directory listing
- K2(1pt) - Root dir has correct nlink after 'newdir' is created
- K3(1pt) - 'newdir' has correct nlink, size, and mode (is a directory)
- K4(1pt) - Directory listing of 'newdir' is empty upon creation
- K5.1(1pt) - 'new.txt' and 'newsubdir' successfully created in 'newdir'
- K5.2(1pt) - 'newsubdir' successfully removed in 'newdir'
- K5.3(1pt) - 'newdir' has correct nlink after 'newsubdir' is removed'
- K6(1pt) - ENOTEMPTY for rmdir on 'newdir'

### L(6pts) - Compile and run executable files
- L1(6pts) - Successfullt run executable to print 'Hello, World!

### M(6pts) - Concurrent test
- M1(3pts) - Multi-thread creation and deletion should work well
- M2(3pts) - Multi-thread wrting, creation and deletion should work well

### N(6pts) - Rename
- N1.1(2pt) - Rename hello.txt to hello2.txt
- N1.2(2pts) - Rename hello2.txt to an existing file

`mkdir subdir1 && mv subdir1 subdir`
- N1.3(2pts) - Rename subdir to subdir1